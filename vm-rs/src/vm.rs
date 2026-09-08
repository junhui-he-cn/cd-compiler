#![allow(dead_code)]

use crate::bytecode::{
    Constant, DataSegment, DebugLocation, DebugRange, DebugSource, FuncId, Function, Instruction,
    MachineFloatFormat, MachineFloatPredicate, MachineIntPredicate, MachineIntWidth,
    MachineMemoryType, MachineScalarType, Program, RelocationKind, RelocationTarget, SymbolTarget,
    TypeId, UpvalueSource, VariantId, MACHINE_ABI_MAX_PARAMS,
};
use crate::format::ParseError;
use crate::jit::{
    JitCallContext, JitExecutionMode, JitFrameMaterialization, JitHelperAbi, JitSafepoint,
    JitSafepointKind, JitState, RuntimeHelper, JIT_ERROR_HANDLE,
};
use crate::memory::{
    LinearMemory, MemoryError, MemoryErrorKind, MemoryRegion, MemoryRegionKind, VmAddress,
};
#[cfg(test)]
use crate::runtime::HeapObjectKind;
use crate::runtime::{
    Cell, FunctionValue, Heap, HeapStats, IteratorSource, IteratorValue, SharedEnvironment,
};
use crate::scheduler::{
    CooperativeScheduler, DispatchContext, FrameStack, JoinStatus, ResumableFrame as Frame,
    ReturnTarget, SchedulerError, TaskStep, VariablePlan,
};
pub use crate::scheduler::{TaskId, TaskState};
use crate::value::Value;
use std::collections::BTreeMap;
use std::fmt;
use std::ops::Index;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StackFrame {
    pub function: String,
    pub location: Option<DebugLocation>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TraceEventKind {
    Enter,
    Line,
    Output,
    Return,
    Exit,
    Error,
}

impl TraceEventKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Enter => "enter",
            Self::Line => "line",
            Self::Output => "output",
            Self::Return => "return",
            Self::Exit => "exit",
            Self::Error => "error",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TraceEvent {
    pub sequence: usize,
    pub kind: TraceEventKind,
    pub function: String,
    pub instruction: Option<usize>,
    pub location: Option<DebugLocation>,
    pub stack: Vec<StackFrame>,
    pub locals: Vec<(String, String)>,
    pub value: Option<String>,
}

pub struct TraceRun {
    pub events: Vec<TraceEvent>,
    pub result: Result<String, RuntimeError>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DebugPause {
    pub function: String,
    pub instruction: usize,
    pub location: Option<DebugLocation>,
    pub stack: Vec<StackFrame>,
    pub locals: Vec<(String, String)>,
    /// Machine registers and frame metadata when this pause observes machine
    /// state. Dynamic-only pauses leave this field unset.
    pub machine: Option<DebugMachineState>,
}

/// Rendered machine state captured at a debugger pause.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DebugMachineState {
    /// Register index -> stable `Value` display text.
    pub registers: Vec<(usize, String)>,
    /// VM address of the active machine frame, if one is allocated.
    pub frame_base: Option<VmAddress>,
    pub frame_size: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DebugControl {
    Continue,
    Quit,
}

pub trait DebugHook {
    fn on_instruction(&mut self, pause: DebugPause) -> DebugControl;

    fn on_error(&mut self, pause: DebugPause, _error: &RuntimeError) -> DebugControl {
        let _ = pause;
        DebugControl::Continue
    }
}

/// Scheduler state observed while one cooperative task is paused in its debug
/// hook. `ready` preserves FIFO dispatch order and excludes `running`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CooperativeDebugState {
    pub running: TaskId,
    pub ready: Vec<TaskId>,
    pub tasks: Vec<(TaskId, TaskState)>,
}

/// One task-attributed cooperative debugger pause.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CooperativeDebugPause {
    pub task_id: TaskId,
    pub function: String,
    pub instruction: usize,
    pub location: Option<DebugLocation>,
    pub stack: Vec<StackFrame>,
    pub locals: Vec<(String, String)>,
    pub machine: Option<DebugMachineState>,
    pub scheduler: CooperativeDebugState,
}

/// Synchronous task-aware debugger hook.
///
/// No other task is dispatched while either callback is running.
pub trait CooperativeDebugHook {
    fn on_instruction(&mut self, pause: CooperativeDebugPause) -> DebugControl;

    fn on_error(&mut self, pause: CooperativeDebugPause, _error: &RuntimeError) -> DebugControl {
        let _ = pause;
        DebugControl::Continue
    }
}

pub struct DebugRun {
    pub result: Result<String, RuntimeError>,
    pub quit: bool,
}

/// Deterministic execution counters collected by the opt-in profile API.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ProfileReport {
    pub instruction_count: usize,
    pub output_bytes: usize,
    /// Number of allocations recorded by the tracked VM storage ledger.
    pub tracked_heap_allocations: usize,
    /// Maximum simultaneously live allocations in the tracked VM storage ledger.
    pub tracked_heap_peak_live: usize,
    /// Estimated bytes retained by currently live tracked VM storage.
    pub tracked_heap_estimated_live_bytes: usize,
    /// Maximum estimated bytes retained by tracked VM storage during execution.
    pub tracked_heap_estimated_peak_live_bytes: usize,
    pub functions: Vec<ProfileFunction>,
    pub natives: Vec<ProfileNative>,
    pub source_ranges: Vec<ProfileSourceRange>,
}

/// Counters for one bytecode function. `index` is `None` for the entry body.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProfileFunction {
    pub index: Option<usize>,
    pub name: String,
    pub calls: usize,
    pub instructions: usize,
}

/// Invocation count for one registered native name.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProfileNative {
    pub name: String,
    pub calls: usize,
}

/// Execution hits for one source-local debug range.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProfileSourceRange {
    pub range: DebugRange,
    pub hits: usize,
}

/// The profile report is returned even when execution fails.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProfileRun {
    pub report: ProfileReport,
    pub result: Result<String, RuntimeError>,
}

/// Deterministic counters attributed to one cooperative task.
///
/// Shared-heap counters remain session-wide because task values can share
/// object identity through globals and task outcomes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TaskProfileReport {
    pub task_id: TaskId,
    pub instruction_count: usize,
    pub output_bytes: usize,
    pub functions: Vec<ProfileFunction>,
    pub natives: Vec<ProfileNative>,
    pub source_ranges: Vec<ProfileSourceRange>,
}

/// Snapshot of an opt-in cooperative profiling session.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CooperativeProfileReport {
    pub aggregate: ProfileReport,
    pub tasks: Vec<TaskProfileReport>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResourceKind {
    InstructionSteps,
    CallDepth,
    RuntimeElements,
    OutputBytes,
}

impl ResourceKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::InstructionSteps => "instruction steps",
            Self::CallDepth => "call depth",
            Self::RuntimeElements => "runtime elements",
            Self::OutputBytes => "output bytes",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RuntimeErrorKind {
    Runtime,
    Resource(ResourceKind),
    Cancelled,
    DebuggerQuit,
    MemoryOutOfBounds,
    NullPointerAccess,
    WriteToReadOnlyMemory,
    InvalidAddress,
    InvalidMemoryOperation,
    StackOverflow,
    IntegerDivisionByZero,
    IntegerDivisionOverflow,
    InvalidShiftAmount,
    InvalidConversion,
    InvalidInstruction,
    InvalidOperand,
}

impl RuntimeErrorKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Runtime => "runtime",
            Self::Resource(_) => "resource",
            Self::Cancelled => "cancelled",
            Self::DebuggerQuit => "debugger_quit",
            Self::MemoryOutOfBounds => "memory_out_of_bounds",
            Self::NullPointerAccess => "null_pointer_access",
            Self::WriteToReadOnlyMemory => "write_to_read_only_memory",
            Self::InvalidAddress => "invalid_address",
            Self::InvalidMemoryOperation => "invalid_memory_operation",
            Self::StackOverflow => "stack_overflow",
            Self::IntegerDivisionByZero => "integer_division_by_zero",
            Self::IntegerDivisionOverflow => "integer_division_overflow",
            Self::InvalidShiftAmount => "invalid_shift_amount",
            Self::InvalidConversion => "invalid_conversion",
            Self::InvalidInstruction => "invalid_instruction",
            Self::InvalidOperand => "invalid_operand",
        }
    }
}

pub const DEFAULT_MAX_INSTRUCTION_STEPS: usize = 10_000_000;
pub const DEFAULT_MAX_CALL_DEPTH: usize = 1_024;
pub const DEFAULT_MAX_RUNTIME_ELEMENTS: usize = 1_000_000;
pub const DEFAULT_MAX_OUTPUT_BYTES: usize = 16 * 1024 * 1024;
pub const DEFAULT_MAX_ARTIFACT_BYTES: usize = 64 * 1024 * 1024;
pub const DEFAULT_MAX_MODULE_COUNT: usize = 1_024;
pub const DEFAULT_MAX_MODULE_INSTRUCTIONS: usize = 1_000_000;
pub const DEFAULT_MAX_MACHINE_STACK_BYTES: usize = 1024 * 1024;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum NativeId {
    Push,
    Pop,
    Remove,
    Clear,
    Merge,
    Keys,
    Values,
    Floor,
    Ceil,
    Sqrt,
    Str,
    Substr,
    CharAt,
    TypeOf,
    Hash,
    Contains,
    Slice,
    Copy,
    Concat,
    Map,
    Filter,
    FlatMap,
    Any,
    All,
    Count,
    Find,
    FindIndex,
    Reduce,
    Range,
    Print,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ModuleState {
    Uninitialized,
    Initializing,
    Initialized,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum NativeResourceProfile {
    None,
    InstructionCheckpoints,
    RuntimeElements,
    Both,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum NativeArgumentShape {
    Any,
    Array,
    Map,
    MapKey,
    Number,
    String,
    Collection,
    Callback1Any,
    Callback1Bool,
    Callback1Array,
    Callback2Any,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum NativeReturnShape {
    Any,
    AnyOrNil,
    Nil,
    Number,
    Bool,
    String,
    Array,
    Map,
    Range,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct NativeSignature {
    arguments: &'static [NativeArgumentShape],
    result: NativeReturnShape,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct NativeSpec {
    id: NativeId,
    name: &'static str,
    min_arity: usize,
    max_arity: usize,
    callback: bool,
    arity_error: &'static str,
    resource: NativeResourceProfile,
    signature: NativeSignature,
}

const NATIVE_SPECS: &[NativeSpec] = &[
    NativeSpec {
        id: NativeId::Push,
        name: "push",
        min_arity: 2,
        max_arity: 2,
        callback: false,
        arity_error: "push expects 2 arguments",
        resource: NativeResourceProfile::RuntimeElements,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Array, NativeArgumentShape::Any],
            result: NativeReturnShape::Nil,
        },
    },
    NativeSpec {
        id: NativeId::Pop,
        name: "pop",
        min_arity: 1,
        max_arity: 1,
        callback: false,
        arity_error: "pop expects 1 arguments",
        resource: NativeResourceProfile::None,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Array],
            result: NativeReturnShape::Any,
        },
    },
    NativeSpec {
        id: NativeId::Remove,
        name: "remove",
        min_arity: 2,
        max_arity: 2,
        callback: false,
        arity_error: "remove expects 2 arguments",
        resource: NativeResourceProfile::None,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Map, NativeArgumentShape::MapKey],
            result: NativeReturnShape::Any,
        },
    },
    NativeSpec {
        id: NativeId::Clear,
        name: "clear",
        min_arity: 1,
        max_arity: 1,
        callback: false,
        arity_error: "clear expects 1 argument",
        resource: NativeResourceProfile::None,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Map],
            result: NativeReturnShape::Nil,
        },
    },
    NativeSpec {
        id: NativeId::Merge,
        name: "merge",
        min_arity: 2,
        max_arity: 2,
        callback: false,
        arity_error: "merge expects 2 arguments",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Map, NativeArgumentShape::Map],
            result: NativeReturnShape::Map,
        },
    },
    NativeSpec {
        id: NativeId::Keys,
        name: "keys",
        min_arity: 1,
        max_arity: 1,
        callback: false,
        arity_error: "keys expects 1 argument",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Map],
            result: NativeReturnShape::Array,
        },
    },
    NativeSpec {
        id: NativeId::Values,
        name: "values",
        min_arity: 1,
        max_arity: 1,
        callback: false,
        arity_error: "values expects 1 argument",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Map],
            result: NativeReturnShape::Array,
        },
    },
    NativeSpec {
        id: NativeId::Floor,
        name: "floor",
        min_arity: 1,
        max_arity: 1,
        callback: false,
        arity_error: "floor expects 1 arguments",
        resource: NativeResourceProfile::None,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Number],
            result: NativeReturnShape::Number,
        },
    },
    NativeSpec {
        id: NativeId::Ceil,
        name: "ceil",
        min_arity: 1,
        max_arity: 1,
        callback: false,
        arity_error: "ceil expects 1 arguments",
        resource: NativeResourceProfile::None,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Number],
            result: NativeReturnShape::Number,
        },
    },
    NativeSpec {
        id: NativeId::Sqrt,
        name: "sqrt",
        min_arity: 1,
        max_arity: 1,
        callback: false,
        arity_error: "sqrt expects 1 arguments",
        resource: NativeResourceProfile::None,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Number],
            result: NativeReturnShape::Number,
        },
    },
    NativeSpec {
        id: NativeId::Str,
        name: "str",
        min_arity: 1,
        max_arity: 1,
        callback: false,
        arity_error: "str expects 1 arguments",
        resource: NativeResourceProfile::None,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Any],
            result: NativeReturnShape::String,
        },
    },
    NativeSpec {
        id: NativeId::Substr,
        name: "substr",
        min_arity: 3,
        max_arity: 3,
        callback: false,
        arity_error: "substr expects 3 arguments",
        resource: NativeResourceProfile::None,
        signature: NativeSignature {
            arguments: &[
                NativeArgumentShape::String,
                NativeArgumentShape::Number,
                NativeArgumentShape::Number,
            ],
            result: NativeReturnShape::String,
        },
    },
    NativeSpec {
        id: NativeId::CharAt,
        name: "charAt",
        min_arity: 2,
        max_arity: 2,
        callback: false,
        arity_error: "charAt expects 2 arguments",
        resource: NativeResourceProfile::None,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::String, NativeArgumentShape::Number],
            result: NativeReturnShape::String,
        },
    },
    NativeSpec {
        id: NativeId::TypeOf,
        name: "typeOf",
        min_arity: 1,
        max_arity: 1,
        callback: false,
        arity_error: "typeOf expects 1 arguments",
        resource: NativeResourceProfile::None,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Any],
            result: NativeReturnShape::String,
        },
    },
    NativeSpec {
        id: NativeId::Hash,
        name: "hash",
        min_arity: 1,
        max_arity: 1,
        callback: false,
        arity_error: "hash expects 1 argument",
        resource: NativeResourceProfile::None,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Any],
            result: NativeReturnShape::Number,
        },
    },
    NativeSpec {
        id: NativeId::Contains,
        name: "contains",
        min_arity: 2,
        max_arity: 2,
        callback: false,
        arity_error: "contains expects 2 arguments",
        resource: NativeResourceProfile::InstructionCheckpoints,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Collection, NativeArgumentShape::Any],
            result: NativeReturnShape::Bool,
        },
    },
    NativeSpec {
        id: NativeId::Slice,
        name: "slice",
        min_arity: 3,
        max_arity: 3,
        callback: false,
        arity_error: "slice expects 3 arguments",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Number,
                NativeArgumentShape::Number,
            ],
            result: NativeReturnShape::Array,
        },
    },
    NativeSpec {
        id: NativeId::Copy,
        name: "copy",
        min_arity: 1,
        max_arity: 1,
        callback: false,
        arity_error: "copy expects 1 argument",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Array],
            result: NativeReturnShape::Array,
        },
    },
    NativeSpec {
        id: NativeId::Concat,
        name: "concat",
        min_arity: 2,
        max_arity: 2,
        callback: false,
        arity_error: "concat expects 2 arguments",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Array, NativeArgumentShape::Array],
            result: NativeReturnShape::Array,
        },
    },
    NativeSpec {
        id: NativeId::Map,
        name: "map",
        min_arity: 2,
        max_arity: 2,
        callback: true,
        arity_error: "map expects 2 arguments",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Any,
            ],
            result: NativeReturnShape::Array,
        },
    },
    NativeSpec {
        id: NativeId::Filter,
        name: "filter",
        min_arity: 2,
        max_arity: 2,
        callback: true,
        arity_error: "filter expects 2 arguments",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Bool,
            ],
            result: NativeReturnShape::Array,
        },
    },
    NativeSpec {
        id: NativeId::FlatMap,
        name: "flatMap",
        min_arity: 2,
        max_arity: 2,
        callback: true,
        arity_error: "flatMap expects 2 arguments",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Array,
            ],
            result: NativeReturnShape::Array,
        },
    },
    NativeSpec {
        id: NativeId::Any,
        name: "any",
        min_arity: 2,
        max_arity: 2,
        callback: true,
        arity_error: "any expects 2 arguments",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Bool,
            ],
            result: NativeReturnShape::Bool,
        },
    },
    NativeSpec {
        id: NativeId::All,
        name: "all",
        min_arity: 2,
        max_arity: 2,
        callback: true,
        arity_error: "all expects 2 arguments",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Bool,
            ],
            result: NativeReturnShape::Bool,
        },
    },
    NativeSpec {
        id: NativeId::Count,
        name: "count",
        min_arity: 2,
        max_arity: 2,
        callback: true,
        arity_error: "count expects 2 arguments",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Bool,
            ],
            result: NativeReturnShape::Number,
        },
    },
    NativeSpec {
        id: NativeId::Find,
        name: "find",
        min_arity: 2,
        max_arity: 2,
        callback: true,
        arity_error: "find expects 2 arguments",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Bool,
            ],
            result: NativeReturnShape::AnyOrNil,
        },
    },
    NativeSpec {
        id: NativeId::FindIndex,
        name: "findIndex",
        min_arity: 2,
        max_arity: 2,
        callback: true,
        arity_error: "findIndex expects 2 arguments",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Bool,
            ],
            result: NativeReturnShape::Number,
        },
    },
    NativeSpec {
        id: NativeId::Reduce,
        name: "reduce",
        min_arity: 3,
        max_arity: 3,
        callback: true,
        arity_error: "reduce expects 3 arguments",
        resource: NativeResourceProfile::Both,
        signature: NativeSignature {
            arguments: &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Any,
                NativeArgumentShape::Callback2Any,
            ],
            result: NativeReturnShape::Any,
        },
    },
    NativeSpec {
        id: NativeId::Range,
        name: "range",
        min_arity: 1,
        max_arity: 3,
        callback: false,
        arity_error: "range expects 1 to 3 arguments",
        resource: NativeResourceProfile::RuntimeElements,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Number],
            result: NativeReturnShape::Range,
        },
    },
    NativeSpec {
        id: NativeId::Print,
        name: "print",
        min_arity: 1,
        max_arity: 1,
        callback: false,
        arity_error: "print expects 1 argument",
        resource: NativeResourceProfile::None,
        signature: NativeSignature {
            arguments: &[NativeArgumentShape::Any],
            result: NativeReturnShape::Nil,
        },
    },
];

fn decode_constant(constant: &Constant) -> Result<Value, RuntimeError> {
    match constant {
        Constant::Nil => Ok(Value::Nil),
        Constant::Number(value) => value
            .parse::<f64>()
            .map(Value::number)
            .map_err(|_| RuntimeError::new("invalid number constant")),
        Constant::Bool(value) => Ok(Value::boolean(*value)),
        Constant::String(value) => Ok(Value::string(value.clone())),
    }
}

fn initialize_machine_segments(
    memory: &mut LinearMemory,
    segments: &[DataSegment],
) -> Result<Vec<MemoryRegion>, RuntimeError> {
    for (index, segment) in segments.iter().enumerate() {
        if segment.alignment == 0 || !segment.alignment.is_power_of_two() {
            return Err(RuntimeError::invalid_instruction(format!(
                "data segment d{} alignment {} is not a positive power of two",
                index, segment.alignment
            )));
        }
        match segment.kind {
            MemoryRegionKind::Rodata | MemoryRegionKind::Data => {
                let Some(initial) = segment.initial.as_deref() else {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "data segment d{} in {} has no initialization payload",
                        index, segment.kind
                    )));
                };
                let initial_size = u64::try_from(initial.len()).map_err(|_| {
                    RuntimeError::invalid_instruction(format!(
                        "data segment d{} initialization is too large",
                        index
                    ))
                })?;
                if initial_size != segment.size {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "data segment d{} declares size {}, but initialization has {} bytes",
                        index, segment.size, initial_size
                    )));
                }
            }
            MemoryRegionKind::Bss => {
                if segment.initial.is_some() {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "bss data segment d{} has an initialization payload",
                        index
                    )));
                }
            }
            MemoryRegionKind::Heap | MemoryRegionKind::Stack => {
                return Err(RuntimeError::invalid_instruction(format!(
                    "data segment d{} has unsupported kind {}",
                    index, segment.kind
                )));
            }
        }
    }

    // The static layout is fixed by kind, while descriptor order remains
    // stable within each segment class. Keep the resulting regions indexed by
    // descriptor so symbols do not depend on allocation order.
    let mut regions = vec![None; segments.len()];
    for kind in [
        MemoryRegionKind::Rodata,
        MemoryRegionKind::Data,
        MemoryRegionKind::Bss,
    ] {
        for (index, segment) in segments
            .iter()
            .enumerate()
            .filter(|(_, segment)| segment.kind == kind)
        {
            let region = match kind {
                MemoryRegionKind::Rodata | MemoryRegionKind::Data => memory
                    .allocate_region_with_bytes(
                        kind,
                        segment.alignment,
                        segment.initial.as_deref().ok_or_else(|| {
                            RuntimeError::invalid_instruction(format!(
                                "data segment d{} in {} has no initialization payload",
                                index, segment.kind
                            ))
                        })?,
                    )
                    .map_err(RuntimeError::from)?,
                MemoryRegionKind::Bss => memory
                    .allocate_region(kind, segment.size, segment.alignment)
                    .map_err(RuntimeError::from)?,
                MemoryRegionKind::Heap | MemoryRegionKind::Stack => {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "data segment d{} has unsupported kind {}",
                        index, segment.kind
                    )))
                }
            };
            regions[index] = Some(region);
        }
    }
    regions
        .into_iter()
        .enumerate()
        .map(|(index, region)| {
            region.ok_or_else(|| {
                RuntimeError::invalid_instruction(format!(
                    "data segment d{} has no allocated static region",
                    index
                ))
            })
        })
        .collect()
}

fn add_machine_address_addend(address: VmAddress, addend: i64) -> Option<VmAddress> {
    let resolved = if addend >= 0 {
        address.checked_add(addend as u64)?
    } else {
        address.checked_sub(addend.unsigned_abs())?
    };
    (resolved >= crate::memory::NULL_GUARD_END).then_some(resolved)
}

fn resolve_machine_relocations(
    memory: &mut LinearMemory,
    program: &Program,
    regions: &[MemoryRegion],
) -> Result<BTreeMap<(usize, usize), FuncId>, RuntimeError> {
    let mut symbols = BTreeMap::new();
    for (index, symbol) in program.symbols.iter().enumerate() {
        if symbol.name.is_empty() {
            return Err(RuntimeError::invalid_instruction(format!(
                "machine symbol s{} has an empty name",
                index
            )));
        }
        if symbols.contains_key(&symbol.name) {
            return Err(RuntimeError::invalid_instruction(format!(
                "duplicate machine symbol `{}`",
                symbol.name
            )));
        }
        let resolved = match &symbol.target {
            SymbolTarget::Function(function) => {
                if program.functions.get(function.0 as usize).is_none() {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine symbol `{}` references function f{} out of range",
                        symbol.name, function.0
                    )));
                }
                ResolvedMachineSymbol::Function(*function)
            }
            SymbolTarget::Data { segment, offset } => {
                let Some(descriptor) = program.data_segments.get(*segment) else {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine symbol `{}` references data segment d{} out of range",
                        symbol.name, segment
                    )));
                };
                let Some(region) = regions.get(*segment) else {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine symbol `{}` has no allocated data segment d{}",
                        symbol.name, segment
                    )));
                };
                let Some(address) = region.base.checked_add(*offset) else {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine symbol `{}` address overflows",
                        symbol.name
                    )));
                };
                if *offset >= descriptor.size || !region.contains(address) {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine symbol `{}` offset {} is outside data segment d{}",
                        symbol.name, offset, segment
                    )));
                }
                ResolvedMachineSymbol::Address(address)
            }
        };
        symbols.insert(symbol.name.clone(), resolved);
    }

    let mut patches = Vec::new();
    let mut direct_calls = BTreeMap::new();
    let mut patched_data: BTreeMap<usize, Vec<(u64, u64, usize)>> = BTreeMap::new();
    for (index, relocation) in program.relocations.iter().enumerate() {
        let Some(symbol) = symbols.get(&relocation.symbol) else {
            return Err(RuntimeError::invalid_instruction(format!(
                "machine relocation r{} references undefined symbol `{}`",
                index, relocation.symbol
            )));
        };
        match (&relocation.kind, &relocation.target, symbol) {
            (
                RelocationKind::Abs64,
                RelocationTarget::Data { segment, offset },
                ResolvedMachineSymbol::Address(symbol_address),
            ) => {
                let Some(descriptor) = program.data_segments.get(*segment) else {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine relocation r{} targets data segment d{} out of range",
                        index, segment
                    )));
                };
                let Some(region) = regions.get(*segment) else {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine relocation r{} has no allocated target segment d{}",
                        index, segment
                    )));
                };
                let Some(end) = offset.checked_add(8) else {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine relocation r{} target offset overflows",
                        index
                    )));
                };
                if end > descriptor.size || end > region.size() {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine relocation r{} target range is outside data segment d{}",
                        index, segment
                    )));
                }
                let target_address = region.base.checked_add(*offset).ok_or_else(|| {
                    RuntimeError::invalid_instruction(format!(
                        "machine relocation r{} target address overflows",
                        index
                    ))
                })?;
                let ranges = patched_data.entry(*segment).or_default();
                if let Some((_, _, previous)) = ranges
                    .iter()
                    .find(|(start, previous_end, _)| *offset < *previous_end && *start < end)
                {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine relocation r{} overlaps relocation r{} in data segment d{}",
                        index, previous, segment
                    )));
                }
                ranges.push((*offset, end, index));
                let resolved = add_machine_address_addend(*symbol_address, relocation.addend)
                    .ok_or_else(|| {
                        RuntimeError::invalid_instruction(format!(
                            "machine relocation r{} symbol address overflows",
                            index
                        ))
                    })?;
                patches.push((target_address, resolved.to_le_bytes()));
            }
            (RelocationKind::Abs64, RelocationTarget::Data { .. }, _) => {
                return Err(RuntimeError::invalid_instruction(format!(
                    "machine relocation r{} ABS64 requires a data symbol",
                    index
                )));
            }
            (RelocationKind::Abs64, RelocationTarget::CallDirect { .. }, _) => {
                return Err(RuntimeError::invalid_instruction(format!(
                    "machine relocation r{} ABS64 requires a data target",
                    index
                )));
            }
            (
                RelocationKind::FuncIndex,
                RelocationTarget::CallDirect {
                    function,
                    instruction,
                },
                ResolvedMachineSymbol::Function(target),
            ) => {
                if relocation.addend != 0 {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine relocation r{} FUNC_INDEX does not accept an addend",
                        index
                    )));
                }
                let Some(body) = program.functions.get(function.0 as usize) else {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine relocation r{} call body f{} is out of range",
                        index, function.0
                    )));
                };
                if !matches!(
                    body.instructions.get(*instruction),
                    Some(Instruction::CallDirect { .. })
                ) {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine relocation r{} target is not a call_direct operand",
                        index
                    )));
                }
                if direct_calls
                    .insert((function.0 as usize, *instruction), *target)
                    .is_some()
                {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "machine relocation r{} duplicates a call target",
                        index
                    )));
                }
            }
            (RelocationKind::FuncIndex, RelocationTarget::CallDirect { .. }, _) => {
                return Err(RuntimeError::invalid_instruction(format!(
                    "machine relocation r{} FUNC_INDEX requires a function symbol",
                    index
                )));
            }
            (RelocationKind::FuncIndex, RelocationTarget::Data { .. }, _) => {
                return Err(RuntimeError::invalid_instruction(format!(
                    "machine relocation r{} FUNC_INDEX requires a call_direct target",
                    index
                )));
            }
        }
    }

    for (address, bytes) in patches {
        memory
            .patch_bytes(address, &bytes)
            .map_err(RuntimeError::from)?;
    }
    Ok(direct_calls)
}

#[derive(Clone, Copy)]
enum ResolvedMachineSymbol {
    Function(FuncId),
    Address(VmAddress),
}

fn decode_machine_memory_bits(
    bytes: &[u8],
    memory_type: MachineMemoryType,
) -> Result<u64, RuntimeError> {
    if bytes.len() != memory_type.size() {
        return Err(RuntimeError::invalid_instruction(format!(
            "load {} expected {} bytes, got {}",
            memory_type.as_str(),
            memory_type.size(),
            bytes.len()
        )));
    }
    let mut raw = 0u64;
    for (index, byte) in bytes.iter().enumerate() {
        raw |= u64::from(*byte) << (index * 8);
    }
    Ok(raw)
}

fn encode_machine_memory_bits(value: u64, size: usize) -> Vec<u8> {
    value.to_le_bytes().iter().take(size).copied().collect()
}

fn signed_machine_int(raw: u64, width: MachineIntWidth) -> i128 {
    let raw = raw & width.mask();
    let sign_bit = 1u64 << (width.bits() - 1);
    if raw & sign_bit == 0 {
        raw as i128
    } else {
        raw as i128 - (1i128 << width.bits())
    }
}

fn canonical_machine_float(value: f64, format: MachineFloatFormat) -> f64 {
    match format {
        MachineFloatFormat::F32 => {
            if value.is_nan() {
                f32::from_bits(0x7fc0_0000) as f64
            } else {
                (value as f32) as f64
            }
        }
        MachineFloatFormat::F64 => {
            if value.is_nan() {
                f64::from_bits(0x7ff8_0000_0000_0000)
            } else {
                value
            }
        }
    }
}

fn machine_float_from_bits(format: MachineFloatFormat, bits: u64) -> Result<f64, String> {
    match format {
        MachineFloatFormat::F32 => {
            let bits =
                u32::try_from(bits).map_err(|_| "fconst f32 bits exceed 32 bits".to_string())?;
            Ok(canonical_machine_float(f32::from_bits(bits) as f64, format))
        }
        MachineFloatFormat::F64 => Ok(canonical_machine_float(f64::from_bits(bits), format)),
    }
}

fn apply_machine_float_predicate(left: f64, right: f64, predicate: MachineFloatPredicate) -> bool {
    let unordered = left.is_nan() || right.is_nan();
    match predicate {
        MachineFloatPredicate::OEq => !unordered && left == right,
        MachineFloatPredicate::ONe => !unordered && left != right,
        MachineFloatPredicate::OLt => !unordered && left < right,
        MachineFloatPredicate::OLe => !unordered && left <= right,
        MachineFloatPredicate::OGt => !unordered && left > right,
        MachineFloatPredicate::OGe => !unordered && left >= right,
        MachineFloatPredicate::UEq => unordered || left == right,
        MachineFloatPredicate::UNe => unordered || left != right,
        MachineFloatPredicate::ULt => unordered || left < right,
        MachineFloatPredicate::ULe => unordered || left <= right,
        MachineFloatPredicate::UGt => unordered || left > right,
        MachineFloatPredicate::UGe => unordered || left >= right,
        MachineFloatPredicate::Ord => !unordered,
        MachineFloatPredicate::Uno => unordered,
    }
}

fn machine_shift_name(kind: MachineShiftKind) -> &'static str {
    match kind {
        MachineShiftKind::Left => "shl",
        MachineShiftKind::LogicalRight => "lshr",
        MachineShiftKind::ArithmeticRight => "ashr",
    }
}

fn prepare_function(function: &Function) -> PreparedFunction {
    let mut variable_plan = VariablePlan::default();
    for param in &function.params {
        variable_plan.local_names.push(param.clone());
    }
    PreparedFunction {
        name: Rc::from(function.name.as_str()),
        params: function.params.clone(),
        body: Rc::new(function.clone()),
        variable_plan: Rc::new(variable_plan),
    }
}

fn machine_scalar_matches(expected: MachineScalarType, value: &Value) -> bool {
    matches!(
        (expected, value),
        (MachineScalarType::MachineInt, Value::MachineInt(_))
            | (MachineScalarType::MachineFloat, Value::MachineFloat(_))
            | (MachineScalarType::Address, Value::Address(_))
    )
}

fn native_spec(name: &str) -> Option<&'static NativeSpec> {
    let index = match name {
        "push" => 0,
        "pop" => 1,
        "remove" => 2,
        "clear" => 3,
        "merge" => 4,
        "keys" => 5,
        "values" => 6,
        "floor" => 7,
        "ceil" => 8,
        "sqrt" => 9,
        "str" => 10,
        "substr" => 11,
        "charAt" => 12,
        "typeOf" => 13,
        "hash" => 14,
        "contains" => 15,
        "slice" => 16,
        "copy" => 17,
        "concat" => 18,
        "map" => 19,
        "filter" => 20,
        "flatMap" => 21,
        "any" => 22,
        "all" => 23,
        "count" => 24,
        "find" => 25,
        "findIndex" => 26,
        "reduce" => 27,
        "range" => 28,
        "print" => 29,
        _ => return None,
    };
    Some(&NATIVE_SPECS[index])
}

/// Verification-time arity bounds for a registered native name.
pub(crate) fn native_arity_bounds(name: &str) -> Option<(usize, usize)> {
    native_spec(name).map(|spec| (spec.min_arity, spec.max_arity))
}

#[derive(Clone, Debug)]
pub struct CancellationToken {
    cancelled: Arc<AtomicBool>,
}

impl CancellationToken {
    pub fn new() -> Self {
        Self {
            cancelled: Arc::new(AtomicBool::new(false)),
        }
    }

    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
    }

    fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Acquire)
    }
}

impl Default for CancellationToken {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Clone, Debug)]
pub struct RunConfig {
    pub max_instruction_steps: Option<usize>,
    pub max_call_depth: Option<usize>,
    pub max_runtime_elements: Option<usize>,
    pub max_output_bytes: Option<usize>,
    pub max_artifact_bytes: Option<usize>,
    pub max_module_count: Option<usize>,
    pub max_module_instructions: Option<usize>,
    /// Capacity reserved for each task's upward-growing machine stack.
    /// `None` selects the default finite capacity.
    pub max_machine_stack_bytes: Option<usize>,
    pub cancellation: Option<CancellationToken>,
}

impl RunConfig {
    pub fn unlimited() -> Self {
        Self {
            max_instruction_steps: None,
            max_call_depth: None,
            max_runtime_elements: None,
            max_output_bytes: None,
            max_artifact_bytes: None,
            max_module_count: None,
            max_module_instructions: None,
            max_machine_stack_bytes: None,
            cancellation: None,
        }
    }

    pub fn with_cancellation(mut self, cancellation: CancellationToken) -> Self {
        self.cancellation = Some(cancellation);
        self
    }

    pub fn with_machine_stack_bytes(mut self, bytes: usize) -> Self {
        self.max_machine_stack_bytes = Some(bytes);
        self
    }
}

impl Default for RunConfig {
    fn default() -> Self {
        Self {
            max_instruction_steps: Some(DEFAULT_MAX_INSTRUCTION_STEPS),
            max_call_depth: Some(DEFAULT_MAX_CALL_DEPTH),
            max_runtime_elements: Some(DEFAULT_MAX_RUNTIME_ELEMENTS),
            max_output_bytes: Some(DEFAULT_MAX_OUTPUT_BYTES),
            max_artifact_bytes: Some(DEFAULT_MAX_ARTIFACT_BYTES),
            max_module_count: Some(DEFAULT_MAX_MODULE_COUNT),
            max_module_instructions: Some(DEFAULT_MAX_MODULE_INSTRUCTIONS),
            max_machine_stack_bytes: Some(DEFAULT_MAX_MACHINE_STACK_BYTES),
            cancellation: None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RuntimeError {
    pub kind: RuntimeErrorKind,
    /// The configured limit when `kind` is `RuntimeErrorKind::Resource`.
    /// Other runtime failures leave this unset.
    pub resource_limit: Option<usize>,
    pub message: String,
    pub location: Option<DebugLocation>,
    pub stack: Vec<StackFrame>,
    pub sources: Vec<DebugSource>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bytecode::{
        BlockId, Function, NativeId, NativeImport, Relocation, RelocationKind, RelocationTarget,
        Symbol, SymbolTarget,
    };
    use crate::runtime::{new_cell, new_environment};
    use std::cell::RefCell;

    #[test]
    fn native_registry_describes_dispatch_names_and_callback_shapes() {
        let mut names = Vec::new();
        for spec in NATIVE_SPECS {
            assert!(names.iter().all(|name| *name != spec.name));
            names.push(spec.name);
            assert_eq!(native_spec(spec.name), Some(spec));
        }
        assert_eq!(NATIVE_SPECS.len(), 30);
        assert_eq!(native_spec("range").unwrap().min_arity, 1);
        assert_eq!(native_spec("range").unwrap().max_arity, 3);
        assert!(native_spec("map").unwrap().callback);
        assert!(native_spec("reduce").unwrap().callback);
        assert!(!native_spec("push").unwrap().callback);
        assert!(native_spec("notRegistered").is_none());
    }

    #[test]
    fn native_registry_enforces_registered_arity_messages() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        for spec in NATIVE_SPECS {
            assert!(spec.min_arity > 0);
            let error = vm
                .execute_native_call(spec.name, Vec::new())
                .expect_err("zero arguments should violate every native contract");
            assert_eq!(error.message, spec.arity_error, "native {}", spec.name);
        }
    }

    #[test]
    fn native_registry_records_resource_touchpoints() {
        for (name, expected) in [
            ("pop", NativeResourceProfile::None),
            ("remove", NativeResourceProfile::None),
            ("clear", NativeResourceProfile::None),
            ("floor", NativeResourceProfile::None),
            ("ceil", NativeResourceProfile::None),
            ("sqrt", NativeResourceProfile::None),
            ("str", NativeResourceProfile::None),
            ("substr", NativeResourceProfile::None),
            ("charAt", NativeResourceProfile::None),
            ("typeOf", NativeResourceProfile::None),
            ("hash", NativeResourceProfile::None),
            ("push", NativeResourceProfile::RuntimeElements),
            ("range", NativeResourceProfile::RuntimeElements),
            ("contains", NativeResourceProfile::InstructionCheckpoints),
            ("merge", NativeResourceProfile::Both),
            ("keys", NativeResourceProfile::Both),
            ("values", NativeResourceProfile::Both),
            ("slice", NativeResourceProfile::Both),
            ("copy", NativeResourceProfile::Both),
            ("concat", NativeResourceProfile::Both),
            ("map", NativeResourceProfile::Both),
            ("filter", NativeResourceProfile::Both),
            ("flatMap", NativeResourceProfile::Both),
            ("any", NativeResourceProfile::Both),
            ("all", NativeResourceProfile::Both),
            ("count", NativeResourceProfile::Both),
            ("find", NativeResourceProfile::Both),
            ("findIndex", NativeResourceProfile::Both),
            ("reduce", NativeResourceProfile::Both),
            ("print", NativeResourceProfile::None),
        ] {
            assert_eq!(
                native_spec(name).unwrap().resource,
                expected,
                "native {}",
                name
            );
        }
    }

    #[test]
    fn native_registry_records_signature_shapes() {
        let assert_signature =
            |name: &str, arguments: &[NativeArgumentShape], result: NativeReturnShape| {
                let spec = native_spec(name).unwrap();
                assert_eq!(
                    spec.signature.arguments, arguments,
                    "native {} arguments",
                    name
                );
                assert_eq!(spec.signature.result, result, "native {} result", name);
            };

        assert_signature(
            "push",
            &[NativeArgumentShape::Array, NativeArgumentShape::Any],
            NativeReturnShape::Nil,
        );
        assert_signature("pop", &[NativeArgumentShape::Array], NativeReturnShape::Any);
        assert_signature(
            "remove",
            &[NativeArgumentShape::Map, NativeArgumentShape::MapKey],
            NativeReturnShape::Any,
        );
        assert_signature("clear", &[NativeArgumentShape::Map], NativeReturnShape::Nil);
        assert_signature(
            "merge",
            &[NativeArgumentShape::Map, NativeArgumentShape::Map],
            NativeReturnShape::Map,
        );
        assert_signature(
            "keys",
            &[NativeArgumentShape::Map],
            NativeReturnShape::Array,
        );
        assert_signature(
            "values",
            &[NativeArgumentShape::Map],
            NativeReturnShape::Array,
        );
        assert_signature(
            "floor",
            &[NativeArgumentShape::Number],
            NativeReturnShape::Number,
        );
        assert_signature(
            "ceil",
            &[NativeArgumentShape::Number],
            NativeReturnShape::Number,
        );
        assert_signature(
            "sqrt",
            &[NativeArgumentShape::Number],
            NativeReturnShape::Number,
        );
        assert_signature(
            "str",
            &[NativeArgumentShape::Any],
            NativeReturnShape::String,
        );
        assert_signature(
            "substr",
            &[
                NativeArgumentShape::String,
                NativeArgumentShape::Number,
                NativeArgumentShape::Number,
            ],
            NativeReturnShape::String,
        );
        assert_signature(
            "charAt",
            &[NativeArgumentShape::String, NativeArgumentShape::Number],
            NativeReturnShape::String,
        );
        assert_signature(
            "typeOf",
            &[NativeArgumentShape::Any],
            NativeReturnShape::String,
        );
        assert_signature(
            "hash",
            &[NativeArgumentShape::Any],
            NativeReturnShape::Number,
        );
        assert_signature(
            "contains",
            &[NativeArgumentShape::Collection, NativeArgumentShape::Any],
            NativeReturnShape::Bool,
        );
        assert_signature(
            "slice",
            &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Number,
                NativeArgumentShape::Number,
            ],
            NativeReturnShape::Array,
        );
        assert_signature(
            "copy",
            &[NativeArgumentShape::Array],
            NativeReturnShape::Array,
        );
        assert_signature(
            "concat",
            &[NativeArgumentShape::Array, NativeArgumentShape::Array],
            NativeReturnShape::Array,
        );
        assert_signature(
            "map",
            &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Any,
            ],
            NativeReturnShape::Array,
        );
        assert_signature(
            "filter",
            &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Bool,
            ],
            NativeReturnShape::Array,
        );
        assert_signature(
            "flatMap",
            &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Array,
            ],
            NativeReturnShape::Array,
        );
        assert_signature(
            "any",
            &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Bool,
            ],
            NativeReturnShape::Bool,
        );
        assert_signature(
            "all",
            &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Bool,
            ],
            NativeReturnShape::Bool,
        );
        assert_signature(
            "count",
            &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Bool,
            ],
            NativeReturnShape::Number,
        );
        assert_signature(
            "find",
            &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Bool,
            ],
            NativeReturnShape::AnyOrNil,
        );
        assert_signature(
            "findIndex",
            &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Callback1Bool,
            ],
            NativeReturnShape::Number,
        );
        assert_signature(
            "reduce",
            &[
                NativeArgumentShape::Array,
                NativeArgumentShape::Any,
                NativeArgumentShape::Callback2Any,
            ],
            NativeReturnShape::Any,
        );
        assert_signature(
            "range",
            &[NativeArgumentShape::Number],
            NativeReturnShape::Range,
        );
    }

    #[test]
    fn unregistered_native_keeps_stable_error_text() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let error = vm
            .execute_native_call("notRegistered", Vec::new())
            .expect_err("unregistered native should be rejected");
        assert_eq!(
            error.message,
            "unknown native stdlib function `notRegistered`"
        );
    }

    fn empty_program() -> Program {
        Program {
            constants: Vec::new(),
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn machine_program(instructions: Vec<Instruction>, registers: usize) -> Program {
        let instruction_count = instructions.len();
        Program {
            constants: Vec::new(),
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers,
                instructions,
                locations: vec![None; instruction_count],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn machine_program_with_frame(
        machine_frame_size: u64,
        instructions: Vec<Instruction>,
        registers: usize,
    ) -> Program {
        let mut program = machine_program(instructions, registers);
        program.functions[0].machine_frame_size = machine_frame_size;
        program
    }

    fn machine_program_with_data_segments(segments: Vec<DataSegment>) -> Program {
        let mut program = empty_program();
        program.data_segments = segments;
        program
    }

    struct RecordingDebugger {
        pauses: Rc<RefCell<Vec<DebugPause>>>,
    }

    impl DebugHook for RecordingDebugger {
        fn on_instruction(&mut self, pause: DebugPause) -> DebugControl {
            self.pauses.borrow_mut().push(pause);
            DebugControl::Continue
        }
    }

    #[test]
    fn debugger_pauses_expose_machine_registers_and_frame_metadata() {
        let program = machine_program_with_frame(
            16,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::IConst {
                    dest: 0,
                    width: MachineIntWidth::W64,
                    raw: 0xfeed,
                },
                Instruction::FrameAddr { dest: 1, offset: 8 },
                Instruction::ReturnNil,
            ],
            2,
        );
        let pauses = Rc::new(RefCell::new(Vec::new()));
        let debug = VM::with_config_verified(&program, RunConfig::unlimited())
            .expect("machine debugger program should verify")
            .debug(Box::new(RecordingDebugger {
                pauses: Rc::clone(&pauses),
            }));
        assert_eq!(
            debug.result.expect("debugged machine program should run"),
            ""
        );

        let pauses = pauses.borrow();
        let machine = pauses
            .iter()
            .filter_map(|pause| pause.machine.as_ref())
            .find(|machine| {
                machine.registers.iter().any(|(index, value)| {
                    *index == 0 && value == "machine_int(65261, 0x000000000000feed)"
                })
            })
            .expect("a pause after iconst should expose machine state");
        assert_eq!(machine.frame_size, 16);
        assert!(machine.frame_base.is_some());
        assert!(machine
            .registers
            .iter()
            .any(|(index, value)| *index == 1 && value == "nil"));
    }

    #[test]
    fn unverified_loader_rejects_overlapping_machine_relocations() {
        let mut program = machine_program_with_data_segments(vec![DataSegment {
            kind: MemoryRegionKind::Data,
            alignment: 8,
            size: 16,
            initial: Some(vec![0; 16]),
        }]);
        program.symbols.push(crate::bytecode::Symbol {
            name: "data".to_string(),
            target: SymbolTarget::Data {
                segment: 0,
                offset: 0,
            },
        });
        program.relocations = vec![
            Relocation {
                kind: RelocationKind::Abs64,
                symbol: "data".to_string(),
                addend: 0,
                target: RelocationTarget::Data {
                    segment: 0,
                    offset: 0,
                },
            },
            Relocation {
                kind: RelocationKind::Abs64,
                symbol: "data".to_string(),
                addend: 0,
                target: RelocationTarget::Data {
                    segment: 0,
                    offset: 4,
                },
            },
        ];

        let error = VM::new(&program)
            .run()
            .expect_err("unverified loader must reject overlapping relocations");
        assert_eq!(error.kind, RuntimeErrorKind::InvalidInstruction);
        assert!(
            error
                .message
                .contains("machine relocation r1 overlaps relocation r0"),
            "{}",
            error.message
        );
    }

    fn machine_function(
        id: u32,
        name: &str,
        arity: usize,
        machine_frame_size: u64,
        local_count: usize,
        params: &[&str],
        registers: usize,
        instructions: Vec<Instruction>,
    ) -> Function {
        let instruction_count = instructions.len();
        Function {
            id: FuncId(id),
            name: name.to_string(),
            arity,
            machine_frame_size,
            machine_params: Vec::new(),
            machine_return: None,
            local_count,
            upvalues: Vec::new(),
            params: params.iter().map(|param| (*param).to_string()).collect(),
            registers,
            instructions,
            locations: vec![None; instruction_count],
        }
    }

    fn machine_abi_function(
        id: u32,
        name: &str,
        machine_params: &[MachineScalarType],
        machine_return: Option<MachineScalarType>,
        local_count: usize,
        params: &[&str],
        registers: usize,
        instructions: Vec<Instruction>,
    ) -> Function {
        let mut function = machine_function(
            id,
            name,
            machine_params.len(),
            0,
            local_count,
            params,
            registers,
            instructions,
        );
        function.machine_params = machine_params.to_vec();
        function.machine_return = machine_return;
        function
    }

    fn machine_program_with_functions(functions: Vec<Function>) -> Program {
        Program {
            constants: Vec::new(),
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions,
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn run_machine_body(
        vm: &mut VM<'_>,
        registers: Vec<Value>,
    ) -> Result<Vec<Value>, RuntimeError> {
        let body = vm.program.functions[0].clone();
        let machine_frame_size = body.machine_frame_size;
        let mut frame = Frame {
            body: None,
            ip: 0,
            registers,
            locals: vm.heap.new_local_slots(),
            closure: vm.heap.new_environment(),
            upvalues: Vec::new(),
            variable_plan: None,
            is_main: true,
            function: Rc::from("main"),
            function_index: None,
            return_target: None,
            machine_frame_base: None,
            machine_frame_size,
        };
        vm.machine_stack = Some(vm.new_machine_stack()?);
        vm.allocate_current_machine_frame(&mut frame)?;
        let result = vm.execute_body(&body, &mut frame);
        let cleanup = vm.release_current_machine_frame(&frame);
        vm.machine_stack.take();
        let result = match (result, cleanup) {
            (Err(error), _) => Err(error),
            (Ok(value), Ok(())) => Ok(value),
            (Ok(_), Err(error)) => Err(error),
        }?;
        assert!(matches!(result, Some(Value::Nil)));
        Ok(frame.registers)
    }

    #[test]
    fn direct_machine_abi_calls_preserve_all_scalar_domains() {
        let main = machine_function(
            0,
            "main",
            0,
            0,
            0,
            &[],
            6,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::CallDirect {
                    dest: 0,
                    function: FuncId(1),
                    arguments: vec![3],
                },
                Instruction::CallDirect {
                    dest: 1,
                    function: FuncId(2),
                    arguments: vec![4],
                },
                Instruction::CallDirect {
                    dest: 2,
                    function: FuncId(3),
                    arguments: vec![5],
                },
                Instruction::ReturnNil,
            ],
        );
        let machine_int = machine_abi_function(
            1,
            "machine_int_identity",
            &[MachineScalarType::MachineInt],
            Some(MachineScalarType::MachineInt),
            1,
            &["value"],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::LoadLocal { dest: 0, slot: 0 },
                Instruction::Return { value: 0 },
            ],
        );
        let machine_float = machine_abi_function(
            2,
            "machine_float_identity",
            &[MachineScalarType::MachineFloat],
            Some(MachineScalarType::MachineFloat),
            1,
            &["value"],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::LoadLocal { dest: 0, slot: 0 },
                Instruction::Return { value: 0 },
            ],
        );
        let address = machine_abi_function(
            3,
            "address_identity",
            &[MachineScalarType::Address],
            Some(MachineScalarType::Address),
            1,
            &["value"],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::LoadLocal { dest: 0, slot: 0 },
                Instruction::Return { value: 0 },
            ],
        );
        let program =
            machine_program_with_functions(vec![main, machine_int, machine_float, address]);
        let mut vm = VM::new(&program);
        let registers = run_machine_body(
            &mut vm,
            vec![
                Value::Nil,
                Value::Nil,
                Value::Nil,
                Value::machine_int(u64::MAX),
                Value::machine_float(-0.0),
                Value::address(0x40),
            ],
        )
        .expect("direct machine ABI calls should execute");

        assert!(matches!(registers[0], Value::MachineInt(value) if value == u64::MAX));
        assert!(
            matches!(registers[1], Value::MachineFloat(value) if value.to_bits() == (-0.0f64).to_bits())
        );
        assert!(matches!(registers[2], Value::Address(value) if value == 0x40));
    }

    #[test]
    fn machine_abi_accepts_eight_parameters_and_rejects_nine() {
        let params = ["p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7"];
        let machine_params = [MachineScalarType::MachineInt; MACHINE_ABI_MAX_PARAMS];
        let main = machine_function(
            0,
            "main",
            0,
            0,
            0,
            &[],
            MACHINE_ABI_MAX_PARAMS + 1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::CallDirect {
                    dest: MACHINE_ABI_MAX_PARAMS,
                    function: FuncId(1),
                    arguments: (0..MACHINE_ABI_MAX_PARAMS).collect(),
                },
                Instruction::ReturnNil,
            ],
        );
        let callee = machine_abi_function(
            1,
            "eight_parameters",
            &machine_params,
            Some(MachineScalarType::MachineInt),
            MACHINE_ABI_MAX_PARAMS,
            &params,
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::LoadLocal {
                    dest: 0,
                    slot: MACHINE_ABI_MAX_PARAMS - 1,
                },
                Instruction::Return { value: 0 },
            ],
        );
        let program = machine_program_with_functions(vec![main, callee]);
        let mut vm = VM::new(&program);
        let registers = run_machine_body(
            &mut vm,
            (0..MACHINE_ABI_MAX_PARAMS + 1)
                .map(|value| Value::machine_int(value as u64 + 10))
                .collect(),
        )
        .expect("the eight-parameter ABI should execute");
        assert!(matches!(
            registers[MACHINE_ABI_MAX_PARAMS],
            Value::MachineInt(value) if value == 17
        ));

        let too_many_params = (0..MACHINE_ABI_MAX_PARAMS + 1)
            .map(|_| MachineScalarType::MachineInt)
            .collect::<Vec<_>>();
        let too_many_names = ["p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8"];
        let invalid = machine_program_with_functions(vec![
            machine_function(
                0,
                "main",
                0,
                0,
                0,
                &[],
                0,
                vec![
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::ReturnNil,
                ],
            ),
            machine_abi_function(
                1,
                "nine_parameters",
                &too_many_params,
                Some(MachineScalarType::MachineInt),
                MACHINE_ABI_MAX_PARAMS + 1,
                &too_many_names,
                1,
                vec![
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::ReturnNil,
                ],
            ),
        ]);
        let error = crate::format::verify_program(&invalid)
            .expect_err("the ABI must reject more than eight parameters");
        assert!(error
            .message
            .contains("declares 9 machine ABI parameters, maximum is 8"));
    }

    #[test]
    fn machine_abi_rejects_wrong_argument_domains_at_the_call_boundary() {
        let main = machine_function(
            0,
            "main",
            0,
            0,
            0,
            &[],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::CallDirect {
                    dest: 0,
                    function: FuncId(1),
                    arguments: vec![0],
                },
                Instruction::ReturnNil,
            ],
        );
        let callee = machine_abi_function(
            1,
            "expects_machine_int",
            &[MachineScalarType::MachineInt],
            Some(MachineScalarType::MachineInt),
            1,
            &["value"],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::LoadLocal { dest: 0, slot: 0 },
                Instruction::Return { value: 0 },
            ],
        );
        let program = machine_program_with_functions(vec![main, callee]);
        let mut vm = VM::new(&program);
        let error = run_machine_body(&mut vm, vec![Value::number(1.0)])
            .expect_err("dynamic numbers must not cross a machine ABI boundary");

        assert_eq!(error.kind, RuntimeErrorKind::InvalidOperand);
        assert!(error
            .message
            .contains("machine ABI argument 0 for function `expects_machine_int` expects machine_int, got number"));
    }

    #[test]
    fn machine_abi_return_domains_and_void_returns_are_checked() {
        let returning_main = machine_function(
            0,
            "main",
            0,
            0,
            0,
            &[],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::CallDirect {
                    dest: 0,
                    function: FuncId(1),
                    arguments: Vec::new(),
                },
                Instruction::ReturnNil,
            ],
        );
        let wrong_return = machine_abi_function(
            1,
            "expects_machine_int_return",
            &[],
            Some(MachineScalarType::MachineInt),
            0,
            &[],
            0,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::ReturnNil,
            ],
        );
        let program = machine_program_with_functions(vec![returning_main, wrong_return]);
        let mut vm = VM::new(&program);
        let error = run_machine_body(&mut vm, vec![Value::Nil])
            .expect_err("a nil result must not satisfy a machine integer return");
        assert!(error.message.contains(
            "machine ABI return for function `expects_machine_int_return` expects machine_int, got nil"
        ));

        let void_main = machine_function(
            0,
            "main",
            0,
            0,
            0,
            &[],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::CallDirect {
                    dest: 0,
                    function: FuncId(1),
                    arguments: vec![0],
                },
                Instruction::ReturnNil,
            ],
        );
        let void = machine_abi_function(
            1,
            "machine_void",
            &[MachineScalarType::MachineInt],
            None,
            1,
            &["value"],
            0,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::ReturnNil,
            ],
        );
        let program = machine_program_with_functions(vec![void_main, void]);
        let mut vm = VM::new(&program);
        let registers = run_machine_body(&mut vm, vec![Value::machine_int(7)])
            .expect("a machine ABI function without a return must return nil");
        assert!(matches!(registers[0], Value::Nil));
    }

    #[test]
    fn indirect_machine_abi_calls_are_rejected_at_runtime() {
        let main = machine_function(
            0,
            "main",
            0,
            0,
            0,
            &[],
            0,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::ReturnNil,
            ],
        );
        let callee = machine_abi_function(
            1,
            "machine_target",
            &[MachineScalarType::MachineInt],
            Some(MachineScalarType::MachineInt),
            1,
            &["value"],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::LoadLocal { dest: 0, slot: 0 },
                Instruction::Return { value: 0 },
            ],
        );
        let program = machine_program_with_functions(vec![main, callee]);
        let mut vm = VM::new(&program);
        let function = FunctionValue {
            name: "machine_target".to_string(),
            function_index: 1,
            arity: 1,
            identity: 0,
            closure: vm.heap.new_environment(),
            upvalues: Vec::new(),
        };
        let error = vm
            .call_function(
                &function,
                CallArguments::One(Value::machine_int(7)),
                false,
                "main",
                None,
            )
            .expect_err("machine ABI targets must not be called indirectly");
        assert!(error
            .message
            .contains("machine ABI function `machine_target` requires a direct call"));
    }

    fn cooperative_machine_abi_call_program() -> Program {
        let main = machine_function(0, "main", 0, 0, 0, &[], 0, Vec::new());
        let worker = machine_function(
            1,
            "worker",
            1,
            0,
            1,
            &["value"],
            2,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::LoadLocal { dest: 0, slot: 0 },
                Instruction::CallDirect {
                    dest: 1,
                    function: FuncId(2),
                    arguments: vec![0],
                },
                Instruction::Return { value: 1 },
            ],
        );
        let callee = machine_abi_function(
            2,
            "scheduled_machine_identity",
            &[MachineScalarType::MachineInt],
            Some(MachineScalarType::MachineInt),
            1,
            &["value"],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::LoadLocal { dest: 0, slot: 0 },
                Instruction::Return { value: 0 },
            ],
        );
        machine_program_with_functions(vec![main, worker, callee])
    }

    #[test]
    fn cooperative_direct_calls_validate_machine_abi_arguments_and_results() {
        let program = cooperative_machine_abi_call_program();
        let mut run = VM::new(&program)
            .start_cooperative(1)
            .expect("cooperative session should start");
        let task = run
            .spawn(TaskSpec::function(1, vec![Value::machine_int(91)]))
            .expect("machine ABI worker should spawn");

        assert_eq!(
            run.run_until_waiting()
                .expect("cooperative machine ABI call should complete"),
            CooperativeStep::Complete
        );
        let outcome = run
            .task_outcome(task)
            .expect("machine ABI task outcome should be readable")
            .expect("machine ABI task should be terminal");
        assert!(matches!(
            outcome,
            TaskOutcome::Completed(Value::MachineInt(91))
        ));
    }

    #[test]
    fn frame_addr_returns_offsets_from_the_live_machine_frame() {
        let program = machine_program_with_frame(
            16,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::FrameAddr { dest: 0, offset: 0 },
                Instruction::FrameAddr { dest: 1, offset: 8 },
                Instruction::FrameAddr {
                    dest: 2,
                    offset: 16,
                },
                Instruction::ReturnNil,
            ],
            3,
        );
        let mut vm = VM::new(&program);
        let registers = run_machine_body(&mut vm, vec![Value::Nil; 3])
            .expect("frame address instructions should execute");

        let address = |register: usize| match registers.get(register) {
            Some(Value::Address(address)) => *address,
            other => panic!("expected address in r{register}, got {other:?}"),
        };
        let base = address(0);
        assert_eq!(address(1), base + 8);
        assert_eq!(address(2), base + 16);
    }

    #[test]
    fn machine_data_segments_load_in_static_order_with_permissions_and_zero_fill() {
        let program = machine_program_with_data_segments(vec![
            DataSegment {
                kind: MemoryRegionKind::Data,
                alignment: 8,
                size: 4,
                initial: Some(vec![1, 2, 3, 4]),
            },
            DataSegment {
                kind: MemoryRegionKind::Bss,
                alignment: 16,
                size: 8,
                initial: None,
            },
            DataSegment {
                kind: MemoryRegionKind::Rodata,
                alignment: 4,
                size: 6,
                initial: Some(b"hello\0".to_vec()),
            },
        ]);
        let mut vm = VM::new(&program);
        assert_eq!(
            vm.memory()
                .regions()
                .iter()
                .map(|region| region.kind)
                .collect::<Vec<_>>(),
            vec![
                MemoryRegionKind::Rodata,
                MemoryRegionKind::Data,
                MemoryRegionKind::Bss
            ]
        );
        let regions = vm.memory().regions();
        assert_eq!(regions[0].base, 0x1000);
        assert_eq!(regions[0].end, 0x1006);
        assert_eq!(regions[1].base, 0x1008);
        assert_eq!(regions[2].base, 0x1010);
        let rodata_base = regions[0].base;
        assert_eq!(vm.memory().read_bytes(rodata_base, 6).unwrap(), b"hello\0");
        assert_eq!(
            vm.memory().read_bytes(regions[1].base, 4).unwrap(),
            &[1, 2, 3, 4]
        );
        assert_eq!(vm.memory().read_bytes(regions[2].base, 8).unwrap(), &[0; 8]);
        assert_eq!(
            vm.memory_mut()
                .write_bytes(rodata_base, &[0])
                .unwrap_err()
                .kind(),
            MemoryErrorKind::WriteToReadOnlyMemory
        );
    }

    #[test]
    fn machine_abs64_relocations_patch_vm_addresses_after_segment_allocation() {
        let mut program = machine_program_with_data_segments(vec![
            DataSegment {
                kind: MemoryRegionKind::Rodata,
                alignment: 8,
                size: 16,
                initial: Some(vec![0; 16]),
            },
            DataSegment {
                kind: MemoryRegionKind::Data,
                alignment: 8,
                size: 8,
                initial: Some(vec![0; 8]),
            },
        ]);
        program.symbols = vec![Symbol {
            name: "global_bytes".to_string(),
            target: SymbolTarget::Data {
                segment: 0,
                offset: 2,
            },
        }];
        program.relocations = vec![
            Relocation {
                kind: RelocationKind::Abs64,
                symbol: "global_bytes".to_string(),
                addend: 3,
                target: RelocationTarget::Data {
                    segment: 1,
                    offset: 0,
                },
            },
            Relocation {
                kind: RelocationKind::Abs64,
                symbol: "global_bytes".to_string(),
                addend: 3,
                target: RelocationTarget::Data {
                    segment: 0,
                    offset: 8,
                },
            },
        ];

        let mut vm = VM::new(&program);
        let regions = vm.memory().regions();
        let rodata_base = regions[0].base;
        let data_base = regions[1].base;
        let rodata_address = rodata_base + 5;
        let patched = vm.memory().read_bytes(data_base, 8).unwrap();
        assert_eq!(
            u64::from_le_bytes(patched.try_into().unwrap()),
            rodata_address
        );
        let rodata_patch = vm.memory().read_bytes(rodata_base + 8, 8).unwrap();
        assert_eq!(
            u64::from_le_bytes(rodata_patch.try_into().unwrap()),
            rodata_address
        );
        assert_eq!(
            vm.memory_mut().write_bytes(rodata_base, &[1]),
            Err(MemoryError::WriteToReadOnlyMemory {
                address: rodata_base,
                size: 1,
                region: MemoryRegionKind::Rodata,
            })
        );
    }

    #[test]
    fn machine_function_relocations_patch_direct_call_targets() {
        let main = machine_function(
            0,
            "main",
            0,
            0,
            0,
            &[],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::CallDirect {
                    dest: 0,
                    function: FuncId(0),
                    arguments: Vec::new(),
                },
                Instruction::Return { value: 0 },
            ],
        );
        let callee = machine_function(
            1,
            "resolved_target",
            0,
            0,
            0,
            &[],
            0,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::ReturnNil,
            ],
        );
        let mut program = machine_program_with_functions(vec![main, callee]);
        program.symbols = vec![Symbol {
            name: "resolved_target".to_string(),
            target: SymbolTarget::Function(FuncId(1)),
        }];
        program.relocations = vec![Relocation {
            kind: RelocationKind::FuncIndex,
            symbol: "resolved_target".to_string(),
            addend: 0,
            target: RelocationTarget::CallDirect {
                function: FuncId(0),
                instruction: 1,
            },
        }];

        VM::new(&program)
            .run()
            .expect("function symbol relocation should select the resolved target");
    }

    #[test]
    fn machine_symbol_loader_rejects_undefined_and_duplicate_symbols() {
        let mut undefined = machine_program_with_data_segments(Vec::new());
        undefined.relocations = vec![Relocation {
            kind: RelocationKind::FuncIndex,
            symbol: "missing".to_string(),
            addend: 0,
            target: RelocationTarget::CallDirect {
                function: FuncId(0),
                instruction: 0,
            },
        }];
        let error = VM::new(&undefined)
            .run()
            .expect_err("undefined machine symbols must fail before execution");
        assert!(error.message.contains("undefined symbol `missing`"));

        let mut duplicate = machine_program_with_data_segments(Vec::new());
        duplicate.symbols = vec![
            Symbol {
                name: "same".to_string(),
                target: SymbolTarget::Function(FuncId(0)),
            },
            Symbol {
                name: "same".to_string(),
                target: SymbolTarget::Function(FuncId(0)),
            },
        ];
        let error = VM::new(&duplicate)
            .run()
            .expect_err("duplicate machine symbols must fail before execution");
        assert!(error.message.contains("duplicate machine symbol `same`"));
    }

    #[test]
    fn machine_bulk_instructions_copy_move_and_fill_bytes() {
        let program = machine_program(
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::Memset {
                    destination: 0,
                    value: 3,
                    size: 2,
                },
                Instruction::IConst {
                    dest: 3,
                    width: MachineIntWidth::W64,
                    raw: 1,
                },
                Instruction::Store {
                    address: 4,
                    source: 3,
                    memory_type: MachineMemoryType::I8,
                },
                Instruction::Memcpy {
                    destination: 1,
                    source: 0,
                    size: 2,
                },
                Instruction::Memmove {
                    destination: 4,
                    source: 0,
                    size: 2,
                },
                Instruction::Load {
                    dest: 3,
                    address: 0,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::ReturnNil,
            ],
            5,
        );
        let mut vm = VM::new(&program);
        let region = vm
            .memory_mut()
            .allocate_region_with_bytes(MemoryRegionKind::Data, 16, &[0; 16])
            .expect("bulk operation region should allocate");
        let registers = run_machine_body(
            &mut vm,
            vec![
                Value::address(region.base),
                Value::address(region.base + 8),
                Value::machine_int(4),
                Value::machine_int(0x1ab),
                Value::address(region.base + 1),
            ],
        )
        .expect("bulk instructions should execute");

        assert!(matches!(
            registers[3],
            Value::MachineInt(value) if value == 0xab01_abab
        ));
        assert_eq!(
            vm.memory().read_bytes(region.base, 4).unwrap(),
            &[0xab, 0xab, 1, 0xab]
        );
        assert_eq!(
            vm.memory().read_bytes(region.base + 8, 4).unwrap(),
            &[0xab, 1, 0xab, 0xab]
        );
    }

    #[test]
    fn machine_memcpy_overlap_is_a_typed_atomic_trap() {
        let program = machine_program(
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::Memcpy {
                    destination: 0,
                    source: 1,
                    size: 2,
                },
                Instruction::ReturnNil,
            ],
            3,
        );
        let mut vm = VM::new(&program);
        let region = vm
            .memory_mut()
            .allocate_region_with_bytes(MemoryRegionKind::Data, 8, &[1, 2, 3, 4, 5, 6, 7, 8])
            .expect("overlap test region should allocate");
        let error = run_machine_body(
            &mut vm,
            vec![
                Value::address(region.base + 1),
                Value::address(region.base),
                Value::machine_int(4),
            ],
        )
        .expect_err("overlapping memcpy should trap");
        assert_eq!(error.kind, RuntimeErrorKind::InvalidMemoryOperation);
        assert_eq!(
            vm.memory().read_bytes(region.base, 8).unwrap(),
            &[1, 2, 3, 4, 5, 6, 7, 8]
        );
    }

    #[test]
    fn machine_bulk_instructions_accept_null_addresses_for_zero_size() {
        let program = machine_program(
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::Memcpy {
                    destination: 0,
                    source: 1,
                    size: 2,
                },
                Instruction::Memmove {
                    destination: 0,
                    source: 1,
                    size: 2,
                },
                Instruction::Memset {
                    destination: 0,
                    value: 3,
                    size: 2,
                },
                Instruction::ReturnNil,
            ],
            4,
        );
        let mut vm = VM::new(&program);
        run_machine_body(
            &mut vm,
            vec![
                Value::address(0),
                Value::address(0),
                Value::machine_int(0),
                Value::machine_int(0),
            ],
        )
        .expect("zero-size bulk operations should not dereference addresses");
    }

    #[test]
    fn invalid_machine_data_segments_fail_before_execution() {
        let program = machine_program_with_data_segments(vec![DataSegment {
            kind: MemoryRegionKind::Data,
            alignment: 8,
            size: 4,
            initial: Some(vec![1, 2, 3]),
        }]);
        let mut vm = VM::new(&program);
        let error = vm
            .run_inner()
            .expect_err("invalid data payload should not reach execution");
        assert_eq!(error.kind, RuntimeErrorKind::InvalidInstruction);
        assert!(error.message.contains("initialization has 3 bytes"));
        assert!(vm
            .memory()
            .regions()
            .iter()
            .all(|region| region.kind != MemoryRegionKind::Stack));
    }

    #[test]
    fn malformed_entry_points_return_checked_instruction_errors() {
        let mut missing_entry = empty_program();
        missing_entry.functions.clear();
        let error = VM::new(&missing_entry)
            .run()
            .expect_err("missing entry functions must not panic");
        assert_eq!(error.kind, RuntimeErrorKind::InvalidInstruction);
        assert!(error.message.contains("entry function f0 is out of range"));

        let mut wrong_entry = empty_program();
        wrong_entry.entry = FuncId(1);
        let error = VM::new(&wrong_entry)
            .run()
            .expect_err("non-zero entry functions must not panic");
        assert_eq!(error.kind, RuntimeErrorKind::InvalidInstruction);
        assert!(error.message.contains("entry must be f0, found f1"));

        let mut session = VM::new(&missing_entry)
            .start_cooperative(1)
            .expect("scheduler construction should not inspect the entry body");
        let error = session
            .spawn(TaskSpec::Main)
            .expect_err("cooperative entry creation must be checked");
        assert_eq!(error.kind, RuntimeErrorKind::InvalidInstruction);
        assert!(error.message.contains("entry function f0 is out of range"));
    }

    #[test]
    fn verifier_rejects_frame_addr_past_function_frame() {
        let program = machine_program_with_frame(
            8,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::FrameAddr { dest: 0, offset: 9 },
                Instruction::ReturnNil,
            ],
            1,
        );
        let error = crate::format::verify_program(&program)
            .expect_err("frame address past the declared frame must be rejected");
        assert!(error
            .message
            .contains("frame_addr offset 9 exceeds machine frame size 8"));
    }

    #[test]
    fn released_machine_frame_addresses_are_no_longer_dereferenceable() {
        let program = machine_program_with_frame(
            8,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::FrameAddr { dest: 0, offset: 0 },
                Instruction::ReturnNil,
            ],
            1,
        );
        let mut vm = VM::new(&program);
        let registers = run_machine_body(&mut vm, vec![Value::Nil])
            .expect("frame address instruction should execute");
        let base = match registers[0] {
            Value::Address(address) => address,
            ref other => panic!("expected frame address, got {other:?}"),
        };

        assert!(vm.memory().region_for_address(base).is_some());
        assert!(!vm.memory().is_mapped(base));
        assert_eq!(
            vm.memory().read_bytes(base, 1).unwrap_err().kind(),
            MemoryErrorKind::InvalidAddress
        );
    }

    fn recursive_frame_address_program() -> Program {
        let main = machine_function(
            0,
            "main",
            0,
            8,
            0,
            &[],
            4,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::FrameAddr { dest: 0, offset: 0 },
                Instruction::IConst {
                    dest: 1,
                    width: MachineIntWidth::W64,
                    raw: 2,
                },
                Instruction::CallDirect {
                    dest: 2,
                    function: FuncId(1),
                    arguments: vec![0, 1],
                },
                Instruction::Load {
                    dest: 3,
                    address: 0,
                    memory_type: MachineMemoryType::Addr,
                },
                Instruction::ReturnNil,
            ],
        );
        let recurse = machine_function(
            1,
            "recurse",
            2,
            8,
            2,
            &["parent", "depth"],
            8,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::LoadLocal { dest: 1, slot: 0 },
                Instruction::LoadLocal { dest: 2, slot: 1 },
                Instruction::FrameAddr { dest: 0, offset: 0 },
                Instruction::Store {
                    address: 1,
                    source: 0,
                    memory_type: MachineMemoryType::Addr,
                },
                Instruction::IConst {
                    dest: 3,
                    width: MachineIntWidth::W64,
                    raw: 0,
                },
                Instruction::ICmp {
                    dest: 4,
                    left: 2,
                    right: 3,
                    width: MachineIntWidth::W64,
                    predicate: MachineIntPredicate::Eq,
                },
                Instruction::BrIf {
                    condition: 4,
                    if_true: BlockId(1),
                    if_false: BlockId(2),
                },
                Instruction::BlockStart { id: BlockId(1) },
                Instruction::ReturnNil,
                Instruction::BlockStart { id: BlockId(2) },
                Instruction::IConst {
                    dest: 5,
                    width: MachineIntWidth::W64,
                    raw: 1,
                },
                Instruction::ISub {
                    dest: 6,
                    left: 2,
                    right: 5,
                    width: MachineIntWidth::W64,
                },
                Instruction::CallDirect {
                    dest: 7,
                    function: FuncId(1),
                    arguments: vec![0, 6],
                },
                Instruction::ReturnNil,
            ],
        );
        machine_program_with_functions(vec![main, recurse])
    }

    #[test]
    fn recursive_calls_receive_distinct_machine_frame_addresses() {
        let program = recursive_frame_address_program();
        let mut vm = VM::new(&program);
        let registers = run_machine_body(&mut vm, vec![Value::Nil; 4])
            .expect("recursive machine frames should execute");
        let base = match registers[0] {
            Value::Address(address) => address,
            ref other => panic!("expected root frame address, got {other:?}"),
        };
        let child = match registers[3] {
            Value::Address(address) => address,
            ref other => panic!("expected child frame address, got {other:?}"),
        };

        assert_eq!(child, base + 8);
        assert_ne!(child, base);
        assert!(!vm.memory().is_mapped(base));
        assert!(!vm.memory().is_mapped(child));
    }

    fn machine_call_program(main_frame_size: u64, callee_frame_size: u64) -> Program {
        let main = machine_function(
            0,
            "main",
            0,
            main_frame_size,
            0,
            &[],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::CallDirect {
                    dest: 0,
                    function: FuncId(1),
                    arguments: Vec::new(),
                },
                Instruction::ReturnNil,
            ],
        );
        let callee = machine_function(
            1,
            "callee",
            0,
            callee_frame_size,
            0,
            &[],
            2,
            vec![Instruction::ReturnNil],
        );
        machine_program_with_functions(vec![main, callee])
    }

    #[test]
    fn machine_stack_overflow_is_a_typed_trap_and_releases_the_root_frame() {
        let program = machine_call_program(8, 8);
        let config = RunConfig::unlimited().with_machine_stack_bytes(8);
        let mut vm = VM::with_config(&program, config);
        let error = vm
            .run_inner()
            .expect_err("callee frame should exceed the configured machine stack");

        assert_eq!(error.kind, RuntimeErrorKind::StackOverflow);
        assert!(error.message.contains("machine stack overflow"));
        assert!(vm.machine_stack.is_none());
        let stack = vm
            .memory()
            .regions()
            .iter()
            .find(|region| region.kind == crate::memory::MemoryRegionKind::Stack)
            .expect("machine stack reservation should be recorded");
        assert!(!vm.memory().is_mapped(stack.base));
    }

    #[test]
    fn machine_traps_unwind_callee_and_root_frames() {
        let main = machine_function(
            0,
            "main",
            0,
            8,
            0,
            &[],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::CallDirect {
                    dest: 0,
                    function: FuncId(1),
                    arguments: Vec::new(),
                },
                Instruction::ReturnNil,
            ],
        );
        let callee = machine_function(
            1,
            "trap",
            0,
            8,
            0,
            &[],
            2,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::FrameAddr { dest: 0, offset: 8 },
                Instruction::Load {
                    dest: 1,
                    address: 0,
                    memory_type: MachineMemoryType::I8,
                },
                Instruction::ReturnNil,
            ],
        );
        let program = machine_program_with_functions(vec![main, callee]);
        let mut vm = VM::new(&program);
        let error = vm
            .run_inner()
            .expect_err("dereferencing a frame-end address should trap");

        assert_eq!(error.kind, RuntimeErrorKind::InvalidAddress);
        assert!(vm.machine_stack.is_none());
        assert!(vm
            .memory()
            .regions()
            .iter()
            .filter(|region| region.kind == crate::memory::MemoryRegionKind::Stack)
            .all(|region| !vm.memory().is_mapped(region.base)));
    }

    fn cooperative_machine_frame_program() -> Program {
        let main = machine_function(0, "main", 0, 0, 0, &[], 0, Vec::new());
        let worker = machine_function(
            1,
            "worker",
            0,
            8,
            0,
            &[],
            1,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::FrameAddr { dest: 0, offset: 0 },
                Instruction::Return { value: 0 },
            ],
        );
        machine_program_with_functions(vec![main, worker])
    }

    #[test]
    fn cooperative_tasks_use_independent_machine_stacks() {
        let program = cooperative_machine_frame_program();
        let mut run = VM::new(&program)
            .start_cooperative(1)
            .expect("cooperative session should start");
        let first = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect("first machine task should spawn");
        let second = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect("second machine task should spawn");

        assert_eq!(
            run.run_until_waiting()
                .expect("machine tasks should complete"),
            CooperativeStep::Complete
        );
        let first_outcome = run
            .task_outcome(first)
            .expect("first task outcome should be readable")
            .expect("first task should be terminal");
        let second_outcome = run
            .task_outcome(second)
            .expect("second task outcome should be readable")
            .expect("second task should be terminal");
        let frame_address = |outcome: &TaskOutcome| match outcome {
            TaskOutcome::Completed(Value::Address(address)) => *address,
            other => panic!("expected completed address outcome, got {other:?}"),
        };

        let first_address = frame_address(&first_outcome);
        let second_address = frame_address(&second_outcome);
        assert_ne!(first_address, second_address);
        assert_eq!(
            run.memory()
                .regions()
                .iter()
                .filter(|region| region.kind == crate::memory::MemoryRegionKind::Stack)
                .count(),
            2
        );
        assert!(!run.memory().is_mapped(first_address));
        assert!(!run.memory().is_mapped(second_address));
    }

    #[test]
    fn failed_cooperative_frame_creation_does_not_reserve_a_stack() {
        let program = cooperative_machine_frame_program();
        let mut run = VM::with_config(&program, RunConfig::unlimited().with_machine_stack_bytes(4))
            .start_cooperative(1)
            .expect("cooperative session should start");
        let error = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect_err("an oversized initial frame should be rejected");

        assert_eq!(error.kind, RuntimeErrorKind::StackOverflow);
        assert!(run
            .memory()
            .regions()
            .iter()
            .all(|region| region.kind != crate::memory::MemoryRegionKind::Stack));
    }

    fn machine_integration_program() -> Program {
        let main = machine_function(
            0,
            "main",
            0,
            24,
            0,
            &[],
            24,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::FrameAddr { dest: 0, offset: 0 },
                Instruction::FrameAddr { dest: 1, offset: 4 },
                Instruction::FrameAddr { dest: 2, offset: 8 },
                Instruction::FrameAddr {
                    dest: 3,
                    offset: 12,
                },
                Instruction::IConst {
                    dest: 4,
                    width: MachineIntWidth::W32,
                    raw: 1,
                },
                Instruction::Store {
                    address: 0,
                    source: 4,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::IConst {
                    dest: 5,
                    width: MachineIntWidth::W32,
                    raw: 2,
                },
                Instruction::Store {
                    address: 1,
                    source: 5,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::IConst {
                    dest: 6,
                    width: MachineIntWidth::W32,
                    raw: 3,
                },
                Instruction::Store {
                    address: 2,
                    source: 6,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::IConst {
                    dest: 7,
                    width: MachineIntWidth::W32,
                    raw: 4,
                },
                Instruction::Store {
                    address: 3,
                    source: 7,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::CallDirect {
                    dest: 8,
                    function: FuncId(1),
                    arguments: vec![0, 1, 2, 3],
                },
                Instruction::FrameAddr {
                    dest: 9,
                    offset: 16,
                },
                Instruction::Store {
                    address: 9,
                    source: 8,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::IConst {
                    dest: 10,
                    width: MachineIntWidth::W32,
                    raw: 2,
                },
                Instruction::FrameAddr {
                    dest: 11,
                    offset: 20,
                },
                Instruction::Store {
                    address: 11,
                    source: 10,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::FrameAddr {
                    dest: 12,
                    offset: 16,
                },
                Instruction::Load {
                    dest: 13,
                    address: 12,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::FrameAddr {
                    dest: 14,
                    offset: 20,
                },
                Instruction::Load {
                    dest: 15,
                    address: 14,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::IAdd {
                    dest: 16,
                    left: 13,
                    right: 15,
                    width: MachineIntWidth::W32,
                },
                Instruction::Return { value: 16 },
            ],
        );
        let sum = machine_function(
            1,
            "sum",
            4,
            8,
            4,
            &["a0", "a1", "a2", "a3"],
            11,
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::LoadLocal { dest: 0, slot: 0 },
                Instruction::LoadLocal { dest: 1, slot: 1 },
                Instruction::LoadLocal { dest: 2, slot: 2 },
                Instruction::LoadLocal { dest: 3, slot: 3 },
                Instruction::Load {
                    dest: 4,
                    address: 0,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::Load {
                    dest: 5,
                    address: 1,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::Load {
                    dest: 6,
                    address: 2,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::Load {
                    dest: 7,
                    address: 3,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::IAdd {
                    dest: 8,
                    left: 4,
                    right: 5,
                    width: MachineIntWidth::W32,
                },
                Instruction::IAdd {
                    dest: 9,
                    left: 8,
                    right: 6,
                    width: MachineIntWidth::W32,
                },
                Instruction::IAdd {
                    dest: 10,
                    left: 9,
                    right: 7,
                    width: MachineIntWidth::W32,
                },
                Instruction::Return { value: 10 },
            ],
        );
        machine_program_with_functions(vec![main, sum])
    }

    #[test]
    fn hand_built_machine_program_returns_twelve() {
        let program = machine_integration_program();
        let source = crate::format::format_program_v03(&program);
        assert!(source.starts_with("cdbc 0.3\n"));
        let parsed = crate::format::parse_program(&source)
            .expect("hand-built machine program should round-trip through cdbc 0.3");
        let vm = VM::with_config_verified(&parsed, RunConfig::unlimited())
            .expect("hand-built machine program should pass verification");
        let mut run = vm
            .start_cooperative(1)
            .expect("cooperative session should start");
        let task = run
            .spawn(TaskSpec::main())
            .expect("machine integration task should spawn");

        assert_eq!(
            run.run_until_waiting()
                .expect("hand-built machine program should complete"),
            CooperativeStep::Complete
        );
        let outcome = run
            .task_outcome(task)
            .expect("integration outcome should be readable")
            .expect("integration task should be terminal");
        assert!(matches!(
            outcome,
            TaskOutcome::Completed(Value::MachineInt(12))
        ));
    }

    fn machine_memory_error(
        region_kind: crate::memory::MemoryRegionKind,
        region_size: u64,
        instruction: Instruction,
        registers: Vec<Value>,
    ) -> RuntimeError {
        let program = machine_program(
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                instruction,
                Instruction::ReturnNil,
            ],
            registers.len(),
        );
        let mut vm = VM::new(&program);
        vm.memory_mut()
            .allocate_region(region_kind, region_size, 1)
            .expect("machine memory test region should allocate");
        run_machine_body(&mut vm, registers).expect_err("machine memory operation should fail")
    }

    #[test]
    fn vm_owns_memory_shared_by_cooperative_sessions() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let region = vm
            .memory_mut()
            .allocate_region(crate::memory::MemoryRegionKind::Data, 4, 4)
            .expect("VM memory allocation should succeed");
        vm.memory_mut()
            .write_bytes(region.base, &[1, 2, 3, 4])
            .expect("VM memory write should succeed");

        let mut cooperative = vm
            .start_cooperative(1)
            .expect("cooperative session should start");
        assert_eq!(cooperative.memory().regions(), &[region]);
        assert_eq!(
            cooperative.memory().read_bytes(region.base, 4).unwrap(),
            &[1, 2, 3, 4]
        );
        cooperative
            .memory_mut()
            .write_bytes(region.base + 1, &[9])
            .expect("cooperative memory write should succeed");
        assert_eq!(
            cooperative.memory().read_bytes(region.base, 4).unwrap(),
            &[1, 9, 3, 4]
        );
    }

    #[test]
    fn memory_errors_map_to_typed_runtime_kinds() {
        let mut memory = LinearMemory::new();
        let data = memory
            .allocate_region(crate::memory::MemoryRegionKind::Data, 1, 1)
            .expect("data allocation");
        let rodata = memory
            .allocate_region_with_bytes(crate::memory::MemoryRegionKind::Rodata, 1, &[7])
            .expect("rodata allocation");
        let errors = [
            (
                memory.read_bytes(0, 1).unwrap_err(),
                RuntimeErrorKind::NullPointerAccess,
            ),
            (
                memory.read_bytes(data.base + 2, 1).unwrap_err(),
                RuntimeErrorKind::InvalidAddress,
            ),
            (
                memory.read_bytes(data.base, 2).unwrap_err(),
                RuntimeErrorKind::MemoryOutOfBounds,
            ),
            (
                memory.write_bytes(rodata.base, &[0]).unwrap_err(),
                RuntimeErrorKind::WriteToReadOnlyMemory,
            ),
        ];
        for (error, expected_kind) in errors {
            let runtime_error: RuntimeError = error.into();
            assert_eq!(runtime_error.kind, expected_kind);
        }
    }

    #[test]
    fn typed_machine_memory_operations_preserve_bits_and_allow_unaligned_access() {
        let program = machine_program(
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::Load {
                    dest: 13,
                    address: 0,
                    memory_type: MachineMemoryType::I8,
                },
                Instruction::Load {
                    dest: 14,
                    address: 0,
                    memory_type: MachineMemoryType::I16,
                },
                Instruction::Load {
                    dest: 15,
                    address: 0,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::Load {
                    dest: 16,
                    address: 0,
                    memory_type: MachineMemoryType::I64,
                },
                Instruction::Load {
                    dest: 17,
                    address: 1,
                    memory_type: MachineMemoryType::F32,
                },
                Instruction::Load {
                    dest: 18,
                    address: 2,
                    memory_type: MachineMemoryType::F64,
                },
                Instruction::Load {
                    dest: 19,
                    address: 3,
                    memory_type: MachineMemoryType::Addr,
                },
                Instruction::Store {
                    address: 4,
                    source: 10,
                    memory_type: MachineMemoryType::I8,
                },
                Instruction::Store {
                    address: 5,
                    source: 10,
                    memory_type: MachineMemoryType::I16,
                },
                Instruction::Store {
                    address: 6,
                    source: 10,
                    memory_type: MachineMemoryType::I32,
                },
                Instruction::Store {
                    address: 7,
                    source: 10,
                    memory_type: MachineMemoryType::I64,
                },
                Instruction::Store {
                    address: 8,
                    source: 11,
                    memory_type: MachineMemoryType::F32,
                },
                Instruction::Store {
                    address: 9,
                    source: 11,
                    memory_type: MachineMemoryType::F64,
                },
                Instruction::Store {
                    address: 20,
                    source: 12,
                    memory_type: MachineMemoryType::Addr,
                },
                Instruction::ReturnNil,
            ],
            21,
        );
        let mut vm = VM::new(&program);
        let region = vm
            .memory_mut()
            .allocate_region(crate::memory::MemoryRegionKind::Data, 96, 1)
            .expect("typed memory test region should allocate");
        let integer_bits = 0x1122_3344_5566_7788u64;
        let loaded_address = 0u64;
        vm.memory_mut()
            .write_bytes(region.base + 1, &integer_bits.to_le_bytes())
            .expect("integer bytes should initialize");
        vm.memory_mut()
            .write_bytes(region.base + 16, &1.5f32.to_bits().to_le_bytes())
            .expect("f32 bytes should initialize");
        vm.memory_mut()
            .write_bytes(region.base + 24, &(-2.25f64).to_bits().to_le_bytes())
            .expect("f64 bytes should initialize");
        vm.memory_mut()
            .write_bytes(region.base + 40, &loaded_address.to_le_bytes())
            .expect("address bytes should initialize");

        let mut initial = vec![Value::Nil; 21];
        for (register, offset) in [
            (0, 1),
            (1, 16),
            (2, 24),
            (3, 40),
            (4, 48),
            (5, 49),
            (6, 51),
            (7, 55),
            (8, 63),
            (9, 67),
            (20, 75),
        ] {
            initial[register] = Value::address(region.base + offset);
        }
        initial[10] = Value::machine_int(integer_bits);
        initial[11] = Value::machine_float(1.5);
        initial[12] = Value::address(0x0123_4567_89ab_cdef);

        let registers = run_machine_body(&mut vm, initial).expect("typed memory should execute");
        assert!(matches!(&registers[13], Value::MachineInt(value) if *value == 0x88));
        assert!(matches!(&registers[14], Value::MachineInt(value) if *value == 0x7788));
        assert!(matches!(&registers[15], Value::MachineInt(value) if *value == 0x5566_7788));
        assert!(matches!(&registers[16], Value::MachineInt(value) if *value == integer_bits));
        assert!(matches!(&registers[17], Value::MachineFloat(value) if *value == 1.5));
        assert!(matches!(&registers[18], Value::MachineFloat(value) if *value == -2.25));
        assert!(matches!(&registers[19], Value::Address(value) if *value == loaded_address));

        assert_eq!(
            vm.memory().read_bytes(region.base + 48, 1).unwrap(),
            &[0x88]
        );
        assert_eq!(
            vm.memory().read_bytes(region.base + 49, 2).unwrap(),
            &[0x88, 0x77]
        );
        assert_eq!(
            vm.memory().read_bytes(region.base + 51, 4).unwrap(),
            &[0x88, 0x77, 0x66, 0x55]
        );
        assert_eq!(
            vm.memory().read_bytes(region.base + 55, 8).unwrap(),
            &integer_bits.to_le_bytes()
        );
        assert_eq!(
            vm.memory().read_bytes(region.base + 63, 4).unwrap(),
            &1.5f32.to_bits().to_le_bytes()
        );
        assert_eq!(
            vm.memory().read_bytes(region.base + 67, 8).unwrap(),
            &1.5f64.to_le_bytes()
        );
        assert_eq!(
            vm.memory().read_bytes(region.base + 75, 8).unwrap(),
            &0x0123_4567_89ab_cdefu64.to_le_bytes()
        );
    }

    #[test]
    fn typed_machine_float_memory_canonicalizes_nan_bits() {
        let program = machine_program(
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::Load {
                    dest: 1,
                    address: 0,
                    memory_type: MachineMemoryType::F32,
                },
                Instruction::Load {
                    dest: 2,
                    address: 2,
                    memory_type: MachineMemoryType::F64,
                },
                Instruction::Store {
                    address: 3,
                    source: 1,
                    memory_type: MachineMemoryType::F32,
                },
                Instruction::Store {
                    address: 4,
                    source: 2,
                    memory_type: MachineMemoryType::F64,
                },
                Instruction::ReturnNil,
            ],
            5,
        );
        let mut vm = VM::new(&program);
        let region = vm
            .memory_mut()
            .allocate_region(crate::memory::MemoryRegionKind::Data, 32, 1)
            .expect("NaN memory test region should allocate");
        vm.memory_mut()
            .write_bytes(region.base, &0x7fc0_0001u32.to_le_bytes())
            .expect("f32 NaN bits should initialize");
        vm.memory_mut()
            .write_bytes(region.base + 8, &0x7ff0_0000_0000_0001u64.to_le_bytes())
            .expect("f64 NaN bits should initialize");

        let registers = run_machine_body(
            &mut vm,
            vec![
                Value::address(region.base),
                Value::Nil,
                Value::address(region.base + 8),
                Value::address(region.base + 16),
                Value::address(region.base + 24),
            ],
        )
        .expect("NaN memory operations should execute");
        assert!(matches!(registers[1], Value::MachineFloat(value) if value.is_nan()));
        assert!(
            matches!(registers[2], Value::MachineFloat(value) if value.to_bits() == 0x7ff8_0000_0000_0000)
        );
        assert_eq!(
            vm.memory().read_bytes(region.base + 16, 4).unwrap(),
            &0x7fc0_0000u32.to_le_bytes()
        );
        assert_eq!(
            vm.memory().read_bytes(region.base + 24, 8).unwrap(),
            &0x7ff8_0000_0000_0000u64.to_le_bytes()
        );
    }

    #[test]
    fn typed_machine_memory_operations_map_address_and_domain_errors() {
        let error = machine_memory_error(
            crate::memory::MemoryRegionKind::Data,
            4,
            Instruction::Load {
                dest: 1,
                address: 0,
                memory_type: MachineMemoryType::I16,
            },
            vec![Value::address(0x1003), Value::Nil],
        );
        assert_eq!(error.kind, RuntimeErrorKind::MemoryOutOfBounds);

        let error = machine_memory_error(
            crate::memory::MemoryRegionKind::Data,
            4,
            Instruction::Store {
                address: 0,
                source: 1,
                memory_type: MachineMemoryType::I16,
            },
            vec![Value::address(0x1003), Value::machine_int(1)],
        );
        assert_eq!(error.kind, RuntimeErrorKind::MemoryOutOfBounds);

        let error = machine_memory_error(
            crate::memory::MemoryRegionKind::Data,
            4,
            Instruction::Load {
                dest: 1,
                address: 0,
                memory_type: MachineMemoryType::I8,
            },
            vec![Value::address(0), Value::Nil],
        );
        assert_eq!(error.kind, RuntimeErrorKind::NullPointerAccess);

        let error = machine_memory_error(
            crate::memory::MemoryRegionKind::Rodata,
            4,
            Instruction::Store {
                address: 0,
                source: 1,
                memory_type: MachineMemoryType::I8,
            },
            vec![Value::address(0x1000), Value::machine_int(1)],
        );
        assert_eq!(error.kind, RuntimeErrorKind::WriteToReadOnlyMemory);

        let error = machine_memory_error(
            crate::memory::MemoryRegionKind::Data,
            4,
            Instruction::Store {
                address: 0,
                source: 1,
                memory_type: MachineMemoryType::F64,
            },
            vec![Value::address(0x1000), Value::number(1.0)],
        );
        assert_eq!(error.kind, RuntimeErrorKind::InvalidOperand);
        assert_eq!(error.message, "store f64 expects machine_float, got number");
    }

    fn print_register(register: usize, destination: usize) -> Instruction {
        Instruction::CallNative {
            dest: destination,
            native: NativeId(0),
            arguments: vec![register],
        }
    }

    #[test]
    fn ordered_comparisons_preserve_primitive_results() {
        let program = Program {
            constants: vec![
                Constant::Number("1".to_string()),
                Constant::Number("2".to_string()),
                Constant::String("a".to_string()),
                Constant::String("b".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 11,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Constant {
                        dest: 1,
                        constant: 1,
                    },
                    Instruction::Constant {
                        dest: 2,
                        constant: 2,
                    },
                    Instruction::Constant {
                        dest: 3,
                        constant: 3,
                    },
                    Instruction::Less {
                        dest: 4,
                        left: 0,
                        right: 1,
                    },
                    Instruction::Greater {
                        dest: 5,
                        left: 1,
                        right: 0,
                    },
                    Instruction::LessEqual {
                        dest: 6,
                        left: 0,
                        right: 0,
                    },
                    Instruction::GreaterEqual {
                        dest: 7,
                        left: 1,
                        right: 1,
                    },
                    Instruction::Less {
                        dest: 8,
                        left: 2,
                        right: 3,
                    },
                    Instruction::Greater {
                        dest: 9,
                        left: 3,
                        right: 2,
                    },
                    Instruction::CallNative {
                        dest: 10,
                        native: NativeId(0),
                        arguments: vec![4],
                    },
                    Instruction::CallNative {
                        dest: 10,
                        native: NativeId(0),
                        arguments: vec![5],
                    },
                    Instruction::CallNative {
                        dest: 10,
                        native: NativeId(0),
                        arguments: vec![6],
                    },
                    Instruction::CallNative {
                        dest: 10,
                        native: NativeId(0),
                        arguments: vec![7],
                    },
                    Instruction::CallNative {
                        dest: 10,
                        native: NativeId(0),
                        arguments: vec![8],
                    },
                    Instruction::CallNative {
                        dest: 10,
                        native: NativeId(0),
                        arguments: vec![9],
                    },
                ],
                locations: vec![None; 16],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };

        assert_eq!(
            VM::new(&program)
                .run()
                .expect("primitive comparisons should run"),
            "true\ntrue\ntrue\ntrue\ntrue\ntrue\n"
        );
    }

    #[test]
    fn typed_arithmetic_and_comparison_opcodes_dispatch_without_type_branching() {
        let program = Program {
            constants: vec![
                Constant::Number("6".to_string()),
                Constant::Number("2".to_string()),
                Constant::String("ab".to_string()),
                Constant::String("cd".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 16,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Constant {
                        dest: 1,
                        constant: 1,
                    },
                    Instruction::AddNum {
                        dest: 2,
                        left: 0,
                        right: 1,
                    },
                    Instruction::SubNum {
                        dest: 3,
                        left: 0,
                        right: 1,
                    },
                    Instruction::MulNum {
                        dest: 4,
                        left: 0,
                        right: 1,
                    },
                    Instruction::DivNum {
                        dest: 5,
                        left: 0,
                        right: 1,
                    },
                    Instruction::NegNum { dest: 6, value: 0 },
                    Instruction::Constant {
                        dest: 7,
                        constant: 2,
                    },
                    Instruction::Constant {
                        dest: 8,
                        constant: 3,
                    },
                    Instruction::ConcatStr {
                        dest: 9,
                        left: 7,
                        right: 8,
                    },
                    Instruction::LessNum {
                        dest: 10,
                        left: 1,
                        right: 0,
                    },
                    Instruction::GreaterEqualNum {
                        dest: 11,
                        left: 0,
                        right: 1,
                    },
                    Instruction::LessStr {
                        dest: 12,
                        left: 7,
                        right: 8,
                    },
                    Instruction::GreaterEqualStr {
                        dest: 13,
                        left: 8,
                        right: 7,
                    },
                    Instruction::CallNative {
                        dest: 14,
                        native: NativeId(0),
                        arguments: vec![2],
                    },
                    Instruction::CallNative {
                        dest: 14,
                        native: NativeId(0),
                        arguments: vec![3],
                    },
                    Instruction::CallNative {
                        dest: 14,
                        native: NativeId(0),
                        arguments: vec![4],
                    },
                    Instruction::CallNative {
                        dest: 14,
                        native: NativeId(0),
                        arguments: vec![5],
                    },
                    Instruction::CallNative {
                        dest: 14,
                        native: NativeId(0),
                        arguments: vec![6],
                    },
                    Instruction::CallNative {
                        dest: 14,
                        native: NativeId(0),
                        arguments: vec![9],
                    },
                    Instruction::CallNative {
                        dest: 14,
                        native: NativeId(0),
                        arguments: vec![10],
                    },
                    Instruction::CallNative {
                        dest: 14,
                        native: NativeId(0),
                        arguments: vec![11],
                    },
                    Instruction::CallNative {
                        dest: 14,
                        native: NativeId(0),
                        arguments: vec![12],
                    },
                    Instruction::CallNative {
                        dest: 14,
                        native: NativeId(0),
                        arguments: vec![13],
                    },
                    Instruction::ReturnNil,
                ],
                locations: vec![None; 25],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };

        assert_eq!(
            VM::new(&program).run().expect("typed opcodes should run"),
            "8\n4\n12\n3\n-6\nabcd\ntrue\ntrue\ntrue\ntrue\n"
        );
    }

    #[test]
    fn machine_integer_ops_wrap_mask_shift_and_compare() {
        let mut instructions = vec![
            Instruction::BlockStart { id: BlockId(0) },
            Instruction::IConst {
                dest: 0,
                width: MachineIntWidth::W8,
                raw: 0x1ff,
            },
            Instruction::IConst {
                dest: 1,
                width: MachineIntWidth::W8,
                raw: 1,
            },
            Instruction::IAdd {
                dest: 2,
                left: 0,
                right: 1,
                width: MachineIntWidth::W8,
            },
            Instruction::ISub {
                dest: 3,
                left: 2,
                right: 1,
                width: MachineIntWidth::W8,
            },
            Instruction::IMul {
                dest: 4,
                left: 0,
                right: 1,
                width: MachineIntWidth::W8,
            },
            Instruction::IntNot {
                dest: 5,
                value: 0,
                width: MachineIntWidth::W8,
            },
            Instruction::IConst {
                dest: 6,
                width: MachineIntWidth::W8,
                raw: 0x80,
            },
            Instruction::Shl {
                dest: 7,
                value: 6,
                amount: 1,
                width: MachineIntWidth::W8,
            },
            Instruction::LShr {
                dest: 8,
                value: 6,
                amount: 1,
                width: MachineIntWidth::W8,
            },
            Instruction::AShr {
                dest: 9,
                value: 6,
                amount: 1,
                width: MachineIntWidth::W8,
            },
            Instruction::And {
                dest: 10,
                left: 6,
                right: 0,
                width: MachineIntWidth::W8,
            },
            Instruction::Or {
                dest: 11,
                left: 6,
                right: 1,
                width: MachineIntWidth::W8,
            },
            Instruction::Xor {
                dest: 12,
                left: 6,
                right: 0,
                width: MachineIntWidth::W8,
            },
            Instruction::SDiv {
                dest: 13,
                left: 6,
                right: 1,
                width: MachineIntWidth::W8,
            },
            Instruction::SRem {
                dest: 14,
                left: 6,
                right: 1,
                width: MachineIntWidth::W8,
            },
            Instruction::UDiv {
                dest: 15,
                left: 6,
                right: 1,
                width: MachineIntWidth::W8,
            },
            Instruction::URem {
                dest: 16,
                left: 6,
                right: 1,
                width: MachineIntWidth::W8,
            },
            Instruction::ICmp {
                dest: 17,
                left: 6,
                right: 1,
                width: MachineIntWidth::W8,
                predicate: MachineIntPredicate::Slt,
            },
            Instruction::ICmp {
                dest: 18,
                left: 6,
                right: 1,
                width: MachineIntWidth::W8,
                predicate: MachineIntPredicate::Ugt,
            },
            Instruction::ICmp {
                dest: 19,
                left: 6,
                right: 1,
                width: MachineIntWidth::W8,
                predicate: MachineIntPredicate::Sge,
            },
            Instruction::ICmp {
                dest: 20,
                left: 6,
                right: 1,
                width: MachineIntWidth::W8,
                predicate: MachineIntPredicate::Ule,
            },
        ];
        for register in 0..=20 {
            instructions.push(print_register(register, 21));
        }
        instructions.push(Instruction::ReturnNil);
        let program = machine_program(instructions, 22);

        let output = VM::with_config_verified(&program, RunConfig::unlimited())
            .expect("machine program should pass structural verification")
            .run()
            .expect("machine integer operations should run");
        assert_eq!(
            output,
            "machine_int(255, 0x00000000000000ff)\n\
machine_int(1, 0x0000000000000001)\n\
machine_int(0, 0x0000000000000000)\n\
machine_int(255, 0x00000000000000ff)\n\
machine_int(255, 0x00000000000000ff)\n\
machine_int(0, 0x0000000000000000)\n\
machine_int(128, 0x0000000000000080)\n\
machine_int(0, 0x0000000000000000)\n\
machine_int(64, 0x0000000000000040)\n\
machine_int(192, 0x00000000000000c0)\n\
machine_int(128, 0x0000000000000080)\n\
machine_int(129, 0x0000000000000081)\n\
machine_int(127, 0x000000000000007f)\n\
machine_int(128, 0x0000000000000080)\n\
machine_int(0, 0x0000000000000000)\n\
machine_int(128, 0x0000000000000080)\n\
machine_int(0, 0x0000000000000000)\n\
true\ntrue\nfalse\nfalse\n"
        );
    }

    #[test]
    fn machine_integer_wide_ops_and_all_compare_predicates() {
        let cases = [
            (MachineIntWidth::W8, 0x80, 0x40, 0xc0, 0xff, 0xfe),
            (MachineIntWidth::W16, 0x8000, 0x4000, 0xc000, 0xffff, 0xfffe),
            (
                MachineIntWidth::W32,
                0x8000_0000,
                0x4000_0000,
                0xc000_0000,
                0xffff_ffff,
                0xffff_fffe,
            ),
            (
                MachineIntWidth::W64,
                0x8000_0000_0000_0000,
                0x4000_0000_0000_0000,
                0xc000_0000_0000_0000,
                u64::MAX,
                u64::MAX - 1,
            ),
        ];
        let mut instructions = vec![Instruction::BlockStart { id: BlockId(0) }];
        let mut expected = String::new();
        for (case_index, (width, minimum, logical_shift, arithmetic_shift, maximum, product)) in
            cases.iter().copied().enumerate()
        {
            let base = case_index * 30;
            instructions.extend([
                Instruction::IConst {
                    dest: base,
                    width,
                    raw: u64::MAX,
                },
                Instruction::IConst {
                    dest: base + 1,
                    width,
                    raw: 1,
                },
                Instruction::IConst {
                    dest: base + 2,
                    width,
                    raw: 0,
                },
                Instruction::IConst {
                    dest: base + 3,
                    width,
                    raw: 2,
                },
                Instruction::IConst {
                    dest: base + 4,
                    width,
                    raw: minimum,
                },
                Instruction::IAdd {
                    dest: base + 5,
                    left: base,
                    right: base + 1,
                    width,
                },
                Instruction::ISub {
                    dest: base + 6,
                    left: base + 2,
                    right: base + 1,
                    width,
                },
                Instruction::IMul {
                    dest: base + 7,
                    left: base,
                    right: base + 3,
                    width,
                },
                Instruction::And {
                    dest: base + 8,
                    left: base,
                    right: base + 3,
                    width,
                },
                Instruction::Or {
                    dest: base + 9,
                    left: base + 2,
                    right: base,
                    width,
                },
                Instruction::Xor {
                    dest: base + 10,
                    left: base,
                    right: base,
                    width,
                },
                Instruction::IntNot {
                    dest: base + 11,
                    value: base + 2,
                    width,
                },
                Instruction::Shl {
                    dest: base + 12,
                    value: base + 4,
                    amount: base + 1,
                    width,
                },
                Instruction::LShr {
                    dest: base + 13,
                    value: base + 4,
                    amount: base + 1,
                    width,
                },
                Instruction::AShr {
                    dest: base + 14,
                    value: base + 4,
                    amount: base + 1,
                    width,
                },
                Instruction::SDiv {
                    dest: base + 15,
                    left: base + 4,
                    right: base + 1,
                    width,
                },
                Instruction::SRem {
                    dest: base + 16,
                    left: base + 4,
                    right: base + 1,
                    width,
                },
                Instruction::UDiv {
                    dest: base + 17,
                    left: base + 4,
                    right: base + 1,
                    width,
                },
                Instruction::URem {
                    dest: base + 18,
                    left: base + 4,
                    right: base + 1,
                    width,
                },
            ]);
            let predicates = [
                MachineIntPredicate::Eq,
                MachineIntPredicate::Ne,
                MachineIntPredicate::Slt,
                MachineIntPredicate::Sle,
                MachineIntPredicate::Sgt,
                MachineIntPredicate::Sge,
                MachineIntPredicate::Ult,
                MachineIntPredicate::Ule,
                MachineIntPredicate::Ugt,
                MachineIntPredicate::Uge,
            ];
            for (predicate_index, predicate) in predicates.into_iter().enumerate() {
                instructions.push(Instruction::ICmp {
                    dest: base + 19 + predicate_index,
                    left: base + 4,
                    right: base + 1,
                    width,
                    predicate,
                });
            }
            for register in (base + 5)..=(base + 18) {
                instructions.push(print_register(register, base + 29));
            }
            for register in (base + 19)..=(base + 28) {
                instructions.push(print_register(register, base + 29));
            }

            for value in [
                0,
                maximum,
                product,
                2,
                maximum,
                0,
                maximum,
                0,
                logical_shift,
                arithmetic_shift,
                minimum,
                0,
                minimum,
                0,
            ] {
                expected.push_str(&format!("machine_int({}, 0x{:016x})\n", value, value));
            }
            for value in [
                false, true, true, true, false, false, false, false, true, true,
            ] {
                expected.push_str(&format!("{}\n", value));
            }
        }
        instructions.push(Instruction::ReturnNil);
        let program = machine_program(instructions, cases.len() * 30);
        assert_eq!(
            VM::with_config_verified(&program, RunConfig::unlimited())
                .expect("wide machine program should verify")
                .run()
                .expect("wide machine operations should run"),
            expected
        );
    }

    #[test]
    fn machine_integer_conversions_preserve_low_bits_and_sign() {
        let mut instructions = vec![
            Instruction::BlockStart { id: BlockId(0) },
            Instruction::IConst {
                dest: 0,
                width: MachineIntWidth::W64,
                raw: 0x1234_5678_9abc_0080,
            },
            Instruction::Trunc {
                dest: 1,
                value: 0,
                from_width: MachineIntWidth::W64,
                to_width: MachineIntWidth::W32,
            },
            Instruction::Trunc {
                dest: 2,
                value: 1,
                from_width: MachineIntWidth::W32,
                to_width: MachineIntWidth::W16,
            },
            Instruction::Trunc {
                dest: 3,
                value: 2,
                from_width: MachineIntWidth::W16,
                to_width: MachineIntWidth::W8,
            },
            Instruction::ZExt {
                dest: 4,
                value: 3,
                from_width: MachineIntWidth::W8,
                to_width: MachineIntWidth::W16,
            },
            Instruction::ZExt {
                dest: 5,
                value: 4,
                from_width: MachineIntWidth::W16,
                to_width: MachineIntWidth::W32,
            },
            Instruction::ZExt {
                dest: 6,
                value: 5,
                from_width: MachineIntWidth::W32,
                to_width: MachineIntWidth::W64,
            },
            Instruction::SExt {
                dest: 7,
                value: 3,
                from_width: MachineIntWidth::W8,
                to_width: MachineIntWidth::W16,
            },
            Instruction::SExt {
                dest: 8,
                value: 7,
                from_width: MachineIntWidth::W16,
                to_width: MachineIntWidth::W32,
            },
            Instruction::SExt {
                dest: 9,
                value: 8,
                from_width: MachineIntWidth::W32,
                to_width: MachineIntWidth::W64,
            },
        ];
        for register in 1..=9 {
            instructions.push(print_register(register, 10));
        }
        instructions.push(Instruction::ReturnNil);
        let program = machine_program(instructions, 11);
        let output = VM::with_config_verified(&program, RunConfig::unlimited())
            .expect("machine conversions should verify")
            .run()
            .expect("machine conversions should run");
        assert_eq!(
            output,
            "machine_int(2596012160, 0x000000009abc0080)\n\
machine_int(128, 0x0000000000000080)\n\
machine_int(128, 0x0000000000000080)\n\
machine_int(128, 0x0000000000000080)\n\
machine_int(128, 0x0000000000000080)\n\
machine_int(128, 0x0000000000000080)\n\
machine_int(65408, 0x000000000000ff80)\n\
machine_int(4294967168, 0x00000000ffffff80)\n\
machine_int(18446744073709551488, 0xffffffffffffff80)\n"
        );
    }

    #[test]
    fn machine_float_arithmetic_comparison_and_conversions_follow_abi() {
        let program = machine_program(
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::FConst {
                    dest: 0,
                    format: MachineFloatFormat::F32,
                    bits: 0x3f80_0000,
                },
                Instruction::FConst {
                    dest: 1,
                    format: MachineFloatFormat::F32,
                    bits: 0x3f80_0000,
                },
                Instruction::FAdd {
                    dest: 2,
                    left: 0,
                    right: 1,
                    format: MachineFloatFormat::F32,
                },
                Instruction::FConst {
                    dest: 3,
                    format: MachineFloatFormat::F64,
                    bits: 0x3ff8_0000_0000_0000,
                },
                Instruction::FConst {
                    dest: 4,
                    format: MachineFloatFormat::F64,
                    bits: 0,
                },
                Instruction::FDiv {
                    dest: 5,
                    left: 3,
                    right: 4,
                    format: MachineFloatFormat::F64,
                },
                Instruction::FConst {
                    dest: 6,
                    format: MachineFloatFormat::F64,
                    bits: 0x7ff8_0000_0000_0001,
                },
                Instruction::FCmp {
                    dest: 7,
                    left: 6,
                    right: 3,
                    format: MachineFloatFormat::F64,
                    predicate: MachineFloatPredicate::OEq,
                },
                Instruction::FCmp {
                    dest: 8,
                    left: 6,
                    right: 3,
                    format: MachineFloatFormat::F64,
                    predicate: MachineFloatPredicate::UNe,
                },
                Instruction::FCmp {
                    dest: 9,
                    left: 6,
                    right: 3,
                    format: MachineFloatFormat::F64,
                    predicate: MachineFloatPredicate::Uno,
                },
                Instruction::FCmp {
                    dest: 10,
                    left: 6,
                    right: 3,
                    format: MachineFloatFormat::F64,
                    predicate: MachineFloatPredicate::Ord,
                },
                Instruction::IConst {
                    dest: 11,
                    width: MachineIntWidth::W8,
                    raw: 0xff,
                },
                Instruction::SIToFp {
                    dest: 12,
                    value: 11,
                    int_width: MachineIntWidth::W8,
                    float_format: MachineFloatFormat::F64,
                },
                Instruction::UIToFp {
                    dest: 13,
                    value: 11,
                    int_width: MachineIntWidth::W8,
                    float_format: MachineFloatFormat::F32,
                },
                Instruction::FPToSI {
                    dest: 14,
                    value: 3,
                    float_format: MachineFloatFormat::F64,
                    int_width: MachineIntWidth::W8,
                },
                Instruction::FPToUI {
                    dest: 15,
                    value: 3,
                    float_format: MachineFloatFormat::F64,
                    int_width: MachineIntWidth::W8,
                },
                Instruction::FPExt {
                    dest: 16,
                    value: 0,
                    from_format: MachineFloatFormat::F32,
                    to_format: MachineFloatFormat::F64,
                },
                Instruction::FPTrunc {
                    dest: 17,
                    value: 3,
                    from_format: MachineFloatFormat::F64,
                    to_format: MachineFloatFormat::F32,
                },
                Instruction::FConst {
                    dest: 18,
                    format: MachineFloatFormat::F64,
                    bits: 0x8000_0000_0000_0000,
                },
                Instruction::FNeg {
                    dest: 19,
                    value: 18,
                    format: MachineFloatFormat::F64,
                },
                Instruction::ReturnNil,
            ],
            20,
        );
        let registers = run_machine_body(
            &mut VM::with_config_verified(&program, RunConfig::unlimited())
                .expect("machine float program should verify"),
            vec![Value::Nil; 20],
        )
        .expect("machine float program should execute");

        assert!(matches!(registers[2], Value::MachineFloat(value) if value == 2.0));
        assert!(
            matches!(registers[5], Value::MachineFloat(value) if value.is_infinite() && value.is_sign_positive())
        );
        assert!(matches!(registers[7], Value::Bool(false)));
        assert!(matches!(registers[8], Value::Bool(true)));
        assert!(matches!(registers[9], Value::Bool(true)));
        assert!(matches!(registers[10], Value::Bool(false)));
        assert!(matches!(registers[12], Value::MachineFloat(value) if value == -1.0));
        assert!(matches!(registers[13], Value::MachineFloat(value) if value == 255.0));
        assert!(matches!(registers[14], Value::MachineInt(1)));
        assert!(matches!(registers[15], Value::MachineInt(1)));
        assert!(
            matches!(registers[16], Value::MachineFloat(value) if value.to_bits() == 1.0f64.to_bits())
        );
        assert!(
            matches!(registers[17], Value::MachineFloat(value) if value.to_bits() == 1.5f64.to_bits())
        );
        assert!(matches!(registers[19], Value::MachineFloat(value) if value.to_bits() == 0));
    }

    #[test]
    fn machine_float_to_int_rejects_non_finite_and_out_of_range_values() {
        let cases = [
            (0x7ff8_0000_0000_0000, MachineFloatFormat::F64, "non-finite"),
            (0x7ff0_0000_0000_0000, MachineFloatFormat::F64, "non-finite"),
            (0xbff0_0000_0000_0000, MachineFloatFormat::F64, "outside"),
            (0x43f0_0000_0000_0000, MachineFloatFormat::F64, "outside"),
        ];
        for (bits, format, expected) in cases {
            let program = machine_program(
                vec![
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::FConst {
                        dest: 0,
                        format,
                        bits,
                    },
                    Instruction::FPToUI {
                        dest: 1,
                        value: 0,
                        float_format: format,
                        int_width: MachineIntWidth::W8,
                    },
                    Instruction::ReturnNil,
                ],
                2,
            );
            let error = VM::with_config_verified(&program, RunConfig::unlimited())
                .expect("conversion trap program should verify")
                .run()
                .expect_err("invalid float conversion should trap");
            assert_eq!(error.kind, RuntimeErrorKind::InvalidConversion);
            assert!(error.message.contains(expected), "{}", error.message);
        }

        let program = machine_program(
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::FConst {
                    dest: 0,
                    format: MachineFloatFormat::F64,
                    bits: 0x3ff8_0000_0000_0000,
                },
                Instruction::FPExt {
                    dest: 1,
                    value: 0,
                    from_format: MachineFloatFormat::F64,
                    to_format: MachineFloatFormat::F32,
                },
                Instruction::ReturnNil,
            ],
            2,
        );
        let error = VM::new(&program)
            .run()
            .expect_err("unverified invalid float direction should trap");
        assert_eq!(error.kind, RuntimeErrorKind::InvalidConversion);
        assert!(error.message.contains("fpext requires f32 -> f64"));
    }

    #[test]
    fn machine_integer_errors_are_checked_without_host_overflow() {
        let cases = [
            (
                vec![
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::IConst {
                        dest: 0,
                        width: MachineIntWidth::W8,
                        raw: 1,
                    },
                    Instruction::IConst {
                        dest: 1,
                        width: MachineIntWidth::W8,
                        raw: 0,
                    },
                    Instruction::SDiv {
                        dest: 2,
                        left: 0,
                        right: 1,
                        width: MachineIntWidth::W8,
                    },
                    Instruction::ReturnNil,
                ],
                3,
                "integer division by zero",
                RuntimeErrorKind::IntegerDivisionByZero,
            ),
            (
                vec![
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::IConst {
                        dest: 0,
                        width: MachineIntWidth::W8,
                        raw: 0x80,
                    },
                    Instruction::IConst {
                        dest: 1,
                        width: MachineIntWidth::W8,
                        raw: 0xff,
                    },
                    Instruction::SDiv {
                        dest: 2,
                        left: 0,
                        right: 1,
                        width: MachineIntWidth::W8,
                    },
                    Instruction::ReturnNil,
                ],
                3,
                "integer division overflow",
                RuntimeErrorKind::IntegerDivisionOverflow,
            ),
            (
                vec![
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::IConst {
                        dest: 0,
                        width: MachineIntWidth::W8,
                        raw: 0x80,
                    },
                    Instruction::IConst {
                        dest: 1,
                        width: MachineIntWidth::W8,
                        raw: 0xff,
                    },
                    Instruction::SRem {
                        dest: 2,
                        left: 0,
                        right: 1,
                        width: MachineIntWidth::W8,
                    },
                    Instruction::ReturnNil,
                ],
                3,
                "integer division overflow",
                RuntimeErrorKind::IntegerDivisionOverflow,
            ),
            (
                vec![
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::IConst {
                        dest: 0,
                        width: MachineIntWidth::W8,
                        raw: 1,
                    },
                    Instruction::IConst {
                        dest: 1,
                        width: MachineIntWidth::W8,
                        raw: 8,
                    },
                    Instruction::Shl {
                        dest: 2,
                        value: 0,
                        amount: 1,
                        width: MachineIntWidth::W8,
                    },
                    Instruction::ReturnNil,
                ],
                3,
                "invalid shift amount",
                RuntimeErrorKind::InvalidShiftAmount,
            ),
        ];

        for (instructions, registers, message, kind) in cases {
            let program = machine_program(instructions, registers);
            let error = VM::with_config_verified(&program, RunConfig::unlimited())
                .expect("trap cases should pass structural verification")
                .run()
                .expect_err("machine trap should be reported as a VM error");
            assert_eq!(error.kind, kind);
            assert_eq!(error.message, message);
        }

        for width in [
            MachineIntWidth::W8,
            MachineIntWidth::W16,
            MachineIntWidth::W32,
            MachineIntWidth::W64,
        ] {
            let minimum = 1u64 << (width.bits() - 1);
            let program = machine_program(
                vec![
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::IConst {
                        dest: 0,
                        width,
                        raw: minimum,
                    },
                    Instruction::IConst {
                        dest: 1,
                        width,
                        raw: width.mask(),
                    },
                    Instruction::SRem {
                        dest: 2,
                        left: 0,
                        right: 1,
                        width,
                    },
                    Instruction::ReturnNil,
                ],
                3,
            );
            let error = VM::with_config_verified(&program, RunConfig::unlimited())
                .expect("wide remainder trap should pass structural verification")
                .run()
                .expect_err("signed minimum remainder by negative one must trap");
            assert_eq!(error.kind, RuntimeErrorKind::IntegerDivisionOverflow);
            assert_eq!(error.message, "integer division overflow");
        }

        let invalid_conversions = [
            (
                Instruction::Trunc {
                    dest: 1,
                    value: 0,
                    from_width: MachineIntWidth::W8,
                    to_width: MachineIntWidth::W16,
                },
                "to_width < from_width",
            ),
            (
                Instruction::ZExt {
                    dest: 1,
                    value: 0,
                    from_width: MachineIntWidth::W16,
                    to_width: MachineIntWidth::W8,
                },
                "to_width > from_width",
            ),
            (
                Instruction::SExt {
                    dest: 1,
                    value: 0,
                    from_width: MachineIntWidth::W32,
                    to_width: MachineIntWidth::W32,
                },
                "to_width > from_width",
            ),
        ];
        for (conversion, expected) in invalid_conversions {
            let program = machine_program(
                vec![
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::IConst {
                        dest: 0,
                        width: MachineIntWidth::W64,
                        raw: 1,
                    },
                    conversion,
                    Instruction::ReturnNil,
                ],
                2,
            );
            let error = match VM::with_config_verified(&program, RunConfig::unlimited()) {
                Ok(_) => panic!("invalid conversion direction must fail verification"),
                Err(error) => error,
            };
            assert!(error.message.contains(expected), "{}", error.message);

            let error = VM::new(&program)
                .run()
                .expect_err("unverified conversion must still reject its direction");
            assert_eq!(error.kind, RuntimeErrorKind::InvalidConversion);
            assert!(error.message.contains(expected), "{}", error.message);
        }

        let mut wrong_domain = machine_program(
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::Constant {
                    dest: 0,
                    constant: 0,
                },
                Instruction::IConst {
                    dest: 1,
                    width: MachineIntWidth::W8,
                    raw: 1,
                },
                Instruction::IAdd {
                    dest: 2,
                    left: 0,
                    right: 1,
                    width: MachineIntWidth::W8,
                },
                Instruction::ReturnNil,
            ],
            3,
        );
        wrong_domain
            .constants
            .push(Constant::Number("1".to_string()));
        let error = VM::with_config_verified(&wrong_domain, RunConfig::unlimited())
            .expect("wrong-domain operands are a runtime condition")
            .run()
            .expect_err("machine arithmetic must reject dynamic values");
        assert_eq!(error.kind, RuntimeErrorKind::InvalidOperand);
        assert_eq!(error.message, "iadd expects machine_int operands");

        let mut wrong_domain = machine_program(
            vec![
                Instruction::BlockStart { id: BlockId(0) },
                Instruction::Constant {
                    dest: 0,
                    constant: 0,
                },
                Instruction::Trunc {
                    dest: 1,
                    value: 0,
                    from_width: MachineIntWidth::W64,
                    to_width: MachineIntWidth::W8,
                },
                Instruction::ReturnNil,
            ],
            2,
        );
        wrong_domain
            .constants
            .push(Constant::Number("1".to_string()));
        let error = VM::with_config_verified(&wrong_domain, RunConfig::unlimited())
            .expect("wrong-domain conversion is valid bytecode")
            .run()
            .expect_err("conversion must reject dynamic values");
        assert_eq!(error.kind, RuntimeErrorKind::InvalidOperand);
        assert_eq!(error.message, "trunc expects machine_int, got number");
    }

    #[test]
    fn machine_integer_widths_have_explicit_masks() {
        assert_eq!(MachineIntWidth::W8.bits(), 8);
        assert_eq!(MachineIntWidth::W16.bits(), 16);
        assert_eq!(MachineIntWidth::W32.bits(), 32);
        assert_eq!(MachineIntWidth::W64.bits(), 64);
        assert_eq!(MachineIntWidth::W8.mask(), 0xff);
        assert_eq!(MachineIntWidth::W16.mask(), 0xffff);
        assert_eq!(MachineIntWidth::W32.mask(), 0xffff_ffff);
        assert_eq!(MachineIntWidth::W64.mask(), u64::MAX);
        assert_eq!(MachineIntWidth::I32, MachineIntWidth::W32);
    }

    #[test]
    fn typed_collection_opcodes_dispatch_with_legacy_runtime_errors() {
        let program = Program {
            constants: vec![
                Constant::Number("1".to_string()),
                Constant::Number("2".to_string()),
                Constant::String("hello".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 13,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Constant {
                        dest: 1,
                        constant: 1,
                    },
                    Instruction::Array {
                        dest: 2,
                        elements: vec![0, 1],
                    },
                    Instruction::ArrayGet {
                        dest: 3,
                        collection: 2,
                        index: 0,
                    },
                    Instruction::ArraySet {
                        dest: 4,
                        collection: 2,
                        index: 0,
                        value: 1,
                    },
                    Instruction::LenArray { dest: 5, value: 2 },
                    Instruction::Map {
                        dest: 6,
                        entries: vec![(0, 1)],
                    },
                    Instruction::MapGet {
                        dest: 7,
                        collection: 6,
                        index: 0,
                    },
                    Instruction::MapSet {
                        dest: 8,
                        collection: 6,
                        index: 1,
                        value: 0,
                    },
                    Instruction::LenMap { dest: 9, value: 6 },
                    Instruction::Constant {
                        dest: 10,
                        constant: 2,
                    },
                    Instruction::LenStr {
                        dest: 11,
                        value: 10,
                    },
                    Instruction::CallNative {
                        dest: 12,
                        native: NativeId(0),
                        arguments: vec![3],
                    },
                    Instruction::CallNative {
                        dest: 12,
                        native: NativeId(0),
                        arguments: vec![4],
                    },
                    Instruction::CallNative {
                        dest: 12,
                        native: NativeId(0),
                        arguments: vec![5],
                    },
                    Instruction::CallNative {
                        dest: 12,
                        native: NativeId(0),
                        arguments: vec![7],
                    },
                    Instruction::CallNative {
                        dest: 12,
                        native: NativeId(0),
                        arguments: vec![8],
                    },
                    Instruction::CallNative {
                        dest: 12,
                        native: NativeId(0),
                        arguments: vec![9],
                    },
                    Instruction::CallNative {
                        dest: 12,
                        native: NativeId(0),
                        arguments: vec![11],
                    },
                    Instruction::ReturnNil,
                ],
                locations: vec![None; 20],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };

        assert_eq!(
            VM::new(&program)
                .run()
                .expect("typed collection ops should run"),
            "2\n2\n2\n2\n1\n2\n5\n"
        );
    }

    #[test]
    fn iterator_protocol_snapshots_array_length_and_map_keys() {
        let program = Program {
            constants: vec![
                Constant::Number("1".to_string()),
                Constant::Number("2".to_string()),
                Constant::Number("3".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![
                NativeImport {
                    name: "push".to_string(),
                    abi: 1,
                },
                NativeImport {
                    name: "print".to_string(),
                    abi: 1,
                },
            ],
            modules: Vec::new(),
            names: vec!["push".to_string()],
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 17,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Constant {
                        dest: 1,
                        constant: 1,
                    },
                    Instruction::Constant {
                        dest: 2,
                        constant: 2,
                    },
                    Instruction::Array {
                        dest: 3,
                        elements: vec![0, 1],
                    },
                    Instruction::IterInit { dest: 4, value: 3 },
                    Instruction::CallNative {
                        dest: 5,
                        native: NativeId(0),
                        arguments: vec![3, 2],
                    },
                    Instruction::IterHas { dest: 6, value: 4 },
                    Instruction::IterNext { dest: 7, value: 4 },
                    Instruction::IterNext { dest: 8, value: 4 },
                    Instruction::IterHas { dest: 9, value: 4 },
                    Instruction::LenArray { dest: 10, value: 3 },
                    Instruction::Map {
                        dest: 11,
                        entries: vec![(0, 1)],
                    },
                    Instruction::IterInit {
                        dest: 12,
                        value: 11,
                    },
                    Instruction::MapSet {
                        dest: 13,
                        collection: 11,
                        index: 2,
                        value: 0,
                    },
                    Instruction::IterNext {
                        dest: 14,
                        value: 12,
                    },
                    Instruction::IterHas {
                        dest: 15,
                        value: 12,
                    },
                    Instruction::CallNative {
                        dest: 16,
                        native: NativeId(1),
                        arguments: vec![7],
                    },
                    Instruction::CallNative {
                        dest: 16,
                        native: NativeId(1),
                        arguments: vec![8],
                    },
                    Instruction::CallNative {
                        dest: 16,
                        native: NativeId(1),
                        arguments: vec![9],
                    },
                    Instruction::CallNative {
                        dest: 16,
                        native: NativeId(1),
                        arguments: vec![10],
                    },
                    Instruction::CallNative {
                        dest: 16,
                        native: NativeId(1),
                        arguments: vec![14],
                    },
                    Instruction::CallNative {
                        dest: 16,
                        native: NativeId(1),
                        arguments: vec![15],
                    },
                    Instruction::ReturnNil,
                ],
                locations: vec![None; 23],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };

        assert_eq!(
            VM::new(&program)
                .run()
                .expect("iterator snapshots should run"),
            "1\n2\nfalse\n3\n1\nfalse\n"
        );
    }

    #[test]
    fn direct_calls_skip_function_value_construction_and_charge() {
        let program = Program {
            constants: vec![Constant::Number("7".to_string())],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 2,
                    instructions: vec![
                        Instruction::CallDirect {
                            dest: 0,
                            function: FuncId(1),
                            arguments: Vec::new(),
                        },
                        Instruction::CallNative {
                            dest: 1,
                            native: NativeId(0),
                            arguments: vec![0],
                        },
                        Instruction::ReturnNil,
                    ],
                    locations: vec![None; 3],
                },
                Function {
                    id: FuncId(1),
                    name: "seven".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 1,
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None; 2],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };

        assert_eq!(
            VM::new(&program).run().expect("direct call should run"),
            "7\n"
        );
    }

    #[test]
    fn map_semantics_are_unique_ordered_and_last_write_wins() {
        let program = Program {
            constants: vec![
                Constant::String("a".to_string()),
                Constant::String("b".to_string()),
                Constant::String("c".to_string()),
                Constant::Number("1".to_string()),
                Constant::Number("2".to_string()),
                Constant::Number("3".to_string()),
                Constant::Number("30".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![
                NativeImport {
                    name: "merge".to_string(),
                    abi: 1,
                },
                NativeImport {
                    name: "print".to_string(),
                    abi: 1,
                },
            ],
            modules: Vec::new(),
            names: vec!["merge".to_string()],
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 12,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Constant {
                        dest: 1,
                        constant: 1,
                    },
                    Instruction::Constant {
                        dest: 2,
                        constant: 2,
                    },
                    Instruction::Constant {
                        dest: 3,
                        constant: 3,
                    },
                    Instruction::Constant {
                        dest: 4,
                        constant: 4,
                    },
                    Instruction::Constant {
                        dest: 5,
                        constant: 5,
                    },
                    Instruction::Constant {
                        dest: 6,
                        constant: 6,
                    },
                    Instruction::Map {
                        dest: 7,
                        entries: vec![(0, 3), (1, 4), (0, 5)],
                    },
                    Instruction::MapSet {
                        dest: 8,
                        collection: 7,
                        index: 0,
                        value: 6,
                    },
                    Instruction::Map {
                        dest: 9,
                        entries: vec![(1, 5), (2, 4)],
                    },
                    Instruction::CallNative {
                        dest: 10,
                        native: NativeId(0),
                        arguments: vec![7, 9],
                    },
                    Instruction::CallNative {
                        dest: 11,
                        native: NativeId(1),
                        arguments: vec![10],
                    },
                    Instruction::ReturnNil,
                ],
                locations: vec![None; 13],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };

        assert_eq!(
            VM::new(&program).run().expect("map semantics should run"),
            "map{a: 30, b: 3, c: 2}\n"
        );
    }

    #[test]
    fn return_transfer_moves_value_out_of_the_dead_frame_register() {
        let program = empty_program();
        let vm = VM::new(&program);
        let mut frame = Frame {
            body: None,
            ip: 0,
            registers: vec![Value::string("returned")],
            locals: vm.heap.new_local_slots(),
            closure: vm.heap.new_environment(),
            upvalues: Vec::new(),
            variable_plan: None,
            is_main: false,
            function: Rc::from("returner"),
            function_index: Some(0),
            return_target: None,
            machine_frame_base: None,
            machine_frame_size: 0,
        };

        let value = vm
            .take_register(&mut frame, 0)
            .expect("return register should be readable");
        assert!(matches!(value, Value::String(value) if value.as_ref() == "returned"));
        assert!(matches!(frame.registers[0], Value::Nil));
    }

    fn cooperative_print_program() -> Program {
        Program {
            constants: vec![Constant::Number("7".to_string())],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 2,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::CallNative {
                        dest: 1,
                        native: NativeId(0),
                        arguments: vec![0],
                    },
                    Instruction::Return { value: 0 },
                ],
                locations: vec![None; 3],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn cooperative_call_program() -> Program {
        Program {
            constants: vec![Constant::Number("42".to_string())],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 4,
                    instructions: vec![
                        Instruction::MakeFunction {
                            dest: 0,
                            function: FuncId(1),
                        },
                        Instruction::Call {
                            dest: 1,
                            callee: 0,
                            arguments: Vec::new(),
                        },
                        Instruction::CallNative {
                            dest: 3,
                            native: NativeId(0),
                            arguments: vec![1],
                        },
                        Instruction::Return { value: 1 },
                    ],
                    locations: vec![None; 4],
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "answer".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 1,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None; 2],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn ordinary_jit_call_program(function: Function, constants: Vec<Constant>) -> Program {
        Program {
            constants,
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 4,
                    instructions: vec![
                        Instruction::MakeFunction {
                            dest: 0,
                            function: FuncId(1),
                        },
                        Instruction::Call {
                            dest: 1,
                            callee: 0,
                            arguments: Vec::new(),
                        },
                        Instruction::CallNative {
                            dest: 3,
                            native: NativeId(0),
                            arguments: vec![1],
                        },
                        Instruction::Return { value: 1 },
                    ],
                    locations: vec![None; 4],
                },
                function,
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn execution_closure_program() -> Program {
        match crate::format::parse_artifact(include_str!(
            "../../tests/bytecode_artifacts/benchmark_execution_closure/expected.cdbc"
        ))
        .expect("execution_closure artifact should parse")
        {
            crate::format::Artifact::Program(program) => program,
            crate::format::Artifact::Module(_) => {
                panic!("execution_closure fixture must be a linked program")
            }
        }
    }

    fn cooperative_loop_program() -> Program {
        Program {
            constants: vec![Constant::Number("1".to_string())],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 1,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::Br { target: BlockId(0) },
                ],
                locations: vec![None; 3],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn cooperative_native_callback_program() -> Program {
        Program {
            constants: vec![
                Constant::Number("1".to_string()),
                Constant::Number("2".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![
                NativeImport {
                    name: "map".to_string(),
                    abi: 1,
                },
                NativeImport {
                    name: "print".to_string(),
                    abi: 1,
                },
            ],
            modules: Vec::new(),
            names: vec!["map".to_string(), "item".to_string()],
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 6,
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::Constant {
                            dest: 1,
                            constant: 1,
                        },
                        Instruction::Array {
                            dest: 2,
                            elements: vec![0, 1],
                        },
                        Instruction::MakeFunction {
                            dest: 3,
                            function: FuncId(1),
                        },
                        Instruction::CallNative {
                            dest: 4,
                            native: NativeId(0),
                            arguments: vec![2, 3],
                        },
                        Instruction::CallNative {
                            dest: 5,
                            native: NativeId(1),
                            arguments: vec![4],
                        },
                        Instruction::Return { value: 4 },
                    ],
                    locations: vec![None; 7],
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "identity".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 1,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::LoadLocal { dest: 0, slot: 0 },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None; 2],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn cooperative_nested_native_callback_program() -> Program {
        Program {
            constants: vec![
                Constant::Number("1".to_string()),
                Constant::Number("2".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "map".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: vec!["map".to_string(), "item".to_string()],
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 2,
                    instructions: vec![
                        Instruction::MakeFunction {
                            dest: 0,
                            function: FuncId(1),
                        },
                        Instruction::Call {
                            dest: 1,
                            callee: 0,
                            arguments: Vec::new(),
                        },
                        Instruction::Return { value: 1 },
                    ],
                    locations: vec![None; 3],
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "worker".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 5,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::Constant {
                            dest: 1,
                            constant: 1,
                        },
                        Instruction::Array {
                            dest: 2,
                            elements: vec![0, 1],
                        },
                        Instruction::MakeFunction {
                            dest: 3,
                            function: FuncId(2),
                        },
                        Instruction::CallNative {
                            dest: 4,
                            native: NativeId(0),
                            arguments: vec![2, 3],
                        },
                        Instruction::Return { value: 4 },
                    ],
                    locations: vec![None; 6],
                },
                Function {
                    id: FuncId(2),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "identity".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 1,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::LoadLocal { dest: 0, slot: 0 },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None; 2],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn cooperative_cycle_program() -> Program {
        Program {
            constants: vec![Constant::Nil, Constant::Number("0".to_string())],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 4,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Constant {
                        dest: 1,
                        constant: 1,
                    },
                    Instruction::Array {
                        dest: 2,
                        elements: vec![0],
                    },
                    Instruction::AssignIndex {
                        dest: 3,
                        collection: 2,
                        index: 1,
                        value: 2,
                    },
                    Instruction::Return { value: 2 },
                ],
                locations: vec![None; 5],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn cooperative_host_task_program() -> Program {
        Program {
            constants: vec![
                Constant::Number("7".to_string()),
                Constant::Number("8".to_string()),
                Constant::Number("1".to_string()),
                Constant::Number("0".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "target".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 1,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None; 2],
                },
                Function {
                    id: FuncId(2),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "waiter".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 1,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 1,
                        },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None; 2],
                },
                Function {
                    id: FuncId(3),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "failure".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 3,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 2,
                        },
                        Instruction::Constant {
                            dest: 1,
                            constant: 3,
                        },
                        Instruction::Divide {
                            dest: 2,
                            left: 0,
                            right: 1,
                        },
                        Instruction::Return { value: 2 },
                    ],
                    locations: vec![None; 4],
                },
                Function {
                    id: FuncId(4),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "pending".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 1,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::BlockStart { id: BlockId(0) },
                        Instruction::Br { target: BlockId(0) },
                    ],
                    locations: vec![None; 3],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn cooperative_output_program() -> Program {
        Program {
            constants: vec![
                Constant::Number("1".to_string()),
                Constant::Number("2".to_string()),
                Constant::Number("3".to_string()),
                Constant::Number("4".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "odd".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 3,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::CallNative {
                            dest: 2,
                            native: NativeId(0),
                            arguments: vec![0],
                        },
                        Instruction::Constant {
                            dest: 1,
                            constant: 2,
                        },
                        Instruction::CallNative {
                            dest: 2,
                            native: NativeId(0),
                            arguments: vec![1],
                        },
                        Instruction::Return { value: 1 },
                    ],
                    locations: vec![None; 5],
                },
                Function {
                    id: FuncId(2),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "even".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 3,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 1,
                        },
                        Instruction::CallNative {
                            dest: 2,
                            native: NativeId(0),
                            arguments: vec![0],
                        },
                        Instruction::Constant {
                            dest: 1,
                            constant: 3,
                        },
                        Instruction::CallNative {
                            dest: 2,
                            native: NativeId(0),
                            arguments: vec![1],
                        },
                        Instruction::Return { value: 1 },
                    ],
                    locations: vec![None; 5],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn cooperative_native_print_program() -> Program {
        Program {
            constants: vec![
                Constant::Number("1".to_string()),
                Constant::Number("2".to_string()),
                Constant::Number("3".to_string()),
                Constant::Number("4".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "odd".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 2,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::CallNative {
                            dest: 1,
                            native: NativeId(0),
                            arguments: vec![0],
                        },
                        Instruction::Constant {
                            dest: 0,
                            constant: 2,
                        },
                        Instruction::CallNative {
                            dest: 1,
                            native: NativeId(0),
                            arguments: vec![0],
                        },
                        Instruction::Return { value: 1 },
                    ],
                    locations: vec![None; 5],
                },
                Function {
                    id: FuncId(2),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "even".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 2,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 1,
                        },
                        Instruction::CallNative {
                            dest: 1,
                            native: NativeId(0),
                            arguments: vec![0],
                        },
                        Instruction::Constant {
                            dest: 0,
                            constant: 3,
                        },
                        Instruction::CallNative {
                            dest: 1,
                            native: NativeId(0),
                            arguments: vec![0],
                        },
                        Instruction::Return { value: 1 },
                    ],
                    locations: vec![None; 5],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    #[test]
    fn cooperative_adapter_preserves_output_across_quantums() {
        let program = cooperative_print_program();
        let expected = VM::new(&program)
            .run()
            .expect("single-task print should succeed");

        for quantum in [1, 2, 8] {
            let actual = VM::new(&program)
                .run_cooperative(quantum)
                .expect("cooperative print should succeed");
            assert_eq!(actual, expected, "quantum {} changed output", quantum);
        }
    }

    #[test]
    fn cooperative_adapter_transfers_call_results_to_the_caller() {
        let program = cooperative_call_program();
        let expected = VM::new(&program)
            .run()
            .expect("single-task call should succeed");
        let actual = VM::new(&program)
            .run_cooperative(1)
            .expect("cooperative call should succeed");

        assert_eq!(actual, expected);
        assert_eq!(actual, "42\n");
    }

    #[test]
    fn cooperative_adapter_preserves_native_callback_behavior() {
        let program = cooperative_native_callback_program();
        let expected = VM::new(&program)
            .run()
            .expect("single-task native callback should succeed");
        let actual = VM::new(&program)
            .run_cooperative(1)
            .expect("cooperative native callback should succeed");

        assert_eq!(actual, expected);
        assert_eq!(actual, "[1, 2]\n");
    }

    #[test]
    fn cooperative_adapter_counts_native_callbacks_in_task_call_depth() {
        let program = cooperative_nested_native_callback_program();
        let mut config = RunConfig::unlimited();
        config.max_call_depth = Some(1);

        let expected = VM::with_config(&program, config.clone())
            .run()
            .expect_err("nested native callback should exceed the depth budget");
        let actual = VM::with_config(&program, config)
            .run_cooperative(1)
            .expect_err("cooperative nested callback should exceed the depth budget");

        assert_eq!(actual.kind, expected.kind);
        assert_eq!(actual.resource_limit, expected.resource_limit);
        assert_eq!(actual.message, expected.message);
        assert_eq!(actual.stack, expected.stack);
    }

    #[test]
    fn cooperative_adapter_preserves_instruction_budget_errors() {
        let program = cooperative_loop_program();
        let mut config = RunConfig::unlimited();
        config.max_instruction_steps = Some(3);

        let expected = VM::with_config(&program, config.clone())
            .run()
            .expect_err("single-task loop should exhaust its budget");
        let actual = VM::with_config(&program, config)
            .run_cooperative(2)
            .expect_err("cooperative loop should exhaust its budget");

        assert_eq!(actual.kind, expected.kind);
        assert_eq!(actual.resource_limit, expected.resource_limit);
        assert_eq!(actual.message, expected.message);
    }

    #[test]
    fn cooperative_adapter_preserves_per_task_call_depth_errors() {
        let program = recursive_closure_program(2);
        let mut config = RunConfig::unlimited();
        config.max_call_depth = Some(1);

        let expected = VM::with_config(&program, config.clone())
            .run()
            .expect_err("single-task recursion should exceed its depth budget");
        let actual = VM::with_config(&program, config)
            .run_cooperative(1)
            .expect_err("cooperative recursion should exceed its depth budget");

        assert_eq!(actual.kind, expected.kind);
        assert_eq!(actual.resource_limit, expected.resource_limit);
        assert_eq!(actual.message, expected.message);
        assert_eq!(actual.stack, expected.stack);
    }

    #[test]
    fn cooperative_adapter_checks_cancellation_before_an_empty_task() {
        let token = CancellationToken::new();
        token.cancel();
        let program = empty_program();
        let error = VM::with_config(&program, RunConfig::unlimited().with_cancellation(token))
            .run_cooperative(1)
            .expect_err("pre-cancelled cooperative task should not start");

        assert_eq!(error.kind, RuntimeErrorKind::Cancelled);
        assert_eq!(error.message, "execution cancelled");
    }

    #[test]
    fn cooperative_adapter_releases_terminated_task_roots_before_gc() {
        let program = cooperative_cycle_program();
        let vm = VM::new(&program);
        let stats = vm.heap_stats();

        vm.run_cooperative(1)
            .expect("cyclic task result should complete");

        assert_eq!(stats.snapshot().total_live, 0);
    }

    #[test]
    fn cooperative_adapter_preserves_nested_runtime_error_stack() {
        let program = debug_failure_program();
        let expected = VM::new(&program)
            .run()
            .expect_err("single-task failure should be reported");
        let actual = VM::new(&program)
            .run_cooperative(1)
            .expect_err("cooperative failure should be reported");

        assert_eq!(actual.message, expected.message);
        assert_eq!(actual.location, expected.location);
        assert_eq!(actual.stack, expected.stack);
        assert_eq!(actual.to_string(), expected.to_string());
    }

    #[test]
    fn cooperative_host_exposes_typed_multi_task_outcomes() {
        let program = cooperative_host_task_program();
        let mut run = VM::new(&program)
            .start_cooperative(1)
            .expect("positive quantum should start a session");
        let first = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect("first task should spawn");
        let second = run
            .spawn(TaskSpec::function(2, Vec::new()))
            .expect("second task should spawn");

        assert_eq!(
            run.step().expect("first dispatch should succeed"),
            CooperativeStep::Dispatched {
                task_id: first,
                state: TaskState::Ready,
            }
        );
        run.run_until_waiting()
            .expect("remaining tasks should complete");
        assert!(run.is_complete());

        let outcomes = run
            .outcomes()
            .expect("terminal outcomes should be readable");
        assert_eq!(outcomes.len(), 2);
        assert_eq!(outcomes[0].0, first);
        assert_eq!(outcomes[1].0, second);
        assert!(matches!(
            &outcomes[0].1,
            TaskOutcome::Completed(Value::Number(value)) if *value == 7.0
        ));
        assert!(matches!(
            &outcomes[1].1,
            TaskOutcome::Completed(Value::Number(value)) if *value == 8.0
        ));
    }

    #[test]
    fn cooperative_host_attributes_output_in_dispatch_order() {
        let program = cooperative_output_program();
        let mut run = VM::new(&program)
            .start_cooperative(1)
            .expect("positive quantum should start a session");
        let odd = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect("odd task should spawn");
        let even = run
            .spawn(TaskSpec::function(2, Vec::new()))
            .expect("even task should spawn");

        assert_eq!(
            run.run_until_waiting()
                .expect("output tasks should complete"),
            CooperativeStep::Complete
        );
        assert_eq!(run.take_output(), "1\n2\n3\n4\n");
        assert_eq!(
            run.output_events(),
            [
                TaskOutputEvent {
                    sequence: 0,
                    task_id: odd,
                    text: "1\n".to_string(),
                },
                TaskOutputEvent {
                    sequence: 1,
                    task_id: even,
                    text: "2\n".to_string(),
                },
                TaskOutputEvent {
                    sequence: 2,
                    task_id: odd,
                    text: "3\n".to_string(),
                },
                TaskOutputEvent {
                    sequence: 3,
                    task_id: even,
                    text: "4\n".to_string(),
                },
            ]
        );

        let drained = run.take_output_events();
        assert_eq!(drained.len(), 4);
        assert!(run.output_events().is_empty());
        assert!(run.trace_events().is_empty());
    }

    #[test]
    fn cooperative_host_attributes_native_print_in_dispatch_order() {
        let program = cooperative_native_print_program();
        let mut run = VM::new(&program)
            .start_cooperative(1)
            .expect("positive quantum should start a session");
        let odd = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect("odd task should spawn");
        let even = run
            .spawn(TaskSpec::function(2, Vec::new()))
            .expect("even task should spawn");

        assert_eq!(
            run.run_until_waiting()
                .expect("native print tasks should complete"),
            CooperativeStep::Complete
        );
        assert_eq!(run.take_output(), "1\n2\n3\n4\n");
        assert_eq!(
            run.output_events(),
            [
                TaskOutputEvent {
                    sequence: 0,
                    task_id: odd,
                    text: "1\n".to_string(),
                },
                TaskOutputEvent {
                    sequence: 1,
                    task_id: even,
                    text: "2\n".to_string(),
                },
                TaskOutputEvent {
                    sequence: 2,
                    task_id: odd,
                    text: "3\n".to_string(),
                },
                TaskOutputEvent {
                    sequence: 3,
                    task_id: even,
                    text: "4\n".to_string(),
                },
            ]
        );
    }

    #[test]
    fn cooperative_output_budget_is_cumulative_across_host_drains() {
        let program = cooperative_output_program();
        let mut config = RunConfig::unlimited();
        config.max_output_bytes = Some(2);
        let mut run = VM::with_config(&program, config)
            .start_cooperative(1)
            .expect("positive quantum should start a session");
        let odd = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect("odd task should spawn");
        let even = run
            .spawn(TaskSpec::function(2, Vec::new()))
            .expect("even task should spawn");

        assert!(matches!(
            run.step().expect("odd constant should execute"),
            CooperativeStep::Dispatched { task_id, .. } if task_id == odd
        ));
        assert!(matches!(
            run.step().expect("even constant should execute"),
            CooperativeStep::Dispatched { task_id, .. } if task_id == even
        ));
        assert!(matches!(
            run.step().expect("odd output should execute"),
            CooperativeStep::Dispatched { task_id, .. } if task_id == odd
        ));
        assert_eq!(run.take_output(), "1\n");

        assert_eq!(
            run.step()
                .expect("even output should hit the shared budget"),
            CooperativeStep::Dispatched {
                task_id: even,
                state: TaskState::Failed,
            }
        );
        assert!(matches!(
            run.task_outcome(even)
                .expect("failed task outcome should be readable"),
            Some(TaskOutcome::Failed(error))
                if error.kind == RuntimeErrorKind::Resource(ResourceKind::OutputBytes)
                    && error.resource_limit == Some(2)
        ));
        assert_eq!(run.output_events().len(), 1);
        assert_eq!(run.output_events()[0].task_id, odd);
        assert!(run.take_output().is_empty());
    }

    #[test]
    fn cooperative_host_attributes_synchronous_native_callback_output() {
        let mut program = cooperative_native_callback_program();
        program.functions[1].registers = 2;
        program.functions[1].instructions = vec![
            Instruction::LoadLocal { dest: 0, slot: 0 },
            Instruction::CallNative {
                dest: 1,
                native: NativeId(1),
                arguments: vec![0],
            },
            Instruction::Return { value: 0 },
        ];
        program.functions[1].locations = vec![None; 3];
        let mut run = VM::new(&program)
            .start_cooperative(8)
            .expect("positive quantum should start a session");
        let task = run
            .spawn(TaskSpec::main())
            .expect("callback task should spawn");

        run.run_until_waiting()
            .expect("native callback task should complete");
        assert_eq!(run.take_output(), "1\n2\n[1, 2]\n");
        assert_eq!(
            run.output_events()
                .iter()
                .map(|event| (event.task_id, event.text.as_str()))
                .collect::<Vec<_>>(),
            vec![(task, "1\n"), (task, "2\n"), (task, "[1, 2]\n")]
        );
        assert!(run.trace_events().is_empty());
    }

    #[test]
    fn cooperative_trace_attributes_events_and_shares_output_sequence() {
        let program = cooperative_output_program();
        let mut run = VM::new(&program)
            .start_cooperative_trace(1)
            .expect("positive quantum should start a traced session");
        let odd = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect("odd task should spawn");
        let even = run
            .spawn(TaskSpec::function(2, Vec::new()))
            .expect("even task should spawn");

        run.step().expect("odd task should enter");
        run.step().expect("even task should enter");
        let initial = run.take_trace_events();
        assert_eq!(
            initial
                .iter()
                .map(|event| (event.sequence, event.task_id, event.kind))
                .collect::<Vec<_>>(),
            vec![
                (0, odd, TraceEventKind::Enter),
                (1, even, TraceEventKind::Enter),
            ]
        );
        assert_eq!(initial[0].stack[0].function, "odd");
        assert_eq!(initial[1].stack[0].function, "even");

        assert_eq!(
            run.run_until_waiting()
                .expect("traced tasks should complete"),
            CooperativeStep::Complete
        );
        assert_eq!(
            run.trace_events()
                .iter()
                .map(|event| (event.sequence, event.task_id, event.kind))
                .collect::<Vec<_>>(),
            vec![
                (2, odd, TraceEventKind::Output),
                (3, even, TraceEventKind::Output),
                (4, odd, TraceEventKind::Output),
                (5, even, TraceEventKind::Output),
                (6, odd, TraceEventKind::Return),
                (7, odd, TraceEventKind::Exit),
                (8, even, TraceEventKind::Return),
                (9, even, TraceEventKind::Exit),
            ]
        );
        assert_eq!(
            run.output_events()
                .iter()
                .map(|event| (event.sequence, event.task_id))
                .collect::<Vec<_>>(),
            vec![(2, odd), (3, even), (4, odd), (5, even)]
        );
        assert!(run.trace_events().iter().all(|event| {
            event
                .stack
                .last()
                .is_some_and(|frame| frame.function == event.function)
        }));
    }

    #[test]
    fn cooperative_trace_emits_task_attributed_line_locations() {
        let mut program = cooperative_output_program();
        program.debug_sources.push(DebugSource {
            module: None,
            path: "trace-task.cd".to_string(),
            text: "one\ntwo\nthree\nfour\nfive\n".to_string(),
        });
        program.functions[1].locations = (1..=5)
            .map(|line| {
                Some(DebugLocation {
                    source: 0,
                    line,
                    column: 1,
                    range: None,
                })
            })
            .collect();
        let mut run = VM::new(&program)
            .start_cooperative_trace(8)
            .expect("positive quantum should start a traced session");
        let task = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect("traced task should spawn");

        run.run_until_waiting()
            .expect("traced task should complete");
        let lines = run
            .trace_events()
            .iter()
            .filter(|event| event.kind == TraceEventKind::Line)
            .collect::<Vec<_>>();
        assert_eq!(lines.len(), 4);
        assert!(lines.iter().all(|event| event.task_id == task));
        assert_eq!(
            lines
                .iter()
                .map(|event| event.location.as_ref().map(|location| location.line))
                .collect::<Vec<_>>(),
            vec![Some(2), Some(3), Some(4), Some(5)]
        );
    }

    #[test]
    fn cooperative_trace_preserves_task_local_nested_call_stack() {
        let program = cooperative_call_program();
        let mut run = VM::new(&program)
            .start_cooperative_trace(1)
            .expect("positive quantum should start a traced session");
        let task = run.spawn(TaskSpec::main()).expect("main task should spawn");

        run.run_until_waiting()
            .expect("nested traced task should complete");
        let enter = run
            .trace_events()
            .iter()
            .find(|event| {
                event.task_id == task
                    && event.kind == TraceEventKind::Enter
                    && event.function == "answer"
            })
            .expect("callee should emit an enter event");
        assert_eq!(
            enter
                .stack
                .iter()
                .map(|frame| frame.function.as_str())
                .collect::<Vec<_>>(),
            vec!["main", "answer"]
        );
        assert!(run.trace_events().iter().any(|event| {
            event.task_id == task
                && event.kind == TraceEventKind::Exit
                && event.function == "answer"
        }));
        assert!(run.trace_events().iter().any(|event| {
            event.task_id == task && event.kind == TraceEventKind::Exit && event.function == "main"
        }));
    }

    #[test]
    fn cooperative_trace_includes_synchronous_native_callback_frames_and_output() {
        let mut program = cooperative_native_callback_program();
        program.functions[1].registers = 2;
        program.functions[1].instructions = vec![
            Instruction::LoadLocal { dest: 0, slot: 0 },
            Instruction::CallNative {
                dest: 1,
                native: NativeId(1),
                arguments: vec![0],
            },
            Instruction::Return { value: 0 },
        ];
        program.functions[1].locations = vec![None; 3];
        let mut run = VM::new(&program)
            .start_cooperative_trace(8)
            .expect("positive quantum should start a traced session");
        let task = run
            .spawn(TaskSpec::main())
            .expect("callback task should spawn");

        run.run_until_waiting()
            .expect("native callback task should complete");
        assert_eq!(run.take_output(), "1\n2\n[1, 2]\n");
        let callback_enters = run
            .trace_events()
            .iter()
            .filter(|event| {
                event.task_id == task
                    && event.kind == TraceEventKind::Enter
                    && event.function == "identity"
            })
            .collect::<Vec<_>>();
        assert_eq!(callback_enters.len(), 2);
        assert!(callback_enters.iter().all(|event| {
            event
                .stack
                .iter()
                .map(|frame| frame.function.as_str())
                .collect::<Vec<_>>()
                == vec!["main", "identity"]
        }));
        assert_eq!(
            run.output_events()
                .iter()
                .map(|event| (event.task_id, event.text.as_str()))
                .collect::<Vec<_>>(),
            vec![(task, "1\n"), (task, "2\n"), (task, "[1, 2]\n")]
        );
        assert_eq!(
            run.trace_events()
                .iter()
                .filter(|event| event.kind == TraceEventKind::Output)
                .map(|event| event.sequence)
                .collect::<Vec<_>>(),
            run.output_events()
                .iter()
                .map(|event| event.sequence)
                .collect::<Vec<_>>()
        );
    }

    #[test]
    fn cooperative_trace_reports_the_failing_task_before_fail_fast_cancellation() {
        let program = cooperative_host_task_program();
        let mut run = VM::new(&program)
            .start_cooperative_trace(1)
            .expect("positive quantum should start a traced session");
        let failure = run
            .spawn(TaskSpec::function(3, Vec::new()))
            .expect("failure task should spawn");
        let pending = run
            .spawn(TaskSpec::function(4, Vec::new()))
            .expect("pending task should spawn");

        run.run_until_waiting()
            .expect("traced failure should reach a terminal session");
        assert!(matches!(
            run.task_outcome(failure)
                .expect("failure outcome should be readable"),
            Some(TaskOutcome::Failed(error)) if error.message == "division by zero"
        ));
        assert!(matches!(
            run.task_outcome(pending)
                .expect("cancelled outcome should be readable"),
            Some(TaskOutcome::Cancelled)
        ));

        let error = run
            .trace_events()
            .iter()
            .find(|event| event.kind == TraceEventKind::Error)
            .expect("failing task should emit an error event");
        assert_eq!(error.task_id, failure);
        assert_eq!(error.function, "failure");
        assert_eq!(error.value.as_deref(), Some("division by zero"));
        assert!(run
            .trace_events()
            .iter()
            .any(|event| { event.task_id == failure && event.kind == TraceEventKind::Exit }));
        assert!(!run
            .trace_events()
            .iter()
            .any(|event| { event.task_id == pending && event.kind == TraceEventKind::Error }));
    }

    #[test]
    fn cooperative_profile_attributes_interleaved_counters_and_ranges() {
        let mut program = cooperative_output_program();
        program.debug_sources.push(DebugSource {
            module: None,
            path: "profile-tasks.cd".to_string(),
            text: "odd\neven\n".to_string(),
        });
        let odd_range = DebugRange {
            source: 0,
            start: 0,
            end: 3,
        };
        let even_range = DebugRange {
            source: 0,
            start: 4,
            end: 8,
        };
        program.functions[1].locations = vec![
            Some(DebugLocation {
                source: 0,
                line: 1,
                column: 1,
                range: Some(odd_range.clone()),
            });
            5
        ];
        program.functions[2].locations = vec![
            Some(DebugLocation {
                source: 0,
                line: 2,
                column: 1,
                range: Some(even_range.clone()),
            });
            5
        ];

        let ordinary = VM::new(&program)
            .start_cooperative(1)
            .expect("ordinary session should start");
        assert!(ordinary.profile_report().is_none());

        let mut run = VM::new(&program)
            .start_cooperative_profile(1)
            .expect("profiled session should start");
        let odd = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect("odd task should spawn");
        let even = run
            .spawn(TaskSpec::function(2, Vec::new()))
            .expect("even task should spawn");
        let initial = run.profile_report().expect("profile should be enabled");
        assert_eq!(initial.aggregate.instruction_count, 0);
        assert!(initial.tasks.iter().all(|task| task.instruction_count == 0));

        assert_eq!(
            run.run_until_waiting()
                .expect("profiled tasks should complete"),
            CooperativeStep::Complete
        );
        let report = run.profile_report().expect("profile should be retained");
        assert_eq!(report.aggregate.instruction_count, 10);
        assert_eq!(report.aggregate.output_bytes, 8);
        assert_eq!(report.aggregate.functions[1].calls, 1);
        assert_eq!(report.aggregate.functions[1].instructions, 5);
        assert_eq!(report.aggregate.functions[2].calls, 1);
        assert_eq!(report.aggregate.functions[2].instructions, 5);
        assert!(report.aggregate.tracked_heap_allocations > 0);
        assert_eq!(report.tasks.len(), 2);
        assert_eq!(report.tasks[0].task_id, odd);
        assert_eq!(report.tasks[0].instruction_count, 5);
        assert_eq!(report.tasks[0].output_bytes, 4);
        assert_eq!(report.tasks[0].functions[1].calls, 1);
        assert_eq!(report.tasks[0].functions[1].instructions, 5);
        assert_eq!(
            report.tasks[0].source_ranges,
            vec![ProfileSourceRange {
                range: odd_range,
                hits: 5,
            }]
        );
        assert_eq!(report.tasks[1].task_id, even);
        assert_eq!(report.tasks[1].instruction_count, 5);
        assert_eq!(report.tasks[1].output_bytes, 4);
        assert_eq!(report.tasks[1].functions[2].calls, 1);
        assert_eq!(report.tasks[1].functions[2].instructions, 5);
        assert_eq!(
            report.tasks[1].source_ranges,
            vec![ProfileSourceRange {
                range: even_range,
                hits: 5,
            }]
        );
    }

    #[test]
    fn cooperative_profile_attributes_native_callbacks_to_the_current_task() {
        let mut program = cooperative_native_callback_program();
        program.functions[1].registers = 2;
        program.functions[1].instructions = vec![
            Instruction::LoadLocal { dest: 0, slot: 0 },
            Instruction::CallNative {
                dest: 1,
                native: NativeId(1),
                arguments: vec![0],
            },
            Instruction::Return { value: 0 },
        ];
        program.functions[1].locations = vec![None; 3];
        let mut run = VM::new(&program)
            .start_cooperative_profile(8)
            .expect("profiled session should start");
        let task = run
            .spawn(TaskSpec::main())
            .expect("callback task should spawn");

        run.run_until_waiting()
            .expect("callback task should complete");
        let report = run.profile_report().expect("profile should be retained");
        assert_eq!(report.aggregate.instruction_count, 13);
        assert_eq!(report.aggregate.output_bytes, 11);
        assert_eq!(report.aggregate.functions[0].calls, 1);
        assert_eq!(report.aggregate.functions[0].instructions, 7);
        assert_eq!(report.aggregate.functions[1].calls, 2);
        assert_eq!(report.aggregate.functions[1].instructions, 6);
        assert_eq!(
            report.aggregate.natives,
            vec![ProfileNative {
                name: "map".to_string(),
                calls: 1,
            }]
        );
        assert_eq!(report.tasks.len(), 1);
        assert_eq!(report.tasks[0].task_id, task);
        assert_eq!(report.tasks[0].instruction_count, 13);
        assert_eq!(report.tasks[0].output_bytes, 11);
        assert_eq!(report.tasks[0].functions, report.aggregate.functions);
        assert_eq!(report.tasks[0].natives, report.aggregate.natives);
    }

    #[test]
    fn cooperative_profile_keeps_partial_failure_and_zero_cancelled_task() {
        let program = cooperative_host_task_program();
        let mut run = VM::new(&program)
            .start_cooperative_profile(8)
            .expect("profiled session should start");
        let failure = run
            .spawn(TaskSpec::function(3, Vec::new()))
            .expect("failure task should spawn");
        let pending = run
            .spawn(TaskSpec::function(4, Vec::new()))
            .expect("pending task should spawn");

        run.run_until_waiting()
            .expect("fail-fast session should terminate");
        let report = run.profile_report().expect("profile should be retained");
        assert_eq!(report.aggregate.instruction_count, 3);
        assert_eq!(report.tasks[0].task_id, failure);
        assert_eq!(report.tasks[0].instruction_count, 3);
        assert_eq!(report.tasks[0].functions[3].calls, 1);
        assert_eq!(report.tasks[0].functions[3].instructions, 3);
        assert_eq!(report.tasks[1].task_id, pending);
        assert_eq!(report.tasks[1].instruction_count, 0);
        assert_eq!(report.tasks[1].functions[4].calls, 0);
        assert_eq!(report.tasks[1].functions[4].instructions, 0);
    }

    struct RecordingCooperativeDebugger {
        pauses: Rc<RefCell<Vec<CooperativeDebugPause>>>,
        errors: Rc<RefCell<Vec<(CooperativeDebugPause, RuntimeError)>>>,
        quit_on_first_instruction: bool,
    }

    impl CooperativeDebugHook for RecordingCooperativeDebugger {
        fn on_instruction(&mut self, pause: CooperativeDebugPause) -> DebugControl {
            self.pauses.borrow_mut().push(pause);
            if self.quit_on_first_instruction {
                DebugControl::Quit
            } else {
                DebugControl::Continue
            }
        }

        fn on_error(&mut self, pause: CooperativeDebugPause, error: &RuntimeError) -> DebugControl {
            self.errors.borrow_mut().push((pause, error.clone()));
            DebugControl::Continue
        }
    }

    fn recording_cooperative_debugger(
        quit_on_first_instruction: bool,
    ) -> (
        Box<RecordingCooperativeDebugger>,
        Rc<RefCell<Vec<CooperativeDebugPause>>>,
        Rc<RefCell<Vec<(CooperativeDebugPause, RuntimeError)>>>,
    ) {
        let pauses = Rc::new(RefCell::new(Vec::new()));
        let errors = Rc::new(RefCell::new(Vec::new()));
        (
            Box::new(RecordingCooperativeDebugger {
                pauses: Rc::clone(&pauses),
                errors: Rc::clone(&errors),
                quit_on_first_instruction,
            }),
            pauses,
            errors,
        )
    }

    #[test]
    fn cooperative_debugger_attributes_pauses_and_fifo_scheduler_state() {
        let program = cooperative_output_program();
        let (hook, pauses, errors) = recording_cooperative_debugger(false);
        let mut run = VM::new(&program)
            .start_cooperative_debug(1, hook)
            .expect("debug session should start");
        let odd = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect("odd task should spawn");
        let even = run
            .spawn(TaskSpec::function(2, Vec::new()))
            .expect("even task should spawn");

        assert_eq!(
            run.run_until_waiting()
                .expect("debugged tasks should complete"),
            CooperativeStep::Complete
        );
        assert!(!run.debug_quit());
        assert!(errors.borrow().is_empty());
        assert!(run.trace_events().is_empty());
        assert_eq!(
            run.output_events()
                .iter()
                .map(|event| event.sequence)
                .collect::<Vec<_>>(),
            vec![0, 1, 2, 3]
        );
        let pauses = pauses.borrow();
        assert_eq!(pauses.len(), 10);
        assert_eq!(pauses[0].task_id, odd);
        assert_eq!(pauses[0].instruction, 0);
        assert_eq!(pauses[0].scheduler.running, odd);
        assert_eq!(pauses[0].scheduler.ready, vec![even]);
        assert_eq!(
            pauses[0].scheduler.tasks,
            vec![(odd, TaskState::Running), (even, TaskState::Ready)]
        );
        assert_eq!(pauses[1].task_id, even);
        assert_eq!(pauses[1].scheduler.running, even);
        assert_eq!(pauses[1].scheduler.ready, vec![odd]);
        assert_eq!(
            pauses.iter().map(|pause| pause.task_id).collect::<Vec<_>>(),
            vec![odd, even, odd, even, odd, even, odd, even, odd, even]
        );
    }

    #[test]
    fn cooperative_debugger_preserves_nested_and_native_callback_stacks() {
        let call_program = cooperative_call_program();
        let (hook, pauses, _) = recording_cooperative_debugger(false);
        let mut call_run = VM::new(&call_program)
            .start_cooperative_debug(8, hook)
            .expect("call debug session should start");
        let call_task = call_run
            .spawn(TaskSpec::main())
            .expect("call task should spawn");
        call_run
            .run_until_waiting()
            .expect("call task should complete");
        let nested = pauses
            .borrow()
            .iter()
            .find(|pause| pause.function == "answer" && pause.instruction == 0)
            .cloned()
            .expect("callee pause should be recorded");
        assert_eq!(nested.task_id, call_task);
        assert_eq!(
            nested
                .stack
                .iter()
                .map(|frame| frame.function.as_str())
                .collect::<Vec<_>>(),
            vec!["main", "answer"]
        );

        let callback_program = cooperative_native_callback_program();
        let (hook, pauses, _) = recording_cooperative_debugger(false);
        let mut callback_run = VM::new(&callback_program)
            .start_cooperative_debug(8, hook)
            .expect("callback debug session should start");
        let callback_task = callback_run
            .spawn(TaskSpec::main())
            .expect("callback task should spawn");
        callback_run
            .run_until_waiting()
            .expect("callback task should complete");
        let callback_pauses = pauses
            .borrow()
            .iter()
            .filter(|pause| pause.function == "identity")
            .cloned()
            .collect::<Vec<_>>();
        assert_eq!(callback_pauses.len(), 4);
        assert!(callback_pauses.iter().all(|pause| {
            pause.task_id == callback_task
                && pause
                    .stack
                    .iter()
                    .map(|frame| frame.function.as_str())
                    .collect::<Vec<_>>()
                    == vec!["main", "identity"]
        }));
    }

    #[test]
    fn cooperative_debugger_quit_cancels_the_whole_session_before_execution() {
        let program = cooperative_output_program();
        let (hook, pauses, errors) = recording_cooperative_debugger(true);
        let mut run = VM::new(&program)
            .start_cooperative_debug(8, hook)
            .expect("debug session should start");
        let odd = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect("odd task should spawn");
        let even = run
            .spawn(TaskSpec::function(2, Vec::new()))
            .expect("even task should spawn");

        assert_eq!(
            run.step().expect("debug quit should terminate the session"),
            CooperativeStep::Complete
        );
        assert!(run.debug_quit());
        assert!(run.is_complete());
        assert_eq!(pauses.borrow().len(), 1);
        assert!(errors.borrow().is_empty());
        assert!(run.take_output().is_empty());
        assert!(run.output_events().is_empty());
        assert!(matches!(
            run.task_outcome(odd).expect("odd outcome should exist"),
            Some(TaskOutcome::Cancelled)
        ));
        assert!(matches!(
            run.task_outcome(even).expect("even outcome should exist"),
            Some(TaskOutcome::Cancelled)
        ));
    }

    #[test]
    fn cooperative_debugger_attributes_runtime_error_before_fail_fast() {
        let program = cooperative_host_task_program();
        let (hook, _, errors) = recording_cooperative_debugger(false);
        let mut run = VM::new(&program)
            .start_cooperative_debug(8, hook)
            .expect("debug session should start");
        let failure = run
            .spawn(TaskSpec::function(3, Vec::new()))
            .expect("failure task should spawn");
        let pending = run
            .spawn(TaskSpec::function(4, Vec::new()))
            .expect("pending task should spawn");

        run.run_until_waiting()
            .expect("debugged failure should terminate");
        let errors = errors.borrow();
        assert_eq!(errors.len(), 1);
        assert_eq!(errors[0].0.task_id, failure);
        assert_eq!(errors[0].0.function, "failure");
        assert_eq!(errors[0].0.scheduler.running, failure);
        assert_eq!(errors[0].0.scheduler.ready, vec![pending]);
        assert_eq!(errors[0].1.message, "division by zero");
        assert!(matches!(
            run.task_outcome(failure).expect("failure outcome should exist"),
            Some(TaskOutcome::Failed(error)) if error.message == "division by zero"
        ));
        assert!(matches!(
            run.task_outcome(pending)
                .expect("pending outcome should exist"),
            Some(TaskOutcome::Cancelled)
        ));
    }

    #[test]
    fn cooperative_host_join_blocks_and_returns_target_outcome_after_wake() {
        let program = cooperative_host_task_program();
        let mut run = VM::new(&program)
            .start_cooperative(1)
            .expect("positive quantum should start a session");
        let waiter = run
            .spawn(TaskSpec::function(2, Vec::new()))
            .expect("waiter should spawn");
        let target = run
            .spawn(TaskSpec::function(1, Vec::new()))
            .expect("target should spawn");

        assert!(matches!(
            run.join(waiter, target).expect("join should register"),
            JoinPoll::Waiting
        ));
        assert_eq!(
            run.task_state(waiter).expect("waiter should exist"),
            TaskState::Blocked
        );
        assert!(matches!(
            run.step().expect("target should make progress"),
            CooperativeStep::Dispatched { task_id, .. } if task_id == target
        ));
        assert!(matches!(
            run.step().expect("target should complete"),
            CooperativeStep::Dispatched {
                task_id,
                state: TaskState::Completed,
            } if task_id == target
        ));
        assert_eq!(
            run.task_state(waiter).expect("joined waiter should wake"),
            TaskState::Ready
        );
        assert!(matches!(
            run.join(waiter, target).expect("completed join should be ready"),
            JoinPoll::Ready(TaskOutcome::Completed(Value::Number(value))) if value == 7.0
        ));

        run.wake(waiter)
            .expect_err("a ready waiter must not be woken twice");
        run.run_until_waiting().expect("woken waiter should finish");
        assert!(run.is_complete());
    }

    #[test]
    fn cooperative_host_failure_is_typed_and_fail_fast() {
        let program = cooperative_host_task_program();
        let mut run = VM::new(&program)
            .start_cooperative(1)
            .expect("positive quantum should start a session");
        let failure = run
            .spawn(TaskSpec::function(3, Vec::new()))
            .expect("failure task should spawn");
        let pending = run
            .spawn(TaskSpec::function(4, Vec::new()))
            .expect("pending task should spawn");

        run.run_until_waiting()
            .expect("fail-fast session should reach completion");
        assert!(run.is_complete());
        assert!(matches!(
            run.task_outcome(failure).expect("failure outcome should exist"),
            Some(TaskOutcome::Failed(error)) if error.message == "division by zero"
        ));
        assert!(matches!(
            run.task_outcome(pending)
                .expect("cancelled outcome should exist"),
            Some(TaskOutcome::Cancelled)
        ));
    }

    fn array_elements(value: &Value) -> Vec<Value> {
        let Value::Array(array) = value else {
            panic!("expected array");
        };
        array.elements.borrow().clone()
    }

    fn debug_failure_program() -> Program {
        let source = DebugSource {
            module: None,
            path: "demo.cd".to_string(),
            text: "fun fail() { return 1 / 0; }\nfail();\n".to_string(),
        };
        Program {
            constants: vec![
                Constant::Number("1".to_string()),
                Constant::Number("0".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 2,
                    instructions: vec![
                        Instruction::MakeFunction {
                            dest: 0,
                            function: FuncId(1),
                        },
                        Instruction::Call {
                            dest: 1,
                            callee: 0,
                            arguments: Vec::new(),
                        },
                    ],
                    locations: vec![
                        Some(DebugLocation {
                            source: 0,
                            line: 2,
                            column: 1,
                            range: None,
                        }),
                        Some(DebugLocation {
                            source: 0,
                            line: 2,
                            column: 1,
                            range: None,
                        }),
                    ],
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "fail".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 4,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::Constant {
                            dest: 1,
                            constant: 1,
                        },
                        Instruction::Divide {
                            dest: 2,
                            left: 0,
                            right: 1,
                        },
                        Instruction::Return { value: 2 },
                    ],
                    locations: vec![
                        Some(DebugLocation {
                            source: 0,
                            line: 1,
                            column: 21,
                            range: None,
                        }),
                        Some(DebugLocation {
                            source: 0,
                            line: 1,
                            column: 25,
                            range: None,
                        }),
                        Some(DebugLocation {
                            source: 0,
                            line: 1,
                            column: 21,
                            range: None,
                        }),
                        Some(DebugLocation {
                            source: 0,
                            line: 1,
                            column: 14,
                            range: None,
                        }),
                    ],
                },
            ],
            entry: FuncId(0),
            debug_sources: vec![source],
        }
    }

    fn array_churn_program(iterations: usize) -> Program {
        Program {
            constants: vec![
                Constant::Number("0".to_string()),
                Constant::Number(iterations.to_string()),
                Constant::Number("1".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 5,
                instructions: vec![
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Constant {
                        dest: 1,
                        constant: 1,
                    },
                    Instruction::Constant {
                        dest: 2,
                        constant: 2,
                    },
                    Instruction::Br { target: BlockId(1) },
                    Instruction::BlockStart { id: BlockId(1) },
                    Instruction::Less {
                        dest: 3,
                        left: 0,
                        right: 1,
                    },
                    Instruction::BrIf {
                        condition: 3,
                        if_true: BlockId(2),
                        if_false: BlockId(3),
                    },
                    Instruction::BlockStart { id: BlockId(2) },
                    Instruction::Array {
                        dest: 4,
                        elements: vec![0],
                    },
                    Instruction::Add {
                        dest: 0,
                        left: 0,
                        right: 2,
                    },
                    Instruction::Br { target: BlockId(1) },
                    Instruction::BlockStart { id: BlockId(3) },
                    Instruction::Return { value: 0 },
                ],
                locations: vec![None; 14],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    fn cycle_until_pause_program() -> Program {
        Program {
            constants: Vec::new(),
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "push".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: vec!["push".to_string()],
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 2,
                instructions: vec![
                    Instruction::Array {
                        dest: 0,
                        elements: Vec::new(),
                    },
                    Instruction::CallNative {
                        dest: 1,
                        native: NativeId(0),
                        arguments: vec![0, 0],
                    },
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::Br { target: BlockId(0) },
                ],
                locations: vec![None; 4],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    struct CancelAtInstruction {
        token: CancellationToken,
        instruction: usize,
    }

    impl DebugHook for CancelAtInstruction {
        fn on_instruction(&mut self, pause: DebugPause) -> DebugControl {
            if pause.instruction == self.instruction {
                self.token.cancel();
            }
            DebugControl::Continue
        }
    }

    struct QuitAtInstruction {
        instruction: usize,
    }

    impl DebugHook for QuitAtInstruction {
        fn on_instruction(&mut self, pause: DebugPause) -> DebugControl {
            if pause.instruction == self.instruction {
                DebugControl::Quit
            } else {
                DebugControl::Continue
            }
        }
    }

    fn recursive_closure_program(depth: usize) -> Program {
        Program {
            constants: vec![
                Constant::Number("0".to_string()),
                Constant::Number("1".to_string()),
                Constant::Number(depth.to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: vec!["n".to_string()],
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 3,
                    instructions: vec![
                        Instruction::MakeFunction {
                            dest: 0,
                            function: FuncId(1),
                        },
                        Instruction::Constant {
                            dest: 1,
                            constant: 2,
                        },
                        Instruction::Call {
                            dest: 2,
                            callee: 0,
                            arguments: vec![1],
                        },
                    ],
                    locations: vec![None; 3],
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "recurse".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 7,
                    params: vec!["n".to_string()],
                    instructions: vec![
                        Instruction::BlockStart { id: BlockId(0) },
                        Instruction::LoadLocal { dest: 0, slot: 0 },
                        Instruction::Constant {
                            dest: 1,
                            constant: 0,
                        },
                        Instruction::LessEqual {
                            dest: 2,
                            left: 0,
                            right: 1,
                        },
                        Instruction::BrIf {
                            condition: 2,
                            if_true: BlockId(1),
                            if_false: BlockId(2),
                        },
                        Instruction::BlockStart { id: BlockId(1) },
                        Instruction::Return { value: 0 },
                        Instruction::BlockStart { id: BlockId(2) },
                        Instruction::MakeFunction {
                            dest: 3,
                            function: FuncId(1),
                        },
                        Instruction::Constant {
                            dest: 4,
                            constant: 1,
                        },
                        Instruction::Subtract {
                            dest: 5,
                            left: 0,
                            right: 4,
                        },
                        Instruction::Call {
                            dest: 6,
                            callee: 3,
                            arguments: vec![5],
                        },
                        Instruction::Return { value: 6 },
                    ],
                    locations: vec![None; 13],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    #[test]
    fn function_bodies_are_prepared_once_at_construction() {
        let program = recursive_closure_program(2);
        let mut vm = VM::new(&program);
        let first_body = vm.prepared_functions[1].clone();

        let invoke = |vm: &mut VM<'_>| {
            let function = FunctionValue {
                name: "recurse".to_string(),
                function_index: 1,
                arity: 1,
                identity: 0,
                closure: vm.heap.new_environment(),
                upvalues: Vec::new(),
            };
            vm.call_function(
                &function,
                CallArguments::One(Value::number(2.0)),
                false,
                "main",
                None,
            )
            .expect("recursive function call should complete")
        };

        let first_result = invoke(&mut vm);
        let second_result = invoke(&mut vm);
        let second_body = vm.prepared_functions[1].clone();

        assert_eq!(first_result.to_string(), "0");
        assert_eq!(second_result.to_string(), "0");
        assert!(Rc::ptr_eq(&first_body, &second_body));
    }

    #[test]
    fn jit_admission_state_is_vm_local_and_disabled_by_default() {
        let program = Program {
            constants: vec![Constant::Number("1".to_string())],
            names: Vec::new(),
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "eligible".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 1,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None; 2],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut first = VM::new(&program);
        let second = VM::new(&program);

        assert_eq!(
            first
                .jit
                .eligibility(&program, Some(2), crate::jit::JitExecutionMode::Ordinary),
            crate::jit::JitEligibility::Fallback(crate::jit::JitFallbackReason::Disabled)
        );
        assert_eq!(
            second
                .jit
                .eligibility(&program, Some(2), crate::jit::JitExecutionMode::Ordinary),
            crate::jit::JitEligibility::Fallback(crate::jit::JitFallbackReason::Disabled)
        );

        first.jit = JitState::enabled_for_tests([1], 1024);
        assert!(matches!(
            first.jit.admit(
                &program,
                Some(1),
                crate::jit::JitExecutionMode::Ordinary,
                1024,
            ),
            crate::jit::JitAdmission::Reserved {
                function_index: 1,
                bytes: 1024,
                ..
            }
        ));
        assert_eq!(
            second
                .jit
                .eligibility(&program, Some(2), crate::jit::JitExecutionMode::Ordinary),
            crate::jit::JitEligibility::Fallback(crate::jit::JitFallbackReason::Disabled)
        );
    }

    #[test]
    fn constant_values_are_decoded_eagerly_at_construction() {
        let program = Program {
            constants: vec![
                Constant::Number("1.5".to_string()),
                Constant::String("cached".to_string()),
            ],
            ..empty_program()
        };
        let vm = VM::new(&program);
        assert!(matches!(
            &vm.decoded_constants[0],
            Value::Number(value) if *value == 1.5
        ));
        assert!(matches!(
            &vm.decoded_constants[1],
            Value::String(value) if value.as_ref() == "cached"
        ));

        assert_eq!(vm.constant_value(0).unwrap().to_string(), "1.5");
        assert_eq!(vm.constant_value(1).unwrap().to_string(), "cached");

        let invalid = vm
            .constant_value(2)
            .expect_err("out-of-range constants should remain rejected");
        assert_eq!(invalid.message, "constant index out of range");
    }

    #[test]
    fn verified_constructor_rejects_malformed_programs_and_unverified_keeps_runtime_checks() {
        let mut program = empty_program();
        program.constants = vec![Constant::Nil];
        program.functions[0].registers = 1;
        program.functions[0].instructions = vec![Instruction::Constant {
            dest: 3,
            constant: 0,
        }];
        program.functions[0].locations = vec![None];

        let verified_error = match VM::with_config_verified(&program, RunConfig::unlimited()) {
            Ok(_) => panic!("out-of-range registers must fail construction-time verification"),
            Err(error) => error,
        };
        assert!(verified_error
            .message
            .contains("destination register r3 out of range"));

        let runtime_error = VM::new(&program)
            .run()
            .expect_err("unverified construction keeps the runtime bounds check");
        assert_eq!(runtime_error.message, "register index out of range");
    }

    #[test]
    fn profile_reports_tracked_array_allocations_and_peak() {
        let profiled = VM::with_config(&array_churn_program(4), RunConfig::unlimited()).profile();
        assert!(profiled.result.is_ok());
        assert_eq!(profiled.report.tracked_heap_allocations, 6);
        assert_eq!(profiled.report.tracked_heap_peak_live, 4);
    }

    #[test]
    fn native_collection_helpers_query_and_copy_shallowly() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let shared = vm.make_array(vec![Value::number(9.0)]);
        let source = vm.make_array(vec![Value::number(1.0), shared.clone(), Value::number(3.0)]);
        let distinct = vm.make_array(vec![Value::number(9.0)]);

        let contains_shared = vm
            .execute_native_call("contains", vec![source.clone(), shared.clone()])
            .expect("contains succeeds");
        let contains_distinct = vm
            .execute_native_call("contains", vec![source.clone(), distinct])
            .expect("contains succeeds");
        assert!(matches!(contains_shared, Value::Bool(true)));
        assert!(matches!(contains_distinct, Value::Bool(false)));

        let sliced = vm
            .execute_native_call(
                "slice",
                vec![source.clone(), Value::number(1.0), Value::number(2.0)],
            )
            .expect("slice succeeds");
        let copied = vm
            .execute_native_call("copy", vec![source.clone()])
            .expect("copy succeeds");
        let concatenated = vm
            .execute_native_call("concat", vec![sliced.clone(), copied.clone()])
            .expect("concat succeeds");

        assert_eq!(array_elements(&sliced).len(), 2);
        assert_eq!(array_elements(&copied).len(), 3);
        assert_eq!(array_elements(&concatenated).len(), 5);
        assert!(!source.runtime_equals(&copied));
        let source_elements = array_elements(&source);
        let copied_elements = array_elements(&copied);
        assert!(source_elements[1].runtime_equals(&copied_elements[1]));
    }

    #[test]
    fn heap_stats_observe_native_temporary_roots_and_release_them() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let stats = vm.heap_stats();
        let source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let temporary = vm
            .execute_native_call(
                "slice",
                vec![source.clone(), Value::number(0.0), Value::number(1.0)],
            )
            .expect("native temporary allocation should succeed");

        let in_flight = stats.snapshot();
        assert_eq!(in_flight.for_kind(HeapObjectKind::Array).allocations, 2);
        assert_eq!(in_flight.for_kind(HeapObjectKind::Array).live, 2);
        assert_eq!(in_flight.peak_live, 2);

        drop(temporary);
        assert_eq!(stats.snapshot().for_kind(HeapObjectKind::Array).live, 1);
        drop(source);
        drop(vm);
        assert_eq!(stats.snapshot().total_live, 0);
    }

    #[test]
    fn heap_stats_peak_live_tracks_a_mixed_vm_workload() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let stats = vm.heap_stats();
        let array = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let map = vm.make_map(vec![(Value::string("items"), array.clone())]);
        let structure = vm
            .heap
            .allocate_struct(
                None,
                Some("Workload".to_string()),
                vec![
                    ("array".to_string(), array.clone()),
                    ("map".to_string(), map.clone()),
                ],
            )
            .expect("struct allocation should succeed");
        let temporary = vm
            .execute_native_call("copy", vec![array.clone()])
            .expect("native copy should succeed");

        let snapshot = stats.snapshot();
        assert_eq!(snapshot.total_live, 4);
        assert_eq!(snapshot.peak_live, 4);
        assert_eq!(snapshot.for_kind(HeapObjectKind::Array).live, 2);
        assert_eq!(snapshot.for_kind(HeapObjectKind::Map).live, 1);
        assert_eq!(snapshot.for_kind(HeapObjectKind::Struct).live, 1);

        drop(temporary);
        drop(structure);
        drop(map);
        drop(array);
        drop(vm);
        let released = stats.snapshot();
        assert_eq!(released.total_live, 0);
        assert_eq!(released.peak_live, 4);
    }

    #[test]
    fn heap_stats_tracks_long_array_churn_without_retaining_short_lived_values() {
        const ITERATIONS: usize = 256;
        let program = array_churn_program(ITERATIONS);
        let vm = VM::with_config(&program, RunConfig::unlimited());
        let stats = vm.heap_stats();
        vm.run().expect("array churn workload should complete");

        let snapshot = stats.snapshot();
        assert_eq!(
            snapshot.for_kind(HeapObjectKind::Array).allocations,
            ITERATIONS
        );
        assert_eq!(snapshot.for_kind(HeapObjectKind::Array).live, 0);
        assert_eq!(snapshot.for_kind(HeapObjectKind::Array).dead, ITERATIONS);
        assert_eq!(snapshot.peak_live, 4);
        assert!(
            snapshot
                .for_kind(HeapObjectKind::Array)
                .peak_estimated_bytes
                > 0
        );
        assert!(snapshot.estimated_peak_live_bytes > 0);
        assert_eq!(snapshot.total_live, 0);
    }

    #[test]
    fn heap_stats_tracks_deep_recursive_closure_environment_and_cell_pressure() {
        const DEPTH: usize = 20;
        let program = recursive_closure_program(DEPTH);
        let vm = VM::with_config(&program, RunConfig::unlimited());
        let stats = vm.heap_stats();
        vm.run()
            .expect("recursive closure workload should complete");

        let snapshot = stats.snapshot();
        let environments = snapshot.for_kind(HeapObjectKind::Environment);
        let cells = snapshot.for_kind(HeapObjectKind::Cell);
        assert!(environments.allocations > DEPTH);
        assert_eq!(cells.allocations, DEPTH + 1);
        assert_eq!(environments.live, 0);
        assert_eq!(cells.live, 0);
        assert!(snapshot.peak_live > DEPTH);
        assert_eq!(snapshot.total_live, 0);
    }

    #[test]
    fn heap_stats_covers_large_array_and_map_payload_workloads_without_display() {
        const ARRAY_LENGTH: usize = 4096;
        const MAP_LENGTH: usize = 1024;
        let program = empty_program();
        let mut vm = VM::new(&program);
        let stats = vm.heap_stats();
        let array = vm.make_array(
            (0..ARRAY_LENGTH)
                .map(|value| Value::number(value as f64))
                .collect(),
        );
        let map = vm.make_map(
            (0..MAP_LENGTH)
                .map(|value| (Value::number(value as f64), Value::number(value as f64)))
                .collect(),
        );

        let array_length = match &array {
            Value::Array(array) => array.elements.borrow().len(),
            _ => panic!("expected large array"),
        };
        let map_length = match &map {
            Value::Map(map) => map.entries.borrow().len(),
            _ => panic!("expected large map"),
        };
        assert_eq!(array_length, ARRAY_LENGTH);
        assert_eq!(map_length, MAP_LENGTH);

        let snapshot = stats.snapshot();
        assert_eq!(snapshot.for_kind(HeapObjectKind::Array).allocations, 1);
        assert_eq!(snapshot.for_kind(HeapObjectKind::Map).allocations, 1);
        assert!(snapshot.for_kind(HeapObjectKind::Array).estimated_bytes > ARRAY_LENGTH);
        assert!(snapshot.for_kind(HeapObjectKind::Map).estimated_bytes > MAP_LENGTH);
        assert_eq!(snapshot.peak_live, 2);
        assert!(snapshot.estimated_peak_live_bytes >= snapshot.estimated_live_bytes);

        drop(array);
        drop(map);
        drop(vm);
        let released = stats.snapshot();
        assert_eq!(released.total_live, 0);
        assert_eq!(released.estimated_live_bytes, 0);
        assert_eq!(
            released.estimated_peak_live_bytes,
            snapshot.estimated_peak_live_bytes
        );
    }

    #[test]
    fn native_map_invokes_callback_and_returns_fresh_array() {
        let program = Program {
            constants: Vec::new(),
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: vec!["item".to_string()],
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "identity".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 1,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::LoadLocal { dest: 0, slot: 0 },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None, None],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let callback = Value::function(FunctionValue {
            name: "identity".to_string(),
            function_index: 1,
            arity: 1,
            identity: 1,
            closure: new_environment(),
            upvalues: Vec::new(),
        });

        let mapped = vm
            .execute_native_call("map", vec![source.clone(), callback])
            .expect("map succeeds");

        let elements = array_elements(&mapped);
        assert_eq!(elements.len(), 2);
        assert!(matches!(elements[0], Value::Number(value) if value == 1.0));
        assert!(matches!(elements[1], Value::Number(value) if value == 2.0));
        assert!(!source.runtime_equals(&mapped));
    }

    #[test]
    fn native_flat_map_flattens_one_level_and_returns_fresh_array() {
        let program = Program {
            constants: vec![Constant::Number("10".to_string())],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: vec!["item".to_string()],
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "expand".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 5,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::LoadLocal { dest: 0, slot: 0 },
                        Instruction::Constant {
                            dest: 1,
                            constant: 0,
                        },
                        Instruction::Add {
                            dest: 2,
                            left: 0,
                            right: 1,
                        },
                        Instruction::Array {
                            dest: 3,
                            elements: vec![0],
                        },
                        Instruction::Array {
                            dest: 4,
                            elements: vec![0, 2, 3],
                        },
                        Instruction::Return { value: 4 },
                    ],
                    locations: vec![None; 6],
                },
                Function {
                    id: FuncId(2),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "identity".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 1,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::LoadLocal { dest: 0, slot: 0 },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None; 2],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let callback = Value::function(FunctionValue {
            name: "expand".to_string(),
            function_index: 1,
            arity: 1,
            identity: 1,
            closure: new_environment(),
            upvalues: Vec::new(),
        });

        let flattened = vm
            .execute_native_call("flatMap", vec![source.clone(), callback])
            .expect("flatMap succeeds");

        let elements = array_elements(&flattened);
        assert_eq!(elements.len(), 6);
        assert!(matches!(elements[0], Value::Number(value) if value == 1.0));
        assert!(matches!(elements[1], Value::Number(value) if value == 11.0));
        assert!(
            matches!(&elements[2], Value::Array(nested) if nested.elements.borrow().len() == 1)
        );
        assert!(matches!(elements[3], Value::Number(value) if value == 2.0));
        assert!(matches!(elements[4], Value::Number(value) if value == 12.0));
        assert!(
            matches!(&elements[5], Value::Array(nested) if nested.elements.borrow().len() == 1)
        );
        assert!(!source.runtime_equals(&flattened));

        let invalid_callback = Value::function(FunctionValue {
            name: "identity".to_string(),
            function_index: 2,
            arity: 1,
            identity: 2,
            closure: new_environment(),
            upvalues: Vec::new(),
        });
        assert_eq!(
            vm.execute_native_call("flatMap", vec![source, invalid_callback])
                .unwrap_err()
                .message,
            "flatMap expects callback to return array"
        );
    }

    #[test]
    fn native_filter_invokes_predicate_and_returns_matching_fresh_array() {
        let program = Program {
            constants: vec![Constant::Number("1".to_string())],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: vec!["item".to_string()],
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "greater_than_one".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 3,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::LoadLocal { dest: 0, slot: 0 },
                        Instruction::Constant {
                            dest: 1,
                            constant: 0,
                        },
                        Instruction::Greater {
                            dest: 2,
                            left: 0,
                            right: 1,
                        },
                        Instruction::Return { value: 2 },
                    ],
                    locations: vec![None, None, None, None],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let source = vm.make_array(vec![
            Value::number(1.0),
            Value::number(2.0),
            Value::number(3.0),
        ]);
        let predicate = Value::function(FunctionValue {
            name: "greater_than_one".to_string(),
            function_index: 1,
            arity: 1,
            identity: 1,
            closure: new_environment(),
            upvalues: Vec::new(),
        });

        let filtered = vm
            .execute_native_call("filter", vec![source.clone(), predicate])
            .expect("filter succeeds");

        let elements = array_elements(&filtered);
        assert_eq!(elements.len(), 2);
        assert!(matches!(elements[0], Value::Number(value) if value == 2.0));
        assert!(matches!(elements[1], Value::Number(value) if value == 3.0));
        assert!(!source.runtime_equals(&filtered));
    }

    #[test]
    fn native_filter_validates_operands_and_boolean_predicate_results() {
        let program = Program {
            constants: vec![Constant::Nil],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "no_args".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 0,
                    params: Vec::new(),
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(2),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "returns_nil".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 1,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None, None],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let array = vm.make_array(vec![Value::number(1.0)]);
        let no_args = Value::function(FunctionValue {
            name: "no_args".to_string(),
            function_index: 1,
            arity: 0,
            identity: 1,
            closure: new_environment(),
            upvalues: Vec::new(),
        });
        let returns_nil = Value::function(FunctionValue {
            name: "returns_nil".to_string(),
            function_index: 2,
            arity: 1,
            identity: 2,
            closure: new_environment(),
            upvalues: Vec::new(),
        });

        assert_eq!(
            vm.execute_native_call("filter", Vec::new())
                .unwrap_err()
                .message,
            "filter expects 2 arguments"
        );
        assert_eq!(
            vm.execute_native_call("filter", vec![Value::number(1.0), no_args.clone()])
                .unwrap_err()
                .message,
            "filter expects array as first argument"
        );
        assert_eq!(
            vm.execute_native_call("filter", vec![array.clone(), Value::number(1.0)])
                .unwrap_err()
                .message,
            "filter expects function as second argument"
        );
        assert_eq!(
            vm.execute_native_call("filter", vec![array.clone(), no_args])
                .unwrap_err()
                .message,
            "filter expects callback with 1 argument"
        );
        assert_eq!(
            vm.execute_native_call("filter", vec![array, returns_nil])
                .unwrap_err()
                .message,
            "filter expects callback to return bool"
        );
    }

    #[test]
    fn native_any_and_all_short_circuit_with_boolean_results() {
        let program = Program {
            constants: vec![Constant::Number("2".to_string())],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: vec!["item".to_string()],
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "is_two".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 3,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::LoadLocal { dest: 0, slot: 0 },
                        Instruction::Constant {
                            dest: 1,
                            constant: 0,
                        },
                        Instruction::Equal {
                            dest: 2,
                            left: 0,
                            right: 1,
                        },
                        Instruction::Return { value: 2 },
                    ],
                    locations: vec![None, None, None, None],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let predicate = Value::function(FunctionValue {
            name: "is_two".to_string(),
            function_index: 1,
            arity: 1,
            identity: 1,
            closure: new_environment(),
            upvalues: Vec::new(),
        });
        let any_source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let all_source = vm.make_array(vec![Value::number(2.0), Value::number(2.0)]);
        let count_source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let empty_any_source = vm.make_array(Vec::new());
        let empty_all_source = vm.make_array(Vec::new());

        assert!(matches!(
            vm.execute_native_call("any", vec![any_source, predicate.clone()])
                .unwrap(),
            Value::Bool(true)
        ));
        assert!(matches!(
            vm.execute_native_call("all", vec![all_source, predicate.clone()])
                .unwrap(),
            Value::Bool(true)
        ));
        assert!(matches!(
            vm.execute_native_call("count", vec![count_source, predicate.clone()])
                .unwrap(),
            Value::Number(value) if value == 1.0
        ));
        let find_source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let find_miss_source = vm.make_array(vec![Value::number(1.0)]);
        assert!(matches!(
            vm.execute_native_call("find", vec![find_source, predicate.clone()])
                .unwrap(),
            Value::Number(value) if value == 2.0
        ));
        assert!(matches!(
            vm.execute_native_call("find", vec![find_miss_source, predicate.clone()])
                .unwrap(),
            Value::Nil
        ));
        let find_index_source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let find_index_miss_source = vm.make_array(vec![Value::number(1.0)]);
        assert!(matches!(
            vm.execute_native_call("findIndex", vec![find_index_source, predicate.clone()])
                .unwrap(),
            Value::Number(value) if value == 1.0
        ));
        assert!(matches!(
            vm.execute_native_call("findIndex", vec![find_index_miss_source, predicate.clone()])
                .unwrap(),
            Value::Number(value) if value == -1.0
        ));
        assert!(matches!(
            vm.execute_native_call("any", vec![empty_any_source, predicate.clone()])
                .unwrap(),
            Value::Bool(false)
        ));
        assert!(matches!(
            vm.execute_native_call("all", vec![empty_all_source, predicate])
                .unwrap(),
            Value::Bool(true)
        ));
    }

    #[test]
    fn native_any_and_all_validate_operands_and_predicate_results() {
        let program = Program {
            constants: vec![Constant::Nil],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "no_args".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 0,
                    params: Vec::new(),
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(2),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "returns_nil".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 1,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None, None],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let array = vm.make_array(vec![Value::number(1.0)]);
        let no_args = Value::function(FunctionValue {
            name: "no_args".to_string(),
            function_index: 1,
            arity: 0,
            identity: 1,
            closure: new_environment(),
            upvalues: Vec::new(),
        });
        let returns_nil = Value::function(FunctionValue {
            name: "returns_nil".to_string(),
            function_index: 2,
            arity: 1,
            identity: 2,
            closure: new_environment(),
            upvalues: Vec::new(),
        });

        assert_eq!(
            vm.execute_native_call("any", Vec::new())
                .unwrap_err()
                .message,
            "any expects 2 arguments"
        );
        assert_eq!(
            vm.execute_native_call("all", vec![Value::number(1.0), no_args.clone()])
                .unwrap_err()
                .message,
            "all expects array as first argument"
        );
        assert_eq!(
            vm.execute_native_call("any", vec![array.clone(), Value::number(1.0)])
                .unwrap_err()
                .message,
            "any expects function as second argument"
        );
        assert_eq!(
            vm.execute_native_call("all", vec![array.clone(), no_args])
                .unwrap_err()
                .message,
            "all expects callback with 1 argument"
        );
        assert_eq!(
            vm.execute_native_call("any", vec![array, returns_nil])
                .unwrap_err()
                .message,
            "any expects callback to return bool"
        );
    }

    #[test]
    fn native_reduce_threads_accumulator_and_returns_initial_for_empty_arrays() {
        let program = Program {
            constants: Vec::new(),
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: vec!["acc".to_string(), "item".to_string()],
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "add".to_string(),
                    arity: 2,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 3,
                    params: vec!["acc".to_string(), "item".to_string()],
                    instructions: vec![
                        Instruction::LoadLocal { dest: 0, slot: 0 },
                        Instruction::LoadLocal { dest: 1, slot: 1 },
                        Instruction::Add {
                            dest: 2,
                            left: 0,
                            right: 1,
                        },
                        Instruction::Return { value: 2 },
                    ],
                    locations: vec![None, None, None, None],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let source = vm.make_array(vec![
            Value::number(1.0),
            Value::number(2.0),
            Value::number(3.0),
        ]);
        let empty = vm.make_array(Vec::new());
        let callback = Value::function(FunctionValue {
            name: "add".to_string(),
            function_index: 1,
            arity: 2,
            identity: 1,
            closure: new_environment(),
            upvalues: Vec::new(),
        });

        let total = vm
            .execute_native_call("reduce", vec![source, Value::number(0.0), callback.clone()])
            .expect("reduce succeeds");
        assert!(matches!(total, Value::Number(value) if value == 6.0));

        let initial = Value::string("seed");
        let returned = vm
            .execute_native_call("reduce", vec![empty, initial.clone(), callback])
            .expect("empty reduce succeeds");
        assert!(returned.runtime_equals(&initial));
    }

    #[test]
    fn native_reduce_validates_operands_and_callback_arity() {
        let program = Program {
            constants: Vec::new(),
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "one_arg".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 0,
                    params: vec!["item".to_string()],
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let array = vm.make_array(vec![Value::number(1.0)]);
        let callback = Value::function(FunctionValue {
            name: "one_arg".to_string(),
            function_index: 1,
            arity: 1,
            identity: 1,
            closure: new_environment(),
            upvalues: Vec::new(),
        });

        assert_eq!(
            vm.execute_native_call("reduce", Vec::new())
                .unwrap_err()
                .message,
            "reduce expects 3 arguments"
        );
        assert_eq!(
            vm.execute_native_call(
                "reduce",
                vec![Value::number(1.0), Value::number(0.0), callback.clone()],
            )
            .unwrap_err()
            .message,
            "reduce expects array as first argument"
        );
        assert_eq!(
            vm.execute_native_call(
                "reduce",
                vec![array.clone(), Value::number(0.0), Value::number(1.0)],
            )
            .unwrap_err()
            .message,
            "reduce expects function as third argument"
        );
        assert_eq!(
            vm.execute_native_call("reduce", vec![array, Value::number(0.0), callback])
                .unwrap_err()
                .message,
            "reduce expects callback with 2 arguments"
        );
    }

    #[test]
    fn map_lookup_update_length_and_contains() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(vec![(Value::string("a"), Value::number(1.0))]);
        let alias = map.clone();

        let updated = vm
            .execute_assign_index(alias, Value::string("b"), Value::number(2.0))
            .expect("map assignment succeeds");
        assert!(matches!(updated, Value::Number(value) if value == 2.0));
        assert!(matches!(
            vm.execute_index(&map, &Value::string("b")).unwrap(),
            Value::Number(value) if value == 2.0
        ));
        assert!(matches!(vm.execute_len(&map).unwrap(), Value::Number(value) if value == 2.0));
        assert!(matches!(
            vm.execute_native_call("contains", vec![map.clone(), Value::string("a")])
                .unwrap(),
            Value::Bool(true)
        ));
        assert!(vm.execute_index(&map, &Value::string("missing")).is_err());
    }

    #[test]
    fn native_map_keys_and_values_return_ordered_fresh_arrays() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(vec![
            (Value::string("a"), Value::number(1.0)),
            (Value::string("b"), Value::number(2.0)),
        ]);

        let keys = vm
            .execute_native_call("keys", vec![map.clone()])
            .expect("keys succeeds");
        let values = vm
            .execute_native_call("values", vec![map.clone()])
            .expect("values succeeds");
        assert_eq!(keys.to_string(), "[a, b]");
        assert_eq!(values.to_string(), "[1, 2]");

        vm.execute_assign_index(map.clone(), Value::string("c"), Value::number(3.0))
            .expect("map mutation succeeds");
        assert_eq!(keys.to_string(), "[a, b]");
        assert_eq!(values.to_string(), "[1, 2]");

        let fresh_keys = vm
            .execute_native_call("keys", vec![map])
            .expect("second keys succeeds");
        assert_eq!(fresh_keys.to_string(), "[a, b, c]");
        assert!(!keys.runtime_equals(&fresh_keys));
    }

    #[test]
    fn assert_array_snapshots_map_keys_in_insertion_order() {
        let program = Program {
            constants: vec![
                Constant::String("a".to_string()),
                Constant::Number("1".to_string()),
                Constant::String("b".to_string()),
                Constant::Number("2".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 10,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Constant {
                        dest: 1,
                        constant: 1,
                    },
                    Instruction::Constant {
                        dest: 2,
                        constant: 2,
                    },
                    Instruction::Constant {
                        dest: 3,
                        constant: 3,
                    },
                    Instruction::Map {
                        dest: 4,
                        entries: vec![(0, 1), (2, 3)],
                    },
                    Instruction::IterInit { dest: 5, value: 4 },
                    Instruction::IterNext { dest: 6, value: 5 },
                    Instruction::IterNext { dest: 7, value: 5 },
                    Instruction::Array {
                        dest: 8,
                        elements: vec![6, 7],
                    },
                    Instruction::CallNative {
                        dest: 9,
                        native: NativeId(0),
                        arguments: vec![8],
                    },
                    Instruction::Return { value: 8 },
                ],
                locations: Vec::new(),
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };

        assert_eq!(
            VM::new(&program).run().expect("map iteration succeeds"),
            "[a, b]\n"
        );
    }

    #[test]
    fn native_remove_updates_shared_maps_and_returns_removed_value() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(vec![
            (Value::string("a"), Value::number(1.0)),
            (Value::string("b"), Value::Nil),
        ]);
        let alias = map.clone();

        let removed = vm
            .execute_native_call("remove", vec![map.clone(), Value::string("a")])
            .expect("remove succeeds");
        assert!(matches!(removed, Value::Number(value) if value == 1.0));
        assert_eq!(map.to_string(), "map{b: nil}");
        assert!(matches!(
            vm.execute_index(&alias, &Value::string("b")).unwrap(),
            Value::Nil
        ));

        let removed_nil = vm
            .execute_native_call("remove", vec![alias.clone(), Value::string("b")])
            .expect("remove can return nil");
        assert!(matches!(removed_nil, Value::Nil));
        assert!(matches!(
            vm.execute_len(&alias).unwrap(),
            Value::Number(value) if value == 0.0
        ));
    }

    #[test]
    fn native_clear_empties_shared_map_and_allows_new_insertion() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(vec![
            (Value::string("a"), Value::number(1.0)),
            (Value::string("b"), Value::number(2.0)),
        ]);
        let alias = map.clone();

        let result = vm
            .execute_native_call("clear", vec![map.clone()])
            .expect("clear succeeds");
        assert!(matches!(result, Value::Nil));
        assert_eq!(alias.to_string(), "map{}");
        assert!(matches!(
            vm.execute_len(&alias).unwrap(),
            Value::Number(value) if value == 0.0
        ));

        vm.execute_assign_index(alias.clone(), Value::string("c"), Value::number(3.0))
            .expect("map insertion after clear succeeds");
        assert_eq!(map.to_string(), "map{c: 3}");
    }

    #[test]
    fn native_merge_returns_fresh_ordered_map_and_shares_values() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let shared = vm.make_array(vec![Value::number(7.0)]);
        let left = vm.make_map(vec![
            (Value::string("a"), Value::number(1.0)),
            (Value::string("b"), shared.clone()),
        ]);
        let right = vm.make_map(vec![
            (Value::string("b"), Value::number(2.0)),
            (Value::string("c"), shared.clone()),
        ]);

        let merged = vm
            .execute_native_call("merge", vec![left.clone(), right])
            .expect("merge succeeds");

        assert_eq!(merged.to_string(), "map{a: 1, b: 2, c: [7]}");
        assert_eq!(left.to_string(), "map{a: 1, b: [7]}");
        assert!(!merged.runtime_equals(&left));

        vm.execute_native_call("push", vec![shared, Value::number(8.0)])
            .expect("shared value mutation succeeds");
        assert_eq!(merged.to_string(), "map{a: 1, b: 2, c: [7, 8]}");
        assert_eq!(left.to_string(), "map{a: 1, b: [7, 8]}");
    }

    #[test]
    fn native_merge_validates_arity_and_map_operands() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(Vec::new());

        assert_eq!(
            vm.execute_native_call("merge", Vec::new())
                .unwrap_err()
                .message,
            "merge expects 2 arguments"
        );
        assert_eq!(
            vm.execute_native_call("merge", vec![Value::number(1.0), map.clone()])
                .unwrap_err()
                .message,
            "merge expects map as first argument"
        );
        assert_eq!(
            vm.execute_native_call("merge", vec![map, Value::number(1.0)])
                .unwrap_err()
                .message,
            "merge expects map as second argument"
        );
    }

    #[test]
    fn map_aliases_share_updates_through_cells() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(vec![(Value::string("a"), Value::number(1.0))]);
        let left = new_cell(map.clone());
        let right = new_cell(map);

        vm.execute_assign_index(
            left.borrow().clone(),
            Value::string("b"),
            Value::number(2.0),
        )
        .expect("map assignment succeeds");

        let left_value = left.borrow();
        assert!(matches!(
            vm.execute_index(&left_value, &Value::string("b")).unwrap(),
            Value::Number(value) if value == 2.0
        ));
        let right_value = right.borrow();
        assert!(matches!(
            vm.execute_index(&right_value, &Value::string("b")).unwrap(),
            Value::Number(value) if value == 2.0
        ));
    }

    #[test]
    fn map_rejects_reference_keys() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(Vec::new());
        let key = vm.make_array(vec![Value::number(1.0)]);

        let error = vm
            .execute_assign_index(map, key, Value::number(1.0))
            .expect_err("array key should fail");
        assert_eq!(
            error.message,
            "map key must be nil, number, bool, or string"
        );
    }

    #[test]
    fn hash_reference_key_identity_remains_stable_after_mutation() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let key = vm.make_array(vec![Value::number(1.0)]);
        let alias = key.clone();
        let before = key.runtime_hash();

        vm.execute_assign_index(alias.clone(), Value::number(0.0), Value::number(2.0))
            .expect("reference key mutation succeeds");

        assert!(key.runtime_equals(&alias));
        assert_eq!(key.runtime_hash(), before);
        assert_eq!(key.to_string(), "[2]");

        let distinct = vm.make_array(vec![Value::number(2.0)]);
        assert!(!key.runtime_equals(&distinct));
    }

    #[test]
    fn map_preserves_order_and_identity_equality() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(vec![
            (Value::string("a"), Value::number(1.0)),
            (Value::string("b"), Value::number(2.0)),
            (Value::string("a"), Value::number(3.0)),
        ]);
        let distinct = vm.make_map(vec![
            (Value::string("a"), Value::number(3.0)),
            (Value::string("b"), Value::number(2.0)),
        ]);

        assert_eq!(map.to_string(), "map{a: 3, b: 2}");
        assert!(map.runtime_equals(&map));
        assert!(!map.runtime_equals(&distinct));
    }

    #[test]
    fn native_ranges_are_indexable_and_iterable_values() {
        let program = empty_program();
        let mut vm = VM::new(&program);

        let ascending = vm
            .execute_native_call("range", vec![Value::number(1.0), Value::number(6.0)])
            .expect("range succeeds");
        let equivalent = vm
            .execute_native_call("range", vec![Value::number(1.0), Value::number(6.0)])
            .expect("equivalent range succeeds");
        let different = vm
            .execute_native_call("range", vec![Value::number(1.0), Value::number(7.0)])
            .expect("different range succeeds");
        assert_eq!(ascending.to_string(), "range(1, 6, 1)");
        assert_eq!(ascending.type_name(), "range");
        assert!(ascending.runtime_equals(&equivalent));
        assert!(!ascending.runtime_equals(&different));
        assert!(matches!(
            vm.execute_len(&ascending).unwrap(),
            Value::Number(value) if value == 5.0
        ));
        assert!(matches!(
            vm.execute_index(&ascending, &Value::number(2.0)).unwrap(),
            Value::Number(value) if value == 3.0
        ));
        assert!(matches!(
            vm.execute_native_call("contains", vec![ascending.clone(), Value::number(5.0)])
                .unwrap(),
            Value::Bool(true)
        ));
        assert!(matches!(
            vm.execute_native_call("contains", vec![ascending.clone(), Value::number(5.5)])
                .unwrap(),
            Value::Bool(false)
        ));

        let descending = vm
            .execute_native_call(
                "range",
                vec![Value::number(5.0), Value::number(0.0), Value::number(-2.0)],
            )
            .expect("descending range succeeds");
        assert!(matches!(
            vm.execute_len(&descending).unwrap(),
            Value::Number(value) if value == 3.0
        ));
        assert!(matches!(
            vm.execute_index(&descending, &Value::number(1.0)).unwrap(),
            Value::Number(value) if value == 3.0
        ));

        let empty = vm
            .execute_native_call("range", vec![Value::number(5.0), Value::number(0.0)])
            .expect("empty range succeeds");
        assert!(matches!(
            vm.execute_len(&empty).unwrap(),
            Value::Number(value) if value == 0.0
        ));
    }

    #[test]
    fn native_ranges_validate_bounds_and_indexes() {
        let program = empty_program();
        let mut vm = VM::new(&program);

        let zero_step = vm
            .execute_native_call(
                "range",
                vec![Value::number(0.0), Value::number(3.0), Value::number(0.0)],
            )
            .expect_err("zero step should fail");
        assert_eq!(zero_step.message, "range step must not be zero");

        let fractional = vm
            .execute_native_call(
                "range",
                vec![Value::number(0.0), Value::number(3.0), Value::number(1.5)],
            )
            .expect_err("fractional step should fail");
        assert_eq!(
            fractional.message,
            "range expects integer as third argument"
        );

        let range = vm
            .execute_native_call("range", vec![Value::number(3.0)])
            .expect("range succeeds");
        let wrong_type = vm
            .execute_index(&range, &Value::boolean(true))
            .expect_err("bool index should fail");
        assert_eq!(wrong_type.message, "range index must be number");
        let out_of_bounds = vm
            .execute_index(&range, &Value::number(3.0))
            .expect_err("out-of-bounds index should fail");
        assert_eq!(out_of_bounds.message, "range index out of bounds");
    }

    #[test]
    fn native_collection_helpers_validate_slice_boundaries() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let source = vm.make_array(vec![Value::number(1.0)]);

        let empty = vm
            .execute_native_call(
                "slice",
                vec![source.clone(), Value::number(1.0), Value::number(0.0)],
            )
            .expect("empty end slice succeeds");
        assert!(array_elements(&empty).is_empty());

        for (start, length, expected) in [
            (f64::NAN, 0.0, "slice expects integer start offset"),
            (-1.0, 0.0, "slice start offset out of bounds"),
            (0.0, f64::INFINITY, "slice expects integer length"),
            (0.0, 2.0, "slice length out of bounds"),
        ] {
            let error = vm
                .execute_native_call(
                    "slice",
                    vec![source.clone(), Value::number(start), Value::number(length)],
                )
                .expect_err("slice should fail");
            assert_eq!(error.message, expected);
        }
    }

    #[test]
    fn native_collection_helpers_validate_arity_and_types() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        assert_eq!(
            vm.execute_native_call("contains", vec![])
                .unwrap_err()
                .message,
            "contains expects 2 arguments"
        );
        assert_eq!(
            vm.execute_native_call("slice", vec![]).unwrap_err().message,
            "slice expects 3 arguments"
        );
        assert_eq!(
            vm.execute_native_call("copy", vec![]).unwrap_err().message,
            "copy expects 1 argument"
        );
        assert_eq!(
            vm.execute_native_call("concat", vec![])
                .unwrap_err()
                .message,
            "concat expects 2 arguments"
        );
        assert_eq!(
            vm.execute_native_call("copy", vec![Value::number(1.0)])
                .unwrap_err()
                .message,
            "copy expects array as first argument"
        );
        assert_eq!(
            vm.execute_native_call("concat", vec![Value::Nil, Value::Nil])
                .unwrap_err()
                .message,
            "concat expects array as first argument"
        );
    }

    #[test]
    fn native_string_helpers_use_unicode_scalar_boundaries() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let source = Value::string("你🙂e\u{301}");

        let length = vm.execute_len(&source).expect("len succeeds");
        assert!(matches!(length, Value::Number(value) if value == 4.0));

        let sliced = vm
            .execute_native_call(
                "substr",
                vec![source.clone(), Value::number(1.0), Value::number(2.0)],
            )
            .expect("substr succeeds");
        assert!(matches!(sliced, Value::String(value) if value.as_ref() == "🙂e"));

        let combined = vm
            .execute_native_call(
                "substr",
                vec![source.clone(), Value::number(2.0), Value::number(2.0)],
            )
            .expect("combining scalar slice succeeds");
        assert!(matches!(combined, Value::String(value) if value.as_ref() == "e\u{301}"));

        let character = vm
            .execute_native_call("charAt", vec![source, Value::number(1.0)])
            .expect("charAt succeeds");
        assert!(matches!(character, Value::String(value) if value.as_ref() == "🙂"));
    }

    #[test]
    fn native_string_helpers_validate_scalar_boundaries() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let source = Value::string("你🙂");

        let empty = vm
            .execute_native_call(
                "substr",
                vec![source.clone(), Value::number(2.0), Value::number(0.0)],
            )
            .expect("empty end slice succeeds");
        assert!(matches!(empty, Value::String(value) if value.is_empty()));

        for (start, length, expected) in [
            (3.0, 0.0, "substr start offset out of bounds"),
            (1.0, 2.0, "substr length out of bounds"),
            (-1.0, 0.0, "substr start offset out of bounds"),
            (1.5, 0.0, "substr expects integer start offset"),
        ] {
            let error = vm
                .execute_native_call(
                    "substr",
                    vec![source.clone(), Value::number(start), Value::number(length)],
                )
                .expect_err("substr should fail");
            assert_eq!(error.message, expected);
        }

        for (index, expected) in [
            (2.0, "charAt index out of bounds"),
            (1.5, "charAt expects integer index"),
        ] {
            let error = vm
                .execute_native_call("charAt", vec![source.clone(), Value::number(index)])
                .expect_err("charAt should fail");
            assert_eq!(error.message, expected);
        }
    }

    #[test]
    fn runtime_error_reports_inner_location_then_outer_call_site() {
        let error = VM::new(&debug_failure_program()).run().unwrap_err();
        assert_eq!(error.location.as_ref().unwrap().line, 1);
        assert_eq!(error.stack[0].function, "fail");
        assert_eq!(error.stack[1].function, "main");
        assert!(error.to_string().contains("Call stack:\n"));
        assert!(error
            .to_string()
            .contains("  fun fail() { return 1 / 0; }\n"));
    }

    #[test]
    fn native_failure_uses_native_call_location() {
        let program = Program {
            constants: vec![Constant::Nil],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "sqrt".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: vec!["sqrt".to_string()],
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 2,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::CallNative {
                        dest: 1,
                        native: NativeId(0),
                        arguments: vec![0],
                    },
                ],
                locations: vec![
                    Some(DebugLocation {
                        source: 0,
                        line: 1,
                        column: 7,
                        range: None,
                    }),
                    Some(DebugLocation {
                        source: 0,
                        line: 1,
                        column: 1,
                        range: None,
                    }),
                ],
            }],
            entry: FuncId(0),
            debug_sources: vec![DebugSource {
                module: None,
                path: "native.cd".to_string(),
                text: "sqrt(nil);\n".to_string(),
            }],
        };
        let error = VM::new(&program).run().unwrap_err();
        assert_eq!(error.location.as_ref().unwrap().column, 1);
        assert!(error.to_string().contains("sqrt(nil);"));
    }

    #[test]
    fn metadata_free_runtime_errors_keep_legacy_format() {
        let error = VM::new(&empty_program())
            .execute_native_call("sqrt", vec![Value::Nil])
            .unwrap_err();
        assert_eq!(error.to_string(), "Runtime error: sqrt expects number");
    }

    #[test]
    fn invalid_debug_source_lookup_does_not_panic() {
        let mut program = debug_failure_program();
        program.functions[1].locations[2] = Some(DebugLocation {
            source: 99,
            line: 1,
            column: 1,
            range: None,
        });
        let error = VM::new(&program).run().unwrap_err();
        assert!(error.to_string().starts_with("Runtime error: "));
    }

    #[test]
    fn heap_stats_observe_runtime_error_root_release() {
        let program = debug_failure_program();
        let vm = VM::new(&program);
        let stats = vm.heap_stats();
        let error = vm.run().expect_err("division by zero should fail");
        assert_eq!(error.message, "division by zero");

        let snapshot = stats.snapshot();
        let environments = snapshot.for_kind(HeapObjectKind::Environment);
        assert!(environments.allocations > 0);
        assert_eq!(environments.live, 0);
        assert_eq!(snapshot.total_live, 0);
        assert_eq!(snapshot.total_dead, snapshot.total_allocations);
    }

    #[test]
    fn heap_stats_observe_trace_debug_values_without_retaining_vm_roots() {
        let program = debug_failure_program();
        let vm = VM::new(&program);
        let stats = vm.heap_stats();
        let trace = vm.trace();
        assert!(trace.result.is_err());
        assert!(trace
            .events
            .iter()
            .any(|event| event.kind == TraceEventKind::Error));
        assert_eq!(stats.snapshot().total_live, 0);
    }

    #[test]
    fn top_level_safepoint_collects_after_cancellation_and_debugger_quit() {
        let token = CancellationToken::new();
        let program = cycle_until_pause_program();
        let vm = VM::with_config(
            &program,
            RunConfig::unlimited().with_cancellation(token.clone()),
        );
        let stats = vm.heap_stats();
        let debug = vm.debug(Box::new(CancelAtInstruction {
            token,
            instruction: 2,
        }));
        let error = debug
            .result
            .expect_err("cancellation should stop the cycle loop");
        assert_eq!(error.kind, RuntimeErrorKind::Cancelled);
        assert!(!debug.quit);
        let cancelled = stats.snapshot();
        assert_eq!(cancelled.total_live, 0);
        assert!(cancelled.peak_live > 0);

        let program = cycle_until_pause_program();
        let vm = VM::with_config(&program, RunConfig::unlimited());
        let stats = vm.heap_stats();
        let debug = vm.debug(Box::new(QuitAtInstruction { instruction: 2 }));
        assert!(debug.quit);
        assert_eq!(
            debug.result.expect("debugger quit is a successful stop"),
            ""
        );
        let quit = stats.snapshot();
        assert_eq!(quit.total_live, 0);
        assert!(quit.peak_live > 0);
    }

    #[test]
    fn instruction_budget_is_deterministic_and_has_an_explicit_unlimited_mode() {
        let program = Program {
            constants: vec![Constant::Number("1".to_string())],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 1,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::BlockStart { id: BlockId(0) },
                    Instruction::Br { target: BlockId(0) },
                ],
                locations: vec![None; 3],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut config = RunConfig::unlimited();
        config.max_instruction_steps = Some(3);
        let first = VM::with_config(&program, config.clone()).run().unwrap_err();
        let second = VM::with_config(&program, config).run().unwrap_err();
        assert_eq!(
            first.kind,
            RuntimeErrorKind::Resource(ResourceKind::InstructionSteps)
        );
        assert_eq!(first.resource_limit, Some(3));
        assert_eq!(
            first.message,
            "resource limit exceeded: instruction steps (limit 3)"
        );
        assert_eq!(first.message, second.message);
        assert!(VM::with_config(
            &Program {
                constants: vec![Constant::Number("1".to_string())],
                names: Vec::new(),
                data_segments: Vec::new(),
                symbols: Vec::new(),
                relocations: Vec::new(),
                globals: Vec::new(),
                types: Vec::new(),
                native_imports: Vec::new(),
                modules: Vec::new(),
                functions: vec![Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 1,
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0
                        },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None, None],
                }],
                entry: FuncId(0),
                debug_sources: Vec::new(),
            },
            RunConfig::unlimited(),
        )
        .run()
        .is_ok());
    }

    #[test]
    fn call_depth_budget_excludes_main_and_covers_callbacks() {
        let function = crate::bytecode::Function {
            id: FuncId(1),
            local_count: 0,
            upvalues: Vec::new(),
            name: "recurse".to_string(),
            arity: 0,
            machine_frame_size: 0,
            machine_params: Vec::new(),
            machine_return: None,
            registers: 2,
            params: Vec::new(),
            instructions: vec![
                Instruction::MakeFunction {
                    dest: 0,
                    function: FuncId(1),
                },
                Instruction::Call {
                    dest: 1,
                    callee: 0,
                    arguments: Vec::new(),
                },
                Instruction::Return { value: 1 },
            ],
            locations: vec![None, None, None],
        };
        let program = Program {
            constants: Vec::new(),
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 2,
                    instructions: vec![
                        Instruction::MakeFunction {
                            dest: 0,
                            function: FuncId(1),
                        },
                        Instruction::Call {
                            dest: 1,
                            callee: 0,
                            arguments: Vec::new(),
                        },
                    ],
                    locations: vec![None, None],
                },
                function,
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut config = RunConfig::unlimited();
        config.max_call_depth = Some(1);
        let error = VM::with_config(&program, config).run().unwrap_err();
        assert_eq!(
            error.kind,
            RuntimeErrorKind::Resource(ResourceKind::CallDepth)
        );
        assert_eq!(error.resource_limit, Some(1));
        assert_eq!(
            error.message,
            "resource limit exceeded: call depth (limit 1)"
        );
    }

    #[test]
    fn native_callback_iteration_consumes_instruction_budget() {
        let program = Program {
            constants: Vec::new(),
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: vec!["item".to_string()],
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                crate::bytecode::Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "identity".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 0,
                    params: vec!["item".to_string()],
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut config = RunConfig::unlimited();
        config.max_instruction_steps = Some(1);
        let mut vm = VM::with_config(&program, config);
        let source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let callback = Value::function(FunctionValue {
            name: "identity".to_string(),
            function_index: 1,
            arity: 1,
            identity: 1,
            closure: new_environment(),
            upvalues: Vec::new(),
        });
        let error = vm
            .execute_native_call("map", vec![source, callback])
            .expect_err("native iteration should consume the step budget");
        assert_eq!(
            error.kind,
            RuntimeErrorKind::Resource(ResourceKind::InstructionSteps)
        );
        assert_eq!(error.resource_limit, Some(1));
        assert_eq!(
            error.message,
            "resource limit exceeded: instruction steps (limit 1)"
        );
    }

    #[test]
    fn runtime_element_budget_rejects_growth_before_allocation() {
        let program = Program {
            constants: vec![Constant::Nil],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 2,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Array {
                        dest: 1,
                        elements: vec![0],
                    },
                ],
                locations: vec![None, None],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut config = RunConfig::unlimited();
        config.max_runtime_elements = Some(1);
        let error = VM::with_config(&program, config).run().unwrap_err();
        assert_eq!(
            error.kind,
            RuntimeErrorKind::Resource(ResourceKind::RuntimeElements)
        );
        assert_eq!(error.resource_limit, Some(1));
        assert_eq!(
            error.message,
            "resource limit exceeded: runtime elements (limit 1)"
        );
    }

    #[test]
    fn output_budget_counts_utf8_bytes_and_hides_partial_run_output() {
        let program = Program {
            constants: vec![Constant::String("é".to_string())],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 2,
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::CallNative {
                        dest: 1,
                        native: NativeId(0),
                        arguments: vec![0],
                    },
                ],
                locations: vec![None, None],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut config = RunConfig::unlimited();
        config.max_output_bytes = Some(2);
        let error = VM::with_config(&program, config).run().unwrap_err();
        assert_eq!(
            error.kind,
            RuntimeErrorKind::Resource(ResourceKind::OutputBytes)
        );
        assert_eq!(error.resource_limit, Some(2));
        assert_eq!(
            error.message,
            "resource limit exceeded: output bytes (limit 2)"
        );
    }

    #[test]
    fn cancellation_is_checked_before_execution_and_does_not_change_other_vms() {
        let token = CancellationToken::new();
        token.cancel();
        let error = VM::with_config(
            &empty_program(),
            RunConfig::unlimited().with_cancellation(token),
        )
        .run()
        .unwrap_err();
        assert_eq!(error.kind, RuntimeErrorKind::Cancelled);
        assert_eq!(error.resource_limit, None);
        assert_eq!(error.message, "execution cancelled");
        assert!(VM::new(&empty_program()).run().is_ok());
    }

    #[test]
    fn checkpoint_cancellation_precedes_a_zero_step_limit() {
        let token = CancellationToken::new();
        let mut config = RunConfig::unlimited();
        config.max_instruction_steps = Some(0);
        let program = empty_program();
        let mut vm = VM::with_config(&program, config.with_cancellation(token.clone()));

        let limited = vm
            .checkpoint_instruction()
            .expect_err("zero step limit should reject an active checkpoint");
        assert_eq!(
            limited.kind,
            RuntimeErrorKind::Resource(ResourceKind::InstructionSteps)
        );

        token.cancel();
        let cancelled = vm
            .checkpoint_instruction()
            .expect_err("cancellation should be checked before the limit");
        assert_eq!(cancelled.kind, RuntimeErrorKind::Cancelled);
    }

    #[test]
    fn jit_safepoint_materializes_before_limit_and_cancellation_failures() {
        let token = CancellationToken::new();
        let mut config = RunConfig::unlimited();
        config.max_instruction_steps = Some(0);
        let program = empty_program();
        let mut vm = VM::with_config(&program, config.with_cancellation(token.clone()));
        let mut frame = Frame::callee(
            Rc::new(Function {
                id: FuncId(0),
                name: String::new(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),

                registers: 1,
                instructions: vec![Instruction::Return { value: 0 }],
                locations: vec![None],
            }),
            "jit_target",
            3,
            1,
            vm.heap.new_local_slots(),
            vm.heap.new_environment(),
            ReturnTarget {
                register: 0,
                call_site: None,
            },
        );
        frame.ip = 4;
        frame.registers[0] = Value::string("rooted across failure");

        let limited = vm.jit_safepoint(&frame, None, JitSafepointKind::Instruction);
        assert_eq!(
            limited.checkpoint.as_ref().map_err(|error| error.kind),
            Err(RuntimeErrorKind::Resource(ResourceKind::InstructionSteps))
        );
        assert_eq!(limited.frame.instruction(), 4);
        assert_eq!(limited.frame.function(), "jit_target");
        assert_eq!(
            limited.frame.registers()[0].to_string(),
            "rooted across failure"
        );
        assert_eq!(
            limited.frame.safepoint(),
            JitSafepoint::new(JitSafepointKind::Instruction, 4)
        );

        token.cancel();
        let cancelled = vm.jit_safepoint(&frame, None, JitSafepointKind::Native);
        assert_eq!(
            cancelled.checkpoint.as_ref().map_err(|error| error.kind),
            Err(RuntimeErrorKind::Cancelled)
        );
        assert_eq!(
            cancelled.frame.registers()[0].to_string(),
            "rooted across failure"
        );
        assert_eq!(
            cancelled.frame.safepoint(),
            JitSafepoint::new(JitSafepointKind::Native, 4)
        );
    }

    #[test]
    fn jit_non_budget_safepoints_do_not_charge_instruction_steps() {
        let program = empty_program();
        let mut config = RunConfig::unlimited();
        config.max_instruction_steps = Some(1);
        let mut vm = VM::with_config(&program, config);
        let frame = Frame::main(
            Rc::new(Function {
                id: FuncId(0),
                name: String::new(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),

                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            }),
            0,
            vm.heap.new_local_slots(),
            vm.heap.new_environment(),
        );

        let garbage_collection =
            vm.jit_safepoint(&frame, None, JitSafepointKind::GarbageCollection);
        assert!(garbage_collection.checkpoint.is_ok());
        assert_eq!(vm.instruction_steps, 0);

        let instruction = vm.jit_safepoint(&frame, None, JitSafepointKind::Instruction);
        assert!(instruction.checkpoint.is_ok());
        assert_eq!(vm.instruction_steps, 1);

        let return_boundary = vm.jit_safepoint(&frame, None, JitSafepointKind::Return);
        assert!(return_boundary.checkpoint.is_ok());
        assert_eq!(vm.instruction_steps, 1);
    }

    #[cfg(all(target_arch = "x86_64", unix))]
    #[test]
    fn jit_run_matches_interpreter_and_reuses_ordinary_call_cache() {
        let program = ordinary_jit_call_program(
            Function {
                id: FuncId(1),
                local_count: 0,
                upvalues: Vec::new(),
                name: "answer".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                registers: 3,
                params: Vec::new(),
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Constant {
                        dest: 1,
                        constant: 1,
                    },
                    Instruction::Add {
                        dest: 2,
                        left: 0,
                        right: 1,
                    },
                    Instruction::Return { value: 2 },
                ],
                locations: vec![None; 4],
            },
            vec![
                Constant::Number("20".to_string()),
                Constant::Number("22".to_string()),
            ],
        );

        let mut interpreter = VM::with_config(&program, RunConfig::unlimited());
        let interpreter_first = interpreter
            .run_inner()
            .expect("interpreter ordinary run should succeed");
        let interpreter_first_steps = interpreter.instruction_steps;
        let interpreter_second = interpreter
            .run_inner()
            .expect("interpreter ordinary rerun should succeed");
        let interpreter_second_steps = interpreter.instruction_steps;

        let mut jit = VM::with_config(&program, RunConfig::unlimited());
        jit.jit = JitState::enabled_for_tests([1], 4096);
        let jit_first = jit.run_inner().expect("JIT ordinary run should succeed");
        let jit_first_steps = jit.instruction_steps;
        let first_cache = jit.jit.cache_stats();
        let jit_second = jit.run_inner().expect("JIT ordinary rerun should succeed");
        let second_cache = jit.jit.cache_stats();

        assert_eq!(interpreter_first, "42\n");
        assert_eq!(jit_first, interpreter_first);
        assert_eq!(jit_first_steps, interpreter_first_steps);
        assert_eq!(jit_second, interpreter_second);
        assert_eq!(jit.instruction_steps, interpreter_second_steps);
        assert_eq!(interpreter_first_steps * 2, interpreter_second_steps);
        assert_eq!(first_cache.entries, 1);
        assert_eq!(second_cache, first_cache);
    }

    #[cfg(all(target_arch = "x86_64", unix))]
    #[test]
    fn jit_falls_back_to_interpreter_for_machine_instructions() {
        let program = ordinary_jit_call_program(
            Function {
                id: FuncId(1),
                local_count: 0,
                upvalues: Vec::new(),
                name: "machine_sum".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                registers: 3,
                params: Vec::new(),
                instructions: vec![
                    Instruction::IConst {
                        dest: 0,
                        width: MachineIntWidth::W32,
                        raw: 40,
                    },
                    Instruction::IConst {
                        dest: 1,
                        width: MachineIntWidth::W32,
                        raw: 2,
                    },
                    Instruction::IAdd {
                        dest: 2,
                        left: 0,
                        right: 1,
                        width: MachineIntWidth::W32,
                    },
                    Instruction::Return { value: 2 },
                ],
                locations: vec![None; 4],
            },
            Vec::new(),
        );

        let mut interpreter = VM::with_config(&program, RunConfig::unlimited());
        let interpreter_output = interpreter
            .run_inner()
            .expect("interpreter machine program should succeed");
        let interpreter_steps = interpreter.instruction_steps;

        let mut jit = VM::with_config(&program, RunConfig::unlimited());
        jit.jit = JitState::enabled_for_tests([1], 4096);
        let jit_output = jit
            .run_inner()
            .expect("machine program should fall back to the interpreter");

        assert_eq!(interpreter_output, "machine_int(42, 0x000000000000002a)\n");
        assert_eq!(jit_output, interpreter_output);
        assert_eq!(jit.instruction_steps, interpreter_steps);
        assert_eq!(jit.jit.cache_stats().entries, 0);
    }

    fn wide_scalar_call_program() -> Program {
        let main_instructions = vec![
            Instruction::BlockStart { id: BlockId(0) },
            Instruction::Constant {
                dest: 0,
                constant: 0,
            },
            Instruction::Constant {
                dest: 1,
                constant: 1,
            },
            Instruction::Constant {
                dest: 2,
                constant: 2,
            },
            Instruction::Constant {
                dest: 3,
                constant: 3,
            },
            Instruction::MakeFunction {
                dest: 4,
                function: FuncId(1),
            },
            Instruction::Br { target: BlockId(1) },
            Instruction::BlockStart { id: BlockId(1) },
            Instruction::Less {
                dest: 5,
                left: 0,
                right: 1,
            },
            Instruction::BrIf {
                condition: 5,
                if_true: BlockId(2),
                if_false: BlockId(3),
            },
            Instruction::BlockStart { id: BlockId(2) },
            Instruction::Call {
                dest: 6,
                callee: 4,
                arguments: vec![2],
            },
            Instruction::Move { dest: 2, source: 6 },
            Instruction::Add {
                dest: 7,
                left: 0,
                right: 3,
            },
            Instruction::Move { dest: 0, source: 7 },
            Instruction::Br { target: BlockId(1) },
            Instruction::BlockStart { id: BlockId(3) },
            Instruction::CallNative {
                dest: 8,
                native: NativeId(0),
                arguments: vec![2],
            },
            Instruction::Return { value: 2 },
        ];

        let mut function_instructions = vec![Instruction::LoadLocal { dest: 0, slot: 0 }];
        for index in 1..=32 {
            function_instructions.push(Instruction::Add {
                dest: index,
                left: index - 1,
                right: index - 1,
            });
        }
        function_instructions.push(Instruction::Return { value: 32 });

        Program {
            constants: vec![
                Constant::Number("0".to_string()),
                Constant::Number("20000".to_string()),
                Constant::Number("0".to_string()),
                Constant::Number("1".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: vec![NativeImport {
                name: "print".to_string(),
                abi: 1,
            }],
            modules: Vec::new(),
            names: vec!["value".to_string()],
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 9,
                    instructions: main_instructions,
                    locations: vec![None; 19],
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "wide_add".to_string(),
                    arity: 1,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 33,
                    params: vec!["value".to_string()],
                    instructions: function_instructions,
                    locations: vec![None; 34],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        }
    }

    #[cfg(all(target_arch = "x86_64", unix))]
    #[test]
    #[ignore = "manual JIT efficiency benchmark; run with -- --ignored --nocapture"]
    fn jit_efficiency_vs_interpreter_report() {
        use std::time::Instant;

        fn median(samples: &[f64]) -> f64 {
            let mut sorted = samples.to_vec();
            sorted.sort_by(|left, right| left.partial_cmp(right).expect("finite timings"));
            sorted[sorted.len() / 2]
        }

        fn measure(program: &Program, jit: bool, repeats: usize) -> (f64, f64, usize) {
            let mut cold_samples = Vec::new();
            let mut warm_samples = Vec::new();
            let mut steps = None;
            let mut expected: Option<String> = None;
            for _ in 0..repeats {
                let mut vm = VM::with_config(program, RunConfig::unlimited());
                if jit {
                    vm.jit = JitState::enabled_for_tests([1], 1 << 20);
                }
                let before = vm.instruction_steps;
                let start = Instant::now();
                let cold_output = vm.run_inner().expect("cold run should succeed");
                cold_samples.push(start.elapsed().as_secs_f64() * 1e3);
                let cold_steps = vm.instruction_steps - before;
                if let Some(expected) = &expected {
                    assert_eq!(&cold_output, expected);
                } else {
                    expected = Some(cold_output);
                }
                if let Some(steps) = &steps {
                    assert_eq!(&cold_steps, steps);
                } else {
                    steps = Some(cold_steps);
                }

                let before = vm.instruction_steps;
                let start = Instant::now();
                let warm_output = vm.run_inner().expect("warm run should succeed");
                warm_samples.push(start.elapsed().as_secs_f64() * 1e3);
                assert_eq!(&warm_output, expected.as_ref().expect("expected output"));
                assert_eq!(vm.instruction_steps - before, cold_steps);
            }
            (
                median(&cold_samples),
                median(&warm_samples),
                steps.expect("steps"),
            )
        }

        let programs: Vec<(&str, Program)> = vec![
            (
                "execution_closure (real fixture, 50000 closure calls)",
                execution_closure_program(),
            ),
            (
                "wide_scalar (synthetic, 20000 calls x 32 scalar ops)",
                wide_scalar_call_program(),
            ),
        ];

        println!("JIT efficiency report (wall-clock medians over 7 runs)");
        for (label, program) in &programs {
            let (cold_interp, warm_interp, interp_steps) = measure(program, false, 7);
            let (cold_jit, warm_jit, jit_steps) = measure(program, true, 7);
            assert_eq!(interp_steps, jit_steps);
            println!(
                "\n{label}\n  interpreter: cold {cold_interp:.3} ms, warm {warm_interp:.3} ms ({} steps)",
                interp_steps
            );
            println!(
                "  jit:         cold {cold_jit:.3} ms, warm {warm_jit:.3} ms ({} steps)",
                jit_steps
            );
            println!(
                "  speedup (interp / jit): cold {:.3}x, warm {:.3}x",
                cold_interp / cold_jit,
                warm_interp / warm_jit
            );
        }
    }

    #[cfg(all(target_arch = "x86_64", unix))]
    #[test]
    fn jit_run_matches_interpreter_runtime_error_for_ordinary_call() {
        let program = ordinary_jit_call_program(
            Function {
                id: FuncId(1),
                local_count: 0,
                upvalues: Vec::new(),
                name: "divide".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                registers: 3,
                params: Vec::new(),
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Constant {
                        dest: 1,
                        constant: 1,
                    },
                    Instruction::Divide {
                        dest: 2,
                        left: 0,
                        right: 1,
                    },
                    Instruction::Return { value: 2 },
                ],
                locations: vec![None; 4],
            },
            vec![
                Constant::Number("1".to_string()),
                Constant::Number("0".to_string()),
            ],
        );

        let mut interpreter = VM::with_config(&program, RunConfig::unlimited());
        let interpreter_error = interpreter
            .run_inner()
            .expect_err("interpreter ordinary run should fail");

        let mut jit = VM::with_config(&program, RunConfig::unlimited());
        jit.jit = JitState::enabled_for_tests([1], 4096);
        let jit_error = jit.run_inner().expect_err("JIT ordinary run should fail");

        assert_eq!(jit_error.kind, interpreter_error.kind);
        assert_eq!(jit_error.resource_limit, interpreter_error.resource_limit);
        assert_eq!(jit_error.message, interpreter_error.message);
        assert_eq!(jit_error.location, interpreter_error.location);
        assert_eq!(jit_error.stack, interpreter_error.stack);
        assert_eq!(jit.instruction_steps, interpreter.instruction_steps);
        assert_eq!(jit.jit.cache_stats().entries, 1);
    }

    #[cfg(all(target_arch = "x86_64", unix))]
    #[test]
    fn jit_runs_the_real_execution_closure_captured_function() {
        let program = execution_closure_program();

        let mut interpreter = VM::with_config(&program, RunConfig::unlimited());
        let interpreter_output = interpreter
            .run_inner()
            .expect("interpreter execution_closure should succeed");
        let interpreter_steps = interpreter.instruction_steps;

        let mut jit = VM::with_config(&program, RunConfig::unlimited());
        jit.jit = JitState::enabled_for_tests([1], 4096);
        let jit_output = jit
            .run_inner()
            .expect("JIT execution_closure should succeed");

        assert_eq!(interpreter_output, "50000\n");
        assert_eq!(jit_output, interpreter_output);
        assert_eq!(jit.instruction_steps, interpreter_steps);
        assert_eq!(jit.jit.cache_stats().entries, 1);
    }

    #[cfg(all(target_arch = "x86_64", unix))]
    #[test]
    fn jit_entry_executes_a_whitelisted_function_with_frame_registers() {
        let program = Program {
            constants: Vec::new(),
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: vec!["left".to_string(), "right".to_string()],
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "add".to_string(),
                    arity: 2,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 3,
                    params: vec!["left".to_string(), "right".to_string()],
                    instructions: vec![
                        Instruction::LoadLocal { dest: 0, slot: 0 },
                        Instruction::LoadLocal { dest: 1, slot: 1 },
                        Instruction::Add {
                            dest: 2,
                            left: 0,
                            right: 1,
                        },
                        Instruction::Return { value: 2 },
                    ],
                    locations: vec![None; 4],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        vm.jit = JitState::enabled_for_tests([1], 4096);
        let function = FunctionValue {
            name: "add".to_string(),
            function_index: 1,
            arity: 2,
            identity: 0,
            closure: vm.heap.new_environment(),
            upvalues: Vec::new(),
        };

        let first = vm
            .call_function(
                &function,
                CallArguments::Two(Value::number(2.0), Value::number(3.0)),
                false,
                "main",
                None,
            )
            .expect("generated function should execute");
        let second = vm
            .call_function(
                &function,
                CallArguments::Two(Value::number(7.0), Value::number(8.0)),
                false,
                "main",
                None,
            )
            .expect("cached generated function should execute");

        assert_eq!(first.to_string(), "5");
        assert_eq!(second.to_string(), "15");
        assert_eq!(vm.instruction_steps, 8);
    }

    #[cfg(all(target_arch = "x86_64", unix))]
    #[test]
    fn jit_protocol_failure_restores_the_entry_snapshot_for_interpreter_fallback() {
        let instructions = vec![
            Instruction::Constant {
                dest: 0,
                constant: 0,
            },
            Instruction::Constant {
                dest: 1,
                constant: 1,
            },
            Instruction::Add {
                dest: 2,
                left: 0,
                right: 1,
            },
            Instruction::Return { value: 2 },
        ];
        let program = Program {
            constants: vec![
                Constant::Number("1".to_string()),
                Constant::Number("2".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "protocol_failure".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 3,
                    params: Vec::new(),
                    instructions: instructions.clone(),
                    locations: vec![None; instructions.len()],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        vm.jit = JitState::enabled_for_tests([1], 4096);
        let mut frame = Frame::callee(
            Rc::new(Function {
                id: FuncId(0),
                name: String::new(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),

                registers: 3,
                instructions,
                locations: vec![None; 4],
            }),
            "protocol_failure",
            0,
            1,
            vm.heap.new_local_slots(),
            vm.heap.new_environment(),
            ReturnTarget {
                register: 0,
                call_site: None,
            },
        );
        frame.ip = 9;
        frame.registers[0] = Value::string("before JIT");

        assert!(matches!(
            vm.execute_jit_function(0, &mut frame, &[], None),
            Ok(JitCallOutcome::Fallback)
        ));
        assert_eq!(frame.ip, 9);
        assert_eq!(frame.registers.len(), 1);
        assert_eq!(frame.registers[0].to_string(), "before JIT");
        assert_eq!(vm.instruction_steps, 0);
    }

    #[cfg(all(target_arch = "x86_64", unix))]
    #[test]
    fn jit_entry_transports_checkpoint_and_runtime_errors_like_the_interpreter() {
        let program = Program {
            constants: vec![
                Constant::Number("1".to_string()),
                Constant::Number("0".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![
                Function {
                    id: FuncId(0),
                    name: "main".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    local_count: 0,
                    upvalues: Vec::new(),
                    params: Vec::new(),
                    registers: 0,
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    id: FuncId(1),
                    local_count: 0,
                    upvalues: Vec::new(),
                    name: "divide".to_string(),
                    arity: 0,
                    machine_frame_size: 0,
                    machine_params: Vec::new(),
                    machine_return: None,
                    registers: 3,
                    params: Vec::new(),
                    instructions: vec![
                        Instruction::Constant {
                            dest: 0,
                            constant: 0,
                        },
                        Instruction::Constant {
                            dest: 1,
                            constant: 1,
                        },
                        Instruction::Divide {
                            dest: 2,
                            left: 0,
                            right: 1,
                        },
                        Instruction::Return { value: 2 },
                    ],
                    locations: vec![None; 4],
                },
            ],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let function = || FunctionValue {
            name: "divide".to_string(),
            function_index: 1,
            arity: 0,
            identity: 0,
            closure: new_environment(),
            upvalues: Vec::new(),
        };
        let mut config = RunConfig::unlimited();
        config.max_instruction_steps = Some(2);
        let mut jit_vm = VM::with_config(&program, config.clone());
        jit_vm.jit = JitState::enabled_for_tests([1], 4096);
        let jit_function = function();
        let jit_error = jit_vm
            .call_function(&jit_function, CallArguments::Empty, false, "main", None)
            .expect_err("the JIT checkpoint should enforce the step limit");

        let mut interpreter_vm = VM::with_config(&program, config);
        let interpreter_function = function();
        let interpreter_error = interpreter_vm
            .call_function(
                &interpreter_function,
                CallArguments::Empty,
                false,
                "main",
                None,
            )
            .expect_err("the interpreter should enforce the same step limit");

        assert_eq!(jit_error.kind, interpreter_error.kind);
        assert_eq!(jit_error.resource_limit, interpreter_error.resource_limit);
        assert_eq!(jit_error.message, interpreter_error.message);
        assert_eq!(jit_vm.instruction_steps, interpreter_vm.instruction_steps);
    }

    #[test]
    fn jit_helper_bridge_dispatches_existing_value_semantics() {
        let program = Program {
            constants: vec![
                Constant::Number("2".to_string()),
                Constant::String("left".to_string()),
                Constant::String("right".to_string()),
            ],
            data_segments: Vec::new(),
            symbols: Vec::new(),
            relocations: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: vec!["value".to_string()],
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let locals = vm.heap.new_local_slots();
        locals
            .borrow_mut()
            .push(Some(vm.heap.new_cell(Value::number(4.0))));
        let mut frame = Frame::callee(
            Rc::new(Function {
                id: FuncId(0),
                name: String::new(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),

                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            }),
            "jit_helper",
            0,
            0,
            locals,
            vm.heap.new_environment(),
            ReturnTarget {
                register: 0,
                call_site: None,
            },
        );
        frame.variable_plan = Some(Rc::new(VariablePlan {
            local_names: vec!["value".to_string()],
        }));

        let mut bridge = vm.jit_helper_bridge(&mut frame, None);
        let number = bridge
            .dispatch(RuntimeHelper::Constant, &[0])
            .expect("constant helper should return a handle");
        let loaded = bridge
            .dispatch(RuntimeHelper::LoadLocal, &[0])
            .expect("load local helper should return a handle");
        let sum = bridge
            .dispatch(RuntimeHelper::Add, &[number, loaded])
            .expect("add helper should return a handle");
        assert_eq!(bridge.value(sum).unwrap().to_string(), "6");

        let negated = bridge
            .dispatch(RuntimeHelper::Negate, &[sum])
            .expect("negate helper should return a handle");
        assert_eq!(bridge.value(negated).unwrap().to_string(), "-6");
        let difference = bridge
            .dispatch(RuntimeHelper::Subtract, &[loaded, number])
            .expect("subtract helper should return a handle");
        assert_eq!(bridge.value(difference).unwrap().to_string(), "2");
        let product = bridge
            .dispatch(RuntimeHelper::Multiply, &[number, loaded])
            .expect("multiply helper should return a handle");
        assert_eq!(bridge.value(product).unwrap().to_string(), "8");
        let quotient = bridge
            .dispatch(RuntimeHelper::Divide, &[loaded, number])
            .expect("divide helper should return a handle");
        assert_eq!(bridge.value(quotient).unwrap().to_string(), "2");

        let left = bridge
            .dispatch(RuntimeHelper::Constant, &[1])
            .expect("left string constant should return a handle");
        let right = bridge
            .dispatch(RuntimeHelper::Constant, &[2])
            .expect("right string constant should return a handle");
        let joined = bridge
            .dispatch(RuntimeHelper::Add, &[left, right])
            .expect("string add helper should return a handle");
        assert_eq!(bridge.value(joined).unwrap().to_string(), "leftright");

        let nil = bridge
            .handle(Value::Nil)
            .expect("nil should fit in the bridge handle table");
        let truthiness = bridge
            .dispatch(RuntimeHelper::Not, &[nil])
            .expect("not helper should return a handle");
        assert_eq!(bridge.value(truthiness).unwrap().to_string(), "true");

        for (helper, expected) in [
            (RuntimeHelper::Equal, "false"),
            (RuntimeHelper::NotEqual, "true"),
            (RuntimeHelper::Greater, "false"),
            (RuntimeHelper::GreaterEqual, "false"),
            (RuntimeHelper::Less, "true"),
            (RuntimeHelper::LessEqual, "true"),
        ] {
            let result = bridge
                .dispatch(helper, &[number, loaded])
                .expect("comparison helper should return a handle");
            assert_eq!(bridge.value(result).unwrap().to_string(), expected);
        }
    }

    #[test]
    fn jit_helper_bridge_rejects_bad_handles_and_preserves_temporary_roots() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let mut frame = Frame::main(
            Rc::new(Function {
                id: FuncId(0),
                name: String::new(),
                arity: 0,
                machine_frame_size: 0,
                machine_params: Vec::new(),
                machine_return: None,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),

                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            }),
            0,
            vm.heap.new_local_slots(),
            vm.heap.new_environment(),
        );
        let array = vm.make_array(vec![Value::number(7.0)]);
        let mut bridge = vm.jit_helper_bridge(&mut frame, None);
        let array_handle = bridge
            .handle(array)
            .expect("array should fit in the bridge handle table");
        assert_eq!(bridge.vm.heap.collect_garbage(), 0);
        assert_eq!(bridge.value(array_handle).unwrap().to_string(), "[7]");

        let wrong_arity = bridge
            .dispatch(RuntimeHelper::Add, &[array_handle])
            .expect_err("wrong helper arity should be rejected");
        assert_eq!(
            wrong_arity.message,
            "JIT helper 3 expects 2 operands, got 1"
        );
        let invalid_handle = bridge
            .dispatch(RuntimeHelper::Not, &[99])
            .expect_err("unknown value handles should be rejected");
        assert_eq!(
            invalid_handle.message,
            "JIT value handle 99 is out of range"
        );
        let invalid_constant = bridge
            .dispatch(RuntimeHelper::Constant, &[99])
            .expect_err("unknown constant indices should be rejected");
        assert_eq!(invalid_constant.message, "constant index out of range");

        let zero = bridge
            .handle(Value::number(0.0))
            .expect("zero should fit in the bridge handle table");
        let one = bridge
            .handle(Value::number(1.0))
            .expect("one should fit in the bridge handle table");
        let division = bridge
            .dispatch(RuntimeHelper::Divide, &[one, zero])
            .expect_err("division by zero should preserve interpreter semantics");
        assert_eq!(division.message, "division by zero");
    }
}

impl RuntimeError {
    fn new(message: impl Into<String>) -> Self {
        Self {
            kind: RuntimeErrorKind::Runtime,
            resource_limit: None,
            message: message.into(),
            location: None,
            stack: Vec::new(),
            sources: Vec::new(),
        }
    }

    fn invalid_address(message: impl Into<String>) -> Self {
        let mut error = Self::new(message);
        error.kind = RuntimeErrorKind::InvalidAddress;
        error
    }

    fn invalid_instruction(message: impl Into<String>) -> Self {
        let mut error = Self::new(message);
        error.kind = RuntimeErrorKind::InvalidInstruction;
        error
    }

    fn invalid_operand(message: impl Into<String>) -> Self {
        let mut error = Self::new(message);
        error.kind = RuntimeErrorKind::InvalidOperand;
        error
    }

    fn integer_division_by_zero(message: impl Into<String>) -> Self {
        let mut error = Self::new(message);
        error.kind = RuntimeErrorKind::IntegerDivisionByZero;
        error
    }

    fn integer_division_overflow(message: impl Into<String>) -> Self {
        let mut error = Self::new(message);
        error.kind = RuntimeErrorKind::IntegerDivisionOverflow;
        error
    }

    fn invalid_shift_amount(message: impl Into<String>) -> Self {
        let mut error = Self::new(message);
        error.kind = RuntimeErrorKind::InvalidShiftAmount;
        error
    }

    fn invalid_conversion(message: impl Into<String>) -> Self {
        let mut error = Self::new(message);
        error.kind = RuntimeErrorKind::InvalidConversion;
        error
    }

    fn invalid_memory_operation(message: impl Into<String>) -> Self {
        let mut error = Self::new(message);
        error.kind = RuntimeErrorKind::InvalidMemoryOperation;
        error
    }

    fn stack_overflow(requested: u64, available: u64) -> Self {
        let mut error = Self::new(format!(
            "machine stack overflow: requested frame of {} bytes with {} bytes available",
            requested, available
        ));
        error.kind = RuntimeErrorKind::StackOverflow;
        error
    }

    fn resource(kind: ResourceKind, limit: usize) -> Self {
        Self {
            kind: RuntimeErrorKind::Resource(kind),
            resource_limit: Some(limit),
            message: format!(
                "resource limit exceeded: {} (limit {})",
                kind.as_str(),
                limit
            ),
            location: None,
            stack: Vec::new(),
            sources: Vec::new(),
        }
    }

    fn cancelled() -> Self {
        Self {
            kind: RuntimeErrorKind::Cancelled,
            resource_limit: None,
            message: "execution cancelled".to_string(),
            location: None,
            stack: Vec::new(),
            sources: Vec::new(),
        }
    }

    fn debug_quit() -> Self {
        Self {
            kind: RuntimeErrorKind::DebuggerQuit,
            resource_limit: None,
            message: "debugger session quit".to_string(),
            location: None,
            stack: Vec::new(),
            sources: Vec::new(),
        }
    }

    fn push_frame(&mut self, function: String, location: Option<DebugLocation>) {
        self.stack.push(StackFrame { function, location });
    }

    fn source_location(&self, location: &DebugLocation) -> Option<(&DebugSource, &str)> {
        let source = self.sources.get(location.source)?;
        if location.line == 0 || location.column == 0 {
            return None;
        }
        let line = source.text.split('\n').nth(location.line - 1)?;
        let line = line.strip_suffix('\r').unwrap_or(line);
        if location.column > line.len() + 1 {
            return None;
        }
        Some((source, line))
    }
}

impl From<MemoryError> for RuntimeError {
    fn from(error: MemoryError) -> Self {
        let kind = match error.kind() {
            MemoryErrorKind::MemoryOutOfBounds => RuntimeErrorKind::MemoryOutOfBounds,
            MemoryErrorKind::NullPointerAccess => RuntimeErrorKind::NullPointerAccess,
            MemoryErrorKind::WriteToReadOnlyMemory => RuntimeErrorKind::WriteToReadOnlyMemory,
            MemoryErrorKind::InvalidAddress => RuntimeErrorKind::InvalidAddress,
            MemoryErrorKind::InvalidMemoryOperation => RuntimeErrorKind::InvalidMemoryOperation,
            MemoryErrorKind::InvalidAlignment
            | MemoryErrorKind::RegionOverlap
            | MemoryErrorKind::AllocationFailure => RuntimeErrorKind::Runtime,
        };
        let mut runtime_error = RuntimeError::new(error.to_string());
        runtime_error.kind = kind;
        runtime_error
    }
}

impl fmt::Display for RuntimeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut output = String::new();
        if let Some(location) = self.location.as_ref() {
            if let Some((source, line)) = self.source_location(location) {
                output.push_str(&format!(
                    "Runtime error at {}:{}:{}: {}\n",
                    source.path, location.line, location.column, self.message
                ));
                output.push_str(&format!("  {}\n", line));
                output.push_str(&format!(
                    "  {}^",
                    " ".repeat(location.column.saturating_sub(1))
                ));
            } else {
                output.push_str(&format!("Runtime error: {}", self.message));
            }
        } else {
            output.push_str(&format!("Runtime error: {}", self.message));
        }

        let valid_frames = self
            .stack
            .iter()
            .filter_map(|frame| frame.location.as_ref().map(|location| (frame, location)))
            .filter(|(_, location)| self.source_location(location).is_some())
            .collect::<Vec<_>>();
        if !valid_frames.is_empty() {
            output.push_str("\nCall stack:\n");
            for (index, (frame, location)) in valid_frames.iter().enumerate() {
                let source = self
                    .sources
                    .get(location.source)
                    .expect("validated debug source");
                if index != 0 {
                    output.push('\n');
                }
                output.push_str(&format!(
                    "  at {} ({}:{}:{})",
                    frame.function, source.path, location.line, location.column
                ));
            }
        }
        write!(f, "{}", output)
    }
}

/// A host-selected entry point for one cooperative VM task.
///
/// `Main` starts a fresh copy of the program entry body. `Function` starts a
/// verified bytecode function with a fresh empty closure environment; global
/// variables still use the session's shared global environment. Language-level
/// task syntax and closure handles remain outside this host-only API.
#[derive(Clone, Debug)]
pub enum TaskSpec {
    Main,
    Function { index: usize, arguments: Vec<Value> },
}

impl TaskSpec {
    pub fn main() -> Self {
        Self::Main
    }

    pub fn function(index: usize, arguments: Vec<Value>) -> Self {
        Self::Function { index, arguments }
    }
}

/// Errors raised while manipulating a cooperative task session.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TaskControlError {
    InvalidQuantum,
    TaskIdOverflow,
    UnknownTask(TaskId),
    TaskNotBlocked(TaskId),
    TaskNotJoinable(TaskId),
    TaskAlreadyWaiting(TaskId),
    SelfJoin(TaskId),
    MissingOutcome(TaskId),
}

impl fmt::Display for TaskControlError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidQuantum => write!(formatter, "scheduler quantum must be positive"),
            Self::TaskIdOverflow => write!(formatter, "scheduler task id exhausted"),
            Self::UnknownTask(task_id) => write!(formatter, "unknown {}", task_id),
            Self::TaskNotBlocked(task_id) => write!(formatter, "{} is not blocked", task_id),
            Self::TaskNotJoinable(task_id) => {
                write!(formatter, "{} cannot wait for a join", task_id)
            }
            Self::TaskAlreadyWaiting(task_id) => {
                write!(formatter, "{} is already waiting", task_id)
            }
            Self::SelfJoin(task_id) => write!(formatter, "{} cannot join itself", task_id),
            Self::MissingOutcome(task_id) => {
                write!(formatter, "{} has no terminal outcome", task_id)
            }
        }
    }
}

impl std::error::Error for TaskControlError {}

impl From<SchedulerError> for TaskControlError {
    fn from(error: SchedulerError) -> Self {
        match error {
            SchedulerError::InvalidQuantum => Self::InvalidQuantum,
            SchedulerError::TaskIdOverflow => Self::TaskIdOverflow,
            SchedulerError::UnknownTask(task_id) => Self::UnknownTask(task_id),
            SchedulerError::TaskNotBlocked(task_id) => Self::TaskNotBlocked(task_id),
            SchedulerError::TaskNotJoinable(task_id) => Self::TaskNotJoinable(task_id),
            SchedulerError::TaskAlreadyWaiting(task_id) => Self::TaskAlreadyWaiting(task_id),
            SchedulerError::SelfJoin(task_id) => Self::SelfJoin(task_id),
        }
    }
}

/// The terminal result retained for one cooperative task.
#[derive(Clone, Debug)]
pub enum TaskOutcome {
    Completed(Value),
    Failed(RuntimeError),
    Cancelled,
}

/// One committed output chunk from a cooperative task.
///
/// `sequence` is monotonic for the lifetime of the cooperative session, even
/// when the host drains buffered output or previously reported events.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TaskOutputEvent {
    pub sequence: usize,
    pub task_id: TaskId,
    pub text: String,
}

/// One trace observation from a cooperative task.
///
/// Sequence numbers share the cooperative session's observable event order
/// with `TaskOutputEvent`. An output trace record therefore has the same
/// sequence as the committed output chunk it describes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TaskTraceEvent {
    pub sequence: usize,
    pub task_id: TaskId,
    pub kind: TraceEventKind,
    pub function: String,
    pub instruction: Option<usize>,
    pub location: Option<DebugLocation>,
    pub stack: Vec<StackFrame>,
    pub locals: Vec<(String, String)>,
    pub value: Option<String>,
}

/// Result of one explicit scheduler dispatch.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CooperativeStep {
    Dispatched { task_id: TaskId, state: TaskState },
    Waiting,
    Complete,
}

/// Result of a host join registration or query.
#[derive(Clone, Debug)]
pub enum JoinPoll {
    Waiting,
    Ready(TaskOutcome),
}

struct PreparedFunction {
    name: Rc<str>,
    params: Vec<String>,
    body: Rc<Function>,
    variable_plan: Rc<VariablePlan>,
}

#[derive(Clone, Copy)]
enum Comparison {
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
}

#[derive(Clone, Copy)]
enum MachineShiftKind {
    Left,
    LogicalRight,
    ArithmeticRight,
}

#[derive(Clone, Copy)]
enum MachineFloatBinaryKind {
    Add,
    Subtract,
    Multiply,
    Divide,
}

impl Comparison {
    fn as_str(self) -> &'static str {
        match self {
            Self::Greater => "greater",
            Self::GreaterEqual => "greater_equal",
            Self::Less => "less",
            Self::LessEqual => "less_equal",
        }
    }

    fn apply_numbers(self, left: f64, right: f64) -> bool {
        match self {
            Self::Greater => left > right,
            Self::GreaterEqual => left >= right,
            Self::Less => left < right,
            Self::LessEqual => left <= right,
        }
    }
}

enum InstructionCheckpoint {
    Limited(usize),
    Unlimited,
    CancelledLimited {
        limit: usize,
        token: CancellationToken,
    },
    CancelledUnlimited {
        token: CancellationToken,
    },
}

enum CallArguments {
    Empty,
    One(Value),
    Two(Value, Value),
    Many(Vec<Value>),
}

struct CallRequest {
    dest: usize,
    function: FunctionValue,
    arguments: CallArguments,
    direct: bool,
    caller: String,
    call_site: Option<DebugLocation>,
}

enum InstructionAction {
    Continue,
    Jumped,
    Call(CallRequest),
    Return(Value),
}

struct MachineStackFrame {
    base: VmAddress,
    size: u64,
    cursor_before: u64,
}

struct MachineStack {
    region: MemoryRegion,
    capacity: u64,
    cursor: u64,
    frames: Vec<MachineStackFrame>,
}

impl MachineStack {
    fn new(memory: &mut LinearMemory, capacity: usize) -> Result<Self, RuntimeError> {
        let capacity = u64::try_from(capacity)
            .map_err(|_| RuntimeError::new("machine stack capacity is too large"))?;
        let region = memory
            .allocate_machine_stack(capacity)
            .map_err(RuntimeError::from)?;
        Ok(Self {
            region,
            capacity,
            cursor: 0,
            frames: Vec::new(),
        })
    }

    fn allocate_frame(
        &mut self,
        memory: &mut LinearMemory,
        frame: &mut Frame,
    ) -> Result<(), RuntimeError> {
        if frame.machine_frame_base.is_some() {
            return Err(RuntimeError::new("machine frame is already allocated"));
        }
        let cursor_before = self.cursor;
        let aligned_cursor = align_machine_stack_offset(cursor_before)?;
        let frame_end = aligned_cursor
            .checked_add(frame.machine_frame_size)
            .ok_or_else(|| RuntimeError::invalid_address("machine frame offset overflow"))?;
        if frame_end > self.capacity {
            return Err(RuntimeError::stack_overflow(
                frame.machine_frame_size,
                self.capacity.saturating_sub(aligned_cursor),
            ));
        }
        let base = self
            .region
            .base
            .checked_add(aligned_cursor)
            .ok_or_else(|| RuntimeError::invalid_address("machine frame base overflow"))?;
        memory
            .activate_stack_frame(self.region.base, base, frame.machine_frame_size)
            .map_err(RuntimeError::from)?;
        self.frames.push(MachineStackFrame {
            base,
            size: frame.machine_frame_size,
            cursor_before,
        });
        self.cursor = frame_end;
        frame.machine_frame_base = Some(base);
        Ok(())
    }

    fn release_frame(
        &mut self,
        memory: &mut LinearMemory,
        base: Option<VmAddress>,
        size: u64,
    ) -> Result<(), RuntimeError> {
        let Some(base) = base else {
            return Ok(());
        };
        let Some(active) = self.frames.last() else {
            return Err(RuntimeError::new("machine frame stack is empty"));
        };
        if active.base != base || active.size != size {
            return Err(RuntimeError::new("machine frame release is not LIFO"));
        }
        if active.size != 0 {
            memory
                .release_stack_frame(self.region.base, active.base, active.size)
                .map_err(RuntimeError::from)?;
        }
        let Some(active) = self.frames.pop() else {
            return Err(RuntimeError::invalid_instruction(
                "machine frame stack became empty during release",
            ));
        };
        self.cursor = active.cursor_before;
        Ok(())
    }

    fn release_all(&mut self, memory: &mut LinearMemory) {
        while let Some(active) = self.frames.pop() {
            if active.size != 0 {
                let _ = memory.release_stack_frame(self.region.base, active.base, active.size);
            }
            self.cursor = active.cursor_before;
        }
    }
}

fn align_machine_stack_offset(offset: u64) -> Result<u64, RuntimeError> {
    let padding = (8 - (offset & 7)) & 7;
    offset
        .checked_add(padding)
        .ok_or_else(|| RuntimeError::invalid_address("machine stack alignment overflow"))
}

#[derive(Default)]
struct TaskTraceState {
    started: bool,
    stack: Vec<StackFrame>,
    last_locations: Vec<Option<DebugLocation>>,
}

struct ActiveTaskTrace {
    task_id: TaskId,
    state: TaskTraceState,
}

#[derive(Default)]
struct TaskProfileState {
    started: bool,
    instruction_count: usize,
    output_bytes: usize,
    functions: Vec<ProfileFunction>,
    natives: BTreeMap<String, usize>,
    source_ranges: BTreeMap<(usize, usize, usize), usize>,
}

impl TaskProfileState {
    fn enabled(program: &Program) -> Self {
        Self {
            functions: empty_profile_functions(program),
            ..Self::default()
        }
    }

    fn function_entry(&mut self, frame: &Frame) {
        let index = profile_function_index(frame);
        if let Some(function) = self.functions.get_mut(index) {
            function.calls = function.calls.saturating_add(1);
        }
    }

    fn instruction(&mut self, frame: &Frame, location: Option<&DebugLocation>) {
        self.instruction_count = self.instruction_count.saturating_add(1);
        let index = profile_function_index(frame);
        if let Some(function) = self.functions.get_mut(index) {
            function.instructions = function.instructions.saturating_add(1);
        }
        if let Some(range) = location.and_then(|location| location.range.as_ref()) {
            let key = (range.source, range.start, range.end);
            let hits = self.source_ranges.entry(key).or_insert(0);
            *hits = hits.saturating_add(1);
        }
    }

    fn native_call(&mut self, name: &str) {
        let calls = self.natives.entry(name.to_string()).or_insert(0);
        *calls = calls.saturating_add(1);
    }

    fn output(&mut self, bytes: usize) {
        self.output_bytes = self.output_bytes.saturating_add(bytes);
    }

    fn report(&self, task_id: TaskId) -> TaskProfileReport {
        TaskProfileReport {
            task_id,
            instruction_count: self.instruction_count,
            output_bytes: self.output_bytes,
            functions: self.functions.clone(),
            natives: self
                .natives
                .iter()
                .map(|(name, calls)| ProfileNative {
                    name: name.clone(),
                    calls: *calls,
                })
                .collect(),
            source_ranges: self
                .source_ranges
                .iter()
                .map(|((source, start, end), hits)| ProfileSourceRange {
                    range: DebugRange {
                        source: *source,
                        start: *start,
                        end: *end,
                    },
                    hits: *hits,
                })
                .collect(),
        }
    }
}

struct ActiveTaskProfile {
    task_id: TaskId,
    state: TaskProfileState,
}

fn profile_function_index(frame: &Frame) -> usize {
    frame.function_index.unwrap_or(0)
}

fn empty_profile_functions(program: &Program) -> Vec<ProfileFunction> {
    std::iter::once(ProfileFunction {
        index: None,
        name: "main".to_string(),
        calls: 0,
        instructions: 0,
    })
    .chain(
        program
            .functions
            .iter()
            .enumerate()
            .filter(|(position, _)| *position as u32 != program.entry.0)
            .map(|(_, function)| ProfileFunction {
                index: Some((function.id.0 as usize).saturating_sub(1)),
                name: function.name.clone(),
                calls: 0,
                instructions: 0,
            }),
    )
    .collect()
}

struct ScheduledVmTask {
    frames: FrameStack,
    machine_stack: Option<MachineStack>,
    result: Option<Value>,
    error: Option<RuntimeError>,
    trace: TaskTraceState,
    profile: TaskProfileState,
}

impl ScheduledVmTask {
    fn release_frames(&mut self) {
        self.frames.clear();
        self.trace = TaskTraceState::default();
    }
}

impl CallArguments {
    fn len(&self) -> usize {
        match self {
            Self::Empty => 0,
            Self::One(_) => 1,
            Self::Two(_, _) => 2,
            Self::Many(arguments) => arguments.len(),
        }
    }

    fn values(&self) -> Vec<Value> {
        match self {
            Self::Empty => Vec::new(),
            Self::One(value) => vec![value.clone()],
            Self::Two(first, second) => vec![first.clone(), second.clone()],
            Self::Many(values) => values.clone(),
        }
    }
}

enum NativeArguments {
    Empty,
    One(Value),
    Two(Value, Value),
    Many(Vec<Value>),
}

impl NativeArguments {
    fn from_vec(arguments: Vec<Value>) -> Self {
        match arguments.len() {
            0 => Self::Empty,
            1 => Self::One(arguments.into_iter().next().expect("one native argument")),
            2 => {
                let mut arguments = arguments.into_iter();
                Self::Two(
                    arguments.next().expect("first native argument"),
                    arguments.next().expect("second native argument"),
                )
            }
            _ => Self::Many(arguments),
        }
    }

    fn len(&self) -> usize {
        match self {
            Self::Empty => 0,
            Self::One(_) => 1,
            Self::Two(_, _) => 2,
            Self::Many(arguments) => arguments.len(),
        }
    }

    fn is_empty(&self) -> bool {
        matches!(self, Self::Empty)
    }
}

impl Index<usize> for NativeArguments {
    type Output = Value;

    fn index(&self, index: usize) -> &Self::Output {
        match self {
            Self::Empty => panic!("native argument index {} out of bounds", index),
            Self::One(argument) if index == 0 => argument,
            Self::Two(first, _) if index == 0 => first,
            Self::Two(_, second) if index == 1 => second,
            Self::Many(arguments) => &arguments[index],
            _ => panic!("native argument index {} out of bounds", index),
        }
    }
}

pub struct VM<'a> {
    program: &'a Program,
    config: RunConfig,
    verified: bool,
    instruction_checkpoint: InstructionCheckpoint,
    heap: Heap,
    memory: LinearMemory,
    initialization_error: Option<RuntimeError>,
    direct_call_relocations: BTreeMap<(usize, usize), FuncId>,
    machine_stack: Option<MachineStack>,
    global_cells: Vec<Option<Cell>>,
    global_names: Vec<String>,
    decoded_constants: Vec<Value>,
    constant_errors: BTreeMap<usize, RuntimeError>,
    native_specs: Vec<Option<&'static NativeSpec>>,
    module_states: Vec<ModuleState>,
    prepared_functions: Vec<Rc<PreparedFunction>>,
    block_maps: Vec<BTreeMap<u32, usize>>,
    jit: JitState,
    output: String,
    output_bytes: usize,
    task_output_events: Vec<TaskOutputEvent>,
    task_trace_enabled: bool,
    task_trace_collect_events: bool,
    task_trace_events: Vec<TaskTraceEvent>,
    active_cooperative_task: Option<TaskId>,
    active_task_trace: Option<ActiveTaskTrace>,
    active_task_profile: Option<ActiveTaskProfile>,
    cooperative_debug_hook: Option<Box<dyn CooperativeDebugHook + 'a>>,
    active_cooperative_debug_state: Option<CooperativeDebugState>,
    cooperative_debug_quit: bool,
    next_task_event_sequence: usize,
    instruction_steps: usize,
    call_depth: usize,
    runtime_elements: usize,
    trace_enabled: bool,
    trace_collect_events: bool,
    trace_events: Vec<TraceEvent>,
    trace_stack: Vec<StackFrame>,
    trace_last_locations: Vec<Option<DebugLocation>>,
    debug_hook: Option<Box<dyn DebugHook + 'a>>,
    profile_enabled: bool,
    profile_instruction_count: usize,
    profile_output_bytes: usize,
    profile_functions: Vec<ProfileFunction>,
    profile_natives: BTreeMap<String, usize>,
    profile_source_ranges: BTreeMap<(usize, usize, usize), usize>,
}

/// The result of crossing a JIT safepoint back into VM-owned state.
///
/// Materialization is performed before the shared instruction/native
/// checkpoint. This keeps the frame and its values available even when
/// cancellation or a resource limit rejects the transition.
#[allow(dead_code)]
#[derive(Debug)]
struct JitSafepointTransition {
    frame: JitFrameMaterialization,
    checkpoint: Result<(), RuntimeError>,
}

enum JitCallOutcome {
    Executed(Value),
    Fallback,
}

/// Typed VM-owned adapter for the helper ABI. The handle table is scoped to
/// one bridge, so its values remain roots during a helper call without
/// becoming a persistent VM cache or a serialized artifact.
#[allow(dead_code)]
struct JitHelperBridge<'vm, 'frame, 'program> {
    vm: &'vm mut VM<'program>,
    frame: &'frame mut Frame,
    values: Vec<Value>,
    call_site: Option<DebugLocation>,
    last_materialization: Option<JitFrameMaterialization>,
    error: Option<RuntimeError>,
    fallback: bool,
}

impl<'vm, 'frame, 'program> JitHelperBridge<'vm, 'frame, 'program> {
    fn new(
        vm: &'vm mut VM<'program>,
        frame: &'frame mut Frame,
        call_site: Option<DebugLocation>,
    ) -> Self {
        Self {
            vm,
            frame,
            values: Vec::new(),
            call_site,
            last_materialization: None,
            error: None,
            fallback: false,
        }
    }

    fn handle(&mut self, value: Value) -> Result<u64, RuntimeError> {
        let handle = u64::try_from(self.values.len())
            .map_err(|_| RuntimeError::new("JIT value handle space exhausted"))?;
        self.values.push(value);
        Ok(handle)
    }

    fn operand_index(raw: u64) -> Result<usize, RuntimeError> {
        usize::try_from(raw)
            .map_err(|_| RuntimeError::new("JIT helper operand does not fit a platform index"))
    }

    fn value(&self, handle: u64) -> Result<Value, RuntimeError> {
        let index = Self::operand_index(handle)?;
        self.values.get(index).cloned().ok_or_else(|| {
            RuntimeError::new(format!("JIT value handle {} is out of range", handle))
        })
    }

    fn materialize(&mut self, kind: JitSafepointKind) {
        let task_id = self.vm.active_cooperative_task;
        let instruction = self.frame.ip;
        self.last_materialization = Some(self.vm.jit.materialize_frame(
            self.frame,
            task_id,
            JitSafepoint::new(kind, instruction),
        ));
    }

    fn checkpoint(&mut self, instruction: u64) -> Result<Value, RuntimeError> {
        let instruction = Self::operand_index(instruction)?;
        let Some(body) = self.frame.body.as_ref() else {
            return Err(RuntimeError::new("JIT frame has no bytecode body"));
        };
        if instruction >= body.instructions.len() {
            return Err(RuntimeError::new(format!(
                "JIT checkpoint instruction {} is out of range",
                instruction
            )));
        }
        self.frame.ip = instruction;
        self.materialize(JitSafepointKind::Instruction);
        self.vm.checkpoint_instruction()?;
        Ok(Value::Nil)
    }

    fn store_register(&mut self, register: u64, value: u64) -> Result<Value, RuntimeError> {
        let register = Self::operand_index(register)?;
        let value = self.value(value)?;
        let Some(slot) = self.frame.registers.get_mut(register) else {
            return Err(RuntimeError::new(format!(
                "JIT register {} is out of range",
                register
            )));
        };
        *slot = value;
        Ok(self.frame.registers[register].clone())
    }

    fn record_c_abi_error(&mut self, error: RuntimeError, fallback: bool) {
        if self.error.is_none() {
            self.error = Some(error);
            self.fallback = fallback;
        }
    }

    fn dispatch_c_abi(&mut self, helper_id: u32, operands: &[u64]) -> u64 {
        if self.error.is_some() {
            return JIT_ERROR_HANDLE;
        }
        let Some(helper) = RuntimeHelper::from_id(helper_id) else {
            self.record_c_abi_error(
                RuntimeError::new(format!("unknown JIT helper {}", helper_id)),
                true,
            );
            return JIT_ERROR_HANDLE;
        };
        match self.dispatch(helper, operands) {
            Ok(handle) => handle,
            Err(error) => {
                let fallback = matches!(helper, RuntimeHelper::StoreRegister)
                    || error.message.starts_with("JIT fallback:")
                    || error.message.starts_with("JIT helper ")
                    || error.message.starts_with("JIT value handle ")
                    || error.message.starts_with("JIT helper operand ")
                    || error.message.starts_with("JIT register ");
                self.record_c_abi_error(error, fallback);
                JIT_ERROR_HANDLE
            }
        }
    }

    fn take_execution_state(&mut self) -> (Option<RuntimeError>, bool) {
        (self.error.take(), self.fallback)
    }

    fn dispatch(&mut self, helper: RuntimeHelper, operands: &[u64]) -> Result<u64, RuntimeError> {
        let abi = JitHelperAbi::for_helper(helper);
        if operands.len() != abi.value_arguments {
            return Err(RuntimeError::new(format!(
                "JIT helper {} expects {} operands, got {}",
                abi.helper_id,
                abi.value_arguments,
                operands.len()
            )));
        }

        let result = match helper {
            RuntimeHelper::Constant => self.vm.constant_value(Self::operand_index(operands[0])?)?,
            RuntimeHelper::LoadLocal => self
                .vm
                .read_local(self.frame, Self::operand_index(operands[0])?)?,
            RuntimeHelper::LoadUpvalue => {
                let cell = self
                    .frame
                    .upvalues
                    .get(Self::operand_index(operands[0])?)
                    .cloned()
                    .ok_or_else(|| RuntimeError::new("upvalue index out of range"))?;
                let value = cell.borrow().clone();
                value
            }
            RuntimeHelper::Negate => {
                let input = self.value(operands[0])?;
                let Value::Number(number) = input else {
                    return Err(RuntimeError::new(format!(
                        "negate expects number, got {}",
                        input.type_name()
                    )));
                };
                Value::number(-number)
            }
            RuntimeHelper::Not => Value::boolean(!self.value(operands[0])?.is_truthy()),
            RuntimeHelper::Checkpoint => self.checkpoint(operands[0])?,
            RuntimeHelper::StoreRegister => self.store_register(operands[0], operands[1])?,
            RuntimeHelper::Add => {
                let left = self.value(operands[0])?;
                let right = self.value(operands[1])?;
                match (left, right) {
                    (Value::Number(left), Value::Number(right)) => Value::number(left + right),
                    (Value::String(left), Value::String(right)) => {
                        Value::string(format!("{}{}", left, right))
                    }
                    _ => return Err(RuntimeError::new("add expects two numbers or two strings")),
                }
            }
            RuntimeHelper::Subtract => {
                let left = self.value(operands[0])?;
                let right = self.value(operands[1])?;
                match (left, right) {
                    (Value::Number(left), Value::Number(right)) => Value::number(left - right),
                    _ => return Err(RuntimeError::new("subtract expects numbers")),
                }
            }
            RuntimeHelper::Multiply => {
                let left = self.value(operands[0])?;
                let right = self.value(operands[1])?;
                match (left, right) {
                    (Value::Number(left), Value::Number(right)) => Value::number(left * right),
                    _ => return Err(RuntimeError::new("multiply expects numbers")),
                }
            }
            RuntimeHelper::Divide => {
                let left = self.value(operands[0])?;
                let right = self.value(operands[1])?;
                match (left, right) {
                    (Value::Number(left), Value::Number(right)) if right != 0.0 => {
                        Value::number(left / right)
                    }
                    (Value::Number(_), Value::Number(0.0)) => {
                        return Err(RuntimeError::new("division by zero"));
                    }
                    _ => return Err(RuntimeError::new("divide expects numbers")),
                }
            }
            RuntimeHelper::Equal => {
                let left = self.value(operands[0])?;
                let right = self.value(operands[1])?;
                Value::boolean(left.runtime_equals(&right))
            }
            RuntimeHelper::NotEqual => {
                let left = self.value(operands[0])?;
                let right = self.value(operands[1])?;
                Value::boolean(!left.runtime_equals(&right))
            }
            RuntimeHelper::Greater
            | RuntimeHelper::GreaterEqual
            | RuntimeHelper::Less
            | RuntimeHelper::LessEqual => {
                let left = self.value(operands[0])?;
                let right = self.value(operands[1])?;
                if matches!((&left, &right), (Value::Struct(_), Value::Struct(_))) {
                    return Err(RuntimeError::new(
                        "JIT fallback: ordered struct comparison requires the interpreter",
                    ));
                }
                let comparison = match helper {
                    RuntimeHelper::Greater => Comparison::Greater,
                    RuntimeHelper::GreaterEqual => Comparison::GreaterEqual,
                    RuntimeHelper::Less => Comparison::Less,
                    RuntimeHelper::LessEqual => Comparison::LessEqual,
                    _ => unreachable!("ordered helper arm is exhaustive"),
                };
                let call_site = self.call_site.clone();
                Value::boolean(self.vm.compare_values(
                    self.frame,
                    &left,
                    &right,
                    comparison,
                    call_site.as_ref(),
                )?)
            }
        };
        self.handle(result)
    }
}

unsafe extern "C" fn jit_helper_dispatch(
    data: *mut (),
    helper_id: u32,
    operands: *const u64,
    operand_count: usize,
) -> u64 {
    if data.is_null() {
        return JIT_ERROR_HANDLE;
    }
    let bridge = unsafe { &mut *(data as *mut JitHelperBridge<'static, 'static, 'static>) };
    let operands = if operand_count == 0 {
        &[]
    } else if operands.is_null() {
        bridge.record_c_abi_error(
            RuntimeError::new("JIT helper received a null operand pointer"),
            true,
        );
        return JIT_ERROR_HANDLE;
    } else {
        unsafe { std::slice::from_raw_parts(operands, operand_count) }
    };
    bridge.dispatch_c_abi(helper_id, operands)
}

/// Host-controlled, deterministic cooperative execution session.
///
/// The session owns one VM heap and scheduler. Tasks run one at a time in
/// FIFO order, and the host explicitly advances the session with `step` or
/// `run_until_waiting`. Task output is appended in dispatch order. This API
/// does not add language syntax, OS threads, or a new `.cdbc` format.
pub struct CooperativeRun<'a> {
    vm: VM<'a>,
    scheduler: CooperativeScheduler<ScheduledVmTask>,
}

impl<'a> CooperativeRun<'a> {
    /// Access the VM-owned machine memory shared by all tasks in this session.
    pub fn memory(&self) -> &LinearMemory {
        &self.vm.memory
    }

    /// Mutably access machine memory between scheduler dispatches.
    pub fn memory_mut(&mut self) -> &mut LinearMemory {
        &mut self.vm.memory
    }

    /// Spawn a fresh task entry into the session's FIFO queue.
    pub fn spawn(&mut self, spec: TaskSpec) -> Result<TaskId, RuntimeError> {
        self.vm.check_machine_memory_initialization()?;
        let task = self.make_task(spec)?;
        self.scheduler
            .spawn(task)
            .map_err(|error| RuntimeError::new(error.to_string()))
    }

    /// Dispatch one task quantum.
    ///
    /// `Waiting` means there are blocked tasks but no ready task. The host can
    /// then call `wake`, or wait for a joined target to reach a terminal state.
    /// Task failures are retained in `task_outcome` and trigger fail-fast
    /// cancellation of all other non-terminal tasks.
    pub fn step(&mut self) -> Result<CooperativeStep, RuntimeError> {
        self.vm.check_machine_memory_initialization()?;
        let debug_state = self.debug_state_for_next_dispatch();
        let dispatch = self
            .scheduler
            .dispatch(|task, context| {
                if let Some(state) = debug_state {
                    debug_assert_eq!(state.running, context.task_id);
                    debug_assert!(self.vm.active_cooperative_debug_state.is_none());
                    self.vm.active_cooperative_debug_state = Some(state);
                }
                let step = self.vm.execute_scheduled_slice(task, context);
                self.vm.active_cooperative_debug_state = None;
                step
            })
            .map_err(|error| RuntimeError::new(error.to_string()))?;

        let Some(result) = dispatch else {
            self.release_terminal_frames();
            self.vm.heap.collect_garbage();
            return Ok(if self.scheduler.is_complete() {
                CooperativeStep::Complete
            } else {
                CooperativeStep::Waiting
            });
        };

        if self.vm.cooperative_debug_quit {
            self.scheduler.cancel_pending_except(result.task_id);
            self.release_terminal_frames();
            self.vm.heap.collect_garbage();
            return Ok(CooperativeStep::Complete);
        }

        if result.state == TaskState::Failed
            || (result.state == TaskState::Cancelled
                && self
                    .vm
                    .config
                    .cancellation
                    .as_ref()
                    .is_some_and(CancellationToken::is_cancelled))
        {
            self.scheduler.cancel_pending_except(result.task_id);
        }
        self.release_terminal_frames();
        self.vm.heap.collect_garbage();
        Ok(CooperativeStep::Dispatched {
            task_id: result.task_id,
            state: result.state,
        })
    }

    /// Dispatch until all currently ready tasks have yielded, completed, or
    /// failed. A blocked session returns `Waiting`; a terminal session returns
    /// `Complete`.
    pub fn run_until_waiting(&mut self) -> Result<CooperativeStep, RuntimeError> {
        loop {
            match self.step()? {
                CooperativeStep::Dispatched { .. } => {}
                waiting_or_complete => return Ok(waiting_or_complete),
            }
        }
    }

    pub fn quantum(&self) -> usize {
        self.scheduler.quantum()
    }

    pub fn task_state(&self, task_id: TaskId) -> Result<TaskState, TaskControlError> {
        self.scheduler.task_state(task_id).map_err(Into::into)
    }

    /// Return a task's terminal value, failure, or cancellation state.
    /// Non-terminal tasks return `Ok(None)`.
    pub fn task_outcome(&self, task_id: TaskId) -> Result<Option<TaskOutcome>, TaskControlError> {
        let state = self.scheduler.task_state(task_id)?;
        if !state.is_terminal() {
            return Ok(None);
        }
        let task = self.scheduler.task_payload(task_id)?;
        let outcome = match state {
            TaskState::Completed => task
                .result
                .clone()
                .map(TaskOutcome::Completed)
                .ok_or(TaskControlError::MissingOutcome(task_id))?,
            TaskState::Failed => {
                let mut error = task
                    .error
                    .clone()
                    .ok_or(TaskControlError::MissingOutcome(task_id))?;
                if error.sources.is_empty() {
                    error.sources = self.vm.program.debug_sources.clone();
                }
                TaskOutcome::Failed(error)
            }
            TaskState::Cancelled => TaskOutcome::Cancelled,
            TaskState::Ready | TaskState::Running | TaskState::Blocked => {
                return Ok(None);
            }
        };
        Ok(Some(outcome))
    }

    /// Register `waiter` for `target` and return the target outcome if it is
    /// already terminal. A pending join blocks the waiter and wakes it in FIFO
    /// registration order when the target terminates.
    pub fn join(&mut self, waiter: TaskId, target: TaskId) -> Result<JoinPoll, TaskControlError> {
        match self.scheduler.join(waiter, target)? {
            JoinStatus::Waiting => Ok(JoinPoll::Waiting),
            JoinStatus::Ready => Ok(JoinPoll::Ready(
                self.task_outcome(target)?
                    .ok_or(TaskControlError::MissingOutcome(target))?,
            )),
        }
    }

    /// Explicitly wake a blocked task. A joined task may be woken early and
    /// can register the join again if it still needs the target outcome.
    pub fn wake(&mut self, task_id: TaskId) -> Result<(), TaskControlError> {
        self.scheduler
            .wake(task_id)
            .map_err(TaskControlError::from)?;
        self.release_terminal_frames();
        self.vm.heap.collect_garbage();
        Ok(())
    }

    pub fn cancel(&mut self, task_id: TaskId) -> Result<(), TaskControlError> {
        self.scheduler
            .cancel(task_id)
            .map_err(TaskControlError::from)?;
        self.release_terminal_frames();
        self.vm.heap.collect_garbage();
        Ok(())
    }

    pub fn is_complete(&self) -> bool {
        self.scheduler.is_complete()
    }

    pub fn is_waiting(&self) -> bool {
        self.scheduler.is_waiting()
    }

    /// Whether the cooperative debugger hook requested a session quit.
    pub fn debug_quit(&self) -> bool {
        self.vm.cooperative_debug_quit
    }

    /// Return all terminal outcomes in stable task-id order.
    pub fn outcomes(&self) -> Result<Vec<(TaskId, TaskOutcome)>, TaskControlError> {
        self.scheduler
            .task_ids()
            .map(|task_id| {
                self.task_outcome(task_id)
                    .map(|outcome| outcome.map(|outcome| (task_id, outcome)))
            })
            .collect::<Result<Vec<_>, _>>()
            .map(|outcomes| outcomes.into_iter().flatten().collect())
    }

    pub fn take_output(&mut self) -> String {
        std::mem::take(&mut self.vm.output)
    }

    /// Return committed output chunks in scheduler dispatch order.
    pub fn output_events(&self) -> &[TaskOutputEvent] {
        &self.vm.task_output_events
    }

    /// Drain committed output chunks without resetting their session sequence
    /// or the cumulative output-byte resource budget.
    pub fn take_output_events(&mut self) -> Vec<TaskOutputEvent> {
        std::mem::take(&mut self.vm.task_output_events)
    }

    /// Return task-attributed trace observations in scheduler event order.
    pub fn trace_events(&self) -> &[TaskTraceEvent] {
        &self.vm.task_trace_events
    }

    /// Drain task trace observations without resetting the shared session
    /// event sequence.
    pub fn take_trace_events(&mut self) -> Vec<TaskTraceEvent> {
        std::mem::take(&mut self.vm.task_trace_events)
    }

    /// Return a deterministic snapshot of aggregate and per-task counters.
    /// Ordinary cooperative sessions return `None` because profiling is
    /// explicitly opt-in.
    pub fn profile_report(&self) -> Option<CooperativeProfileReport> {
        if !self.vm.profile_enabled {
            return None;
        }
        let heap_stats = self.vm.heap.stats();
        let tasks = self
            .scheduler
            .task_ids()
            .map(|task_id| {
                self.scheduler
                    .task_payload(task_id)
                    .expect("scheduler task id retains a payload")
                    .profile
                    .report(task_id)
            })
            .collect();
        Some(CooperativeProfileReport {
            aggregate: self.vm.profile_report(&heap_stats),
            tasks,
        })
    }

    fn debug_state_for_next_dispatch(&self) -> Option<CooperativeDebugState> {
        self.vm.cooperative_debug_hook.as_ref()?;
        let mut ready = self.scheduler.ready_task_ids();
        let running = ready.first().copied()?;
        ready.remove(0);
        let tasks = self
            .scheduler
            .task_states()
            .into_iter()
            .map(|(task_id, state)| {
                if task_id == running {
                    (task_id, TaskState::Running)
                } else {
                    (task_id, state)
                }
            })
            .collect();
        Some(CooperativeDebugState {
            running,
            ready,
            tasks,
        })
    }

    fn release_terminal_frames(&mut self) {
        let terminal_ids = self
            .scheduler
            .task_ids()
            .filter(|task_id| {
                self.scheduler
                    .task_state(*task_id)
                    .is_ok_and(TaskState::is_terminal)
            })
            .collect::<Vec<_>>();
        for task_id in terminal_ids {
            if let Ok(task) = self.scheduler.task_payload_mut(task_id) {
                self.vm.release_task_frames(task);
            }
        }
    }

    fn make_task(&mut self, spec: TaskSpec) -> Result<ScheduledVmTask, RuntimeError> {
        let mut frame = match spec {
            TaskSpec::Main => {
                let entry = self.vm.program.entry.0 as usize;
                if entry != 0 {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "entry must be f0, found f{}",
                        self.vm.program.entry.0
                    )));
                }
                let Some(entry_function) = self.vm.program.functions.get(entry) else {
                    return Err(RuntimeError::invalid_instruction(format!(
                        "entry function f{} is out of range",
                        self.vm.program.entry.0
                    )));
                };
                self.vm.validate_machine_call(entry_function, &[], true)?;
                let machine_frame_size = entry_function.machine_frame_size;
                Frame {
                    body: Some(Rc::new(entry_function.clone())),
                    ip: 0,
                    registers: vec![Value::Nil; entry_function.registers],
                    locals: self.vm.heap.new_local_slots(),
                    closure: self.vm.heap.new_environment(),
                    upvalues: Vec::new(),
                    variable_plan: None,
                    is_main: true,
                    function: Rc::from("main"),
                    function_index: None,
                    return_target: None,
                    machine_frame_base: None,
                    machine_frame_size,
                }
            }
            TaskSpec::Function { index, arguments } => {
                let Some(cached) = self.vm.prepared_function(index) else {
                    return Err(RuntimeError::new("function index out of range"));
                };
                if arguments.len() != cached.params.len() {
                    return Err(RuntimeError::new(format!(
                        "expected {} arguments but got {}",
                        cached.params.len(),
                        arguments.len()
                    )));
                }
                self.vm
                    .validate_machine_call(&cached.body, &arguments, true)?;
                self.vm.new_function_frame(
                    index,
                    &cached,
                    arguments,
                    self.vm.heap.new_environment(),
                    Vec::new(),
                    None,
                )
            }
        };
        self.vm
            .ensure_machine_frame_fits(frame.machine_frame_size)?;
        let mut machine_stack = self.vm.new_machine_stack()?;
        self.vm
            .allocate_task_machine_frame(&mut machine_stack, &mut frame)?;
        let frames =
            FrameStack::new(frame).map_err(|error| RuntimeError::new(error.to_string()))?;
        Ok(ScheduledVmTask {
            frames,
            machine_stack: Some(machine_stack),
            result: None,
            error: None,
            trace: TaskTraceState::default(),
            profile: if self.vm.profile_enabled {
                TaskProfileState::enabled(self.vm.program)
            } else {
                TaskProfileState::default()
            },
        })
    }
}

impl<'a> VM<'a> {
    pub fn new(program: &'a Program) -> Self {
        Self::with_config(program, RunConfig::default())
    }

    pub fn with_config(program: &'a Program, config: RunConfig) -> Self {
        Self::build(program, config, false)
    }

    /// Construct a VM that has verified the program once, up front. The
    /// execution paths may then skip the per-operand bounds checks they keep
    /// for hand-built, unverified programs.
    pub fn with_config_verified(
        program: &'a Program,
        config: RunConfig,
    ) -> Result<Self, ParseError> {
        crate::format::verify_program(program)?;
        Ok(Self::build(program, config, true))
    }

    fn build(program: &'a Program, config: RunConfig, verified: bool) -> Self {
        let instruction_checkpoint = match (&config.cancellation, config.max_instruction_steps) {
            (None, Some(limit)) => InstructionCheckpoint::Limited(limit),
            (None, None) => InstructionCheckpoint::Unlimited,
            (Some(token), Some(limit)) => InstructionCheckpoint::CancelledLimited {
                limit,
                token: token.clone(),
            },
            (Some(token), None) => InstructionCheckpoint::CancelledUnlimited {
                token: token.clone(),
            },
        };
        let heap = Heap::new();
        let mut memory = LinearMemory::new();
        let (initialization_error, direct_call_relocations) =
            match initialize_machine_segments(&mut memory, &program.data_segments) {
                Ok(regions) => match resolve_machine_relocations(&mut memory, program, &regions) {
                    Ok(relocations) => (None, relocations),
                    Err(error) => (Some(error), BTreeMap::new()),
                },
                Err(error) => (Some(error), BTreeMap::new()),
            };
        // Global slots are an artifact-table boundary. A malformed
        // instruction must not turn its arbitrary slot number into a host
        // allocation request; the verifier checks every instruction slot.
        let global_count = program.globals.len();
        let global_names = program
            .globals
            .iter()
            .map(|name_index| program.names.get(*name_index).cloned().unwrap_or_default())
            .collect();
        let block_maps = program
            .functions
            .iter()
            .map(|function| {
                function
                    .instructions
                    .iter()
                    .enumerate()
                    .filter_map(|(index, instruction)| match instruction {
                        Instruction::BlockStart { id } => Some((id.0, index)),
                        _ => None,
                    })
                    .collect()
            })
            .collect();
        let mut decoded_constants = Vec::with_capacity(program.constants.len());
        let mut constant_errors = BTreeMap::new();
        for (index, constant) in program.constants.iter().enumerate() {
            match decode_constant(constant) {
                Ok(value) => decoded_constants.push(value),
                Err(error) => {
                    decoded_constants.push(Value::Nil);
                    constant_errors.insert(index, error);
                }
            }
        }
        Self {
            program,
            config,
            verified,
            instruction_checkpoint,
            heap,
            memory,
            initialization_error,
            direct_call_relocations,
            machine_stack: None,
            global_cells: vec![None; global_count],
            global_names,
            block_maps,
            decoded_constants,
            constant_errors,
            native_specs: program
                .native_imports
                .iter()
                .map(|import| native_spec(&import.name))
                .collect(),
            module_states: vec![ModuleState::Uninitialized; program.modules.len()],
            prepared_functions: program
                .functions
                .iter()
                .map(|function| Rc::new(prepare_function(function)))
                .collect(),
            jit: JitState::disabled(),
            output: String::new(),
            output_bytes: 0,
            task_output_events: Vec::new(),
            task_trace_enabled: false,
            task_trace_collect_events: false,
            task_trace_events: Vec::new(),
            active_cooperative_task: None,
            active_task_trace: None,
            active_task_profile: None,
            cooperative_debug_hook: None,
            active_cooperative_debug_state: None,
            cooperative_debug_quit: false,
            next_task_event_sequence: 0,
            instruction_steps: 0,
            call_depth: 0,
            runtime_elements: 0,
            trace_enabled: false,
            trace_collect_events: false,
            trace_events: Vec::new(),
            trace_stack: Vec::new(),
            trace_last_locations: Vec::new(),
            debug_hook: None,
            profile_enabled: false,
            profile_instruction_count: 0,
            profile_output_bytes: 0,
            profile_functions: Vec::new(),
            profile_natives: BTreeMap::new(),
            profile_source_ranges: BTreeMap::new(),
        }
    }

    #[cfg(test)]
    fn heap_stats(&self) -> HeapStats {
        self.heap.stats()
    }

    /// Access the VM-owned machine memory.
    pub fn memory(&self) -> &LinearMemory {
        &self.memory
    }

    /// Mutably access machine memory before or between execution boundaries.
    pub fn memory_mut(&mut self) -> &mut LinearMemory {
        &mut self.memory
    }

    fn check_machine_memory_initialization(&self) -> Result<(), RuntimeError> {
        self.initialization_error.clone().map_or(Ok(()), Err)
    }

    fn machine_stack_capacity(&self) -> usize {
        self.config
            .max_machine_stack_bytes
            .unwrap_or(DEFAULT_MAX_MACHINE_STACK_BYTES)
    }

    fn ensure_machine_frame_fits(&self, frame_size: u64) -> Result<(), RuntimeError> {
        let capacity = u64::try_from(self.machine_stack_capacity())
            .map_err(|_| RuntimeError::new("machine stack capacity is too large"))?;
        if frame_size > capacity {
            return Err(RuntimeError::stack_overflow(frame_size, capacity));
        }
        Ok(())
    }

    fn new_machine_stack(&mut self) -> Result<MachineStack, RuntimeError> {
        let capacity = self.machine_stack_capacity();
        MachineStack::new(&mut self.memory, capacity)
    }

    fn allocate_current_machine_frame(&mut self, frame: &mut Frame) -> Result<(), RuntimeError> {
        let stack = self
            .machine_stack
            .as_mut()
            .ok_or_else(|| RuntimeError::new("machine stack is not active"))?;
        stack.allocate_frame(&mut self.memory, frame)
    }

    fn release_current_machine_frame(&mut self, frame: &Frame) -> Result<(), RuntimeError> {
        let base = frame.machine_frame_base;
        let size = frame.machine_frame_size;
        let Some(stack) = self.machine_stack.as_mut() else {
            return Ok(());
        };
        stack.release_frame(&mut self.memory, base, size)
    }

    fn allocate_task_machine_frame(
        &mut self,
        stack: &mut MachineStack,
        frame: &mut Frame,
    ) -> Result<(), RuntimeError> {
        stack.allocate_frame(&mut self.memory, frame)
    }

    fn release_task_current_machine_frame(
        &mut self,
        task: &mut ScheduledVmTask,
    ) -> Result<(), RuntimeError> {
        let (base, size) = task
            .frames
            .current()
            .map(|frame| (frame.machine_frame_base, frame.machine_frame_size))
            .map_err(|error| RuntimeError::new(error.to_string()))?;
        task.machine_stack
            .as_mut()
            .ok_or_else(|| RuntimeError::new("task machine stack is not active"))?
            .release_frame(&mut self.memory, base, size)
    }

    fn release_task_frames(&mut self, task: &mut ScheduledVmTask) {
        if let Some(stack) = task.machine_stack.as_mut() {
            stack.release_all(&mut self.memory);
        }
        task.frames.clear();
        task.trace = TaskTraceState::default();
    }

    pub fn run(mut self) -> Result<String, RuntimeError> {
        self.run_inner()
    }

    /// Start a host-controlled cooperative session without changing the
    /// existing single-task `run`, trace, debug, or profile APIs.
    pub fn start_cooperative(self, quantum: usize) -> Result<CooperativeRun<'a>, TaskControlError> {
        let scheduler = CooperativeScheduler::new(quantum).map_err(TaskControlError::from)?;
        Ok(CooperativeRun {
            vm: self,
            scheduler,
        })
    }

    /// Start a cooperative session with task-attributed trace collection.
    /// Existing single-task trace and ordinary cooperative sessions remain
    /// unchanged.
    pub fn start_cooperative_trace(
        mut self,
        quantum: usize,
    ) -> Result<CooperativeRun<'a>, TaskControlError> {
        self.task_trace_enabled = true;
        self.task_trace_collect_events = true;
        self.start_cooperative(quantum)
    }

    /// Start a cooperative session with deterministic aggregate and per-task
    /// profile counters. Existing single-task profile and ordinary cooperative
    /// sessions remain unchanged.
    pub fn start_cooperative_profile(
        mut self,
        quantum: usize,
    ) -> Result<CooperativeRun<'a>, TaskControlError> {
        self.enable_profile();
        self.start_cooperative(quantum)
    }

    /// Start a cooperative session with task-attributed synchronous debugger
    /// callbacks. Task trace stacks are maintained privately so pauses remain
    /// task-local without collecting trace events.
    pub fn start_cooperative_debug(
        mut self,
        quantum: usize,
        hook: Box<dyn CooperativeDebugHook + 'a>,
    ) -> Result<CooperativeRun<'a>, TaskControlError> {
        self.task_trace_enabled = true;
        self.task_trace_collect_events = false;
        self.cooperative_debug_hook = Some(hook);
        self.start_cooperative(quantum)
    }

    /// Execute the program with deterministic counters enabled.
    ///
    /// Program output remains in `result` for successful execution, while the
    /// report is returned for both successful and failed execution. The CLI
    /// deliberately renders only the report so program stdout cannot be
    /// confused with profile records.
    pub fn profile(mut self) -> ProfileRun {
        self.enable_profile();
        let heap_stats = self.heap.stats();
        let result = self.run_inner();
        ProfileRun {
            report: self.profile_report(&heap_stats),
            result,
        }
    }

    pub fn trace(mut self) -> TraceRun {
        self.trace_enabled = true;
        self.trace_collect_events = true;
        let result = self.run_inner();
        TraceRun {
            events: self.trace_events,
            result,
        }
    }

    pub fn debug(mut self, hook: Box<dyn DebugHook + 'a>) -> DebugRun {
        self.trace_enabled = true;
        self.trace_collect_events = false;
        self.debug_hook = Some(hook);
        let result = self.run_inner();
        let quit = result
            .as_ref()
            .err()
            .is_some_and(|error| error.kind == RuntimeErrorKind::DebuggerQuit);
        let result = if quit {
            Ok(std::mem::take(&mut self.output))
        } else {
            result
        };
        DebugRun { result, quit }
    }

    fn profile_report(&self, heap_stats: &HeapStats) -> ProfileReport {
        let (tracked_heap_allocations, tracked_heap_peak_live) = self.heap.profile_counts();
        let heap_snapshot = heap_stats.snapshot();
        ProfileReport {
            instruction_count: self.profile_instruction_count,
            output_bytes: self.profile_output_bytes,
            tracked_heap_allocations,
            tracked_heap_peak_live,
            tracked_heap_estimated_live_bytes: heap_snapshot.estimated_live_bytes,
            tracked_heap_estimated_peak_live_bytes: heap_snapshot.estimated_peak_live_bytes,
            functions: self.profile_functions.clone(),
            natives: self
                .profile_natives
                .iter()
                .map(|(name, calls)| ProfileNative {
                    name: name.clone(),
                    calls: *calls,
                })
                .collect(),
            source_ranges: self
                .profile_source_ranges
                .iter()
                .map(|((source, start, end), hits)| ProfileSourceRange {
                    range: DebugRange {
                        source: *source,
                        start: *start,
                        end: *end,
                    },
                    hits: *hits,
                })
                .collect(),
        }
    }

    fn enable_profile(&mut self) {
        self.profile_enabled = true;
        self.profile_functions = empty_profile_functions(self.program);
    }

    fn profile_function_entry(&mut self, frame: &Frame) {
        if !self.profile_enabled {
            return;
        }
        let index = profile_function_index(frame);
        if let Some(function) = self.profile_functions.get_mut(index) {
            function.calls = function.calls.saturating_add(1);
        }
        if let Some(active) = self.active_task_profile.as_mut() {
            active.state.function_entry(frame);
        }
    }

    fn profile_instruction(&mut self, frame: &Frame, location: Option<&DebugLocation>) {
        if !self.profile_enabled {
            return;
        }
        self.profile_instruction_count = self.profile_instruction_count.saturating_add(1);
        let index = profile_function_index(frame);
        if let Some(function) = self.profile_functions.get_mut(index) {
            function.instructions = function.instructions.saturating_add(1);
        }
        if let Some(range) = location.and_then(|location| location.range.as_ref()) {
            let key = (range.source, range.start, range.end);
            let hits = self.profile_source_ranges.entry(key).or_insert(0);
            *hits = hits.saturating_add(1);
        }
        if let Some(active) = self.active_task_profile.as_mut() {
            active.state.instruction(frame, location);
        }
    }

    fn profile_native_call(&mut self, name: &str) {
        if !self.profile_enabled {
            return;
        }
        let calls = self.profile_natives.entry(name.to_string()).or_insert(0);
        *calls = calls.saturating_add(1);
        if let Some(active) = self.active_task_profile.as_mut() {
            active.state.native_call(name);
        }
    }

    fn run_inner(&mut self) -> Result<String, RuntimeError> {
        self.check_machine_memory_initialization()?;
        self.check_cancellation()?;
        let entry = self.program.entry.0 as usize;
        if entry != 0 {
            return Err(RuntimeError::invalid_instruction(format!(
                "entry must be f0, found f{}",
                self.program.entry.0
            )));
        }
        let Some(entry_body) = self.program.functions.get(entry) else {
            return Err(RuntimeError::invalid_instruction(format!(
                "entry function f{} is out of range",
                self.program.entry.0
            )));
        };
        self.machine_stack = Some(self.new_machine_stack()?);
        let execution = {
            let mut frame = Frame {
                body: None,
                ip: 0,
                registers: vec![Value::Nil; entry_body.registers],
                locals: self.heap.new_local_slots(),
                closure: self.heap.new_environment(),
                upvalues: Vec::new(),
                variable_plan: None,
                is_main: true,
                function: Rc::from("main"),
                function_index: None,
                return_target: None,
                machine_frame_base: None,
                machine_frame_size: entry_body.machine_frame_size,
            };
            // The entry body is immutable after artifact verification. Borrow it
            // directly instead of cloning its instruction and debug-location
            // vectors for the one execution of this VM instance.
            let execution = self
                .allocate_current_machine_frame(&mut frame)
                .and_then(|_| self.execute_body(entry_body, &mut frame))
                .and_then(|result| {
                    self.validate_machine_return(entry_body, result.as_ref())?;
                    Ok(result)
                });
            let cleanup = self.release_current_machine_frame(&frame);
            match (execution, cleanup) {
                (Err(error), _) => Err(error),
                (Ok(value), Ok(())) => Ok(value),
                (Ok(_), Err(error)) => Err(error),
            }
        };
        self.machine_stack.take();
        let result = match execution {
            Ok(value) => {
                drop(value);
                Ok(std::mem::take(&mut self.output))
            }
            Err(mut error) => {
                if error.sources.is_empty() {
                    error.sources = self.program.debug_sources.clone();
                }
                Err(error)
            }
        };
        self.heap.collect_garbage();
        result
    }

    /// Execute one host-only cooperative task through the explicit frame
    /// stack. The public single-task APIs continue to use `run_inner`; this
    /// compatibility adapter remains private while the public cooperative
    /// session owns task results and output events. Task-aware tracing and
    /// profiling are public opt-in sessions; debugging remains a later V5B
    /// slice.
    fn run_cooperative(mut self, quantum: usize) -> Result<String, RuntimeError> {
        self.check_machine_memory_initialization()?;
        self.check_cancellation()?;
        let mut scheduler = CooperativeScheduler::new(quantum)
            .map_err(|error| RuntimeError::new(error.to_string()))?;
        let entry = self.program.entry.0 as usize;
        if entry != 0 {
            return Err(RuntimeError::invalid_instruction(format!(
                "entry must be f0, found f{}",
                self.program.entry.0
            )));
        }
        let Some(entry_body) = self.program.functions.get(entry) else {
            return Err(RuntimeError::invalid_instruction(format!(
                "entry function f{} is out of range",
                self.program.entry.0
            )));
        };
        let main_body = Rc::new(entry_body.clone());
        let mut root = Frame::main(
            main_body,
            entry_body.registers,
            self.heap.new_local_slots(),
            self.heap.new_environment(),
        );
        let mut machine_stack = self.new_machine_stack()?;
        self.allocate_task_machine_frame(&mut machine_stack, &mut root)?;
        let frames = FrameStack::new(root).map_err(|error| RuntimeError::new(error.to_string()))?;
        let task_id = scheduler
            .spawn(ScheduledVmTask {
                frames,
                machine_stack: Some(machine_stack),
                result: None,
                error: None,
                trace: TaskTraceState::default(),
                profile: TaskProfileState::default(),
            })
            .map_err(|error| RuntimeError::new(error.to_string()))?;

        loop {
            let dispatch = scheduler
                .dispatch(|task, context| self.execute_scheduled_slice(task, context))
                .map_err(|error| RuntimeError::new(error.to_string()))?;
            self.heap.collect_garbage();
            let Some(result) = dispatch else {
                if scheduler.is_complete() {
                    break;
                }
                return Err(RuntimeError::new("scheduler has blocked tasks"));
            };
            if matches!(result.state, TaskState::Failed | TaskState::Cancelled)
                || scheduler.is_complete()
            {
                break;
            }
        }

        let task_error = scheduler
            .task_payload(task_id)
            .map_err(|error| RuntimeError::new(error.to_string()))?
            .error
            .clone();
        let task_result = scheduler
            .task_payload_mut(task_id)
            .map_err(|error| RuntimeError::new(error.to_string()))?
            .result
            .take();
        let result = match task_error {
            Some(mut error) => {
                if error.sources.is_empty() {
                    error.sources = self.program.debug_sources.clone();
                }
                Err(error)
            }
            None => {
                drop(task_result);
                Ok(std::mem::take(&mut self.output))
            }
        };
        if let Ok(task) = scheduler.task_payload_mut(task_id) {
            self.release_task_frames(task);
        }
        drop(scheduler);
        self.heap.collect_garbage();
        result
    }

    fn execute_scheduled_slice(
        &mut self,
        task: &mut ScheduledVmTask,
        context: DispatchContext,
    ) -> TaskStep {
        for _ in 0..context.quantum {
            if context.cancellation_requested {
                return self.stop_scheduled_task(context.task_id, task, RuntimeError::cancelled());
            }

            let (body, instruction_index) = match task.frames.current() {
                Ok(frame) => {
                    let Some(body) = frame.body.as_ref() else {
                        return self.stop_scheduled_task(
                            context.task_id,
                            task,
                            RuntimeError::new("scheduled frame has no bytecode body"),
                        );
                    };
                    (Rc::clone(body), frame.ip)
                }
                Err(error) => {
                    return self.stop_scheduled_task(
                        context.task_id,
                        task,
                        RuntimeError::new(error.to_string()),
                    );
                }
            };

            if self.profile_enabled && !task.profile.started {
                let frame = match task.frames.current() {
                    Ok(frame) => frame,
                    Err(error) => {
                        return self.stop_scheduled_task(
                            context.task_id,
                            task,
                            RuntimeError::new(error.to_string()),
                        );
                    }
                };
                self.profile_function_entry(frame);
                task.profile.function_entry(frame);
                task.profile.started = true;
            }

            if self.task_trace_enabled && !task.trace.started {
                let location = body.locations.first().cloned().flatten();
                let trace_result = {
                    let ScheduledVmTask { frames, trace, .. } = task;
                    match frames.current() {
                        Ok(frame) => self.task_trace_enter(context.task_id, trace, frame, location),
                        Err(error) => Err(RuntimeError::new(error.to_string())),
                    }
                };
                if let Err(error) = trace_result {
                    task.error = Some(error);
                    return TaskStep::Fail;
                }
            }

            if instruction_index >= body.instructions.len() {
                if let Err(error) = self.validate_machine_return(&body, None) {
                    let error =
                        self.decorate_scheduled_error(error, task, &body, instruction_index);
                    return self.stop_scheduled_task(context.task_id, task, error);
                }
                if self.task_trace_enabled {
                    let trace_result = {
                        let ScheduledVmTask { frames, trace, .. } = task;
                        match frames.current() {
                            Ok(frame) => self.task_trace_leave(
                                context.task_id,
                                trace,
                                frame,
                                body.instructions.len().checked_sub(1),
                                None,
                            ),
                            Err(error) => Err(RuntimeError::new(error.to_string())),
                        }
                    };
                    if let Err(error) = trace_result {
                        task.error = Some(error);
                        return TaskStep::Fail;
                    }
                }
                if let Err(error) = self.release_task_current_machine_frame(task) {
                    return self.stop_scheduled_task(context.task_id, task, error);
                }
                match task.frames.return_value(Value::Nil) {
                    Ok(Some(value)) => {
                        task.result = Some(value);
                        return TaskStep::Complete;
                    }
                    Ok(None) => continue,
                    Err(error) => {
                        return self.stop_scheduled_task(
                            context.task_id,
                            task,
                            RuntimeError::new(error.to_string()),
                        );
                    }
                }
            }

            let location = body.locations.get(instruction_index).cloned().flatten();
            if self.task_trace_enabled {
                let trace_result = {
                    let ScheduledVmTask { frames, trace, .. } = task;
                    match frames.current() {
                        Ok(frame) => self.task_trace_instruction(
                            context.task_id,
                            trace,
                            frame,
                            instruction_index,
                            location.clone(),
                        ),
                        Err(error) => Err(RuntimeError::new(error.to_string())),
                    }
                };
                if let Err(error) = trace_result {
                    task.error = Some(error);
                    return TaskStep::Fail;
                }
            }
            if self.cooperative_debug_hook.is_some() {
                let debug_result = {
                    let ScheduledVmTask { frames, trace, .. } = task;
                    match frames.current() {
                        Ok(frame) => self.cooperative_debug_instruction(
                            context.task_id,
                            trace.stack.clone(),
                            frame,
                            instruction_index,
                            location.clone(),
                        ),
                        Err(error) => Err(RuntimeError::new(error.to_string())),
                    }
                };
                if let Err(error) = debug_result {
                    return self.stop_scheduled_task(context.task_id, task, error);
                }
            }
            if let Err(error) = self.checkpoint_instruction() {
                let error = self.decorate_scheduled_error(error, task, &body, instruction_index);
                return self.stop_scheduled_task(context.task_id, task, error);
            }
            let previous_call_depth = self.call_depth;
            let scheduled_call_depth = task.frames.len().saturating_sub(1);
            debug_assert!(self.active_cooperative_task.is_none());
            self.active_cooperative_task = Some(context.task_id);
            if self.task_trace_enabled {
                debug_assert!(self.active_task_trace.is_none());
                self.active_task_trace = Some(ActiveTaskTrace {
                    task_id: context.task_id,
                    state: std::mem::take(&mut task.trace),
                });
            }
            if self.profile_enabled {
                debug_assert!(self.active_task_profile.is_none());
                self.active_task_profile = Some(ActiveTaskProfile {
                    task_id: context.task_id,
                    state: std::mem::take(&mut task.profile),
                });
            }
            let active_stack = match task.machine_stack.take() {
                Some(stack) => stack,
                None => {
                    return self.stop_scheduled_task(
                        context.task_id,
                        task,
                        RuntimeError::new("task machine stack is not active"),
                    )
                }
            };
            let saved_stack = self.machine_stack.replace(active_stack);
            let action = match task.frames.current_mut() {
                Ok(frame) => {
                    if self.profile_enabled {
                        self.profile_instruction(frame, location.as_ref());
                    }
                    self.call_depth = scheduled_call_depth;
                    let action = self.execute_instruction(
                        &body,
                        frame,
                        context.task_id,
                        instruction_index,
                        &body.instructions[instruction_index],
                    );
                    self.call_depth = previous_call_depth;
                    action
                }
                Err(error) => Err(RuntimeError::new(error.to_string())),
            };
            let active_stack = match self.machine_stack.take() {
                Some(stack) => stack,
                None => {
                    self.machine_stack = saved_stack;
                    return self.stop_scheduled_task(
                        context.task_id,
                        task,
                        RuntimeError::new("machine stack was lost during task execution"),
                    );
                }
            };
            task.machine_stack = Some(active_stack);
            self.machine_stack = saved_stack;
            if self.task_trace_enabled {
                let active = self
                    .active_task_trace
                    .take()
                    .expect("traced scheduled execution retains task state");
                debug_assert_eq!(active.task_id, context.task_id);
                task.trace = active.state;
            }
            if self.profile_enabled {
                let active = self
                    .active_task_profile
                    .take()
                    .expect("profiled scheduled execution retains task state");
                debug_assert_eq!(active.task_id, context.task_id);
                task.profile = active.state;
            }
            debug_assert_eq!(self.active_cooperative_task.take(), Some(context.task_id));
            self.heap.observe_estimated_bytes();

            match action {
                Ok(InstructionAction::Continue) => {
                    if let Ok(frame) = task.frames.current_mut() {
                        frame.ip += 1;
                    }
                }
                Ok(InstructionAction::Jumped) => {}
                Ok(InstructionAction::Return(value)) => {
                    if let Err(error) = self.validate_machine_return(&body, Some(&value)) {
                        let error =
                            self.decorate_scheduled_error(error, task, &body, instruction_index);
                        return self.stop_scheduled_task(context.task_id, task, error);
                    }
                    if self.task_trace_enabled {
                        let rendered = value.to_string();
                        let trace_result = {
                            let ScheduledVmTask { frames, trace, .. } = task;
                            match frames.current() {
                                Ok(frame) => self
                                    .task_trace_event(
                                        context.task_id,
                                        trace,
                                        TraceEventKind::Return,
                                        frame,
                                        Some(instruction_index),
                                        location.clone(),
                                        Some(rendered.clone()),
                                    )
                                    .and_then(|_| {
                                        self.task_trace_leave(
                                            context.task_id,
                                            trace,
                                            frame,
                                            Some(instruction_index),
                                            Some(rendered),
                                        )
                                    }),
                                Err(error) => Err(RuntimeError::new(error.to_string())),
                            }
                        };
                        if let Err(error) = trace_result {
                            task.error = Some(error);
                            return TaskStep::Fail;
                        }
                    }
                    if let Err(error) = self.release_task_current_machine_frame(task) {
                        return self.stop_scheduled_task(context.task_id, task, error);
                    }
                    match task.frames.return_value(value) {
                        Ok(Some(value)) => {
                            task.result = Some(value);
                            return TaskStep::Complete;
                        }
                        Ok(None) => {}
                        Err(error) => {
                            return self.stop_scheduled_task(
                                context.task_id,
                                task,
                                RuntimeError::new(error.to_string()),
                            );
                        }
                    }
                }
                Ok(InstructionAction::Call(request)) => {
                    if let Err(error) = self.push_scheduled_call(context.task_id, task, request) {
                        let error =
                            self.decorate_scheduled_error(error, task, &body, instruction_index);
                        return self.stop_scheduled_task(context.task_id, task, error);
                    }
                }
                Err(error) => {
                    let error =
                        self.decorate_scheduled_error(error, task, &body, instruction_index);
                    return self.stop_scheduled_task(context.task_id, task, error);
                }
            }
        }
        TaskStep::Yield
    }

    fn push_scheduled_call(
        &mut self,
        task_id: TaskId,
        task: &mut ScheduledVmTask,
        request: CallRequest,
    ) -> Result<(), RuntimeError> {
        let Some(cached) = self.prepared_function(request.function.function_index) else {
            let mut error = RuntimeError::new("function index out of range");
            error.location = request.call_site;
            error.push_frame(request.caller, error.location.clone());
            return Err(error);
        };
        if request.arguments.len() != cached.params.len() {
            let mut error = RuntimeError::new(format!(
                "expected {} arguments but got {}",
                cached.params.len(),
                request.arguments.len()
            ));
            error.location = request.call_site;
            error.push_frame(request.caller, error.location.clone());
            return Err(error);
        }
        let arguments = request.arguments.values();
        if let Err(mut error) = self.validate_machine_call(&cached.body, &arguments, request.direct)
        {
            error.location = request.call_site.clone();
            error.push_frame(request.caller.clone(), error.location.clone());
            return Err(error);
        }
        self.check_call_depth_at(task.frames.len().saturating_sub(1))?;

        let frame = self.new_function_frame(
            request.function.function_index,
            &cached,
            arguments,
            request.function.closure.clone(),
            request.function.upvalues.clone(),
            Some(ReturnTarget {
                register: request.dest,
                call_site: request.call_site,
            }),
        );
        let mut frame = frame;
        task.machine_stack
            .as_mut()
            .ok_or_else(|| RuntimeError::new("task machine stack is not active"))
            .and_then(|stack| self.allocate_task_machine_frame(stack, &mut frame))?;
        if self.profile_enabled {
            self.profile_function_entry(&frame);
            task.profile.function_entry(&frame);
        }
        task.frames
            .current_mut()
            .map_err(|error| RuntimeError::new(error.to_string()))?
            .ip += 1;
        task.frames
            .push(frame)
            .map_err(|error| RuntimeError::new(error.to_string()))?;
        if self.task_trace_enabled {
            let ScheduledVmTask { frames, trace, .. } = task;
            let frame = frames
                .current()
                .map_err(|error| RuntimeError::new(error.to_string()))?;
            let location = frame
                .body
                .as_ref()
                .and_then(|body| body.locations.first().cloned().flatten());
            self.task_trace_enter(task_id, trace, frame, location)?;
        }
        Ok(())
    }

    fn decorate_scheduled_error(
        &self,
        mut error: RuntimeError,
        task: &ScheduledVmTask,
        body: &Function,
        instruction_index: usize,
    ) -> RuntimeError {
        let location = body.locations.get(instruction_index).cloned().flatten();
        if error.location.is_none() {
            error.location = location;
        }
        if error.stack.is_empty() {
            let frames = task.frames.frames();
            for (depth, frame) in frames.iter().rev().enumerate() {
                let location = if depth == 0 {
                    frame
                        .body
                        .as_ref()
                        .and_then(|body| body.locations.get(frame.ip).cloned().flatten())
                } else {
                    frames
                        .get(frames.len().saturating_sub(depth))
                        .and_then(|child| child.return_target.as_ref())
                        .and_then(|target| target.call_site.clone())
                        .or_else(|| {
                            frame
                                .body
                                .as_ref()
                                .and_then(|body| body.locations.get(frame.ip).cloned().flatten())
                        })
                };
                error.stack.push(StackFrame {
                    function: frame.function.to_string(),
                    location,
                });
            }
        }
        error
    }

    fn stop_scheduled_task(
        &mut self,
        task_id: TaskId,
        task: &mut ScheduledVmTask,
        error: RuntimeError,
    ) -> TaskStep {
        if error.kind == RuntimeErrorKind::DebuggerQuit && self.cooperative_debug_hook.is_some() {
            task.error = None;
            self.release_task_frames(task);
            return TaskStep::Cancel;
        }
        if self.cooperative_debug_hook.is_some() && task.trace.started {
            let debug_result = task.frames.current().map_or(Ok(()), |frame| {
                self.cooperative_debug_error(
                    task_id,
                    task.trace.stack.clone(),
                    frame,
                    frame.ip,
                    error.location.clone(),
                    &error,
                )
            });
            if debug_result.is_err() {
                task.error = None;
                self.release_task_frames(task);
                return TaskStep::Cancel;
            }
        }
        let step = if error.kind == RuntimeErrorKind::Cancelled {
            TaskStep::Cancel
        } else {
            TaskStep::Fail
        };
        if self.task_trace_enabled && task.trace.started {
            if let Err(trace_error) = self.task_trace_failure(task_id, task, &error.message) {
                task.error = Some(trace_error);
                self.release_task_frames(task);
                return TaskStep::Fail;
            }
        }
        task.error = Some(error);
        self.release_task_frames(task);
        step
    }

    fn enter_recursive_trace(
        &mut self,
        frame: &Frame,
        location: Option<DebugLocation>,
    ) -> Result<(), RuntimeError> {
        if self.trace_enabled {
            self.trace_enter(frame, location.clone());
        }
        if self.active_task_trace.is_some() {
            self.active_task_trace_enter(frame, location)?;
        }
        Ok(())
    }

    fn trace_recursive_instruction(
        &mut self,
        frame: &Frame,
        instruction: usize,
        location: Option<DebugLocation>,
    ) -> Result<(), RuntimeError> {
        if self.trace_enabled {
            self.trace_instruction(frame, instruction, location.clone());
        }
        if self.active_task_trace.is_some() {
            self.active_task_trace_instruction(frame, instruction, location)?;
        }
        Ok(())
    }

    fn execute_recursive_print(
        &mut self,
        body: &Function,
        frame: &Frame,
        instruction: usize,
        register: usize,
    ) -> Result<(), RuntimeError> {
        let value = self.read_register_ref(frame, register)?.to_string();
        let mut output = value.clone();
        output.push('\n');
        let location = body.locations.get(instruction).cloned().flatten();
        if let Some(task_id) = self.active_cooperative_task {
            let sequence = self.append_task_output(task_id, &output)?;
            if self.active_task_trace.is_some() {
                self.active_task_trace_event_at_sequence(
                    sequence,
                    TraceEventKind::Output,
                    frame,
                    Some(instruction),
                    location.clone(),
                    Some(value.clone()),
                )?;
            }
        } else {
            self.append_output(&output)?;
        }
        if self.trace_enabled {
            self.emit_trace(
                TraceEventKind::Output,
                frame,
                Some(instruction),
                location,
                Some(value),
            );
        }
        Ok(())
    }

    fn trace_recursive_return(
        &mut self,
        body: &Function,
        frame: &Frame,
        instruction: usize,
        value: &Value,
    ) -> Result<(), RuntimeError> {
        let location = body.locations.get(instruction).cloned().flatten();
        if self.active_task_trace.is_some() {
            let rendered = value.to_string();
            self.active_task_trace_event(
                TraceEventKind::Return,
                frame,
                Some(instruction),
                location.clone(),
                Some(rendered.clone()),
            )?;
            self.active_task_trace_leave(frame, Some(instruction), Some(rendered))?;
        }
        if self.trace_enabled {
            self.emit_trace(
                TraceEventKind::Return,
                frame,
                Some(instruction),
                location,
                Some(value.to_string()),
            );
            self.trace_leave(frame, Some(instruction), Some(value.to_string()));
        }
        Ok(())
    }

    fn trace_recursive_error(
        &mut self,
        body: &Function,
        frame: &Frame,
        instruction: usize,
        error: &RuntimeError,
    ) -> Result<(), RuntimeError> {
        let location = body.locations.get(instruction).cloned().flatten();
        if self.trace_enabled {
            self.emit_trace(
                TraceEventKind::Error,
                frame,
                Some(instruction),
                location.clone(),
                Some(error.message.clone()),
            );
            self.trace_leave(frame, Some(instruction), None);
        }
        if self.active_task_trace.is_some() {
            self.active_task_trace_event(
                TraceEventKind::Error,
                frame,
                Some(instruction),
                location,
                Some(error.message.clone()),
            )?;
            self.active_task_trace_leave(frame, Some(instruction), None)?;
        }
        Ok(())
    }

    fn leave_recursive_trace(
        &mut self,
        frame: &Frame,
        instruction: Option<usize>,
    ) -> Result<(), RuntimeError> {
        if self.trace_enabled {
            self.trace_leave(frame, instruction, None);
        }
        if self.active_task_trace.is_some() {
            self.active_task_trace_leave(frame, instruction, None)?;
        }
        Ok(())
    }

    fn execute_body(
        &mut self,
        body: &Function,
        frame: &mut Frame,
    ) -> Result<Option<Value>, RuntimeError> {
        frame.ip = 0;
        if self.profile_enabled {
            self.profile_function_entry(frame);
        }
        self.enter_recursive_trace(frame, body.locations.first().cloned().flatten())?;
        // The default `run` path has no observability consumer. The set of
        // active hooks is stable for the duration of one body execution, so
        // resolve it once instead of re-testing every instruction; diagnostics
        // reconstruct the location on failure.
        let needs_location = self.trace_enabled
            || self.active_task_trace.is_some()
            || self.debug_hook.is_some()
            || self.profile_enabled;
        while frame.ip < body.instructions.len() {
            let instruction_index = frame.ip;
            let location = needs_location
                .then(|| body.locations.get(instruction_index).cloned().flatten())
                .flatten();
            self.trace_recursive_instruction(frame, instruction_index, location.clone())?;
            if self.cooperative_debug_hook.is_some() {
                let active = self
                    .active_task_trace
                    .as_ref()
                    .map(|active| (active.task_id, active.state.stack.clone()));
                if let Some((task_id, stack)) = active {
                    self.cooperative_debug_instruction(
                        task_id,
                        stack,
                        frame,
                        instruction_index,
                        location.clone(),
                    )?;
                }
            }
            if self.debug_hook.is_some() {
                self.debug_instruction(frame, instruction_index, location.clone())?;
            }
            let instruction = &body.instructions[instruction_index];
            let mut jumped = false;
            let result = (|| -> Result<Option<Value>, RuntimeError> {
                self.checkpoint_instruction()?;
                if self.profile_enabled {
                    self.profile_instruction(frame, location.as_ref());
                }
                let call_site = body.locations.get(frame.ip).and_then(Option::as_ref);
                match instruction {
                    Instruction::Call {
                        dest,
                        callee,
                        arguments,
                    } => {
                        let Value::Function(function) = self.read_register_ref(frame, *callee)?
                        else {
                            return Err(RuntimeError::new("can only call functions"));
                        };
                        let values = match arguments.as_slice() {
                            [] => CallArguments::Empty,
                            [argument] => CallArguments::One(self.read_register(frame, *argument)?),
                            [left, right] => {
                                let left = self.read_register(frame, *left)?;
                                let right = self.read_register(frame, *right)?;
                                CallArguments::Two(left, right)
                            }
                            arguments => {
                                let mut values = Vec::with_capacity(arguments.len());
                                for argument in arguments {
                                    values.push(self.read_register(frame, *argument)?);
                                }
                                CallArguments::Many(values)
                            }
                        };
                        let result = self.call_function(
                            function,
                            values,
                            false,
                            frame.function.as_ref(),
                            call_site,
                        )?;
                        self.write_register(frame, *dest, result)?;
                    }
                    Instruction::CallDirect {
                        dest,
                        function,
                        arguments,
                    } => {
                        let target =
                            self.resolved_direct_call_target(frame, instruction_index, *function);
                        let function = self.direct_call_function_value(target.0 as usize, frame)?;
                        let values = match arguments.as_slice() {
                            [] => CallArguments::Empty,
                            [argument] => CallArguments::One(self.read_register(frame, *argument)?),
                            [left, right] => {
                                let left = self.read_register(frame, *left)?;
                                let right = self.read_register(frame, *right)?;
                                CallArguments::Two(left, right)
                            }
                            arguments => {
                                let mut values = Vec::with_capacity(arguments.len());
                                for argument in arguments {
                                    values.push(self.read_register(frame, *argument)?);
                                }
                                CallArguments::Many(values)
                            }
                        };
                        let result = self.call_function(
                            &function,
                            values,
                            true,
                            frame.function.as_ref(),
                            call_site,
                        )?;
                        self.write_register(frame, *dest, result)?;
                    }
                    Instruction::BlockStart { .. } => {}
                    Instruction::Br { target } => {
                        let block_map = frame
                            .function_index
                            .and_then(|index| self.block_maps.get(index));
                        let block_map =
                            block_map
                                .or_else(|| self.block_maps.first())
                                .ok_or_else(|| {
                                    RuntimeError::invalid_instruction("branch has no function body")
                                })?;
                        let next = block_map.get(&target.0).copied().ok_or_else(|| {
                            RuntimeError::invalid_instruction("branch target out of range")
                        })?;
                        frame.ip = next;
                        jumped = true;
                        return Ok(None);
                    }
                    Instruction::BrIf {
                        condition,
                        if_true,
                        if_false,
                    } => {
                        let block_map = frame
                            .function_index
                            .and_then(|index| self.block_maps.get(index));
                        let block_map =
                            block_map
                                .or_else(|| self.block_maps.first())
                                .ok_or_else(|| {
                                    RuntimeError::invalid_instruction("branch has no function body")
                                })?;
                        let taken = self.read_register_ref(frame, *condition)?.is_truthy();
                        let next = block_map
                            .get(if taken { &if_true.0 } else { &if_false.0 })
                            .copied()
                            .ok_or_else(|| {
                                RuntimeError::invalid_instruction("branch target out of range")
                            })?;
                        frame.ip = next;
                        jumped = true;
                        return Ok(None);
                    }
                    Instruction::ReturnNil => return Ok(Some(Value::Nil)),
                    Instruction::Return { value } => {
                        return Ok(Some(self.take_register(frame, *value)?))
                    }
                    _ => {
                        self.execute_common_instruction(
                            body,
                            frame,
                            instruction_index,
                            instruction,
                        )?;
                    }
                }
                Ok(None)
            })();
            self.heap.observe_estimated_bytes();
            match result {
                Ok(Some(value)) => {
                    self.trace_recursive_return(body, frame, instruction_index, &value)?;
                    return Ok(Some(value));
                }
                Ok(None) => {
                    if !jumped {
                        frame.ip += 1;
                    }
                }
                Err(mut error) => {
                    let location = body.locations.get(frame.ip).cloned().flatten();
                    if error.location.is_none() {
                        error.location = location.clone();
                    }
                    if error.stack.is_empty() {
                        error.push_frame(frame.function.to_string(), location);
                    }
                    if error.kind != RuntimeErrorKind::DebuggerQuit {
                        let active = self
                            .active_task_trace
                            .as_ref()
                            .map(|active| (active.task_id, active.state.stack.clone()));
                        if let Some((task_id, stack)) = active {
                            self.cooperative_debug_error(
                                task_id,
                                stack,
                                frame,
                                instruction_index,
                                body.locations.get(instruction_index).cloned().flatten(),
                                &error,
                            )?;
                        }
                        let pause = DebugPause {
                            function: frame.function.to_string(),
                            instruction: instruction_index,
                            location: body.locations.get(instruction_index).cloned().flatten(),
                            stack: self.trace_stack.clone(),
                            locals: self.trace_locals(frame),
                            machine: self.machine_debug_state(frame),
                        };
                        let control = self
                            .debug_hook
                            .as_mut()
                            .map(|hook| hook.on_error(pause, &error))
                            .unwrap_or(DebugControl::Continue);
                        if control == DebugControl::Quit {
                            return Err(RuntimeError::debug_quit());
                        }
                    }
                    self.trace_recursive_error(body, frame, instruction_index, &error)?;
                    return Err(error);
                }
            }
        }
        self.leave_recursive_trace(frame, body.instructions.len().checked_sub(1))?;
        Ok(None)
    }

    /// Shared instruction semantics for both the ordinary and cooperative
    /// dispatch paths. Output, calls, returns, and jump control flow stay in
    /// the callers because the two paths attribute those observably.
    fn execute_common_instruction(
        &mut self,
        body: &Function,
        frame: &mut Frame,
        instruction_index: usize,
        instruction: &Instruction,
    ) -> Result<(), RuntimeError> {
        let call_site = body
            .locations
            .get(instruction_index)
            .and_then(Option::as_ref);
        match instruction {
            Instruction::Constant { dest, constant } => {
                let value = self.constant_value(*constant)?;
                self.write_register(frame, *dest, value)
            }
            Instruction::MakeFunction { dest, function } => {
                let value = self.make_function(function.0 as usize, frame)?;
                self.write_register(frame, *dest, value)
            }
            Instruction::Array { dest, elements } => {
                let mut values = Vec::with_capacity(elements.len());
                for element in elements {
                    values.push(self.read_register(frame, *element)?);
                }
                let value = self.allocate_array(values)?;
                self.write_register(frame, *dest, value)
            }
            Instruction::Map { dest, entries } => {
                let mut values = Vec::with_capacity(entries.len());
                for (key_register, value_register) in entries {
                    let key = self.read_register(frame, *key_register)?;
                    self.validate_map_key(&key)?;
                    let value = self.read_register(frame, *value_register)?;
                    values.push((key, value));
                }
                let value = self.allocate_map(values)?;
                self.write_register(frame, *dest, value)
            }
            Instruction::MakeStruct {
                dest,
                type_id,
                elements,
            } => {
                let layout = self.program.types.get(type_id.0 as usize).ok_or_else(|| {
                    RuntimeError::new(format!("type t{} out of range", type_id.0))
                })?;
                let type_name = Some(layout.name.clone());
                let mut fields = Vec::with_capacity(elements.len());
                for (slot, register) in elements.iter().enumerate() {
                    let name = layout
                        .field_names
                        .get(slot)
                        .cloned()
                        .unwrap_or_else(|| format!("f{slot}"));
                    fields.push((name, self.read_register(frame, *register)?));
                }
                self.charge_runtime_elements(1usize.saturating_add(fields.len()))?;
                let value = self
                    .heap
                    .allocate_struct(Some(*type_id), type_name, fields)
                    .map_err(|error| RuntimeError::new(error.to_string()))?;
                self.write_register(frame, *dest, value)
            }
            Instruction::StructGet {
                dest,
                object,
                type_id,
                slot,
            } => {
                let object = self.read_register(frame, *object)?;
                let Value::Struct(struct_value) = object else {
                    return Err(RuntimeError::new("can only access fields on structs"));
                };
                let fields = struct_value.fields.borrow();
                let (_, value) = fields.get(*slot).ok_or_else(|| {
                    RuntimeError::new(format!("struct field slot {} out of range", slot))
                })?;
                let _ = type_id;
                self.write_register(frame, *dest, value.clone())
            }
            Instruction::StructSet {
                dest,
                object,
                type_id,
                slot,
                value,
            } => {
                let object = self.read_register(frame, *object)?;
                let value = self.read_register(frame, *value)?;
                let Value::Struct(struct_value) = object else {
                    return Err(RuntimeError::new("can only assign fields on structs"));
                };
                let mut fields = struct_value.fields.borrow_mut();
                let (_, existing) = fields.get_mut(*slot).ok_or_else(|| {
                    RuntimeError::new(format!("struct field slot {} out of range", slot))
                })?;
                let _ = type_id;
                *existing = value.clone();
                self.write_register(frame, *dest, value)
            }
            Instruction::MakeVariant {
                dest,
                type_id,
                variant_id,
                payload,
            } => {
                let layout = self.program.types.get(type_id.0 as usize).ok_or_else(|| {
                    RuntimeError::new(format!("type t{} out of range", type_id.0))
                })?;
                let variant = layout.variants.get(variant_id.0 as usize).ok_or_else(|| {
                    RuntimeError::new(format!("variant v{} out of range", variant_id.0))
                })?;
                let mut fields = Vec::with_capacity(payload.len());
                for register in payload {
                    fields.push(self.read_register(frame, *register)?);
                }
                let value = self.allocate_variant(
                    *type_id,
                    *variant_id,
                    layout.name.clone(),
                    variant.name.clone(),
                    fields,
                )?;
                self.write_register(frame, *dest, value)
            }
            Instruction::IsVariant {
                dest,
                value,
                type_id,
                variant_id,
            } => {
                let input = self.read_register_ref(frame, *value)?;
                let matched = matches!(
                    input,
                    Value::Variant(variant)
                        if variant.type_id == *type_id && variant.variant_id == *variant_id
                );
                self.write_register(frame, *dest, Value::boolean(matched))
            }
            Instruction::VariantGet {
                dest,
                value,
                type_id,
                variant_id,
                index,
            } => {
                let input = self.read_register_ref(frame, *value)?;
                let Value::Variant(variant) = input else {
                    return Err(RuntimeError::new("can only access fields on enum variants"));
                };
                if variant.type_id != *type_id || variant.variant_id != *variant_id {
                    return Err(RuntimeError::new("enum variant identity mismatch"));
                }
                let field =
                    variant.fields.get(*index).cloned().ok_or_else(|| {
                        RuntimeError::new("enum variant field index out of bounds")
                    })?;
                self.write_register(frame, *dest, field)
            }
            Instruction::Move { dest, source } => {
                let value = self.read_register(frame, *source)?;
                self.write_register(frame, *dest, value)
            }
            Instruction::LoadLocal { dest, slot } => {
                let value = self.read_local(frame, *slot)?;
                self.write_register(frame, *dest, value)
            }
            Instruction::BindLocal { slot, value } => {
                let value = self.read_register(frame, *value)?;
                self.bind_local(frame, *slot, value);
                Ok(())
            }
            Instruction::SetLocal { slot, value } => {
                let value = self.read_register(frame, *value)?;
                self.set_local(frame, *slot, value)
            }
            Instruction::LoadUpvalue { dest, slot } => {
                let cell = frame
                    .upvalues
                    .get(*slot)
                    .cloned()
                    .ok_or_else(|| RuntimeError::new("upvalue index out of range"))?;
                let value = cell.borrow().clone();
                self.write_register(frame, *dest, value)
            }
            Instruction::SetUpvalue { slot, value } => {
                let value = self.read_register(frame, *value)?;
                let cell = frame
                    .upvalues
                    .get(*slot)
                    .cloned()
                    .ok_or_else(|| RuntimeError::new("upvalue index out of range"))?;
                *cell.borrow_mut() = value;
                Ok(())
            }
            Instruction::LoadGlobal { dest, slot } => {
                let value = self.read_global(*slot)?;
                self.write_register(frame, *dest, value)
            }
            Instruction::InitGlobal { slot, value } => {
                let value = self.read_register(frame, *value)?;
                self.init_global(*slot, value)
            }
            Instruction::SetGlobal { slot, value } => {
                let value = self.read_register(frame, *value)?;
                self.set_global(*slot, value)
            }
            Instruction::CallNative {
                dest,
                native,
                arguments,
            } => {
                let spec = self
                    .native_specs
                    .get(native.0 as usize)
                    .copied()
                    .flatten()
                    .ok_or_else(|| {
                        let name = self
                            .program
                            .native_imports
                            .get(native.0 as usize)
                            .map(|import| import.name.as_str())
                            .unwrap_or("?");
                        RuntimeError::new(format!("unknown native stdlib function `{}`", name))
                    })?;
                if spec.id == NativeId::Print {
                    let Some(value_register) = arguments.first() else {
                        return Err(RuntimeError::new(spec.arity_error));
                    };
                    if arguments.len() != 1 {
                        return Err(RuntimeError::new(spec.arity_error));
                    }
                    self.write_register(frame, *dest, Value::Nil)?;
                    self.execute_recursive_print(body, frame, instruction_index, *value_register)?;
                    return Ok(());
                }
                let values = match arguments.as_slice() {
                    [] => NativeArguments::Empty,
                    [argument] => NativeArguments::One(self.read_register(frame, *argument)?),
                    [left, right] => {
                        let left = self.read_register(frame, *left)?;
                        let right = self.read_register(frame, *right)?;
                        NativeArguments::Two(left, right)
                    }
                    arguments => {
                        let mut values = Vec::with_capacity(arguments.len());
                        for argument in arguments {
                            values.push(self.read_register(frame, *argument)?);
                        }
                        NativeArguments::Many(values)
                    }
                };
                let result = self.execute_native_call_indexed(
                    native.0 as usize,
                    values,
                    frame.function.as_ref(),
                    call_site,
                )?;
                self.write_register(frame, *dest, result)
            }
            Instruction::FrameAddr { dest, offset } => {
                self.execute_machine_frame_addr(frame, *dest, *offset)
            }
            Instruction::IConst { dest, width, raw } => {
                self.write_register(frame, *dest, Value::machine_int(raw & width.mask()))
            }
            Instruction::Load {
                dest,
                address,
                memory_type,
            } => self.execute_machine_load(frame, *dest, *address, *memory_type),
            Instruction::Store {
                address,
                source,
                memory_type,
            } => self.execute_machine_store(frame, *address, *source, *memory_type),
            Instruction::Memcpy {
                destination,
                source,
                size,
            } => self.execute_machine_memcpy(frame, *destination, *source, *size),
            Instruction::Memmove {
                destination,
                source,
                size,
            } => self.execute_machine_memmove(frame, *destination, *source, *size),
            Instruction::Memset {
                destination,
                value,
                size,
            } => self.execute_machine_memset(frame, *destination, *value, *size),
            Instruction::Trunc {
                dest,
                value,
                from_width,
                to_width,
            } => self.execute_machine_int_conversion(
                frame,
                *dest,
                *value,
                *from_width,
                *to_width,
                "trunc",
                false,
                false,
            ),
            Instruction::ZExt {
                dest,
                value,
                from_width,
                to_width,
            } => self.execute_machine_int_conversion(
                frame,
                *dest,
                *value,
                *from_width,
                *to_width,
                "zext",
                true,
                false,
            ),
            Instruction::SExt {
                dest,
                value,
                from_width,
                to_width,
            } => self.execute_machine_int_conversion(
                frame,
                *dest,
                *value,
                *from_width,
                *to_width,
                "sext",
                true,
                true,
            ),
            Instruction::FConst { dest, format, bits } => {
                let value = machine_float_from_bits(*format, *bits)
                    .map_err(RuntimeError::invalid_instruction)?;
                self.write_register(frame, *dest, Value::machine_float(value))
            }
            Instruction::FAdd {
                dest,
                left,
                right,
                format,
            } => self.execute_machine_float_binary(
                frame,
                *dest,
                *left,
                *right,
                *format,
                "fadd",
                MachineFloatBinaryKind::Add,
            ),
            Instruction::FSub {
                dest,
                left,
                right,
                format,
            } => self.execute_machine_float_binary(
                frame,
                *dest,
                *left,
                *right,
                *format,
                "fsub",
                MachineFloatBinaryKind::Subtract,
            ),
            Instruction::FMul {
                dest,
                left,
                right,
                format,
            } => self.execute_machine_float_binary(
                frame,
                *dest,
                *left,
                *right,
                *format,
                "fmul",
                MachineFloatBinaryKind::Multiply,
            ),
            Instruction::FDiv {
                dest,
                left,
                right,
                format,
            } => self.execute_machine_float_binary(
                frame,
                *dest,
                *left,
                *right,
                *format,
                "fdiv",
                MachineFloatBinaryKind::Divide,
            ),
            Instruction::FNeg {
                dest,
                value,
                format,
            } => {
                let value = self.expect_machine_float_format(frame, *value, *format, "fneg")?;
                self.write_register(
                    frame,
                    *dest,
                    Value::machine_float(canonical_machine_float(-value, *format)),
                )
            }
            Instruction::FCmp {
                dest,
                left,
                right,
                format,
                predicate,
            } => {
                self.execute_machine_float_compare(frame, *dest, *left, *right, *format, *predicate)
            }
            Instruction::SIToFp {
                dest,
                value,
                int_width,
                float_format,
            } => self.execute_machine_int_to_float(
                frame,
                *dest,
                *value,
                *int_width,
                *float_format,
                true,
            ),
            Instruction::UIToFp {
                dest,
                value,
                int_width,
                float_format,
            } => self.execute_machine_int_to_float(
                frame,
                *dest,
                *value,
                *int_width,
                *float_format,
                false,
            ),
            Instruction::FPToSI {
                dest,
                value,
                float_format,
                int_width,
            } => self.execute_machine_float_to_int(
                frame,
                *dest,
                *value,
                *float_format,
                *int_width,
                true,
            ),
            Instruction::FPToUI {
                dest,
                value,
                float_format,
                int_width,
            } => self.execute_machine_float_to_int(
                frame,
                *dest,
                *value,
                *float_format,
                *int_width,
                false,
            ),
            Instruction::FPExt {
                dest,
                value,
                from_format,
                to_format,
            } => self.execute_machine_float_width_conversion(
                frame,
                *dest,
                *value,
                *from_format,
                *to_format,
                "fpext",
            ),
            Instruction::FPTrunc {
                dest,
                value,
                from_format,
                to_format,
            } => self.execute_machine_float_width_conversion(
                frame,
                *dest,
                *value,
                *from_format,
                *to_format,
                "fptrunc",
            ),
            Instruction::IAdd {
                dest,
                left,
                right,
                width,
            } => self.execute_machine_int_binary(
                frame,
                *dest,
                *left,
                *right,
                *width,
                "iadd",
                u64::wrapping_add,
            ),
            Instruction::ISub {
                dest,
                left,
                right,
                width,
            } => self.execute_machine_int_binary(
                frame,
                *dest,
                *left,
                *right,
                *width,
                "isub",
                u64::wrapping_sub,
            ),
            Instruction::IMul {
                dest,
                left,
                right,
                width,
            } => self.execute_machine_int_binary(
                frame,
                *dest,
                *left,
                *right,
                *width,
                "imul",
                u64::wrapping_mul,
            ),
            Instruction::SDiv {
                dest,
                left,
                right,
                width,
            } => {
                self.execute_machine_int_division(frame, *dest, *left, *right, *width, true, false)
            }
            Instruction::UDiv {
                dest,
                left,
                right,
                width,
            } => {
                self.execute_machine_int_division(frame, *dest, *left, *right, *width, false, false)
            }
            Instruction::SRem {
                dest,
                left,
                right,
                width,
            } => self.execute_machine_int_division(frame, *dest, *left, *right, *width, true, true),
            Instruction::URem {
                dest,
                left,
                right,
                width,
            } => {
                self.execute_machine_int_division(frame, *dest, *left, *right, *width, false, true)
            }
            Instruction::And {
                dest,
                left,
                right,
                width,
            } => self.execute_machine_int_binary(
                frame,
                *dest,
                *left,
                *right,
                *width,
                "and",
                |left, right| left & right,
            ),
            Instruction::Or {
                dest,
                left,
                right,
                width,
            } => self.execute_machine_int_binary(
                frame,
                *dest,
                *left,
                *right,
                *width,
                "or",
                |left, right| left | right,
            ),
            Instruction::Xor {
                dest,
                left,
                right,
                width,
            } => self.execute_machine_int_binary(
                frame,
                *dest,
                *left,
                *right,
                *width,
                "xor",
                |left, right| left ^ right,
            ),
            Instruction::IntNot { dest, value, width } => {
                let value = self.expect_machine_int(frame, *value, "not_int")?;
                self.write_register(frame, *dest, Value::machine_int((!value) & width.mask()))
            }
            Instruction::Shl {
                dest,
                value,
                amount,
                width,
            } => self.execute_machine_int_shift(
                frame,
                *dest,
                *value,
                *amount,
                *width,
                MachineShiftKind::Left,
            ),
            Instruction::LShr {
                dest,
                value,
                amount,
                width,
            } => self.execute_machine_int_shift(
                frame,
                *dest,
                *value,
                *amount,
                *width,
                MachineShiftKind::LogicalRight,
            ),
            Instruction::AShr {
                dest,
                value,
                amount,
                width,
            } => self.execute_machine_int_shift(
                frame,
                *dest,
                *value,
                *amount,
                *width,
                MachineShiftKind::ArithmeticRight,
            ),
            Instruction::ICmp {
                dest,
                left,
                right,
                width,
                predicate,
            } => self.execute_machine_int_compare(frame, *dest, *left, *right, *width, *predicate),
            Instruction::Negate { dest, value } => {
                let input = self.expect_number(frame, *value, "negate")?;
                self.write_register(frame, *dest, Value::number(-input))
            }
            Instruction::NegNum { dest, value } => {
                let input = self.expect_number(frame, *value, "neg_num")?;
                self.write_register(frame, *dest, Value::number(-input))
            }
            Instruction::Not { dest, value } => {
                let result = !self.read_register_ref(frame, *value)?.is_truthy();
                self.write_register(frame, *dest, Value::boolean(result))
            }
            Instruction::Add { dest, left, right } => {
                let left_value = self.read_register_ref(frame, *left)?;
                let right_value = self.read_register_ref(frame, *right)?;
                let result = match (left_value, right_value) {
                    (Value::Number(left), Value::Number(right)) => Value::number(left + right),
                    (Value::String(left), Value::String(right)) => {
                        Value::string(format!("{}{}", left, right))
                    }
                    _ => return Err(RuntimeError::new("add expects two numbers or two strings")),
                };
                self.write_register(frame, *dest, result)
            }
            Instruction::AddNum { dest, left, right } => {
                let (left, right) = self.expect_two_numbers(frame, *left, *right, "add_num")?;
                self.write_register(frame, *dest, Value::number(left + right))
            }
            Instruction::ConcatStr { dest, left, right } => {
                let result = match (
                    self.read_register_ref(frame, *left)?,
                    self.read_register_ref(frame, *right)?,
                ) {
                    (Value::String(left), Value::String(right)) => {
                        Value::string(format!("{}{}", left, right))
                    }
                    _ => return Err(RuntimeError::new("concat_str expects two strings")),
                };
                self.write_register(frame, *dest, result)
            }
            Instruction::Subtract { dest, left, right } => {
                let (left, right) = self.expect_two_numbers(frame, *left, *right, "subtract")?;
                self.write_register(frame, *dest, Value::number(left - right))
            }
            Instruction::SubNum { dest, left, right } => {
                let (left, right) = self.expect_two_numbers(frame, *left, *right, "sub_num")?;
                self.write_register(frame, *dest, Value::number(left - right))
            }
            Instruction::Multiply { dest, left, right } => {
                let (left, right) = self.expect_two_numbers(frame, *left, *right, "multiply")?;
                self.write_register(frame, *dest, Value::number(left * right))
            }
            Instruction::MulNum { dest, left, right } => {
                let (left, right) = self.expect_two_numbers(frame, *left, *right, "mul_num")?;
                self.write_register(frame, *dest, Value::number(left * right))
            }
            Instruction::Divide { dest, left, right } => {
                let (left, right) = self.expect_two_numbers(frame, *left, *right, "divide")?;
                if right == 0.0 {
                    return Err(RuntimeError::new("division by zero"));
                }
                self.write_register(frame, *dest, Value::number(left / right))
            }
            Instruction::DivNum { dest, left, right } => {
                let (left, right) = self.expect_two_numbers(frame, *left, *right, "div_num")?;
                if right == 0.0 {
                    return Err(RuntimeError::new("division by zero"));
                }
                self.write_register(frame, *dest, Value::number(left / right))
            }
            Instruction::Equal { dest, left, right } => {
                let result = self
                    .read_register_ref(frame, *left)?
                    .runtime_equals(self.read_register_ref(frame, *right)?);
                self.write_register(frame, *dest, Value::boolean(result))
            }
            Instruction::NotEqual { dest, left, right } => {
                let result = !self
                    .read_register_ref(frame, *left)?
                    .runtime_equals(self.read_register_ref(frame, *right)?);
                self.write_register(frame, *dest, Value::boolean(result))
            }
            Instruction::Greater { dest, left, right } => {
                self.compare(frame, *dest, *left, *right, Comparison::Greater, call_site)
            }
            Instruction::GreaterNum { dest, left, right } => {
                self.compare_numbers(frame, *dest, *left, *right, "gt_num", |left, right| {
                    left > right
                })
            }
            Instruction::GreaterStr { dest, left, right } => {
                self.compare_strings(frame, *dest, *left, *right, "gt_str", |ordering| {
                    ordering.is_gt()
                })
            }
            Instruction::GreaterEqual { dest, left, right } => self.compare(
                frame,
                *dest,
                *left,
                *right,
                Comparison::GreaterEqual,
                call_site,
            ),
            Instruction::GreaterEqualNum { dest, left, right } => {
                self.compare_numbers(frame, *dest, *left, *right, "ge_num", |left, right| {
                    left >= right
                })
            }
            Instruction::GreaterEqualStr { dest, left, right } => {
                self.compare_strings(frame, *dest, *left, *right, "ge_str", |ordering| {
                    ordering.is_ge()
                })
            }
            Instruction::Less { dest, left, right } => {
                self.compare(frame, *dest, *left, *right, Comparison::Less, call_site)
            }
            Instruction::LessNum { dest, left, right } => {
                self.compare_numbers(frame, *dest, *left, *right, "lt_num", |left, right| {
                    left < right
                })
            }
            Instruction::LessStr { dest, left, right } => {
                self.compare_strings(frame, *dest, *left, *right, "lt_str", |ordering| {
                    ordering.is_lt()
                })
            }
            Instruction::LessEqual { dest, left, right } => self.compare(
                frame,
                *dest,
                *left,
                *right,
                Comparison::LessEqual,
                call_site,
            ),
            Instruction::LessEqualNum { dest, left, right } => {
                self.compare_numbers(frame, *dest, *left, *right, "le_num", |left, right| {
                    left <= right
                })
            }
            Instruction::LessEqualStr { dest, left, right } => {
                self.compare_strings(frame, *dest, *left, *right, "le_str", |ordering| {
                    ordering.is_le()
                })
            }
            Instruction::Index {
                dest,
                collection,
                index,
            } => {
                let collection = self.read_register_ref(frame, *collection)?;
                let index = self.read_register_ref(frame, *index)?;
                let value = self.execute_index(collection, index)?;
                self.write_register(frame, *dest, value)
            }
            Instruction::ArrayGet {
                dest,
                collection,
                index,
            } => {
                let collection = self.read_register_ref(frame, *collection)?;
                let index = self.read_register_ref(frame, *index)?;
                if !matches!(collection, Value::Array(_)) {
                    return Err(RuntimeError::new("array_get expects array"));
                }
                let value = self.execute_index(collection, index)?;
                self.write_register(frame, *dest, value)
            }
            Instruction::MapGet {
                dest,
                collection,
                index,
            } => {
                let collection = self.read_register_ref(frame, *collection)?;
                let index = self.read_register_ref(frame, *index)?;
                if !matches!(collection, Value::Map(_)) {
                    return Err(RuntimeError::new("map_get expects map"));
                }
                let value = self.execute_index(collection, index)?;
                self.write_register(frame, *dest, value)
            }
            Instruction::RangeGet {
                dest,
                collection,
                index,
            } => {
                let collection = self.read_register_ref(frame, *collection)?;
                let index = self.read_register_ref(frame, *index)?;
                if !matches!(collection, Value::Range(_)) {
                    return Err(RuntimeError::new("range_get expects range"));
                }
                let value = self.execute_index(collection, index)?;
                self.write_register(frame, *dest, value)
            }
            Instruction::AssignIndex {
                dest,
                collection,
                index,
                value,
            } => {
                let collection = self.read_register(frame, *collection)?;
                let index = self.read_register(frame, *index)?;
                let value = self.read_register(frame, *value)?;
                let assigned = self.execute_assign_index(collection, index, value)?;
                self.write_register(frame, *dest, assigned)
            }
            Instruction::ArraySet {
                dest,
                collection,
                index,
                value,
            } => {
                let collection = self.read_register(frame, *collection)?;
                let index = self.read_register(frame, *index)?;
                let value = self.read_register(frame, *value)?;
                if !matches!(collection, Value::Array(_)) {
                    return Err(RuntimeError::new("array_set expects array"));
                }
                let assigned = self.execute_assign_index(collection, index, value)?;
                self.write_register(frame, *dest, assigned)
            }
            Instruction::MapSet {
                dest,
                collection,
                index,
                value,
            } => {
                let collection = self.read_register(frame, *collection)?;
                let index = self.read_register(frame, *index)?;
                let value = self.read_register(frame, *value)?;
                if !matches!(collection, Value::Map(_)) {
                    return Err(RuntimeError::new("map_set expects map"));
                }
                let assigned = self.execute_assign_index(collection, index, value)?;
                self.write_register(frame, *dest, assigned)
            }
            Instruction::Field { dest, object, name } => {
                let object = self.read_register_ref(frame, *object)?;
                let name = self.read_name_ref(*name)?;
                let value = self.execute_field(object, name)?;
                self.write_register(frame, *dest, value)
            }
            Instruction::AssignField {
                dest,
                object,
                name,
                value,
            } => {
                let object = self.read_register(frame, *object)?;
                let name = self.read_name_ref(*name)?;
                let value = self.read_register(frame, *value)?;
                let assigned = self.execute_assign_field(object, name, value)?;
                self.write_register(frame, *dest, assigned)
            }
            Instruction::Len { dest, value } => {
                let value = self.read_register_ref(frame, *value)?;
                let length = self.execute_len(value)?;
                self.write_register(frame, *dest, length)
            }
            Instruction::LenArray { dest, value } => {
                let value = self.read_register_ref(frame, *value)?;
                if !matches!(value, Value::Array(_)) {
                    return Err(RuntimeError::new("len_array expects array"));
                }
                let length = self.execute_len(value)?;
                self.write_register(frame, *dest, length)
            }
            Instruction::LenMap { dest, value } => {
                let value = self.read_register_ref(frame, *value)?;
                if !matches!(value, Value::Map(_)) {
                    return Err(RuntimeError::new("len_map expects map"));
                }
                let length = self.execute_len(value)?;
                self.write_register(frame, *dest, length)
            }
            Instruction::LenRange { dest, value } => {
                let value = self.read_register_ref(frame, *value)?;
                if !matches!(value, Value::Range(_)) {
                    return Err(RuntimeError::new("len_range expects range"));
                }
                let length = self.execute_len(value)?;
                self.write_register(frame, *dest, length)
            }
            Instruction::LenStr { dest, value } => {
                let value = self.read_register_ref(frame, *value)?;
                if !matches!(value, Value::String(_)) {
                    return Err(RuntimeError::new("len_str expects string"));
                }
                let length = self.execute_len(value)?;
                self.write_register(frame, *dest, length)
            }
            Instruction::IterInit { dest, value } => {
                let input = self.read_register_ref(frame, *value)?;
                let iterator = match input {
                    Value::Array(array) => IteratorValue {
                        source: IteratorSource::Array(array.clone(), array.elements.borrow().len()),
                        position: Rc::new(std::cell::Cell::new(0)),
                    },
                    Value::Map(map) => {
                        let keys = map
                            .entries
                            .borrow()
                            .iter()
                            .map(|(key, _)| key.clone())
                            .collect();
                        let snapshot = self.allocate_array(keys)?;
                        let Value::Array(snapshot) = snapshot else {
                            return Err(RuntimeError::new("iter_init failed to snapshot map keys"));
                        };
                        IteratorValue {
                            source: IteratorSource::MapKeys(snapshot),
                            position: Rc::new(std::cell::Cell::new(0)),
                        }
                    }
                    Value::Range(range) => IteratorValue {
                        source: IteratorSource::Range(range.clone()),
                        position: Rc::new(std::cell::Cell::new(0)),
                    },
                    Value::String(text) => IteratorValue {
                        source: IteratorSource::StringScalars(
                            text.chars()
                                .map(|character| character.to_string())
                                .collect(),
                        ),
                        position: Rc::new(std::cell::Cell::new(0)),
                    },
                    _ => {
                        return Err(RuntimeError::new(
                            "for-in expects array, range, map, or string",
                        ))
                    }
                };
                self.write_register(frame, *dest, Value::iterator(iterator))
            }
            Instruction::IterHas { dest, value } => {
                let input = self.read_register_ref(frame, *value)?;
                let Value::Iterator(iterator) = input else {
                    return Err(RuntimeError::new("iter_has expects iterator"));
                };
                let position = iterator.position.get();
                let has = match &iterator.source {
                    IteratorSource::Array(_, length) => position < *length,
                    IteratorSource::MapKeys(array) => position < array.elements.borrow().len(),
                    IteratorSource::Range(range) => position < range.length,
                    IteratorSource::StringScalars(values) => position < values.len(),
                };
                self.write_register(frame, *dest, Value::boolean(has))
            }
            Instruction::IterNext { dest, value } => {
                let input = self.read_register_ref(frame, *value)?;
                let Value::Iterator(iterator) = input else {
                    return Err(RuntimeError::new("iter_next expects iterator"));
                };
                let position = iterator.position.get();
                let element = match &iterator.source {
                    IteratorSource::Array(array, length) => {
                        if position >= *length {
                            return Err(RuntimeError::new("iterator exhausted"));
                        }
                        array
                            .elements
                            .borrow()
                            .get(position)
                            .cloned()
                            .ok_or_else(|| RuntimeError::new("array index out of range"))?
                    }
                    IteratorSource::MapKeys(array) => {
                        let elements = array.elements.borrow();
                        if position >= elements.len() {
                            return Err(RuntimeError::new("iterator exhausted"));
                        }
                        elements[position].clone()
                    }
                    IteratorSource::Range(range) => {
                        if position >= range.length {
                            return Err(RuntimeError::new("iterator exhausted"));
                        }
                        let value = range.start as i128 + range.step as i128 * position as i128;
                        Value::number(value as i64 as f64)
                    }
                    IteratorSource::StringScalars(values) => values
                        .get(position)
                        .cloned()
                        .map(Value::string)
                        .ok_or_else(|| RuntimeError::new("iterator exhausted"))?,
                };
                iterator.position.set(position + 1);
                self.write_register(frame, *dest, element)
            }
            Instruction::InitModule { module } => {
                self.execute_module_init(*module, frame)?;
                Ok(())
            }
            Instruction::AssertNumber {
                dest,
                value,
                message,
            } => {
                let number = match self.read_register_ref(frame, *value)? {
                    Value::Number(number) => *number,
                    _ => {
                        let message = self.read_name(*message)?;
                        return Err(RuntimeError::new(message));
                    }
                };
                self.write_register(frame, *dest, Value::number(number))
            }
            Instruction::Call { .. }
            | Instruction::CallDirect { .. }
            | Instruction::BlockStart { .. }
            | Instruction::Br { .. }
            | Instruction::BrIf { .. }
            | Instruction::ReturnNil
            | Instruction::Return { .. } => Err(RuntimeError::invalid_instruction(
                "instruction is specific to one dispatch path",
            )),
        }
    }

    fn execute_instruction(
        &mut self,
        body: &Function,
        frame: &mut Frame,
        _task_id: TaskId,
        instruction_index: usize,
        instruction: &Instruction,
    ) -> Result<InstructionAction, RuntimeError> {
        let call_site = body
            .locations
            .get(instruction_index)
            .and_then(Option::as_ref);
        match instruction {
            Instruction::BlockStart { .. } => {}
            Instruction::Br { target } => {
                let block_map = frame
                    .function_index
                    .and_then(|index| self.block_maps.get(index));
                let block_map = block_map
                    .or_else(|| self.block_maps.first())
                    .ok_or_else(|| {
                        RuntimeError::invalid_instruction("branch has no function body")
                    })?;
                let next = block_map.get(&target.0).copied().ok_or_else(|| {
                    RuntimeError::invalid_instruction("branch target out of range")
                })?;
                frame.ip = next;
                return Ok(InstructionAction::Jumped);
            }
            Instruction::BrIf {
                condition,
                if_true,
                if_false,
            } => {
                let block_map = frame
                    .function_index
                    .and_then(|index| self.block_maps.get(index));
                let block_map = block_map
                    .or_else(|| self.block_maps.first())
                    .ok_or_else(|| {
                        RuntimeError::invalid_instruction("branch has no function body")
                    })?;
                let taken = self.read_register_ref(frame, *condition)?.is_truthy();
                let next = block_map
                    .get(if taken { &if_true.0 } else { &if_false.0 })
                    .copied()
                    .ok_or_else(|| {
                        RuntimeError::invalid_instruction("branch target out of range")
                    })?;
                frame.ip = next;
                return Ok(InstructionAction::Jumped);
            }
            Instruction::Call {
                dest,
                callee,
                arguments,
            } => {
                let Value::Function(function) = self.read_register_ref(frame, *callee)? else {
                    return Err(RuntimeError::new("can only call functions"));
                };
                let values = match arguments.as_slice() {
                    [] => CallArguments::Empty,
                    [argument] => CallArguments::One(self.read_register(frame, *argument)?),
                    [left, right] => {
                        let left = self.read_register(frame, *left)?;
                        let right = self.read_register(frame, *right)?;
                        CallArguments::Two(left, right)
                    }
                    arguments => {
                        let mut values = Vec::with_capacity(arguments.len());
                        for argument in arguments {
                            values.push(self.read_register(frame, *argument)?);
                        }
                        CallArguments::Many(values)
                    }
                };
                return Ok(InstructionAction::Call(CallRequest {
                    dest: *dest,
                    function: function.clone(),
                    arguments: values,
                    direct: false,
                    caller: frame.function.to_string(),
                    call_site: call_site.cloned(),
                }));
            }
            Instruction::CallDirect {
                dest,
                function,
                arguments,
            } => {
                let target = self.resolved_direct_call_target(frame, instruction_index, *function);
                let function = self.direct_call_function_value(target.0 as usize, frame)?;
                let values = match arguments.as_slice() {
                    [] => CallArguments::Empty,
                    [argument] => CallArguments::One(self.read_register(frame, *argument)?),
                    [left, right] => {
                        let left = self.read_register(frame, *left)?;
                        let right = self.read_register(frame, *right)?;
                        CallArguments::Two(left, right)
                    }
                    arguments => {
                        let mut values = Vec::with_capacity(arguments.len());
                        for argument in arguments {
                            values.push(self.read_register(frame, *argument)?);
                        }
                        CallArguments::Many(values)
                    }
                };
                return Ok(InstructionAction::Call(CallRequest {
                    dest: *dest,
                    function,
                    arguments: values,
                    direct: true,
                    caller: frame.function.to_string(),
                    call_site: call_site.cloned(),
                }));
            }
            Instruction::Return { value } => {
                return Ok(InstructionAction::Return(
                    self.take_register(frame, *value)?,
                ));
            }
            Instruction::ReturnNil => {
                return Ok(InstructionAction::Return(Value::Nil));
            }
            _ => {
                self.execute_common_instruction(body, frame, instruction_index, instruction)?;
            }
        }
        Ok(InstructionAction::Continue)
    }

    fn check_cancellation(&self) -> Result<(), RuntimeError> {
        if self
            .config
            .cancellation
            .as_ref()
            .is_some_and(CancellationToken::is_cancelled)
        {
            Err(RuntimeError::cancelled())
        } else {
            Ok(())
        }
    }

    fn checkpoint_instruction(&mut self) -> Result<(), RuntimeError> {
        self.checkpoint_safepoint(JitSafepointKind::Instruction)
    }

    /// Materialize a borrow-free frame before handing control to a VM
    /// boundary. Instruction and native boundaries use the same checkpoint
    /// implementation as the interpreter; scheduler, GC, error, and return
    /// boundaries only capture state because their owner performs the
    /// corresponding transition.
    #[allow(dead_code)]
    fn jit_safepoint(
        &mut self,
        frame: &Frame,
        task_id: Option<TaskId>,
        kind: JitSafepointKind,
    ) -> JitSafepointTransition {
        let frame = self
            .jit
            .materialize_frame(frame, task_id, JitSafepoint::new(kind, frame.ip));
        let checkpoint = if kind.uses_instruction_budget() {
            self.checkpoint_safepoint(kind)
        } else {
            Ok(())
        };
        JitSafepointTransition { frame, checkpoint }
    }

    #[allow(dead_code)]
    fn jit_helper_bridge<'vm, 'frame>(
        &'vm mut self,
        frame: &'frame mut Frame,
        call_site: Option<DebugLocation>,
    ) -> JitHelperBridge<'vm, 'frame, 'a> {
        JitHelperBridge::new(self, frame, call_site)
    }

    fn checkpoint_safepoint(&mut self, safepoint: JitSafepointKind) -> Result<(), RuntimeError> {
        debug_assert!(safepoint.uses_instruction_budget());
        match &self.instruction_checkpoint {
            InstructionCheckpoint::Limited(limit) => {
                if self.instruction_steps >= *limit {
                    return Err(RuntimeError::resource(
                        ResourceKind::InstructionSteps,
                        *limit,
                    ));
                }
                self.instruction_steps += 1;
            }
            InstructionCheckpoint::Unlimited => {
                self.instruction_steps =
                    self.instruction_steps.checked_add(1).ok_or_else(|| {
                        RuntimeError::resource(ResourceKind::InstructionSteps, usize::MAX)
                    })?;
            }
            InstructionCheckpoint::CancelledLimited { limit, token } => {
                if token.is_cancelled() {
                    return Err(RuntimeError::cancelled());
                }
                if self.instruction_steps >= *limit {
                    return Err(RuntimeError::resource(
                        ResourceKind::InstructionSteps,
                        *limit,
                    ));
                }
                self.instruction_steps =
                    self.instruction_steps.checked_add(1).ok_or_else(|| {
                        RuntimeError::resource(ResourceKind::InstructionSteps, usize::MAX)
                    })?;
            }
            InstructionCheckpoint::CancelledUnlimited { token } => {
                if token.is_cancelled() {
                    return Err(RuntimeError::cancelled());
                }
                self.instruction_steps =
                    self.instruction_steps.checked_add(1).ok_or_else(|| {
                        RuntimeError::resource(ResourceKind::InstructionSteps, usize::MAX)
                    })?;
            }
        }
        Ok(())
    }

    fn checkpoint_native(&mut self) -> Result<(), RuntimeError> {
        self.checkpoint_safepoint(JitSafepointKind::Native)
    }

    fn check_call_depth(&self) -> Result<(), RuntimeError> {
        self.check_call_depth_at(self.call_depth)
    }

    fn check_call_depth_at(&self, current_depth: usize) -> Result<(), RuntimeError> {
        let next_depth = current_depth
            .checked_add(1)
            .ok_or_else(|| RuntimeError::resource(ResourceKind::CallDepth, usize::MAX))?;
        if let Some(limit) = self.config.max_call_depth {
            if next_depth > limit {
                return Err(RuntimeError::resource(ResourceKind::CallDepth, limit));
            }
        }
        Ok(())
    }

    fn charge_runtime_elements(&mut self, amount: usize) -> Result<(), RuntimeError> {
        self.check_cancellation()?;
        self.ensure_runtime_elements(amount)?;
        self.runtime_elements = self
            .runtime_elements
            .checked_add(amount)
            .ok_or_else(|| RuntimeError::resource(ResourceKind::RuntimeElements, usize::MAX))?;
        Ok(())
    }

    fn ensure_runtime_elements(&self, amount: usize) -> Result<(), RuntimeError> {
        self.check_cancellation()?;
        let next = self
            .runtime_elements
            .checked_add(amount)
            .ok_or_else(|| RuntimeError::resource(ResourceKind::RuntimeElements, usize::MAX))?;
        if let Some(limit) = self.config.max_runtime_elements {
            if next > limit {
                return Err(RuntimeError::resource(ResourceKind::RuntimeElements, limit));
            }
        }
        Ok(())
    }

    fn append_output(&mut self, text: &str) -> Result<(), RuntimeError> {
        let next = self
            .output_bytes
            .checked_add(text.len())
            .ok_or_else(|| RuntimeError::resource(ResourceKind::OutputBytes, usize::MAX))?;
        if let Some(limit) = self.config.max_output_bytes {
            if next > limit {
                return Err(RuntimeError::resource(ResourceKind::OutputBytes, limit));
            }
        }
        self.output.push_str(text);
        self.output_bytes = next;
        if self.profile_enabled {
            self.profile_output_bytes = self.output_bytes;
            if let Some(active) = self.active_task_profile.as_mut() {
                active.state.output(text.len());
            }
        }
        Ok(())
    }

    fn next_cooperative_event_sequence(&self) -> Result<(usize, usize), RuntimeError> {
        let sequence = self.next_task_event_sequence;
        let next_sequence = sequence
            .checked_add(1)
            .ok_or_else(|| RuntimeError::new("cooperative task event sequence exhausted"))?;
        Ok((sequence, next_sequence))
    }

    fn append_task_output(&mut self, task_id: TaskId, text: &str) -> Result<usize, RuntimeError> {
        let (sequence, next_sequence) = self.next_cooperative_event_sequence()?;
        self.append_output(text)?;
        self.next_task_event_sequence = next_sequence;
        self.task_output_events.push(TaskOutputEvent {
            sequence,
            task_id,
            text: text.to_string(),
        });
        Ok(sequence)
    }

    fn task_trace_enter(
        &mut self,
        task_id: TaskId,
        trace: &mut TaskTraceState,
        frame: &Frame,
        location: Option<DebugLocation>,
    ) -> Result<(), RuntimeError> {
        trace.started = true;
        trace.stack.push(StackFrame {
            function: frame.function.to_string(),
            location: location.clone(),
        });
        trace.last_locations.push(location.clone());
        if self.task_trace_collect_events {
            let (sequence, next_sequence) = self.next_cooperative_event_sequence()?;
            self.next_task_event_sequence = next_sequence;
            self.push_task_trace_event(
                sequence,
                task_id,
                trace,
                TraceEventKind::Enter,
                frame,
                Some(0),
                location,
                None,
            );
        }
        Ok(())
    }

    fn task_trace_instruction(
        &mut self,
        task_id: TaskId,
        trace: &mut TaskTraceState,
        frame: &Frame,
        instruction: usize,
        location: Option<DebugLocation>,
    ) -> Result<(), RuntimeError> {
        let changed = trace
            .last_locations
            .last()
            .map(|last| *last != location)
            .unwrap_or(true);
        let sequence = if changed && self.task_trace_collect_events {
            Some(self.next_cooperative_event_sequence()?)
        } else {
            None
        };
        if let Some(last) = trace.last_locations.last_mut() {
            *last = location.clone();
        }
        if let Some(active) = trace.stack.last_mut() {
            active.location = location.clone();
        }
        if let Some((sequence, next_sequence)) = sequence {
            self.next_task_event_sequence = next_sequence;
            self.push_task_trace_event(
                sequence,
                task_id,
                trace,
                TraceEventKind::Line,
                frame,
                Some(instruction),
                location,
                None,
            );
        }
        Ok(())
    }

    fn task_trace_event(
        &mut self,
        task_id: TaskId,
        trace: &TaskTraceState,
        kind: TraceEventKind,
        frame: &Frame,
        instruction: Option<usize>,
        location: Option<DebugLocation>,
        value: Option<String>,
    ) -> Result<(), RuntimeError> {
        if !self.task_trace_collect_events {
            return Ok(());
        }
        let (sequence, next_sequence) = self.next_cooperative_event_sequence()?;
        self.next_task_event_sequence = next_sequence;
        self.push_task_trace_event(
            sequence,
            task_id,
            trace,
            kind,
            frame,
            instruction,
            location,
            value,
        );
        Ok(())
    }

    fn task_trace_event_at_sequence(
        &mut self,
        sequence: usize,
        task_id: TaskId,
        trace: &TaskTraceState,
        kind: TraceEventKind,
        frame: &Frame,
        instruction: Option<usize>,
        location: Option<DebugLocation>,
        value: Option<String>,
    ) {
        if self.task_trace_collect_events {
            self.push_task_trace_event(
                sequence,
                task_id,
                trace,
                kind,
                frame,
                instruction,
                location,
                value,
            );
        }
    }

    fn task_trace_leave(
        &mut self,
        task_id: TaskId,
        trace: &mut TaskTraceState,
        frame: &Frame,
        instruction: Option<usize>,
        value: Option<String>,
    ) -> Result<(), RuntimeError> {
        let location = trace
            .stack
            .last()
            .and_then(|active| active.location.clone());
        self.task_trace_event(
            task_id,
            trace,
            TraceEventKind::Exit,
            frame,
            instruction,
            location,
            value,
        )?;
        trace.stack.pop();
        trace.last_locations.pop();
        Ok(())
    }

    fn push_task_trace_event(
        &mut self,
        sequence: usize,
        task_id: TaskId,
        trace: &TaskTraceState,
        kind: TraceEventKind,
        frame: &Frame,
        instruction: Option<usize>,
        location: Option<DebugLocation>,
        value: Option<String>,
    ) {
        self.task_trace_events.push(TaskTraceEvent {
            sequence,
            task_id,
            kind,
            function: frame.function.to_string(),
            instruction,
            location,
            stack: trace.stack.clone(),
            locals: self.trace_locals(frame),
            value,
        });
    }

    fn task_trace_failure(
        &mut self,
        task_id: TaskId,
        task: &mut ScheduledVmTask,
        message: &str,
    ) -> Result<(), RuntimeError> {
        while !task.trace.stack.is_empty() {
            let frame_count = task.frames.frames().len();
            if frame_count == 0 {
                task.trace = TaskTraceState::default();
                break;
            }
            let frame_index = task
                .trace
                .stack
                .len()
                .saturating_sub(1)
                .min(frame_count - 1);
            let is_current = frame_index + 1 == frame_count;
            let instruction = {
                let frame = &task.frames.frames()[frame_index];
                if is_current {
                    Some(frame.ip)
                } else {
                    frame.ip.checked_sub(1)
                }
            };
            let location = task
                .trace
                .stack
                .get(frame_index)
                .and_then(|frame| frame.location.clone());
            let ScheduledVmTask { frames, trace, .. } = task;
            let frame = &frames.frames()[frame_index];
            self.task_trace_event(
                task_id,
                trace,
                TraceEventKind::Error,
                frame,
                instruction,
                location,
                Some(message.to_string()),
            )?;
            self.task_trace_leave(task_id, trace, frame, instruction, None)?;
        }
        Ok(())
    }

    fn active_task_trace_enter(
        &mut self,
        frame: &Frame,
        location: Option<DebugLocation>,
    ) -> Result<(), RuntimeError> {
        let Some(mut active) = self.active_task_trace.take() else {
            return Err(RuntimeError::new("missing cooperative task trace state"));
        };
        let result = self.task_trace_enter(active.task_id, &mut active.state, frame, location);
        self.active_task_trace = Some(active);
        result
    }

    fn active_task_trace_instruction(
        &mut self,
        frame: &Frame,
        instruction: usize,
        location: Option<DebugLocation>,
    ) -> Result<(), RuntimeError> {
        let Some(mut active) = self.active_task_trace.take() else {
            return Err(RuntimeError::new("missing cooperative task trace state"));
        };
        let result = self.task_trace_instruction(
            active.task_id,
            &mut active.state,
            frame,
            instruction,
            location,
        );
        self.active_task_trace = Some(active);
        result
    }

    fn active_task_trace_event(
        &mut self,
        kind: TraceEventKind,
        frame: &Frame,
        instruction: Option<usize>,
        location: Option<DebugLocation>,
        value: Option<String>,
    ) -> Result<(), RuntimeError> {
        let Some(active) = self.active_task_trace.take() else {
            return Err(RuntimeError::new("missing cooperative task trace state"));
        };
        let result = self.task_trace_event(
            active.task_id,
            &active.state,
            kind,
            frame,
            instruction,
            location,
            value,
        );
        self.active_task_trace = Some(active);
        result
    }

    fn active_task_trace_event_at_sequence(
        &mut self,
        sequence: usize,
        kind: TraceEventKind,
        frame: &Frame,
        instruction: Option<usize>,
        location: Option<DebugLocation>,
        value: Option<String>,
    ) -> Result<(), RuntimeError> {
        let Some(active) = self.active_task_trace.take() else {
            return Err(RuntimeError::new("missing cooperative task trace state"));
        };
        self.task_trace_event_at_sequence(
            sequence,
            active.task_id,
            &active.state,
            kind,
            frame,
            instruction,
            location,
            value,
        );
        self.active_task_trace = Some(active);
        Ok(())
    }

    fn active_task_trace_leave(
        &mut self,
        frame: &Frame,
        instruction: Option<usize>,
        value: Option<String>,
    ) -> Result<(), RuntimeError> {
        let Some(mut active) = self.active_task_trace.take() else {
            return Err(RuntimeError::new("missing cooperative task trace state"));
        };
        let result =
            self.task_trace_leave(active.task_id, &mut active.state, frame, instruction, value);
        self.active_task_trace = Some(active);
        result
    }

    fn trace_enter(&mut self, frame: &Frame, location: Option<DebugLocation>) {
        if !self.trace_enabled {
            return;
        }
        self.trace_stack.push(StackFrame {
            function: frame.function.to_string(),
            location: location.clone(),
        });
        self.trace_last_locations.push(location.clone());
        self.emit_trace(TraceEventKind::Enter, frame, Some(0), location, None);
    }

    fn debug_instruction(
        &mut self,
        frame: &Frame,
        instruction: usize,
        location: Option<DebugLocation>,
    ) -> Result<(), RuntimeError> {
        if self.debug_hook.is_none() {
            return Ok(());
        }
        let pause = DebugPause {
            function: frame.function.to_string(),
            instruction,
            location,
            stack: self.trace_stack.clone(),
            locals: self.trace_locals(frame),
            machine: self.machine_debug_state(frame),
        };
        let control = self
            .debug_hook
            .as_mut()
            .expect("debug hook checked above")
            .on_instruction(pause);
        if control == DebugControl::Quit {
            return Err(RuntimeError::debug_quit());
        }
        Ok(())
    }

    fn cooperative_debug_instruction(
        &mut self,
        task_id: TaskId,
        stack: Vec<StackFrame>,
        frame: &Frame,
        instruction: usize,
        location: Option<DebugLocation>,
    ) -> Result<(), RuntimeError> {
        if self.cooperative_debug_hook.is_none() {
            return Ok(());
        }
        let pause = self.cooperative_debug_pause(task_id, stack, frame, instruction, location);
        let control = self
            .cooperative_debug_hook
            .as_mut()
            .expect("cooperative debug hook checked above")
            .on_instruction(pause);
        if control == DebugControl::Quit {
            self.cooperative_debug_quit = true;
            return Err(RuntimeError::debug_quit());
        }
        Ok(())
    }

    fn cooperative_debug_error(
        &mut self,
        task_id: TaskId,
        stack: Vec<StackFrame>,
        frame: &Frame,
        instruction: usize,
        location: Option<DebugLocation>,
        error: &RuntimeError,
    ) -> Result<(), RuntimeError> {
        if self.cooperative_debug_hook.is_none() {
            return Ok(());
        }
        let pause = self.cooperative_debug_pause(task_id, stack, frame, instruction, location);
        let control = self
            .cooperative_debug_hook
            .as_mut()
            .expect("cooperative debug hook checked above")
            .on_error(pause, error);
        if control == DebugControl::Quit {
            self.cooperative_debug_quit = true;
            return Err(RuntimeError::debug_quit());
        }
        Ok(())
    }

    fn cooperative_debug_pause(
        &self,
        task_id: TaskId,
        stack: Vec<StackFrame>,
        frame: &Frame,
        instruction: usize,
        location: Option<DebugLocation>,
    ) -> CooperativeDebugPause {
        let scheduler = self
            .active_cooperative_debug_state
            .clone()
            .expect("cooperative debug callback retains scheduler state");
        debug_assert_eq!(scheduler.running, task_id);
        CooperativeDebugPause {
            task_id,
            function: frame.function.to_string(),
            instruction,
            location,
            stack,
            locals: self.trace_locals(frame),
            machine: self.machine_debug_state(frame),
            scheduler,
        }
    }

    fn trace_instruction(
        &mut self,
        frame: &Frame,
        instruction: usize,
        location: Option<DebugLocation>,
    ) {
        if !self.trace_enabled {
            return;
        }
        let changed = self
            .trace_last_locations
            .last()
            .map(|last| *last != location)
            .unwrap_or(true);
        if let Some(last) = self.trace_last_locations.last_mut() {
            *last = location.clone();
        }
        if let Some(active) = self.trace_stack.last_mut() {
            active.location = location.clone();
        }
        if changed {
            self.emit_trace(
                TraceEventKind::Line,
                frame,
                Some(instruction),
                location,
                None,
            );
        }
    }

    fn trace_leave(&mut self, frame: &Frame, instruction: Option<usize>, value: Option<String>) {
        if !self.trace_enabled {
            return;
        }
        let location = self
            .trace_stack
            .last()
            .and_then(|active| active.location.clone());
        self.emit_trace(TraceEventKind::Exit, frame, instruction, location, value);
        self.trace_stack.pop();
        self.trace_last_locations.pop();
    }

    fn emit_trace(
        &mut self,
        kind: TraceEventKind,
        frame: &Frame,
        instruction: Option<usize>,
        location: Option<DebugLocation>,
        value: Option<String>,
    ) {
        if !self.trace_enabled || !self.trace_collect_events {
            return;
        }
        self.trace_events.push(TraceEvent {
            sequence: self.trace_events.len(),
            kind,
            function: frame.function.to_string(),
            instruction,
            location,
            stack: self.trace_stack.clone(),
            locals: self.trace_locals(frame),
            value,
        });
    }

    fn trace_locals(&self, frame: &Frame) -> Vec<(String, String)> {
        let mut locals = BTreeMap::new();
        if frame.is_main {
            for (slot, cell) in self.global_cells.iter().enumerate() {
                let Some(cell) = cell else { continue };
                let name = self
                    .global_names
                    .get(slot)
                    .cloned()
                    .unwrap_or_else(|| format!("g{slot}"));
                locals.insert(name, cell.borrow().to_string());
            }
        }
        for (name, cell) in frame.closure.borrow().iter() {
            locals.insert(name.clone(), cell.borrow().to_string());
        }
        if let Some(plan) = frame.variable_plan.as_ref() {
            for (slot, cell) in frame.locals.borrow().iter().enumerate() {
                let Some(cell) = cell else { continue };
                let name = plan.local_names.get(slot).cloned().unwrap_or_default();
                locals.insert(name, cell.borrow().to_string());
            }
        }
        locals.into_iter().collect()
    }

    fn machine_debug_state(&self, frame: &Frame) -> Option<DebugMachineState> {
        let has_machine_value = frame.registers.iter().any(|value| {
            matches!(
                value,
                Value::MachineInt(_) | Value::MachineFloat(_) | Value::Address(_)
            )
        });
        if frame.machine_frame_size == 0 && !has_machine_value {
            return None;
        }
        Some(DebugMachineState {
            registers: frame
                .registers
                .iter()
                .enumerate()
                .map(|(index, value)| (index, value.to_string()))
                .collect(),
            frame_base: frame.machine_frame_base,
            frame_size: frame.machine_frame_size,
        })
    }

    fn capture_environment(&self, frame: &Frame) -> SharedEnvironment {
        let captured = self.heap.new_environment();
        {
            let mut target = captured.borrow_mut();
            for (name, cell) in frame.closure.borrow().iter() {
                target.insert(name.clone(), cell.clone());
            }
            if let Some(plan) = frame.variable_plan.as_ref() {
                for (slot, cell) in frame.locals.borrow().iter().enumerate() {
                    let Some(cell) = cell else { continue };
                    let name = plan.local_names.get(slot).cloned().unwrap_or_default();
                    target.insert(name, cell.clone());
                }
            }
        }
        captured
    }

    fn prepared_function(&self, function_index: usize) -> Option<Rc<PreparedFunction>> {
        self.prepared_functions.get(function_index).cloned()
    }

    fn resolved_direct_call_target(
        &self,
        frame: &Frame,
        instruction_index: usize,
        encoded: FuncId,
    ) -> FuncId {
        let caller = frame
            .function_index
            .unwrap_or(self.program.entry.0 as usize);
        self.direct_call_relocations
            .get(&(caller, instruction_index))
            .copied()
            .unwrap_or(encoded)
    }

    fn new_function_frame(
        &mut self,
        function_index: usize,
        cached: &PreparedFunction,
        arguments: Vec<Value>,
        closure: SharedEnvironment,
        upvalues: Vec<Cell>,
        return_target: Option<ReturnTarget>,
    ) -> Frame {
        let locals = self.heap.new_local_slots();
        locals
            .borrow_mut()
            .resize(cached.variable_plan.local_names.len(), None);
        {
            let mut slots = locals.borrow_mut();
            for (slot, value) in arguments.into_iter().enumerate() {
                if let Some(target) = slots.get_mut(slot) {
                    *target = Some(self.heap.new_cell(value));
                }
            }
        }
        Frame {
            body: Some(Rc::clone(&cached.body)),
            ip: 0,
            registers: vec![Value::Nil; cached.body.registers],
            locals,
            closure,
            upvalues,
            variable_plan: Some(Rc::clone(&cached.variable_plan)),
            is_main: false,
            function: Rc::clone(&cached.name),
            function_index: Some(function_index),
            return_target,
            machine_frame_base: None,
            machine_frame_size: cached.body.machine_frame_size,
        }
    }

    fn make_function(
        &mut self,
        function_index: usize,
        frame: &Frame,
    ) -> Result<Value, RuntimeError> {
        let function = self
            .program
            .functions
            .get(function_index)
            .ok_or_else(|| RuntimeError::new("function index out of range"))?;
        if function.has_machine_abi() {
            return Err(RuntimeError::new(format!(
                "machine ABI function `{}` cannot be used as a function value; use call_direct",
                function.name
            )));
        }
        self.charge_runtime_elements(1)?;
        let closure = self.capture_environment(frame);
        let upvalues = function
            .upvalues
            .iter()
            .map(|descriptor| match descriptor.source {
                UpvalueSource::Local(local) => frame
                    .locals
                    .borrow()
                    .get(local.0 as usize)
                    .cloned()
                    .flatten()
                    .ok_or_else(|| RuntimeError::new("missing captured local slot")),
                UpvalueSource::Upvalue(upvalue) => frame
                    .upvalues
                    .get(upvalue.0 as usize)
                    .cloned()
                    .ok_or_else(|| RuntimeError::new("missing captured upvalue slot")),
                UpvalueSource::Global(global) => self
                    .global_cells
                    .get(global.0 as usize)
                    .cloned()
                    .flatten()
                    .ok_or_else(|| RuntimeError::new("missing captured global slot")),
            })
            .collect::<Result<Vec<_>, _>>()?;
        self.heap
            .allocate_function(
                function.name.clone(),
                function_index,
                function.params.len(),
                closure,
                upvalues,
            )
            .map_err(|error| RuntimeError::new(error.to_string()))
    }

    /// Build the lightweight callable value for a verified non-capturing
    /// direct call without charging runtime elements or copying the caller's
    /// name environment. Global-source upvalues still resolve through the
    /// global cells, matching the ordinary closure path.
    fn direct_call_function_value(
        &self,
        function_index: usize,
        frame: &Frame,
    ) -> Result<FunctionValue, RuntimeError> {
        let function = self
            .program
            .functions
            .get(function_index)
            .ok_or_else(|| RuntimeError::new("function index out of range"))?;
        let upvalues = function
            .upvalues
            .iter()
            .map(|descriptor| match descriptor.source {
                UpvalueSource::Local(local) => frame
                    .locals
                    .borrow()
                    .get(local.0 as usize)
                    .cloned()
                    .flatten()
                    .ok_or_else(|| RuntimeError::new("missing captured local slot")),
                UpvalueSource::Upvalue(upvalue) => frame
                    .upvalues
                    .get(upvalue.0 as usize)
                    .cloned()
                    .ok_or_else(|| RuntimeError::new("missing captured upvalue slot")),
                UpvalueSource::Global(global) => self
                    .global_cells
                    .get(global.0 as usize)
                    .cloned()
                    .flatten()
                    .ok_or_else(|| RuntimeError::new("missing captured global slot")),
            })
            .collect::<Result<Vec<_>, _>>()?;
        Ok(FunctionValue {
            name: function.name.clone(),
            function_index,
            arity: function.params.len(),
            identity: 0,
            closure: self.heap.new_environment(),
            upvalues,
        })
    }

    fn execute_module_init(
        &mut self,
        module: usize,
        frame: &mut Frame,
    ) -> Result<(), RuntimeError> {
        let Some(&state) = self.module_states.get(module) else {
            return Err(RuntimeError::new("module index out of range"));
        };
        match state {
            ModuleState::Initialized => Ok(()),
            ModuleState::Initializing => Err(RuntimeError::new("cyclic module initialization")),
            ModuleState::Uninitialized => {
                let Some(record) = self.program.modules.get(module) else {
                    return Err(RuntimeError::new("module index out of range"));
                };
                let init = record.init.0 as usize;
                self.module_states[module] = ModuleState::Initializing;
                let function = self.direct_call_function_value(init, frame)?;
                let caller = frame.function.to_string();
                let result =
                    self.call_function(&function, CallArguments::Empty, true, &caller, None);
                match result {
                    Ok(_) => {
                        self.module_states[module] = ModuleState::Initialized;
                        Ok(())
                    }
                    Err(error) => {
                        self.module_states[module] = ModuleState::Uninitialized;
                        Err(error)
                    }
                }
            }
        }
    }

    fn jit_execution_mode(&self) -> JitExecutionMode {
        if self.active_cooperative_task.is_some() {
            JitExecutionMode::Cooperative
        } else if self.debug_hook.is_some() || self.cooperative_debug_hook.is_some() {
            JitExecutionMode::Debug
        } else if self.profile_enabled {
            JitExecutionMode::Profile
        } else if self.trace_enabled {
            JitExecutionMode::Trace
        } else {
            JitExecutionMode::Ordinary
        }
    }

    fn execute_jit_function(
        &mut self,
        function_index: usize,
        frame: &mut Frame,
        arguments: &[Value],
        call_site: Option<&DebugLocation>,
    ) -> Result<JitCallOutcome, RuntimeError> {
        let mode = self.jit_execution_mode();
        let Some(function) = self.program.functions.get(function_index) else {
            return Ok(JitCallOutcome::Fallback);
        };
        // Helper calls and register stores make the generated entry larger
        // than the bytecode body. Keep the admission estimate conservative;
        // the backend still verifies the finalized machine-code size before
        // publishing the cache entry.
        let estimated_code_bytes = function
            .instructions
            .len()
            .saturating_mul(96)
            .saturating_add(function.registers.saturating_mul(16))
            .saturating_add(64)
            .max(1);
        let admission = self.jit.admit(
            self.program,
            Some(function_index),
            mode,
            estimated_code_bytes,
        );
        let handle = match admission {
            crate::jit::JitAdmission::Reserved { handle, .. }
            | crate::jit::JitAdmission::Cached { handle, .. } => handle,
            crate::jit::JitAdmission::Fallback(_) => return Ok(JitCallOutcome::Fallback),
        };
        let code = match self.jit.resolve_code_pointer(&handle) {
            Ok(code) => code,
            Err(_) => return Ok(JitCallOutcome::Fallback),
        };

        // A protocol failure can only occur before an observable operation in
        // the initial scalar subset. Restore the frame and checkpoint counter
        // before handing the same callee to the authoritative interpreter.
        let baseline = self.jit.materialize_frame(
            frame,
            self.active_cooperative_task,
            JitSafepoint::new(JitSafepointKind::Error, frame.ip),
        );
        let checkpoint_start = self.instruction_steps;
        let mut bridge = self.jit_helper_bridge(frame, call_site.cloned());
        let mut argument_handles = Vec::with_capacity(arguments.len());
        for argument in arguments {
            argument_handles.push(bridge.handle(argument.clone())?);
        }
        let mut context = JitCallContext {
            data: &mut bridge as *mut _ as *mut (),
            dispatch: jit_helper_dispatch,
        };
        let invocation = unsafe {
            code.invoke(
                &mut context as *mut JitCallContext as usize as u64,
                &argument_handles,
            )
        };
        let invocation_failed = invocation.is_err();
        if !invocation_failed && bridge.error.is_none() {
            bridge.materialize(JitSafepointKind::Return);
        }
        let (helper_error, helper_fallback) = bridge.take_execution_state();
        let raw_result = invocation.ok();
        let result = raw_result
            .filter(|handle| *handle != JIT_ERROR_HANDLE)
            .map(|handle| bridge.value(handle));
        drop(context);
        drop(bridge);

        if helper_fallback || invocation_failed {
            baseline.restore_into(frame);
            self.instruction_steps = checkpoint_start;
            return Ok(JitCallOutcome::Fallback);
        }
        if let Some(mut error) = helper_error {
            if error.location.is_none() {
                error.location = frame
                    .body
                    .as_ref()
                    .and_then(|body| body.locations.get(frame.ip).cloned().flatten());
            }
            if error.stack.is_empty() {
                error.push_frame(frame.function.to_string(), error.location.clone());
            }
            return Err(error);
        }
        if raw_result == Some(JIT_ERROR_HANDLE) {
            baseline.restore_into(frame);
            self.instruction_steps = checkpoint_start;
            return Ok(JitCallOutcome::Fallback);
        }
        let Some(result) = result else {
            baseline.restore_into(frame);
            self.instruction_steps = checkpoint_start;
            return Ok(JitCallOutcome::Fallback);
        };
        match result {
            Ok(value) => Ok(JitCallOutcome::Executed(value)),
            Err(_) => {
                baseline.restore_into(frame);
                self.instruction_steps = checkpoint_start;
                Ok(JitCallOutcome::Fallback)
            }
        }
    }

    fn call_function(
        &mut self,
        function: &FunctionValue,
        arguments: CallArguments,
        direct: bool,
        caller: &str,
        call_site: Option<&DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        let owns_machine_stack = self.machine_stack.is_none();
        if owns_machine_stack {
            self.machine_stack = Some(self.new_machine_stack()?);
        }
        let result = self.call_function_active(function, arguments, direct, caller, call_site);
        if owns_machine_stack {
            if let Some(stack) = self.machine_stack.as_mut() {
                stack.release_all(&mut self.memory);
            }
            self.machine_stack.take();
        }
        result
    }

    fn call_function_active(
        &mut self,
        function: &FunctionValue,
        arguments: CallArguments,
        direct: bool,
        caller: &str,
        call_site: Option<&DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        let Some(cached) = self.prepared_function(function.function_index) else {
            let mut error = RuntimeError::new("function index out of range");
            error.location = call_site.cloned();
            error.push_frame(caller.to_string(), call_site.cloned());
            return Err(error);
        };

        if arguments.len() != cached.params.len() {
            let mut error = RuntimeError::new(format!(
                "expected {} arguments but got {}",
                cached.params.len(),
                arguments.len()
            ));
            error.location = call_site.cloned();
            error.push_frame(caller.to_string(), call_site.cloned());
            return Err(error);
        }

        let jit_arguments = arguments.values();
        if let Err(mut error) = self.validate_machine_call(&cached.body, &jit_arguments, direct) {
            error.location = call_site.cloned();
            error.push_frame(caller.to_string(), call_site.cloned());
            return Err(error);
        }

        self.check_call_depth()?;

        let mut frame = self.new_function_frame(
            function.function_index,
            &cached,
            jit_arguments.clone(),
            function.closure.clone(),
            function.upvalues.clone(),
            None,
        );
        self.allocate_current_machine_frame(&mut frame)?;

        self.call_depth += 1;
        let result = if self.jit.is_enabled() && !cached.body.has_machine_abi() {
            match self.execute_jit_function(
                function.function_index,
                &mut frame,
                &jit_arguments,
                call_site,
            ) {
                Ok(JitCallOutcome::Executed(value)) => Ok(Some(value)),
                Ok(JitCallOutcome::Fallback) => self.execute_body(&cached.body, &mut frame),
                Err(error) => Err(error),
            }
        } else {
            self.execute_body(&cached.body, &mut frame)
        };
        self.call_depth -= 1;
        let result = match self.release_current_machine_frame(&frame) {
            Ok(()) => result,
            Err(error) => Err(error),
        };
        let result = match result {
            Ok(result) => {
                let result = result.unwrap_or(Value::Nil);
                self.validate_machine_return(&cached.body, Some(&result))
                    .map(|_| result)
            }
            Err(error) => Err(error),
        };
        match result {
            Ok(result) => Ok(result),
            Err(mut error) => {
                if error.location.is_none() {
                    error.location = call_site.cloned();
                }
                error.push_frame(caller.to_string(), call_site.cloned());
                Err(error)
            }
        }
    }

    fn validate_machine_call(
        &self,
        function: &Function,
        arguments: &[Value],
        direct: bool,
    ) -> Result<(), RuntimeError> {
        if !function.has_machine_abi() {
            return Ok(());
        }
        if function.machine_params.len() > MACHINE_ABI_MAX_PARAMS {
            return Err(RuntimeError::invalid_instruction(format!(
                "function `{}` declares {} machine ABI parameters, maximum is {}",
                function.name,
                function.machine_params.len(),
                MACHINE_ABI_MAX_PARAMS
            )));
        }
        if function.machine_params.len() != function.arity {
            return Err(RuntimeError::invalid_instruction(format!(
                "function `{}` machine ABI declares {} parameters, but function arity is {}",
                function.name,
                function.machine_params.len(),
                function.arity
            )));
        }
        if !direct {
            return Err(RuntimeError::invalid_instruction(format!(
                "machine ABI function `{}` requires a direct call",
                function.name
            )));
        }
        if arguments.len() != function.machine_params.len() {
            return Err(RuntimeError::invalid_operand(format!(
                "machine ABI function `{}` expects {} arguments, got {}",
                function.name,
                function.machine_params.len(),
                arguments.len()
            )));
        }
        for (index, (expected, actual)) in function
            .machine_params
            .iter()
            .zip(arguments.iter())
            .enumerate()
        {
            if !machine_scalar_matches(*expected, actual) {
                return Err(RuntimeError::invalid_operand(format!(
                    "machine ABI argument {} for function `{}` expects {}, got {}",
                    index,
                    function.name,
                    expected.as_str(),
                    actual.type_name()
                )));
            }
        }
        Ok(())
    }

    fn validate_machine_return(
        &self,
        function: &Function,
        value: Option<&Value>,
    ) -> Result<(), RuntimeError> {
        if !function.has_machine_abi() {
            return Ok(());
        }
        let value = value.unwrap_or(&Value::Nil);
        let Some(expected) = function.machine_return else {
            if matches!(value, Value::Nil) {
                return Ok(());
            }
            return Err(RuntimeError::invalid_operand(format!(
                "machine ABI function `{}` has no return value, got {}",
                function.name,
                value.type_name()
            )));
        };
        if !machine_scalar_matches(expected, value) {
            return Err(RuntimeError::invalid_operand(format!(
                "machine ABI return for function `{}` expects {}, got {}",
                function.name,
                expected.as_str(),
                value.type_name()
            )));
        }
        Ok(())
    }

    fn allocate_array(&mut self, elements: Vec<Value>) -> Result<Value, RuntimeError> {
        self.charge_runtime_elements(1usize.saturating_add(elements.len()))?;
        self.heap
            .allocate_array(elements)
            .map_err(|error| RuntimeError::new(error.to_string()))
    }

    fn allocate_map(&mut self, entries: Vec<(Value, Value)>) -> Result<Value, RuntimeError> {
        let entry_count = Heap::map_entry_count(&entries);
        self.charge_runtime_elements(1usize.saturating_add(entry_count))?;
        self.heap
            .allocate_map(entries)
            .map_err(|error| RuntimeError::new(error.to_string()))
    }

    #[cfg(test)]
    fn make_array(&mut self, elements: Vec<Value>) -> Value {
        self.allocate_array(elements)
            .expect("test array allocation should fit the default budget")
    }

    #[cfg(test)]
    fn make_map(&mut self, entries: Vec<(Value, Value)>) -> Value {
        self.allocate_map(entries)
            .expect("test map allocation should fit the default budget")
    }

    fn range_bound(value: &Value, position: usize) -> Result<i64, RuntimeError> {
        let ordinal = match position {
            0 => "first",
            1 => "second",
            _ => "third",
        };
        let Value::Number(number) = value else {
            return Err(RuntimeError::new(format!(
                "range expects number as {} argument",
                ordinal
            )));
        };
        if !number.is_finite() || number.fract() != 0.0 {
            return Err(RuntimeError::new(format!(
                "range expects integer as {} argument",
                ordinal
            )));
        }
        const MIN: f64 = -9223372036854775808.0;
        const MAX_EXCLUSIVE: f64 = 9223372036854775808.0;
        if *number < MIN || *number >= MAX_EXCLUSIVE {
            return Err(RuntimeError::new("range bound out of range"));
        }
        Ok(*number as i64)
    }

    fn range_length(start: i64, stop: i64, step: i64) -> Result<usize, RuntimeError> {
        let start = start as i128;
        let stop = stop as i128;
        let step = step as i128;
        let length = if step > 0 && start < stop {
            ((stop - start - 1) / step + 1) as u128
        } else if step < 0 && start > stop {
            ((start - stop - 1) / (-step) + 1) as u128
        } else {
            0
        };
        if length > usize::MAX as u128 {
            return Err(RuntimeError::new("range length out of bounds"));
        }
        Ok(length as usize)
    }

    fn execute_native_range(&mut self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let (start, stop, step) = match arguments.len() {
            1 => (0, Self::range_bound(&arguments[0], 0)?, 1),
            2 => (
                Self::range_bound(&arguments[0], 0)?,
                Self::range_bound(&arguments[1], 1)?,
                1,
            ),
            3 => (
                Self::range_bound(&arguments[0], 0)?,
                Self::range_bound(&arguments[1], 1)?,
                Self::range_bound(&arguments[2], 2)?,
            ),
            _ => unreachable!(),
        };
        if step == 0 {
            return Err(RuntimeError::new("range step must not be zero"));
        }
        let length = Self::range_length(start, stop, step)?;
        self.charge_runtime_elements(1)?;
        Ok(self.heap.allocate_range(start, stop, step, length))
    }

    fn validate_map_key(&self, key: &Value) -> Result<(), RuntimeError> {
        if matches!(
            key,
            Value::Nil | Value::Number(_) | Value::Bool(_) | Value::String(_)
        ) {
            Ok(())
        } else {
            Err(RuntimeError::new(
                "map key must be nil, number, bool, or string",
            ))
        }
    }

    fn allocate_variant(
        &mut self,
        type_id: TypeId,
        variant_id: VariantId,
        enum_name: String,
        variant_name: String,
        fields: Vec<Value>,
    ) -> Result<Value, RuntimeError> {
        self.charge_runtime_elements(1usize.saturating_add(fields.len()))?;
        Ok(self
            .heap
            .allocate_variant(type_id, variant_id, enum_name, variant_name, fields))
    }

    fn checked_array_index(&self, index_value: &Value) -> Result<usize, RuntimeError> {
        let Value::Number(number) = index_value else {
            return Err(RuntimeError::new("array index must be number"));
        };
        let integer = number.trunc();
        if integer != *number {
            return Err(RuntimeError::new("array index must be integer"));
        }
        if integer < 0.0 {
            return Err(RuntimeError::new("array index out of range"));
        }
        Ok(integer as usize)
    }

    fn execute_index(&self, collection: &Value, index: &Value) -> Result<Value, RuntimeError> {
        match collection {
            Value::Array(array) => {
                let position = self.checked_array_index(index)?;
                let elements = array.elements.borrow();
                elements
                    .get(position)
                    .cloned()
                    .ok_or_else(|| RuntimeError::new("array index out of range"))
            }
            Value::Map(map) => {
                self.validate_map_key(&index)?;
                map.entries
                    .borrow()
                    .iter()
                    .find(|(key, _)| key.runtime_equals(&index))
                    .map(|(_, value)| value.clone())
                    .ok_or_else(|| RuntimeError::new("map key not found"))
            }
            Value::Range(range) => {
                if range.length == 0 {
                    return Err(RuntimeError::new("range index out of bounds"));
                }
                let position = Self::checked_integer_index(
                    match index {
                        Value::Number(value) => *value,
                        _ => return Err(RuntimeError::new("range index must be number")),
                    },
                    "range index must be integer",
                    "range index out of bounds",
                    range.length.saturating_sub(1),
                )?;
                let value = range.start as i128 + range.step as i128 * position as i128;
                Ok(Value::number(value as i64 as f64))
            }
            _ => Err(RuntimeError::new("can only index arrays, maps, or ranges")),
        }
    }

    fn execute_assign_index(
        &mut self,
        collection: Value,
        index: Value,
        value: Value,
    ) -> Result<Value, RuntimeError> {
        match collection {
            Value::Array(array) => {
                let position = self.checked_array_index(&index)?;
                let mut elements = array.elements.borrow_mut();
                if position >= elements.len() {
                    return Err(RuntimeError::new("array index out of range"));
                }
                elements[position] = value.clone();
                Ok(value)
            }
            Value::Map(map) => {
                self.validate_map_key(&index)?;
                let exists = map
                    .entries
                    .borrow()
                    .iter()
                    .any(|(key, _)| key.runtime_equals(&index));
                if !exists {
                    self.charge_runtime_elements(1)?;
                }
                let mut entries = map.entries.borrow_mut();
                if let Some((_, existing)) = entries
                    .iter_mut()
                    .find(|(key, _)| key.runtime_equals(&index))
                {
                    *existing = value.clone();
                } else {
                    entries.push((index, value.clone()));
                }
                Ok(value)
            }
            Value::Range(_) => Err(RuntimeError::new("cannot assign range elements")),
            _ => Err(RuntimeError::new(
                "can only assign array elements, map entries, or range elements",
            )),
        }
    }

    fn execute_field(&self, object: &Value, name: &str) -> Result<Value, RuntimeError> {
        let Value::Struct(value) = object else {
            return Err(RuntimeError::new("can only access fields on structs"));
        };
        for (field_name, field_value) in value.fields.borrow().iter() {
            if field_name == name {
                return Ok(field_value.clone());
            }
        }
        Err(RuntimeError::new(format!("undefined field `{}`", name)))
    }

    fn execute_assign_field(
        &self,
        object: Value,
        name: &str,
        value: Value,
    ) -> Result<Value, RuntimeError> {
        let Value::Struct(struct_value) = object else {
            return Err(RuntimeError::new("can only assign fields on structs"));
        };
        let mut fields = struct_value.fields.borrow_mut();
        for (field_name, field_value) in fields.iter_mut() {
            if field_name == name {
                *field_value = value.clone();
                return Ok(value);
            }
        }
        Err(RuntimeError::new(format!("undefined field `{}`", name)))
    }

    fn execute_len(&self, value: &Value) -> Result<Value, RuntimeError> {
        match value {
            Value::Array(array) => Ok(Value::number(array.elements.borrow().len() as f64)),
            Value::Map(map) => Ok(Value::number(map.entries.borrow().len() as f64)),
            Value::Range(range) => Ok(Value::number(range.length as f64)),
            Value::String(value) => Ok(Value::number(value.chars().count() as f64)),
            _ => Err(RuntimeError::new(
                "len expects array, string, map, or range",
            )),
        }
    }

    fn execute_native_call(
        &mut self,
        name: &str,
        arguments: Vec<Value>,
    ) -> Result<Value, RuntimeError> {
        self.execute_native_call_at(name, NativeArguments::from_vec(arguments), "<native>", None)
    }

    fn execute_native_call_at(
        &mut self,
        name: &str,
        arguments: NativeArguments,
        caller: &str,
        call_site: Option<&DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        let spec = native_spec(name).ok_or_else(|| {
            RuntimeError::new(format!("unknown native stdlib function `{}`", name))
        })?;
        self.execute_native_call_with_spec(spec, arguments, caller, call_site)
    }

    fn execute_native_call_indexed(
        &mut self,
        import_index: usize,
        arguments: NativeArguments,
        caller: &str,
        call_site: Option<&DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        match self.native_specs.get(import_index).copied().flatten() {
            Some(spec) => self.execute_native_call_with_spec(spec, arguments, caller, call_site),
            None => {
                let name = self
                    .program
                    .native_imports
                    .get(import_index)
                    .map(|import| import.name.as_str())
                    .unwrap_or("?");
                Err(RuntimeError::new(format!(
                    "unknown native stdlib function `{}`",
                    name
                )))
            }
        }
    }

    fn execute_native_call_with_spec(
        &mut self,
        spec: &'static NativeSpec,
        arguments: NativeArguments,
        caller: &str,
        call_site: Option<&DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        if self.profile_enabled {
            self.profile_native_call(spec.name);
        }
        if arguments.len() < spec.min_arity || arguments.len() > spec.max_arity {
            return Err(RuntimeError::new(spec.arity_error));
        }
        match spec.id {
            NativeId::Push => self.execute_native_push(arguments),
            NativeId::Pop => self.execute_native_pop(arguments),
            NativeId::Remove => self.execute_native_remove(arguments),
            NativeId::Clear => self.execute_native_clear(arguments),
            NativeId::Merge => self.execute_native_merge(arguments),
            NativeId::Keys => self.execute_native_keys(arguments),
            NativeId::Values => self.execute_native_values(arguments),
            NativeId::Floor => self.execute_native_floor(arguments),
            NativeId::Ceil => self.execute_native_ceil(arguments),
            NativeId::Sqrt => self.execute_native_sqrt(arguments),
            NativeId::Str => self.execute_native_str(arguments),
            NativeId::Substr => self.execute_native_substr(arguments),
            NativeId::CharAt => self.execute_native_char_at(arguments),
            NativeId::TypeOf => self.execute_native_type_of(arguments),
            NativeId::Hash => self.execute_native_hash(arguments),
            NativeId::Contains => self.execute_native_contains(arguments),
            NativeId::Slice => self.execute_native_slice(arguments),
            NativeId::Copy => self.execute_native_copy(arguments),
            NativeId::Concat => self.execute_native_concat(arguments),
            NativeId::Map => self.execute_native_map(arguments, caller, call_site),
            NativeId::Filter => self.execute_native_filter(arguments, caller, call_site),
            NativeId::FlatMap => self.execute_native_flat_map(arguments, caller, call_site),
            NativeId::Any => self.execute_native_any_all(arguments, caller, call_site, true),
            NativeId::All => self.execute_native_any_all(arguments, caller, call_site, false),
            NativeId::Count => self.execute_native_count(arguments, caller, call_site),
            NativeId::Find => self.execute_native_find(arguments, caller, call_site),
            NativeId::FindIndex => self.execute_native_find_index(arguments, caller, call_site),
            NativeId::Reduce => self.execute_native_reduce(arguments, caller, call_site),
            NativeId::Range => self.execute_native_range(arguments),
            NativeId::Print => Err(RuntimeError::new("print requires an execution frame")),
        }
    }

    fn execute_native_map(
        &mut self,
        arguments: NativeArguments,
        caller: &str,
        call_site: Option<&DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("map expects array as first argument"));
        };
        let Value::Function(callback) = &arguments[1] else {
            return Err(RuntimeError::new("map expects function as second argument"));
        };
        if callback.arity != 1 {
            return Err(RuntimeError::new("map expects callback with 1 argument"));
        }

        let elements = array.elements.borrow().clone();
        let mut mapped = Vec::with_capacity(elements.len());
        for element in elements {
            self.checkpoint_native()?;
            mapped.push(self.call_function(
                callback,
                CallArguments::One(element),
                false,
                caller,
                call_site,
            )?);
        }
        self.allocate_array(mapped)
    }

    fn execute_native_filter(
        &mut self,
        arguments: NativeArguments,
        caller: &str,
        call_site: Option<&DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("filter expects array as first argument"));
        };
        let Value::Function(predicate) = &arguments[1] else {
            return Err(RuntimeError::new(
                "filter expects function as second argument",
            ));
        };
        if predicate.arity != 1 {
            return Err(RuntimeError::new("filter expects callback with 1 argument"));
        }

        let elements = array.elements.borrow().clone();
        let mut filtered = Vec::with_capacity(elements.len());
        for element in elements {
            self.checkpoint_native()?;
            let keep = self.call_function(
                predicate,
                CallArguments::One(element.clone()),
                false,
                caller,
                call_site,
            )?;
            match keep {
                Value::Bool(true) => filtered.push(element),
                Value::Bool(false) => {}
                _ => return Err(RuntimeError::new("filter expects callback to return bool")),
            }
        }
        self.allocate_array(filtered)
    }

    fn execute_native_flat_map(
        &mut self,
        arguments: NativeArguments,
        caller: &str,
        call_site: Option<&DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("flatMap expects array as first argument"));
        };
        let Value::Function(callback) = &arguments[1] else {
            return Err(RuntimeError::new(
                "flatMap expects function as second argument",
            ));
        };
        if callback.arity != 1 {
            return Err(RuntimeError::new(
                "flatMap expects callback with 1 argument",
            ));
        }

        let elements = array.elements.borrow().clone();
        let mut flattened = Vec::new();
        for element in elements {
            self.checkpoint_native()?;
            let result = self.call_function(
                callback,
                CallArguments::One(element),
                false,
                caller,
                call_site,
            )?;
            let Value::Array(mapped) = result else {
                return Err(RuntimeError::new(
                    "flatMap expects callback to return array",
                ));
            };
            for value in mapped.elements.borrow().iter().cloned() {
                self.checkpoint_native()?;
                flattened.push(value);
            }
        }
        self.allocate_array(flattened)
    }

    fn execute_native_any_all(
        &mut self,
        arguments: NativeArguments,
        caller: &str,
        call_site: Option<&DebugLocation>,
        any: bool,
    ) -> Result<Value, RuntimeError> {
        let name = if any { "any" } else { "all" };
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new(format!(
                "{} expects array as first argument",
                name
            )));
        };
        let Value::Function(predicate) = &arguments[1] else {
            return Err(RuntimeError::new(format!(
                "{} expects function as second argument",
                name
            )));
        };
        if predicate.arity != 1 {
            return Err(RuntimeError::new(format!(
                "{} expects callback with 1 argument",
                name
            )));
        }

        let elements = array.elements.borrow().clone();
        for element in elements {
            self.checkpoint_native()?;
            let result = self.call_function(
                predicate,
                CallArguments::One(element),
                false,
                caller,
                call_site,
            )?;
            let Value::Bool(result) = result else {
                return Err(RuntimeError::new(format!(
                    "{} expects callback to return bool",
                    name
                )));
            };
            if (any && result) || (!any && !result) {
                return Ok(Value::boolean(result));
            }
        }
        Ok(Value::boolean(!any))
    }

    fn execute_native_count(
        &mut self,
        arguments: NativeArguments,
        caller: &str,
        call_site: Option<&DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("count expects array as first argument"));
        };
        let Value::Function(predicate) = &arguments[1] else {
            return Err(RuntimeError::new(
                "count expects function as second argument",
            ));
        };
        if predicate.arity != 1 {
            return Err(RuntimeError::new("count expects callback with 1 argument"));
        }

        let elements = array.elements.borrow().clone();
        let mut count = 0usize;
        for element in elements {
            self.checkpoint_native()?;
            let result = self.call_function(
                predicate,
                CallArguments::One(element),
                false,
                caller,
                call_site,
            )?;
            match result {
                Value::Bool(true) => count += 1,
                Value::Bool(false) => {}
                _ => return Err(RuntimeError::new("count expects callback to return bool")),
            }
        }
        Ok(Value::number(count as f64))
    }

    fn execute_native_find(
        &mut self,
        arguments: NativeArguments,
        caller: &str,
        call_site: Option<&DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("find expects array as first argument"));
        };
        let Value::Function(predicate) = &arguments[1] else {
            return Err(RuntimeError::new(
                "find expects function as second argument",
            ));
        };
        if predicate.arity != 1 {
            return Err(RuntimeError::new("find expects callback with 1 argument"));
        }

        let elements = array.elements.borrow().clone();
        for element in elements {
            self.checkpoint_native()?;
            let result = self.call_function(
                predicate,
                CallArguments::One(element.clone()),
                false,
                caller,
                call_site,
            )?;
            match result {
                Value::Bool(true) => return Ok(element),
                Value::Bool(false) => {}
                _ => return Err(RuntimeError::new("find expects callback to return bool")),
            }
        }
        Ok(Value::Nil)
    }

    fn execute_native_find_index(
        &mut self,
        arguments: NativeArguments,
        caller: &str,
        call_site: Option<&DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new(
                "findIndex expects array as first argument",
            ));
        };
        let Value::Function(predicate) = &arguments[1] else {
            return Err(RuntimeError::new(
                "findIndex expects function as second argument",
            ));
        };
        if predicate.arity != 1 {
            return Err(RuntimeError::new(
                "findIndex expects callback with 1 argument",
            ));
        }

        let elements = array.elements.borrow().clone();
        for (index, element) in elements.into_iter().enumerate() {
            self.checkpoint_native()?;
            let result = self.call_function(
                predicate,
                CallArguments::One(element),
                false,
                caller,
                call_site,
            )?;
            match result {
                Value::Bool(true) => return Ok(Value::number(index as f64)),
                Value::Bool(false) => {}
                _ => {
                    return Err(RuntimeError::new(
                        "findIndex expects callback to return bool",
                    ))
                }
            }
        }
        Ok(Value::number(-1.0))
    }

    fn execute_native_reduce(
        &mut self,
        arguments: NativeArguments,
        caller: &str,
        call_site: Option<&DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("reduce expects array as first argument"));
        };
        let Value::Function(callback) = &arguments[2] else {
            return Err(RuntimeError::new(
                "reduce expects function as third argument",
            ));
        };
        if callback.arity != 2 {
            return Err(RuntimeError::new(
                "reduce expects callback with 2 arguments",
            ));
        }

        let elements = array.elements.borrow().clone();
        let mut accumulator = arguments[1].clone();
        for element in elements {
            self.checkpoint_native()?;
            accumulator = self.call_function(
                callback,
                CallArguments::Two(accumulator, element),
                false,
                caller,
                call_site,
            )?;
        }
        Ok(accumulator)
    }

    fn execute_native_push(&mut self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("push expects array as first argument"));
        };
        self.charge_runtime_elements(1)?;
        array.elements.borrow_mut().push(arguments[1].clone());
        Ok(Value::Nil)
    }

    fn execute_native_pop(&self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("pop expects array as first argument"));
        };
        array
            .elements
            .borrow_mut()
            .pop()
            .ok_or_else(|| RuntimeError::new("cannot pop from empty array"))
    }

    fn execute_native_remove(&self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Map(map) = &arguments[0] else {
            return Err(RuntimeError::new("remove expects map as first argument"));
        };
        self.validate_map_key(&arguments[1])?;
        let mut entries = map.entries.borrow_mut();
        let position = entries
            .iter()
            .position(|(key, _)| key.runtime_equals(&arguments[1]))
            .ok_or_else(|| RuntimeError::new("map key not found"))?;
        Ok(entries.remove(position).1)
    }

    fn execute_native_clear(&self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Map(map) = &arguments[0] else {
            return Err(RuntimeError::new("clear expects map as first argument"));
        };
        map.entries.borrow_mut().clear();
        Ok(Value::Nil)
    }

    fn execute_native_merge(&mut self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Map(left) = &arguments[0] else {
            return Err(RuntimeError::new("merge expects map as first argument"));
        };
        let Value::Map(right) = &arguments[1] else {
            return Err(RuntimeError::new("merge expects map as second argument"));
        };
        let mut entries = left.entries.borrow().clone();
        for entry in right.entries.borrow().iter().cloned() {
            self.checkpoint_native()?;
            entries.push(entry);
        }
        self.allocate_map(entries)
    }

    fn execute_native_keys(&mut self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Map(map) = &arguments[0] else {
            return Err(RuntimeError::new("keys expects map as first argument"));
        };
        self.ensure_runtime_elements(1usize.saturating_add(map.entries.borrow().len()))?;
        let entries = map.entries.borrow().clone();
        let mut elements = Vec::with_capacity(entries.len());
        for (key, _) in entries {
            self.checkpoint_native()?;
            elements.push(key);
        }
        self.allocate_array(elements)
    }

    fn execute_native_values(&mut self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Map(map) = &arguments[0] else {
            return Err(RuntimeError::new("values expects map as first argument"));
        };
        self.ensure_runtime_elements(1usize.saturating_add(map.entries.borrow().len()))?;
        let entries = map.entries.borrow().clone();
        let mut elements = Vec::with_capacity(entries.len());
        for (_, value) in entries {
            self.checkpoint_native()?;
            elements.push(value);
        }
        self.allocate_array(elements)
    }

    fn execute_native_floor(&self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Number(value) = &arguments[0] else {
            return Err(RuntimeError::new("floor expects number"));
        };
        Ok(Value::number(value.floor()))
    }

    fn execute_native_ceil(&self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Number(value) = &arguments[0] else {
            return Err(RuntimeError::new("ceil expects number"));
        };
        Ok(Value::number(value.ceil()))
    }

    fn execute_native_sqrt(&self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Number(value) = &arguments[0] else {
            return Err(RuntimeError::new("sqrt expects number"));
        };
        if *value < 0.0 {
            return Err(RuntimeError::new("sqrt expects non-negative number"));
        }
        Ok(Value::number(value.sqrt()))
    }

    fn checked_integer_index(
        value: f64,
        integer_message: &'static str,
        bounds_message: &'static str,
        upper_bound_inclusive: usize,
    ) -> Result<usize, RuntimeError> {
        if !value.is_finite() || value.floor() != value {
            return Err(RuntimeError::new(integer_message));
        }
        if value < 0.0 || value > upper_bound_inclusive as f64 {
            return Err(RuntimeError::new(bounds_message));
        }
        Ok(value as usize)
    }

    fn string_scalar_offsets(text: &str) -> Vec<usize> {
        let mut offsets = text
            .char_indices()
            .map(|(offset, _)| offset)
            .collect::<Vec<_>>();
        offsets.push(text.len());
        offsets
    }

    fn execute_native_str(&self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        Ok(Value::string(arguments[0].to_string()))
    }

    fn execute_native_substr(&self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::String(text) = &arguments[0] else {
            return Err(RuntimeError::new("substr expects string as first argument"));
        };
        let Value::Number(start_value) = &arguments[1] else {
            return Err(RuntimeError::new(
                "substr expects number as second argument",
            ));
        };
        let Value::Number(length_value) = &arguments[2] else {
            return Err(RuntimeError::new("substr expects number as third argument"));
        };

        let offsets = Self::string_scalar_offsets(text);
        let scalar_count = offsets.len() - 1;
        let start = Self::checked_integer_index(
            *start_value,
            "substr expects integer start offset",
            "substr start offset out of bounds",
            scalar_count,
        )?;
        let length = Self::checked_integer_index(
            *length_value,
            "substr expects integer length",
            "substr length out of bounds",
            scalar_count,
        )?;
        if length > scalar_count - start {
            return Err(RuntimeError::new("substr length out of bounds"));
        }
        let begin = offsets[start];
        let end = offsets[start + length];
        Ok(Value::string(text.as_ref()[begin..end].to_string()))
    }

    fn execute_native_char_at(&self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::String(text) = &arguments[0] else {
            return Err(RuntimeError::new("charAt expects string as first argument"));
        };
        let Value::Number(index_value) = &arguments[1] else {
            return Err(RuntimeError::new(
                "charAt expects number as second argument",
            ));
        };
        let offsets = Self::string_scalar_offsets(text);
        let scalar_count = offsets.len() - 1;
        if scalar_count == 0 {
            return Err(RuntimeError::new("charAt index out of bounds"));
        }
        let index = Self::checked_integer_index(
            *index_value,
            "charAt expects integer index",
            "charAt index out of bounds",
            scalar_count - 1,
        )?;
        let begin = offsets[index];
        let end = offsets[index + 1];
        Ok(Value::string(text.as_ref()[begin..end].to_string()))
    }

    fn execute_native_type_of(&self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        Ok(Value::string(arguments[0].type_name()))
    }

    fn execute_native_hash(&self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        Ok(Value::number(arguments[0].runtime_hash()))
    }

    fn execute_native_contains(
        &mut self,
        arguments: NativeArguments,
    ) -> Result<Value, RuntimeError> {
        match &arguments[0] {
            Value::Array(array) => {
                let elements = array.elements.borrow().clone();
                let mut found = false;
                for element in elements {
                    self.checkpoint_native()?;
                    if element.runtime_equals(&arguments[1]) {
                        found = true;
                        break;
                    }
                }
                Ok(Value::boolean(found))
            }
            Value::Map(map) => {
                self.validate_map_key(&arguments[1])?;
                let entries = map.entries.borrow().clone();
                let mut found = false;
                for (key, _) in entries {
                    self.checkpoint_native()?;
                    if key.runtime_equals(&arguments[1]) {
                        found = true;
                        break;
                    }
                }
                Ok(Value::boolean(found))
            }
            Value::Range(range) => {
                let Value::Number(number) = &arguments[1] else {
                    return Err(RuntimeError::new("contains expects number for range"));
                };
                if !number.is_finite() || number.fract() != 0.0 {
                    return Ok(Value::boolean(false));
                }
                const MIN: f64 = -9223372036854775808.0;
                const MAX_EXCLUSIVE: f64 = 9223372036854775808.0;
                if *number < MIN || *number >= MAX_EXCLUSIVE {
                    return Ok(Value::boolean(false));
                }
                let candidate = *number as i64;
                let in_bounds = if range.step > 0 {
                    candidate >= range.start && candidate < range.stop
                } else {
                    candidate <= range.start && candidate > range.stop
                };
                let offset = candidate as i128 - range.start as i128;
                Ok(Value::boolean(
                    in_bounds && offset % range.step as i128 == 0,
                ))
            }
            _ => Err(RuntimeError::new(
                "contains expects array, map, or range as first argument",
            )),
        }
    }

    fn execute_native_slice(&mut self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("slice expects array as first argument"));
        };
        let Value::Number(start_value) = &arguments[1] else {
            return Err(RuntimeError::new("slice expects number as second argument"));
        };
        let Value::Number(length_value) = &arguments[2] else {
            return Err(RuntimeError::new("slice expects number as third argument"));
        };

        let source_len = array.elements.borrow().len();
        let start = Self::checked_integer_index(
            *start_value,
            "slice expects integer start offset",
            "slice start offset out of bounds",
            source_len,
        )?;
        let length = Self::checked_integer_index(
            *length_value,
            "slice expects integer length",
            "slice length out of bounds",
            source_len,
        )?;
        if length > source_len - start {
            return Err(RuntimeError::new("slice length out of bounds"));
        }
        self.ensure_runtime_elements(1usize.saturating_add(length))?;
        let borrowed = array.elements.borrow();
        let mut elements = Vec::with_capacity(length);
        for element in borrowed[start..start + length].iter() {
            self.checkpoint_native()?;
            elements.push(element.clone());
        }
        self.allocate_array(elements)
    }

    fn execute_native_copy(&mut self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("copy expects array as first argument"));
        };
        self.ensure_runtime_elements(1usize.saturating_add(array.elements.borrow().len()))?;
        let borrowed = array.elements.borrow();
        let mut elements = Vec::with_capacity(borrowed.len());
        for element in borrowed.iter() {
            self.checkpoint_native()?;
            elements.push(element.clone());
        }
        self.allocate_array(elements)
    }

    fn execute_native_concat(&mut self, arguments: NativeArguments) -> Result<Value, RuntimeError> {
        let Value::Array(left) = &arguments[0] else {
            return Err(RuntimeError::new("concat expects array as first argument"));
        };
        let Value::Array(right) = &arguments[1] else {
            return Err(RuntimeError::new("concat expects array as second argument"));
        };
        let left_len = left.elements.borrow().len();
        let right_len = right.elements.borrow().len();
        self.ensure_runtime_elements(1usize.saturating_add(left_len.saturating_add(right_len)))?;
        let left_elements = left.elements.borrow().clone();
        let right_elements = right.elements.borrow().clone();
        let mut elements = Vec::with_capacity(left_elements.len() + right_elements.len());
        for element in left_elements.into_iter().chain(right_elements) {
            self.checkpoint_native()?;
            elements.push(element);
        }
        self.allocate_array(elements)
    }

    fn read_name_ref(&self, index: usize) -> Result<&'a str, RuntimeError> {
        if self.verified {
            debug_assert!(index < self.program.names.len());
            Ok(&self.program.names[index])
        } else {
            self.program
                .names
                .get(index)
                .map(String::as_str)
                .ok_or_else(|| RuntimeError::new("name index out of range"))
        }
    }

    fn read_name(&self, index: usize) -> Result<String, RuntimeError> {
        Ok(self.read_name_ref(index)?.to_string())
    }

    fn read_local(&self, frame: &Frame, slot: usize) -> Result<Value, RuntimeError> {
        let value = {
            let locals = frame.locals.borrow();
            let cell = locals
                .get(slot)
                .cloned()
                .flatten()
                .ok_or_else(|| RuntimeError::new(format!("unbound local l{slot}")))?;
            let value = cell.borrow().clone();
            value
        };
        Ok(value)
    }

    fn bind_local(&mut self, frame: &mut Frame, slot: usize, value: Value) {
        let cell = self.heap.new_cell(value);
        let mut locals = frame.locals.borrow_mut();
        if locals.len() <= slot {
            locals.resize(slot + 1, None);
        }
        locals[slot] = Some(cell);
    }

    fn set_local(&self, frame: &Frame, slot: usize, value: Value) -> Result<(), RuntimeError> {
        let locals = frame.locals.borrow();
        let cell = locals
            .get(slot)
            .and_then(Option::as_ref)
            .ok_or_else(|| RuntimeError::new(format!("unbound local l{slot}")))?;
        *cell.borrow_mut() = value;
        Ok(())
    }

    fn read_global(&self, slot: usize) -> Result<Value, RuntimeError> {
        let cell = self
            .global_cells
            .get(slot)
            .and_then(Option::as_ref)
            .ok_or_else(|| RuntimeError::new(format!("unbound global g{slot}")))?;
        Ok(cell.borrow().clone())
    }

    fn init_global(&mut self, slot: usize, value: Value) -> Result<(), RuntimeError> {
        if slot >= self.global_cells.len() {
            return Err(RuntimeError::invalid_instruction(format!(
                "global slot g{} is out of range",
                slot
            )));
        }
        let cell = self.heap.new_cell(value);
        self.global_cells[slot] = Some(cell);
        Ok(())
    }

    fn set_global(&mut self, slot: usize, value: Value) -> Result<(), RuntimeError> {
        let cell = self
            .global_cells
            .get_mut(slot)
            .and_then(Option::as_mut)
            .ok_or_else(|| RuntimeError::new(format!("unbound global g{slot}")))?;
        *cell.borrow_mut() = value;
        Ok(())
    }

    fn constant_value(&self, index: usize) -> Result<Value, RuntimeError> {
        if self.verified {
            debug_assert!(index < self.decoded_constants.len());
            return Ok(self.decoded_constants[index].clone());
        }
        let value = self
            .decoded_constants
            .get(index)
            .ok_or_else(|| RuntimeError::new("constant index out of range"))?;
        if let Some(error) = self.constant_errors.get(&index) {
            return Err(error.clone());
        }
        Ok(value.clone())
    }

    fn read_register(&self, frame: &Frame, index: usize) -> Result<Value, RuntimeError> {
        if self.verified {
            debug_assert!(index < frame.registers.len());
            Ok(frame.registers[index].clone())
        } else {
            frame
                .registers
                .get(index)
                .cloned()
                .ok_or_else(|| RuntimeError::new("register index out of range"))
        }
    }

    fn read_register_ref<'frame>(
        &self,
        frame: &'frame Frame,
        index: usize,
    ) -> Result<&'frame Value, RuntimeError> {
        if self.verified {
            debug_assert!(index < frame.registers.len());
            Ok(&frame.registers[index])
        } else {
            frame
                .registers
                .get(index)
                .ok_or_else(|| RuntimeError::new("register index out of range"))
        }
    }

    fn write_register(
        &self,
        frame: &mut Frame,
        index: usize,
        value: Value,
    ) -> Result<(), RuntimeError> {
        if self.verified {
            debug_assert!(index < frame.registers.len());
            frame.registers[index] = value;
        } else {
            let slot = frame
                .registers
                .get_mut(index)
                .ok_or_else(|| RuntimeError::new("register index out of range"))?;
            *slot = value;
        }
        Ok(())
    }

    fn take_register(&self, frame: &mut Frame, index: usize) -> Result<Value, RuntimeError> {
        if self.verified {
            debug_assert!(index < frame.registers.len());
            Ok(std::mem::replace(&mut frame.registers[index], Value::Nil))
        } else {
            let slot = frame
                .registers
                .get_mut(index)
                .ok_or_else(|| RuntimeError::new("register index out of range"))?;
            Ok(std::mem::replace(slot, Value::Nil))
        }
    }

    fn execute_machine_int_conversion(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        value: usize,
        from_width: MachineIntWidth,
        to_width: MachineIntWidth,
        op_name: &str,
        widening: bool,
        sign_extend: bool,
    ) -> Result<(), RuntimeError> {
        let valid = if widening {
            to_width.bits() > from_width.bits()
        } else {
            to_width.bits() < from_width.bits()
        };
        if !valid {
            let relation = if widening {
                "to_width > from_width"
            } else {
                "to_width < from_width"
            };
            return Err(RuntimeError::invalid_conversion(format!(
                "{} requires {}, found {} -> {}",
                op_name,
                relation,
                from_width.as_str(),
                to_width.as_str()
            )));
        }

        let input = self.expect_machine_int(frame, value, op_name)? & from_width.mask();
        let result = if sign_extend {
            signed_machine_int(input, from_width) as u64
        } else {
            input
        } & to_width.mask();
        self.write_register(frame, dest, Value::machine_int(result))
    }

    fn expect_machine_int(
        &self,
        frame: &Frame,
        value: usize,
        op_name: &str,
    ) -> Result<u64, RuntimeError> {
        match self.read_register_ref(frame, value)? {
            Value::MachineInt(value) => Ok(*value),
            other => Err(RuntimeError::invalid_operand(format!(
                "{} expects machine_int, got {}",
                op_name,
                other.type_name()
            ))),
        }
    }

    fn expect_machine_float(
        &self,
        frame: &Frame,
        value: usize,
        op_name: &str,
    ) -> Result<f64, RuntimeError> {
        match self.read_register_ref(frame, value)? {
            Value::MachineFloat(value) => Ok(*value),
            other => Err(RuntimeError::invalid_operand(format!(
                "{} expects machine_float, got {}",
                op_name,
                other.type_name()
            ))),
        }
    }

    fn expect_machine_float_format(
        &self,
        frame: &Frame,
        value: usize,
        format: MachineFloatFormat,
        op_name: &str,
    ) -> Result<f64, RuntimeError> {
        let value = self.expect_machine_float(frame, value, op_name)?;
        Ok(canonical_machine_float(value, format))
    }

    fn execute_machine_float_binary(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        left: usize,
        right: usize,
        format: MachineFloatFormat,
        op_name: &str,
        operation: MachineFloatBinaryKind,
    ) -> Result<(), RuntimeError> {
        let left = self.expect_machine_float_format(frame, left, format, op_name)?;
        let right = self.expect_machine_float_format(frame, right, format, op_name)?;
        let result = match format {
            MachineFloatFormat::F32 => {
                let left = left as f32;
                let right = right as f32;
                let result = match operation {
                    MachineFloatBinaryKind::Add => left + right,
                    MachineFloatBinaryKind::Subtract => left - right,
                    MachineFloatBinaryKind::Multiply => left * right,
                    MachineFloatBinaryKind::Divide => left / right,
                };
                canonical_machine_float(result as f64, format)
            }
            MachineFloatFormat::F64 => {
                let result = match operation {
                    MachineFloatBinaryKind::Add => left + right,
                    MachineFloatBinaryKind::Subtract => left - right,
                    MachineFloatBinaryKind::Multiply => left * right,
                    MachineFloatBinaryKind::Divide => left / right,
                };
                canonical_machine_float(result, format)
            }
        };
        self.write_register(frame, dest, Value::machine_float(result))
    }

    fn execute_machine_float_compare(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        left: usize,
        right: usize,
        format: MachineFloatFormat,
        predicate: MachineFloatPredicate,
    ) -> Result<(), RuntimeError> {
        let left = self.expect_machine_float_format(frame, left, format, "fcmp")?;
        let right = self.expect_machine_float_format(frame, right, format, "fcmp")?;
        self.write_register(
            frame,
            dest,
            Value::boolean(apply_machine_float_predicate(left, right, predicate)),
        )
    }

    fn execute_machine_int_to_float(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        value: usize,
        int_width: MachineIntWidth,
        float_format: MachineFloatFormat,
        signed: bool,
    ) -> Result<(), RuntimeError> {
        let value =
            self.expect_machine_int(frame, value, if signed { "sitofp" } else { "uitofp" })?;
        let value = if signed {
            signed_machine_int(value, int_width) as f64
        } else {
            (value & int_width.mask()) as f64
        };
        self.write_register(
            frame,
            dest,
            Value::machine_float(canonical_machine_float(value, float_format)),
        )
    }

    fn execute_machine_float_to_int(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        value: usize,
        float_format: MachineFloatFormat,
        int_width: MachineIntWidth,
        signed: bool,
    ) -> Result<(), RuntimeError> {
        let value = self.expect_machine_float_format(
            frame,
            value,
            float_format,
            if signed { "fptosi" } else { "fptoui" },
        )?;
        let truncated = value.trunc();
        if !truncated.is_finite() {
            return Err(RuntimeError::invalid_conversion(format!(
                "{} cannot convert non-finite machine_float",
                if signed { "fptosi" } else { "fptoui" }
            )));
        }
        let bits = int_width.bits();
        let valid = if signed {
            let minimum = -((1u128 << (bits - 1)) as f64);
            let maximum_exclusive = (1u128 << (bits - 1)) as f64;
            truncated >= minimum && truncated < maximum_exclusive
        } else {
            let maximum_exclusive = if bits == 64 {
                18446744073709551616.0
            } else {
                (1u128 << bits) as f64
            };
            truncated >= 0.0 && truncated < maximum_exclusive
        };
        if !valid {
            return Err(RuntimeError::invalid_conversion(format!(
                "{} machine_float is outside the {}-bit integer range",
                if signed { "fptosi" } else { "fptoui" },
                bits
            )));
        }
        let raw = if signed {
            (truncated as i128 as u128 as u64) & int_width.mask()
        } else {
            (truncated as u128 as u64) & int_width.mask()
        };
        self.write_register(frame, dest, Value::machine_int(raw))
    }

    fn execute_machine_float_width_conversion(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        value: usize,
        from_format: MachineFloatFormat,
        to_format: MachineFloatFormat,
        op_name: &str,
    ) -> Result<(), RuntimeError> {
        let valid = match op_name {
            "fpext" => {
                from_format == MachineFloatFormat::F32 && to_format == MachineFloatFormat::F64
            }
            "fptrunc" => {
                from_format == MachineFloatFormat::F64 && to_format == MachineFloatFormat::F32
            }
            _ => true,
        };
        if !valid {
            return Err(RuntimeError::invalid_conversion(format!(
                "{} requires {} -> {}, found {} -> {}",
                op_name,
                if op_name == "fpext" { "f32" } else { "f64" },
                if op_name == "fpext" { "f64" } else { "f32" },
                from_format.as_str(),
                to_format.as_str()
            )));
        }
        let value = self.expect_machine_float_format(frame, value, from_format, op_name)?;
        self.write_register(
            frame,
            dest,
            Value::machine_float(canonical_machine_float(value, to_format)),
        )
    }

    fn expect_address(
        &self,
        frame: &Frame,
        value: usize,
        op_name: &str,
    ) -> Result<u64, RuntimeError> {
        match self.read_register_ref(frame, value)? {
            Value::Address(value) => Ok(*value),
            other => Err(RuntimeError::invalid_operand(format!(
                "{} expects address, got {}",
                op_name,
                other.type_name()
            ))),
        }
    }

    fn execute_machine_frame_addr(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        offset: u64,
    ) -> Result<(), RuntimeError> {
        if offset > frame.machine_frame_size {
            return Err(RuntimeError::invalid_address(format!(
                "frame address offset {} exceeds machine frame size {}",
                offset, frame.machine_frame_size
            )));
        }
        let Some(base) = frame.machine_frame_base else {
            return Err(RuntimeError::invalid_address(
                "frame address requested without a machine frame",
            ));
        };
        let address = base
            .checked_add(offset)
            .ok_or_else(|| RuntimeError::invalid_address("frame address arithmetic overflow"))?;
        self.write_register(frame, dest, Value::address(address))
    }

    fn execute_machine_load(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        address: usize,
        memory_type: MachineMemoryType,
    ) -> Result<(), RuntimeError> {
        let address = self.expect_address(frame, address, "load")?;
        let bytes = self.memory.read_bytes(address, memory_type.size())?;
        let raw = decode_machine_memory_bits(bytes, memory_type)?;
        let value = match memory_type {
            MachineMemoryType::I8
            | MachineMemoryType::I16
            | MachineMemoryType::I32
            | MachineMemoryType::I64 => Value::machine_int(raw),
            MachineMemoryType::F32 => {
                let bits = u32::try_from(raw).map_err(|_| {
                    RuntimeError::invalid_instruction("load f32 bits exceed 32 bits")
                })?;
                Value::machine_float(canonical_machine_float(
                    f32::from_bits(bits) as f64,
                    MachineFloatFormat::F32,
                ))
            }
            MachineMemoryType::F64 => Value::machine_float(canonical_machine_float(
                f64::from_bits(raw),
                MachineFloatFormat::F64,
            )),
            MachineMemoryType::Addr => Value::address(raw),
        };
        self.write_register(frame, dest, value)
    }

    fn execute_machine_store(
        &mut self,
        frame: &mut Frame,
        address: usize,
        source: usize,
        memory_type: MachineMemoryType,
    ) -> Result<(), RuntimeError> {
        let address = self.expect_address(frame, address, "store")?;
        let bytes = match memory_type {
            MachineMemoryType::I8
            | MachineMemoryType::I16
            | MachineMemoryType::I32
            | MachineMemoryType::I64 => {
                let source = self.read_register(frame, source)?;
                let value = match source {
                    Value::MachineInt(value) => value,
                    other => {
                        return Err(RuntimeError::invalid_operand(format!(
                            "store {} expects machine_int, got {}",
                            memory_type.as_str(),
                            other.type_name()
                        )))
                    }
                };
                encode_machine_memory_bits(value, memory_type.size())
            }
            MachineMemoryType::F32 => {
                let value = canonical_machine_float(
                    self.expect_machine_float(frame, source, "store f32")?,
                    MachineFloatFormat::F32,
                );
                (value as f32).to_bits().to_le_bytes().to_vec()
            }
            MachineMemoryType::F64 => {
                let value = canonical_machine_float(
                    self.expect_machine_float(frame, source, "store f64")?,
                    MachineFloatFormat::F64,
                );
                value.to_bits().to_le_bytes().to_vec()
            }
            MachineMemoryType::Addr => {
                let value = self.expect_address(frame, source, "store addr")?;
                value.to_le_bytes().to_vec()
            }
        };
        self.memory.write_bytes(address, &bytes)?;
        Ok(())
    }

    fn expect_machine_byte_count(
        &self,
        frame: &Frame,
        value: usize,
        op_name: &str,
    ) -> Result<usize, RuntimeError> {
        let value = self.expect_machine_int(frame, value, op_name)?;
        usize::try_from(value).map_err(|_| {
            RuntimeError::invalid_address(format!(
                "{} byte count {} does not fit a host index",
                op_name, value
            ))
        })
    }

    fn execute_machine_memcpy(
        &mut self,
        frame: &Frame,
        destination: usize,
        source: usize,
        size: usize,
    ) -> Result<(), RuntimeError> {
        let destination = self.expect_address(frame, destination, "memcpy destination")?;
        let source = self.expect_address(frame, source, "memcpy source")?;
        let size = self.expect_machine_byte_count(frame, size, "memcpy")?;
        self.memory.memcpy(destination, source, size)?;
        Ok(())
    }

    fn execute_machine_memmove(
        &mut self,
        frame: &Frame,
        destination: usize,
        source: usize,
        size: usize,
    ) -> Result<(), RuntimeError> {
        let destination = self.expect_address(frame, destination, "memmove destination")?;
        let source = self.expect_address(frame, source, "memmove source")?;
        let size = self.expect_machine_byte_count(frame, size, "memmove")?;
        self.memory.memmove(destination, source, size)?;
        Ok(())
    }

    fn execute_machine_memset(
        &mut self,
        frame: &Frame,
        destination: usize,
        value: usize,
        size: usize,
    ) -> Result<(), RuntimeError> {
        let destination = self.expect_address(frame, destination, "memset destination")?;
        let value = self.expect_machine_int(frame, value, "memset")? as u8;
        let size = self.expect_machine_byte_count(frame, size, "memset")?;
        self.memory.memset(destination, value, size)?;
        Ok(())
    }

    fn expect_two_machine_ints(
        &self,
        frame: &Frame,
        left: usize,
        right: usize,
        op_name: &str,
    ) -> Result<(u64, u64), RuntimeError> {
        match (
            self.read_register_ref(frame, left)?,
            self.read_register_ref(frame, right)?,
        ) {
            (Value::MachineInt(left), Value::MachineInt(right)) => Ok((*left, *right)),
            _ => Err(RuntimeError::invalid_operand(format!(
                "{} expects machine_int operands",
                op_name
            ))),
        }
    }

    fn execute_machine_int_binary(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
        op_name: &str,
        operation: fn(u64, u64) -> u64,
    ) -> Result<(), RuntimeError> {
        let (left, right) = self.expect_two_machine_ints(frame, left, right, op_name)?;
        let result = operation(left & width.mask(), right & width.mask()) & width.mask();
        self.write_register(frame, dest, Value::machine_int(result))
    }

    fn execute_machine_int_division(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
        signed: bool,
        remainder: bool,
    ) -> Result<(), RuntimeError> {
        let (left, right) = self.expect_two_machine_ints(
            frame,
            left,
            right,
            if signed {
                if remainder {
                    "srem"
                } else {
                    "sdiv"
                }
            } else if remainder {
                "urem"
            } else {
                "udiv"
            },
        )?;
        let right = right & width.mask();
        if right == 0 {
            return Err(RuntimeError::integer_division_by_zero(
                "integer division by zero",
            ));
        }

        let result = if signed {
            let left = signed_machine_int(left, width);
            let right = signed_machine_int(right, width);
            let minimum = -(1i128 << (width.bits() - 1));
            if left == minimum && right == -1 {
                return Err(RuntimeError::integer_division_overflow(
                    "integer division overflow",
                ));
            }
            if remainder {
                left % right
            } else {
                left / right
            }
        } else {
            let left = left & width.mask();
            (if remainder {
                left % right
            } else {
                left / right
            }) as i128
        };
        self.write_register(
            frame,
            dest,
            Value::machine_int((result as u64) & width.mask()),
        )
    }

    fn execute_machine_int_shift(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        value: usize,
        amount: usize,
        width: MachineIntWidth,
        kind: MachineShiftKind,
    ) -> Result<(), RuntimeError> {
        let value = self.expect_machine_int(frame, value, machine_shift_name(kind))?;
        let amount = self.expect_machine_int(frame, amount, machine_shift_name(kind))?;
        if amount >= u64::from(width.bits()) {
            return Err(RuntimeError::invalid_shift_amount("invalid shift amount"));
        }
        let value = value & width.mask();
        let result = match kind {
            MachineShiftKind::Left => value.wrapping_shl(amount as u32),
            MachineShiftKind::LogicalRight => value >> amount,
            MachineShiftKind::ArithmeticRight => {
                (signed_machine_int(value, width) >> amount) as u64
            }
        } & width.mask();
        self.write_register(frame, dest, Value::machine_int(result))
    }

    fn execute_machine_int_compare(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
        predicate: MachineIntPredicate,
    ) -> Result<(), RuntimeError> {
        let (left, right) = self.expect_two_machine_ints(frame, left, right, "icmp")?;
        let left = left & width.mask();
        let right = right & width.mask();
        let result = match predicate {
            MachineIntPredicate::Eq => left == right,
            MachineIntPredicate::Ne => left != right,
            MachineIntPredicate::Slt => {
                signed_machine_int(left, width) < signed_machine_int(right, width)
            }
            MachineIntPredicate::Sle => {
                signed_machine_int(left, width) <= signed_machine_int(right, width)
            }
            MachineIntPredicate::Sgt => {
                signed_machine_int(left, width) > signed_machine_int(right, width)
            }
            MachineIntPredicate::Sge => {
                signed_machine_int(left, width) >= signed_machine_int(right, width)
            }
            MachineIntPredicate::Ult => left < right,
            MachineIntPredicate::Ule => left <= right,
            MachineIntPredicate::Ugt => left > right,
            MachineIntPredicate::Uge => left >= right,
        };
        self.write_register(frame, dest, Value::boolean(result))
    }

    fn expect_number(
        &self,
        frame: &Frame,
        value: usize,
        op_name: &str,
    ) -> Result<f64, RuntimeError> {
        match self.read_register_ref(frame, value)? {
            Value::Number(value) => Ok(*value),
            other => Err(RuntimeError::new(format!(
                "{} expects number, got {}",
                op_name,
                other.type_name()
            ))),
        }
    }

    fn expect_two_numbers(
        &self,
        frame: &Frame,
        left: usize,
        right: usize,
        op_name: &str,
    ) -> Result<(f64, f64), RuntimeError> {
        match (
            self.read_register_ref(frame, left)?,
            self.read_register_ref(frame, right)?,
        ) {
            (Value::Number(left), Value::Number(right)) => Ok((*left, *right)),
            _ => Err(RuntimeError::new(format!("{} expects numbers", op_name))),
        }
    }

    fn compare_numbers(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        left: usize,
        right: usize,
        op_name: &str,
        predicate: fn(f64, f64) -> bool,
    ) -> Result<(), RuntimeError> {
        let (left, right) = self.expect_two_numbers(frame, left, right, op_name)?;
        self.write_register(frame, dest, Value::boolean(predicate(left, right)))
    }

    fn compare_strings(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        left: usize,
        right: usize,
        op_name: &str,
        predicate: fn(std::cmp::Ordering) -> bool,
    ) -> Result<(), RuntimeError> {
        let result = match (
            self.read_register_ref(frame, left)?,
            self.read_register_ref(frame, right)?,
        ) {
            (Value::String(left), Value::String(right)) => {
                predicate(left.chars().cmp(right.chars()))
            }
            _ => {
                return Err(RuntimeError::new(format!(
                    "{} expects two strings",
                    op_name
                )))
            }
        };
        self.write_register(frame, dest, Value::boolean(result))
    }

    fn compare(
        &mut self,
        frame: &mut Frame,
        dest: usize,
        left: usize,
        right: usize,
        comparison: Comparison,
        call_site: Option<&DebugLocation>,
    ) -> Result<(), RuntimeError> {
        let left_value = self.read_register(frame, left)?;
        let right_value = self.read_register(frame, right)?;
        let result =
            self.compare_values(frame, &left_value, &right_value, comparison, call_site)?;
        self.write_register(frame, dest, Value::boolean(result))
    }

    fn compare_values(
        &mut self,
        _frame: &Frame,
        left_value: &Value,
        right_value: &Value,
        comparison: Comparison,
        _call_site: Option<&DebugLocation>,
    ) -> Result<bool, RuntimeError> {
        Ok(match (left_value, right_value) {
            (Value::Number(left), Value::Number(right)) => comparison.apply_numbers(*left, *right),
            (Value::String(left), Value::String(right)) => {
                let ordering = left.chars().cmp(right.chars());
                match comparison {
                    Comparison::Greater => ordering.is_gt(),
                    Comparison::GreaterEqual => ordering.is_ge(),
                    Comparison::Less => ordering.is_lt(),
                    Comparison::LessEqual => ordering.is_le(),
                }
            }
            _ => {
                return Err(RuntimeError::new(format!(
                    "{} expects two numbers or two strings",
                    comparison.as_str()
                )))
            }
        })
    }
}
