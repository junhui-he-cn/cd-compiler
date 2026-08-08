#![allow(dead_code)]

use crate::bytecode::{FunctionBody, Instruction, Program};
use crate::runtime::SharedEnvironment;
use crate::scheduler::{ResumableFrame, ReturnTarget, TaskId};
use crate::value::Value as VmValue;
use cranelift_codegen::ir::{
    types, AbiParam, ExtFuncData, ExternalName, Function, InstBuilder, Signature, UserExternalName,
    UserFuncName, Value,
};
use cranelift_codegen::isa::{CallConv, TargetFrontendConfig};
use cranelift_codegen::{settings, verifier::verify_function};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext};
use std::collections::{BTreeMap, BTreeSet};
use std::rc::Rc;
use target_lexicon::PointerWidth;

/// The first V6C cache budget is an internal admission bound. It is not a
/// runtime-element or artifact budget, and it is not exposed through the
/// public VM configuration until a concrete code representation exists.
pub(crate) const DEFAULT_CODE_CACHE_BYTES: usize = 64 * 1024;

/// Stable boundary categories used when compiled code hands control back to
/// the VM. The compiled path never owns scheduling, cancellation, or GC; it
/// only describes the boundary at which the existing VM frame is restored.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum JitSafepointKind {
    Instruction,
    Native,
    Scheduler,
    GarbageCollection,
    Error,
    Return,
}

impl JitSafepointKind {
    pub(crate) const fn uses_instruction_budget(self) -> bool {
        matches!(self, Self::Instruction | Self::Native)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct JitSafepoint {
    pub(crate) kind: JitSafepointKind,
    pub(crate) instruction: usize,
}

impl JitSafepoint {
    pub(crate) const fn new(kind: JitSafepointKind, instruction: usize) -> Self {
        Self { kind, instruction }
    }
}

/// Owned, borrow-free state transferred across a future compiled/interpreter
/// or compiled/runtime boundary. It clones the existing frame's `Rc` handles
/// and register values instead of retaining raw pointers or `RefCell` borrows.
#[derive(Clone, Debug)]
pub(crate) struct JitFrameMaterialization {
    body: Option<Rc<FunctionBody>>,
    ip: usize,
    registers: Vec<VmValue>,
    locals: SharedEnvironment,
    closure: SharedEnvironment,
    is_main: bool,
    function: Rc<str>,
    function_index: Option<usize>,
    return_target: Option<ReturnTarget>,
    task_id: Option<TaskId>,
    safepoint: JitSafepoint,
}

impl JitFrameMaterialization {
    pub(crate) fn capture(
        frame: &ResumableFrame,
        task_id: Option<TaskId>,
        safepoint: JitSafepoint,
    ) -> Self {
        Self {
            body: frame.body.clone(),
            ip: frame.ip,
            registers: frame.registers.clone(),
            locals: frame.locals.clone(),
            closure: frame.closure.clone(),
            is_main: frame.is_main,
            function: frame.function.clone(),
            function_index: frame.function_index,
            return_target: frame.return_target.clone(),
            task_id,
            safepoint,
        }
    }

    pub(crate) fn restore_into(&self, frame: &mut ResumableFrame) {
        frame.body = self.body.clone();
        frame.ip = self.ip;
        frame.registers = self.registers.clone();
        frame.locals = self.locals.clone();
        frame.closure = self.closure.clone();
        frame.is_main = self.is_main;
        frame.function = self.function.clone();
        frame.function_index = self.function_index;
        frame.return_target = self.return_target.clone();
    }

    pub(crate) fn body(&self) -> Option<&FunctionBody> {
        self.body.as_deref()
    }

    pub(crate) fn instruction(&self) -> usize {
        self.ip
    }

    pub(crate) fn registers(&self) -> &[VmValue] {
        &self.registers
    }

    pub(crate) fn registers_mut(&mut self) -> &mut [VmValue] {
        &mut self.registers
    }

    pub(crate) fn locals(&self) -> &SharedEnvironment {
        &self.locals
    }

    pub(crate) fn closure(&self) -> &SharedEnvironment {
        &self.closure
    }

    pub(crate) fn is_main(&self) -> bool {
        self.is_main
    }

    pub(crate) fn function(&self) -> &str {
        &self.function
    }

    pub(crate) fn function_index(&self) -> Option<usize> {
        self.function_index
    }

    pub(crate) fn return_target(&self) -> Option<&ReturnTarget> {
        self.return_target.as_ref()
    }

    pub(crate) fn task_id(&self) -> Option<TaskId> {
        self.task_id
    }

    pub(crate) fn safepoint(&self) -> JitSafepoint {
        self.safepoint
    }
}

/// VM-local identity for a cached compiled entry. The owner token is an
/// allocation identity, not a code pointer; it prevents a handle from being
/// reused with another VM instance after the original cache is dropped.
#[derive(Debug)]
struct JitCacheOwner {
    marker: u8,
}

#[derive(Clone, Debug)]
pub(crate) struct JitCodeEntryHandle {
    owner: Rc<JitCacheOwner>,
    function_index: usize,
    generation: u64,
}

impl JitCodeEntryHandle {
    fn function_index(&self) -> usize {
        self.function_index
    }

    fn generation(&self) -> u64 {
        self.generation
    }
}

impl PartialEq for JitCodeEntryHandle {
    fn eq(&self, other: &Self) -> bool {
        self.function_index == other.function_index
            && self.generation == other.generation
            && Rc::ptr_eq(&self.owner, &other.owner)
    }
}

impl Eq for JitCodeEntryHandle {}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum JitExecutionMode {
    Ordinary,
    Trace,
    Profile,
    Debug,
    Cooperative,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum JitFallbackReason {
    Disabled,
    MainBody,
    MissingFunction(usize),
    FunctionIndexMismatch {
        requested: usize,
        declared: usize,
    },
    NotWhitelisted(usize),
    ObservableMode(JitExecutionMode),
    CooperativeMode,
    DynamicCall {
        instruction: usize,
    },
    NativeBoundary {
        instruction: usize,
        name: String,
    },
    CallbackBoundary {
        instruction: usize,
        name: String,
    },
    UnsupportedInstruction {
        instruction: usize,
        opcode: &'static str,
    },
    CodeCacheExhausted {
        requested: usize,
        remaining: usize,
    },
    CodeHandleExhausted,
    ForeignCodeEntry {
        function_index: usize,
    },
    StaleCodeEntry {
        function_index: usize,
        generation: u64,
    },
    CraneliftIr {
        message: String,
    },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum JitEligibility {
    Eligible { function_index: usize },
    Fallback(JitFallbackReason),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum JitAdmission {
    Reserved {
        function_index: usize,
        bytes: usize,
        handle: JitCodeEntryHandle,
    },
    Cached {
        function_index: usize,
        bytes: usize,
        handle: JitCodeEntryHandle,
    },
    Fallback(JitFallbackReason),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct JitCacheStats {
    pub(crate) capacity: usize,
    pub(crate) used: usize,
    pub(crate) entries: usize,
}

#[derive(Debug)]
pub(crate) struct CraneliftIrUnit {
    pub(crate) function_index: usize,
    pub(crate) function: Function,
}

impl CraneliftIrUnit {
    #[cfg(test)]
    fn display(&self) -> String {
        self.function.to_string()
    }
}

#[derive(Clone, Debug)]
struct JitConfig {
    enabled: bool,
    max_code_cache_bytes: usize,
    whitelist: BTreeSet<usize>,
}

impl JitConfig {
    fn disabled() -> Self {
        Self {
            enabled: false,
            max_code_cache_bytes: DEFAULT_CODE_CACHE_BYTES,
            whitelist: BTreeSet::new(),
        }
    }

    #[cfg(test)]
    fn enabled(whitelist: impl IntoIterator<Item = usize>, max_code_cache_bytes: usize) -> Self {
        Self {
            enabled: true,
            max_code_cache_bytes,
            whitelist: whitelist.into_iter().collect(),
        }
    }
}

#[derive(Debug)]
struct JitCodeCache {
    capacity: usize,
    used: usize,
    owner: Rc<JitCacheOwner>,
    next_generation: u64,
    entries: BTreeMap<usize, CachedJitUnit>,
}

#[derive(Debug)]
struct CachedJitUnit {
    bytes: usize,
    handle: JitCodeEntryHandle,
    ir: CraneliftIrUnit,
}

enum CacheReservation {
    Inserted(JitCodeEntryHandle),
    Existing {
        handle: JitCodeEntryHandle,
        bytes: usize,
    },
    Exhausted {
        remaining: usize,
    },
    HandleExhausted,
}

impl JitCodeCache {
    fn new(capacity: usize) -> Self {
        Self {
            capacity,
            used: 0,
            owner: Rc::new(JitCacheOwner { marker: 0 }),
            next_generation: 0,
            entries: BTreeMap::new(),
        }
    }

    fn reserve(&mut self, function_index: usize, bytes: usize) -> CacheReservation {
        if let Some(existing) = self.entries.get(&function_index) {
            return CacheReservation::Existing {
                handle: existing.handle.clone(),
                bytes: existing.bytes,
            };
        }

        let remaining = self.capacity.saturating_sub(self.used);
        if bytes > remaining {
            return CacheReservation::Exhausted { remaining };
        }

        let Some(next_generation) = self.next_generation.checked_add(1) else {
            return CacheReservation::HandleExhausted;
        };
        let handle = JitCodeEntryHandle {
            owner: self.owner.clone(),
            function_index,
            generation: self.next_generation,
        };
        self.next_generation = next_generation;
        CacheReservation::Inserted(handle)
    }

    fn insert(&mut self, handle: JitCodeEntryHandle, bytes: usize, ir: CraneliftIrUnit) {
        debug_assert!(Rc::ptr_eq(&self.owner, &handle.owner));
        debug_assert!(!self.entries.contains_key(&handle.function_index));
        self.entries
            .insert(handle.function_index, CachedJitUnit { bytes, handle, ir });
        self.used = self.used.saturating_add(bytes);
    }

    #[cfg(test)]
    fn cached_ir(&self, function_index: usize) -> Option<&CraneliftIrUnit> {
        self.entries.get(&function_index).map(|entry| &entry.ir)
    }

    fn clear(&mut self) {
        self.entries.clear();
        self.used = 0;
    }

    fn stats(&self) -> JitCacheStats {
        JitCacheStats {
            capacity: self.capacity,
            used: self.used,
            entries: self.entries.len(),
        }
    }

    fn resolve(&self, handle: &JitCodeEntryHandle) -> Result<&CraneliftIrUnit, JitFallbackReason> {
        if !Rc::ptr_eq(&self.owner, &handle.owner) {
            return Err(JitFallbackReason::ForeignCodeEntry {
                function_index: handle.function_index,
            });
        }

        let Some(entry) = self.entries.get(&handle.function_index) else {
            return Err(JitFallbackReason::StaleCodeEntry {
                function_index: handle.function_index,
                generation: handle.generation,
            });
        };
        if entry.handle.generation != handle.generation {
            return Err(JitFallbackReason::StaleCodeEntry {
                function_index: handle.function_index,
                generation: handle.generation,
            });
        }
        Ok(&entry.ir)
    }
}

/// Per-VM JIT admission state. This slice owns policy and cache accounting;
/// it deliberately does not contain executable code or a machine-code
/// backend.
#[derive(Debug)]
pub(crate) struct JitState {
    config: JitConfig,
    cache: JitCodeCache,
}

impl JitState {
    pub(crate) fn disabled() -> Self {
        let config = JitConfig::disabled();
        let cache = JitCodeCache::new(config.max_code_cache_bytes);
        Self { config, cache }
    }

    pub(crate) fn materialize_frame(
        &self,
        frame: &ResumableFrame,
        task_id: Option<TaskId>,
        safepoint: JitSafepoint,
    ) -> JitFrameMaterialization {
        JitFrameMaterialization::capture(frame, task_id, safepoint)
    }

    #[cfg(test)]
    pub(crate) fn enabled_for_tests(
        whitelist: impl IntoIterator<Item = usize>,
        max_code_cache_bytes: usize,
    ) -> Self {
        let config = JitConfig::enabled(whitelist, max_code_cache_bytes);
        let cache = JitCodeCache::new(config.max_code_cache_bytes);
        Self { config, cache }
    }

    pub(crate) fn eligibility(
        &self,
        program: &Program,
        function_index: Option<usize>,
        mode: JitExecutionMode,
    ) -> JitEligibility {
        if !self.config.enabled {
            return JitEligibility::Fallback(JitFallbackReason::Disabled);
        }

        match mode {
            JitExecutionMode::Ordinary => {}
            JitExecutionMode::Cooperative => {
                return JitEligibility::Fallback(JitFallbackReason::CooperativeMode);
            }
            observable => {
                return JitEligibility::Fallback(JitFallbackReason::ObservableMode(observable));
            }
        }

        let Some(function_index) = function_index else {
            return JitEligibility::Fallback(JitFallbackReason::MainBody);
        };
        let Some(function) = program.functions.get(function_index) else {
            return JitEligibility::Fallback(JitFallbackReason::MissingFunction(function_index));
        };
        if function.index != function_index {
            return JitEligibility::Fallback(JitFallbackReason::FunctionIndexMismatch {
                requested: function_index,
                declared: function.index,
            });
        }
        if !self.config.whitelist.contains(&function_index) {
            return JitEligibility::Fallback(JitFallbackReason::NotWhitelisted(function_index));
        }

        for (instruction, operation) in function.instructions.iter().enumerate() {
            match operation {
                Instruction::Constant { .. }
                | Instruction::Move { .. }
                | Instruction::LoadVar { .. }
                | Instruction::Negate { .. }
                | Instruction::Not { .. }
                | Instruction::Add { .. }
                | Instruction::Subtract { .. }
                | Instruction::Multiply { .. }
                | Instruction::Divide { .. }
                | Instruction::Equal { .. }
                | Instruction::NotEqual { .. }
                | Instruction::Greater { .. }
                | Instruction::GreaterEqual { .. }
                | Instruction::Less { .. }
                | Instruction::LessEqual { .. }
                | Instruction::Return { .. } => {}
                Instruction::Call { .. } => {
                    return JitEligibility::Fallback(JitFallbackReason::DynamicCall {
                        instruction,
                    });
                }
                Instruction::NativeCall { name, .. } => {
                    let name = program
                        .names
                        .get(*name)
                        .cloned()
                        .unwrap_or_else(|| format!("name#{}", name));
                    if is_callback_native(&name) {
                        return JitEligibility::Fallback(JitFallbackReason::CallbackBoundary {
                            instruction,
                            name,
                        });
                    }
                    return JitEligibility::Fallback(JitFallbackReason::NativeBoundary {
                        instruction,
                        name,
                    });
                }
                unsupported => {
                    return JitEligibility::Fallback(JitFallbackReason::UnsupportedInstruction {
                        instruction,
                        opcode: opcode_name(unsupported),
                    });
                }
            }
        }

        JitEligibility::Eligible { function_index }
    }

    pub(crate) fn admit(
        &mut self,
        program: &Program,
        function_index: Option<usize>,
        mode: JitExecutionMode,
        estimated_code_bytes: usize,
    ) -> JitAdmission {
        let eligibility = self.eligibility(program, function_index, mode);
        let JitEligibility::Eligible { function_index } = eligibility else {
            let JitEligibility::Fallback(reason) = eligibility else {
                unreachable!("eligibility is either eligible or fallback")
            };
            return JitAdmission::Fallback(reason);
        };

        if let Some((handle, bytes)) = self.cache.cached_entry(function_index) {
            return JitAdmission::Cached {
                function_index,
                bytes,
                handle,
            };
        }

        let Some(function) = program.functions.get(function_index) else {
            return JitAdmission::Fallback(JitFallbackReason::MissingFunction(function_index));
        };
        let ir = match lower_to_cranelift_ir(function_index, function) {
            Ok(ir) => ir,
            Err(message) => {
                return JitAdmission::Fallback(JitFallbackReason::CraneliftIr { message });
            }
        };

        match self.cache.reserve(function_index, estimated_code_bytes) {
            CacheReservation::Inserted(handle) => {
                let admission_handle = handle.clone();
                self.cache.insert(handle, estimated_code_bytes, ir);
                JitAdmission::Reserved {
                    function_index,
                    bytes: estimated_code_bytes,
                    handle: admission_handle,
                }
            }
            CacheReservation::Existing { handle, bytes } => JitAdmission::Cached {
                function_index,
                bytes,
                handle,
            },
            CacheReservation::Exhausted { remaining } => {
                JitAdmission::Fallback(JitFallbackReason::CodeCacheExhausted {
                    requested: estimated_code_bytes,
                    remaining,
                })
            }
            CacheReservation::HandleExhausted => {
                JitAdmission::Fallback(JitFallbackReason::CodeHandleExhausted)
            }
        }
    }

    pub(crate) fn clear_cache(&mut self) {
        self.cache.clear();
    }

    /// Resolve a cache handle immediately before a future compiled entry is
    /// used. Eviction and VM ownership mismatches become interpreter fallback
    /// reasons instead of allowing a stale entry to cross the boundary.
    pub(crate) fn resolve_code(
        &self,
        handle: &JitCodeEntryHandle,
    ) -> Result<&CraneliftIrUnit, JitFallbackReason> {
        self.cache.resolve(handle)
    }

    #[cfg(test)]
    fn cache_stats(&self) -> JitCacheStats {
        self.cache.stats()
    }

    #[cfg(test)]
    fn cached_ir(&self, function_index: usize) -> Option<&CraneliftIrUnit> {
        self.cache.cached_ir(function_index)
    }
}

impl JitCodeCache {
    fn cached_entry(&self, function_index: usize) -> Option<(JitCodeEntryHandle, usize)> {
        self.entries
            .get(&function_index)
            .map(|entry| (entry.handle.clone(), entry.bytes))
    }
}

pub(crate) const JIT_HELPER_ABI_VERSION: u32 = 1;
pub(crate) const JIT_VALUE_HANDLE_BITS: u32 = 64;

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum RuntimeHelper {
    Constant = 0,
    LoadVar = 1,
    Negate = 2,
    Not = 3,
    Add = 4,
    Subtract = 5,
    Multiply = 6,
    Divide = 7,
    Equal = 8,
    NotEqual = 9,
    Greater = 10,
    GreaterEqual = 11,
    Less = 12,
    LessEqual = 13,
}

impl RuntimeHelper {
    const ALL: [Self; 14] = [
        Self::Constant,
        Self::LoadVar,
        Self::Negate,
        Self::Not,
        Self::Add,
        Self::Subtract,
        Self::Multiply,
        Self::Divide,
        Self::Equal,
        Self::NotEqual,
        Self::Greater,
        Self::GreaterEqual,
        Self::Less,
        Self::LessEqual,
    ];

    const fn value_arguments(self) -> usize {
        match self {
            Self::Constant | Self::LoadVar | Self::Negate | Self::Not => 1,
            Self::Add
            | Self::Subtract
            | Self::Multiply
            | Self::Divide
            | Self::Equal
            | Self::NotEqual
            | Self::Greater
            | Self::GreaterEqual
            | Self::Less
            | Self::LessEqual => 2,
        }
    }
}

/// The internal helper ABI is deliberately handle-based: one VM context
/// handle, zero or more opaque `Value` handles, and one opaque result handle.
/// The descriptor is shared by Cranelift import generation and the future VM
/// helper bridge so those paths cannot silently drift apart.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct JitHelperAbi {
    pub(crate) version: u32,
    pub(crate) helper_id: u32,
    pub(crate) context_arguments: usize,
    pub(crate) value_arguments: usize,
    pub(crate) value_results: usize,
}

impl JitHelperAbi {
    pub(crate) const fn for_helper(helper: RuntimeHelper) -> Self {
        Self {
            version: JIT_HELPER_ABI_VERSION,
            helper_id: helper as u32,
            context_arguments: 1,
            value_arguments: helper.value_arguments(),
            value_results: 1,
        }
    }
}

fn lower_to_cranelift_ir(
    function_index: usize,
    function: &crate::bytecode::Function,
) -> Result<CraneliftIrUnit, String> {
    let mut signature = Signature::new(CallConv::SystemV);
    signature.params.push(AbiParam::new(types::I64));
    signature.params.extend(std::iter::repeat_n(
        AbiParam::new(types::I64),
        function.arity,
    ));
    signature.returns.push(AbiParam::new(types::I64));

    let mut ir_function =
        Function::with_name_signature(UserFuncName::user(0, function_index as u32), signature);
    let mut builder_context = FunctionBuilderContext::new();
    {
        let mut builder = FunctionBuilder::new(&mut ir_function, &mut builder_context);
        let entry = builder.create_block();
        builder.append_block_params_for_function_params(entry);
        builder.switch_to_block(entry);
        builder.seal_block(entry);

        let context = builder
            .block_params(entry)
            .first()
            .copied()
            .ok_or_else(|| "Cranelift entry block is missing the VM context".to_string())?;
        let mut registers = vec![None; function.registers];
        let mut returned = false;

        for (instruction_index, operation) in function.instructions.iter().enumerate() {
            if returned {
                return Err(format!(
                    "instruction {} follows a return in function {}",
                    instruction_index, function_index
                ));
            }

            match operation {
                Instruction::Constant { dest, constant } => {
                    let constant = i64::try_from(*constant).map_err(|_| {
                        format!("constant index {} does not fit Cranelift i64", constant)
                    })?;
                    let constant = builder.ins().iconst(types::I64, constant);
                    let value = emit_runtime_call(
                        &mut builder,
                        context,
                        RuntimeHelper::Constant,
                        &[constant],
                    );
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::Move { dest, source } => {
                    let value = read_register(&registers, *source, instruction_index)?;
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::LoadVar { dest, name } => {
                    let name = i64::try_from(*name)
                        .map_err(|_| format!("name index {} does not fit Cranelift i64", name))?;
                    let name = builder.ins().iconst(types::I64, name);
                    let value =
                        emit_runtime_call(&mut builder, context, RuntimeHelper::LoadVar, &[name]);
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::Negate { dest, value } => {
                    let value = read_register(&registers, *value, instruction_index)?;
                    let value =
                        emit_runtime_call(&mut builder, context, RuntimeHelper::Negate, &[value]);
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::Not { dest, value } => {
                    let value = read_register(&registers, *value, instruction_index)?;
                    let value =
                        emit_runtime_call(&mut builder, context, RuntimeHelper::Not, &[value]);
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::Add { dest, left, right } => {
                    let value = emit_binary_runtime_call(
                        &mut builder,
                        context,
                        RuntimeHelper::Add,
                        &registers,
                        *left,
                        *right,
                        instruction_index,
                    )?;
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::Subtract { dest, left, right } => {
                    let value = emit_binary_runtime_call(
                        &mut builder,
                        context,
                        RuntimeHelper::Subtract,
                        &registers,
                        *left,
                        *right,
                        instruction_index,
                    )?;
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::Multiply { dest, left, right } => {
                    let value = emit_binary_runtime_call(
                        &mut builder,
                        context,
                        RuntimeHelper::Multiply,
                        &registers,
                        *left,
                        *right,
                        instruction_index,
                    )?;
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::Divide { dest, left, right } => {
                    let value = emit_binary_runtime_call(
                        &mut builder,
                        context,
                        RuntimeHelper::Divide,
                        &registers,
                        *left,
                        *right,
                        instruction_index,
                    )?;
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::Equal { dest, left, right } => {
                    let value = emit_binary_runtime_call(
                        &mut builder,
                        context,
                        RuntimeHelper::Equal,
                        &registers,
                        *left,
                        *right,
                        instruction_index,
                    )?;
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::NotEqual { dest, left, right } => {
                    let value = emit_binary_runtime_call(
                        &mut builder,
                        context,
                        RuntimeHelper::NotEqual,
                        &registers,
                        *left,
                        *right,
                        instruction_index,
                    )?;
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::Greater { dest, left, right } => {
                    let value = emit_binary_runtime_call(
                        &mut builder,
                        context,
                        RuntimeHelper::Greater,
                        &registers,
                        *left,
                        *right,
                        instruction_index,
                    )?;
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::GreaterEqual { dest, left, right } => {
                    let value = emit_binary_runtime_call(
                        &mut builder,
                        context,
                        RuntimeHelper::GreaterEqual,
                        &registers,
                        *left,
                        *right,
                        instruction_index,
                    )?;
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::Less { dest, left, right } => {
                    let value = emit_binary_runtime_call(
                        &mut builder,
                        context,
                        RuntimeHelper::Less,
                        &registers,
                        *left,
                        *right,
                        instruction_index,
                    )?;
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::LessEqual { dest, left, right } => {
                    let value = emit_binary_runtime_call(
                        &mut builder,
                        context,
                        RuntimeHelper::LessEqual,
                        &registers,
                        *left,
                        *right,
                        instruction_index,
                    )?;
                    write_register(&mut registers, *dest, value, instruction_index)?;
                }
                Instruction::Return { value } => {
                    let value = read_register(&registers, *value, instruction_index)?;
                    builder.ins().return_(&[value]);
                    returned = true;
                }
                _ => {
                    return Err(format!(
                        "unsupported instruction {} reached Cranelift lowering",
                        opcode_name(operation)
                    ));
                }
            }
        }

        if !returned {
            return Err(format!(
                "function {} has no return instruction",
                function_index
            ));
        }

        builder.finalize(TargetFrontendConfig {
            default_call_conv: CallConv::SystemV,
            pointer_width: PointerWidth::U64,
            page_size_align_log2: 12,
        });
    }

    let flags = settings::Flags::new(settings::builder());
    verify_function(&ir_function, &flags).map_err(|errors| errors.to_string())?;
    Ok(CraneliftIrUnit {
        function_index,
        function: ir_function,
    })
}

fn read_register(
    registers: &[Option<Value>],
    register: usize,
    instruction: usize,
) -> Result<Value, String> {
    registers.get(register).copied().flatten().ok_or_else(|| {
        format!(
            "register {} is undefined at instruction {}",
            register, instruction
        )
    })
}

fn write_register(
    registers: &mut [Option<Value>],
    register: usize,
    value: Value,
    instruction: usize,
) -> Result<(), String> {
    let Some(slot) = registers.get_mut(register) else {
        return Err(format!(
            "register {} is out of range at instruction {}",
            register, instruction
        ));
    };
    *slot = Some(value);
    Ok(())
}

fn emit_binary_runtime_call(
    builder: &mut FunctionBuilder<'_>,
    context: Value,
    helper: RuntimeHelper,
    registers: &[Option<Value>],
    left: usize,
    right: usize,
    instruction: usize,
) -> Result<Value, String> {
    let left = read_register(registers, left, instruction)?;
    let right = read_register(registers, right, instruction)?;
    Ok(emit_runtime_call(builder, context, helper, &[left, right]))
}

fn emit_runtime_call(
    builder: &mut FunctionBuilder<'_>,
    context: Value,
    helper: RuntimeHelper,
    arguments: &[Value],
) -> Value {
    let abi = JitHelperAbi::for_helper(helper);
    debug_assert_eq!(abi.context_arguments, 1);
    debug_assert_eq!(abi.value_arguments, arguments.len());
    debug_assert_eq!(abi.value_results, 1);
    let mut signature = Signature::new(CallConv::SystemV);
    signature.params.extend(std::iter::repeat_n(
        AbiParam::new(types::I64),
        abi.context_arguments,
    ));
    signature.params.extend(std::iter::repeat_n(
        AbiParam::new(types::I64),
        abi.value_arguments,
    ));
    signature.returns.extend(std::iter::repeat_n(
        AbiParam::new(types::I64),
        abi.value_results,
    ));
    let signature = builder.import_signature(signature);
    let user_name = builder
        .func
        .declare_imported_user_function(UserExternalName::new(1, abi.helper_id));
    let function = builder.import_function(ExtFuncData {
        name: ExternalName::user(user_name),
        signature,
        colocated: false,
        patchable: false,
    });
    let mut call_arguments = Vec::with_capacity(arguments.len() + 1);
    call_arguments.push(context);
    call_arguments.extend_from_slice(arguments);
    let call = builder.ins().call(function, &call_arguments);
    builder.inst_results(call)[0]
}

fn is_callback_native(name: &str) -> bool {
    matches!(
        name,
        "map" | "filter" | "flatMap" | "any" | "all" | "count" | "find" | "findIndex" | "reduce"
    )
}

fn opcode_name(instruction: &Instruction) -> &'static str {
    match instruction {
        Instruction::Constant { .. } => "constant",
        Instruction::MakeFunction { .. } => "make_function",
        Instruction::Array { .. } => "array",
        Instruction::Map { .. } => "map",
        Instruction::Struct { .. } => "struct",
        Instruction::Variant { .. } => "variant",
        Instruction::VariantTag { .. } => "variant_tag",
        Instruction::VariantField { .. } => "variant_field",
        Instruction::Move { .. } => "move",
        Instruction::LoadVar { .. } => "load_var",
        Instruction::StoreVar { .. } => "store_var",
        Instruction::AssignVar { .. } => "assign_var",
        Instruction::Call { .. } => "call",
        Instruction::NativeCall { .. } => "native_call",
        Instruction::Index { .. } => "index",
        Instruction::AssignIndex { .. } => "assign_index",
        Instruction::Field { .. } => "field",
        Instruction::AssignField { .. } => "assign_field",
        Instruction::Len { .. } => "len",
        Instruction::AssertArray { .. } => "assert_array",
        Instruction::AssertNumber { .. } => "assert_number",
        Instruction::Print { .. } => "print",
        Instruction::Return { .. } => "return",
        Instruction::Negate { .. } => "negate",
        Instruction::Not { .. } => "not",
        Instruction::Add { .. } => "add",
        Instruction::Subtract { .. } => "subtract",
        Instruction::Multiply { .. } => "multiply",
        Instruction::Divide { .. } => "divide",
        Instruction::Equal { .. } => "equal",
        Instruction::NotEqual { .. } => "not_equal",
        Instruction::Greater { .. } => "greater",
        Instruction::GreaterEqual { .. } => "greater_equal",
        Instruction::Less { .. } => "less",
        Instruction::LessEqual { .. } => "less_equal",
        Instruction::Jump { .. } => "jump",
        Instruction::JumpIfFalse { .. } => "jump_if_false",
        Instruction::JumpIfTrue { .. } => "jump_if_true",
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bytecode::{Function, FunctionBody};
    use crate::runtime::Heap;
    use crate::scheduler::{CooperativeScheduler, ResumableFrame};
    use crate::value::Value as VmValue;
    use std::collections::BTreeSet;

    fn function(index: usize, instructions: Vec<Instruction>) -> Function {
        Function {
            index,
            name: format!("function{index}"),
            arity: 0,
            registers: 4,
            params: Vec::new(),
            locations: vec![None; instructions.len()],
            instructions,
        }
    }

    fn program(functions: Vec<Function>) -> Program {
        Program {
            constants: Vec::new(),
            names: vec!["map".to_string(), "print".to_string()],
            main: FunctionBody {
                registers: 1,
                instructions: Vec::new(),
                locations: Vec::new(),
            },
            functions,
            debug_sources: Vec::new(),
        }
    }

    #[test]
    fn helper_abi_is_stable_and_handle_based() {
        let mut helper_ids = BTreeSet::new();
        assert_eq!(JIT_VALUE_HANDLE_BITS, 64);

        for helper in RuntimeHelper::ALL {
            let abi = JitHelperAbi::for_helper(helper);
            assert_eq!(abi.version, JIT_HELPER_ABI_VERSION);
            assert_eq!(abi.context_arguments, 1);
            assert_eq!(abi.value_results, 1);
            assert!(helper_ids.insert(abi.helper_id));
        }

        assert_eq!(
            JitHelperAbi::for_helper(RuntimeHelper::Constant).value_arguments,
            1
        );
        assert_eq!(
            JitHelperAbi::for_helper(RuntimeHelper::Add).value_arguments,
            2
        );
        assert_eq!(helper_ids.len(), RuntimeHelper::ALL.len());
    }

    #[test]
    fn frame_materialization_captures_and_restores_the_existing_frame_contract() {
        let heap = Heap::new();
        let locals = heap.new_environment();
        let closure = heap.new_environment();
        let body = Rc::new(FunctionBody {
            registers: 2,
            instructions: vec![Instruction::Return { value: 0 }],
            locations: vec![None],
        });
        let mut frame = ResumableFrame::callee(
            body.clone(),
            "callee",
            7,
            2,
            locals.clone(),
            closure.clone(),
            ReturnTarget {
                register: 1,
                call_site: None,
            },
        );
        frame.ip = 3;
        frame.registers[0] = VmValue::number(7.0);
        frame.registers[1] = VmValue::string("before");

        let mut scheduler = CooperativeScheduler::<()>::new(1).expect("valid quantum");
        let task_id = scheduler.spawn(()).expect("task id should be allocated");
        let state = JitState::disabled();
        let mut materialized = state.materialize_frame(
            &frame,
            Some(task_id),
            JitSafepoint::new(JitSafepointKind::Native, frame.ip),
        );

        assert_eq!(materialized.body().map(|body| body.registers), Some(2));
        assert_eq!(materialized.instruction(), 3);
        assert_eq!(materialized.function(), "callee");
        assert_eq!(materialized.function_index(), Some(7));
        assert!(!materialized.is_main());
        assert_eq!(
            materialized.return_target().map(|target| target.register),
            Some(1)
        );
        assert_eq!(materialized.task_id(), Some(task_id));
        assert_eq!(
            materialized.safepoint(),
            JitSafepoint::new(JitSafepointKind::Native, 3)
        );
        assert!(JitSafepointKind::Native.uses_instruction_budget());
        assert!(!JitSafepointKind::GarbageCollection.uses_instruction_budget());
        assert!(Rc::ptr_eq(materialized.locals(), &locals));
        assert!(Rc::ptr_eq(materialized.closure(), &closure));

        materialized.registers_mut()[0] = VmValue::number(11.0);
        frame.ip = 0;
        frame.registers[0] = VmValue::Nil;
        frame.registers[1] = VmValue::Nil;
        materialized.restore_into(&mut frame);

        assert!(Rc::ptr_eq(
            frame.body.as_ref().expect("body is retained"),
            &body
        ));
        assert_eq!(frame.ip, 3);
        assert_eq!(frame.function_index, Some(7));
        assert_eq!(frame.registers[0].to_string(), "11");
        assert_eq!(frame.registers[1].to_string(), "before");
        assert!(Rc::ptr_eq(&frame.locals, &locals));
        assert!(Rc::ptr_eq(&frame.closure, &closure));
        assert_eq!(
            frame.return_target.as_ref().map(|target| target.register),
            Some(1)
        );
    }

    fn eligible_function(index: usize) -> Function {
        function(
            index,
            vec![
                Instruction::LoadVar { dest: 0, name: 0 },
                Instruction::Add {
                    dest: 1,
                    left: 0,
                    right: 0,
                },
                Instruction::Return { value: 1 },
            ],
        )
    }

    #[test]
    fn disabled_state_falls_back_without_inspecting_bytecode() {
        let program = program(vec![eligible_function(0)]);
        let mut state = JitState::disabled();

        assert_eq!(
            state.eligibility(&program, Some(0), JitExecutionMode::Ordinary),
            JitEligibility::Fallback(JitFallbackReason::Disabled)
        );
        assert_eq!(
            state.admit(&program, Some(0), JitExecutionMode::Ordinary, 4),
            JitAdmission::Fallback(JitFallbackReason::Disabled)
        );
    }

    #[test]
    fn eligibility_requires_an_explicit_whitelist_and_ordinary_mode() {
        let program = program(vec![eligible_function(0), eligible_function(1)]);
        let state = JitState::enabled_for_tests([0], 64);

        assert_eq!(
            state.eligibility(&program, Some(0), JitExecutionMode::Ordinary),
            JitEligibility::Eligible { function_index: 0 }
        );
        assert_eq!(
            state.eligibility(&program, Some(1), JitExecutionMode::Ordinary),
            JitEligibility::Fallback(JitFallbackReason::NotWhitelisted(1))
        );
        assert_eq!(
            state.eligibility(&program, Some(0), JitExecutionMode::Profile),
            JitEligibility::Fallback(JitFallbackReason::ObservableMode(JitExecutionMode::Profile))
        );
        assert_eq!(
            state.eligibility(&program, Some(0), JitExecutionMode::Cooperative),
            JitEligibility::Fallback(JitFallbackReason::CooperativeMode)
        );
        assert_eq!(
            state.eligibility(&program, None, JitExecutionMode::Ordinary),
            JitEligibility::Fallback(JitFallbackReason::MainBody)
        );
    }

    #[test]
    fn dynamic_calls_and_native_boundaries_report_specific_fallbacks() {
        let dynamic = program(vec![function(
            0,
            vec![
                Instruction::Call {
                    dest: 0,
                    callee: 1,
                    arguments: Vec::new(),
                },
                Instruction::Return { value: 0 },
            ],
        )]);
        let state = JitState::enabled_for_tests([0], 64);
        assert_eq!(
            state.eligibility(&dynamic, Some(0), JitExecutionMode::Ordinary),
            JitEligibility::Fallback(JitFallbackReason::DynamicCall { instruction: 0 })
        );

        let callback = program(vec![function(
            0,
            vec![
                Instruction::NativeCall {
                    dest: 0,
                    name: 0,
                    arguments: Vec::new(),
                },
                Instruction::Return { value: 0 },
            ],
        )]);
        assert_eq!(
            state.eligibility(&callback, Some(0), JitExecutionMode::Ordinary),
            JitEligibility::Fallback(JitFallbackReason::CallbackBoundary {
                instruction: 0,
                name: "map".to_string(),
            })
        );

        let unsupported = program(vec![function(
            0,
            vec![
                Instruction::Array {
                    dest: 0,
                    elements: Vec::new(),
                },
                Instruction::Return { value: 0 },
            ],
        )]);
        assert_eq!(
            state.eligibility(&unsupported, Some(0), JitExecutionMode::Ordinary),
            JitEligibility::Fallback(JitFallbackReason::UnsupportedInstruction {
                instruction: 0,
                opcode: "array",
            })
        );
    }

    #[test]
    fn cache_admission_is_bounded_reusable_and_evictable() {
        let program = program(vec![eligible_function(0), eligible_function(1)]);
        let mut state = JitState::enabled_for_tests([0, 1], 8);

        let first_handle = match state.admit(&program, Some(0), JitExecutionMode::Ordinary, 5) {
            JitAdmission::Reserved {
                function_index: 0,
                bytes: 5,
                handle,
            } => handle,
            admission => panic!("unexpected first admission: {admission:?}"),
        };
        assert_eq!(first_handle.function_index(), 0);
        assert!(state.resolve_code(&first_handle).is_ok());
        let ir = state
            .cached_ir(0)
            .expect("admission should retain verified Cranelift IR");
        assert_eq!(ir.function_index, 0);
        assert!(ir.display().contains("call"));
        assert!(ir.display().contains("i64"));
        let cached_handle = match state.admit(&program, Some(0), JitExecutionMode::Ordinary, 7) {
            JitAdmission::Cached {
                function_index: 0,
                bytes: 5,
                handle,
            } => handle,
            admission => panic!("unexpected cached admission: {admission:?}"),
        };
        assert_eq!(cached_handle, first_handle);
        assert_eq!(
            state.admit(&program, Some(1), JitExecutionMode::Ordinary, 4),
            JitAdmission::Fallback(JitFallbackReason::CodeCacheExhausted {
                requested: 4,
                remaining: 3,
            })
        );
        assert_eq!(
            state.cache_stats(),
            JitCacheStats {
                capacity: 8,
                used: 5,
                entries: 1,
            }
        );

        state.clear_cache();
        assert!(state.cached_ir(0).is_none());
        let first_generation = first_handle.generation();
        assert!(matches!(
            state.resolve_code(&first_handle),
            Err(JitFallbackReason::StaleCodeEntry {
                function_index: 0,
                generation,
            })
                if generation == first_generation
        ));
        assert_eq!(
            state.cache_stats(),
            JitCacheStats {
                capacity: 8,
                used: 0,
                entries: 0,
            }
        );
    }

    #[test]
    fn code_entry_handles_reject_foreign_caches_and_failed_publication_rolls_back() {
        let base_program = program(vec![eligible_function(0)]);
        let mut state = JitState::enabled_for_tests([0], 16);
        let handle = match state.admit(&base_program, Some(0), JitExecutionMode::Ordinary, 4) {
            JitAdmission::Reserved { handle, .. } => handle,
            admission => panic!("unexpected admission: {admission:?}"),
        };

        let foreign_state = JitState::enabled_for_tests([0], 16);
        assert!(matches!(
            foreign_state.resolve_code(&handle),
            Err(JitFallbackReason::ForeignCodeEntry { function_index: 0 })
        ));

        state.clear_cache();
        let replacement = match state.admit(&base_program, Some(0), JitExecutionMode::Ordinary, 4) {
            JitAdmission::Reserved { handle, .. } => handle,
            admission => panic!("unexpected replacement admission: {admission:?}"),
        };
        assert_ne!(replacement, handle);
        assert_ne!(replacement.generation(), handle.generation());
        assert!(state.resolve_code(&replacement).is_ok());
        let stale_generation = handle.generation();
        assert!(matches!(
            state.resolve_code(&handle),
            Err(JitFallbackReason::StaleCodeEntry {
                function_index: 0,
                generation,
            })
                if generation == stale_generation
        ));

        let failed_program = program(vec![function(
            0,
            vec![
                Instruction::Return { value: 0 },
                Instruction::Return { value: 0 },
            ],
        )]);
        let mut failed_state = JitState::enabled_for_tests([0], 16);
        let failed = failed_state.admit(&failed_program, Some(0), JitExecutionMode::Ordinary, 4);
        assert!(matches!(
            failed,
            JitAdmission::Fallback(JitFallbackReason::CraneliftIr { .. })
        ));
        assert_eq!(
            failed_state.cache_stats(),
            JitCacheStats {
                capacity: 16,
                used: 0,
                entries: 0,
            }
        );
    }
}
