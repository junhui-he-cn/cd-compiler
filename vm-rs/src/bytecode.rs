//! Bytecode data model for `.cdbc` artifacts.
//!
//! The 0.2 format introduces strong index types so the VM never re-derives
//! language-level identity (locals, upvalues, globals, types, variants, native
//! imports, blocks) from strings. The parser maps the `main` section to
//! `functions[0]` and `function fN` sections to `functions[N + 1]`;
//! `Program::entry` names the unified entry function.

use crate::memory::MemoryRegionKind;

macro_rules! id_type {
    ($(#[$doc:meta])* $name:ident) => {
        $(#[$doc])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
        pub struct $name(pub u32);
    };
}

id_type!(
    /// Virtual-register identifier inside one function body.
    RegId
);
id_type!(
    /// Compiler-assigned local slot inside one function frame.
    LocalId
);
id_type!(
    /// Compiler-assigned upvalue slot inside one function's closure.
    UpvalueId
);
id_type!(
    /// Compiler-assigned global slot.
    GlobalId
);
id_type!(
    /// Function table index.
    FuncId
);
id_type!(
    /// Type layout table index.
    TypeId
);
id_type!(
    /// Enum variant table index.
    VariantId
);
id_type!(
    /// Native import table index.
    NativeId
);
id_type!(
    /// Basic block identifier.
    BlockId
);
id_type!(
    /// String table index (display/debug/import metadata only).
    StringId
);
id_type!(
    /// Constant table index.
    ConstId
);

#[derive(Clone, Debug, PartialEq)]
pub struct Program {
    pub constants: Vec<Constant>,
    pub names: Vec<String>,
    pub globals: Vec<usize>,
    /// Static machine storage loaded before execution. Dynamic globals remain
    /// represented by `globals` and are intentionally kept separate.
    pub data_segments: Vec<DataSegment>,
    /// Machine symbols are resolved after all static segments have been
    /// allocated. They are intentionally separate from dynamic globals.
    pub symbols: Vec<Symbol>,
    /// Machine relocations are applied by the loader after symbol resolution.
    pub relocations: Vec<Relocation>,
    pub types: Vec<TypeLayout>,
    pub native_imports: Vec<NativeImport>,
    pub modules: Vec<ModuleInit>,
    pub functions: Vec<Function>,
    pub entry: FuncId,
    pub debug_sources: Vec<DebugSource>,
}

/// One static machine-memory segment. `Rodata` and `Data` carry an
/// initialization payload whose length must equal `size`; `Bss` is always
/// zero-initialized and carries no payload.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DataSegment {
    pub kind: MemoryRegionKind,
    pub alignment: u64,
    pub size: u64,
    pub initial: Option<Vec<u8>>,
}

/// The VM-level target denoted by one machine symbol.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SymbolTarget {
    /// A function-table entry. This is a VM function index, never a host
    /// executable address.
    Function(FuncId),
    /// A byte inside one static data segment.
    Data { segment: usize, offset: u64 },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Symbol {
    pub name: String,
    pub target: SymbolTarget,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RelocationKind {
    /// Write a resolved VM address as a little-endian u64.
    Abs64,
    /// Resolve a function symbol into a direct-call function-table index.
    FuncIndex,
}

impl RelocationKind {
    pub const ABS64: Self = Self::Abs64;
    pub const FUNC_INDEX: Self = Self::FuncIndex;

    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Abs64 => "ABS64",
            Self::FuncIndex => "FUNC_INDEX",
        }
    }
}

/// The artifact location patched by a machine relocation.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RelocationTarget {
    /// An eight-byte payload location in a static segment.
    Data { segment: usize, offset: u64 },
    /// The function operand of one `CallDirect` instruction.
    CallDirect { function: FuncId, instruction: usize },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Relocation {
    pub kind: RelocationKind,
    pub symbol: String,
    /// Signed VM-level addend. `FUNC_INDEX` currently requires zero.
    pub addend: i64,
    pub target: RelocationTarget,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModuleInit {
    pub init: FuncId,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NativeImport {
    pub name: String,
    pub abi: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VariantLayout {
    pub name: String,
    pub payload_count: usize,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TypeLayout {
    pub is_enum: bool,
    pub name: String,
    pub field_names: Vec<String>,
    pub variants: Vec<VariantLayout>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DebugSource {
    pub module: Option<String>,
    pub path: String,
    pub text: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DebugRange {
    pub source: usize,
    pub start: usize,
    pub end: usize,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DebugLocation {
    pub source: usize,
    pub line: usize,
    pub column: usize,
    pub range: Option<DebugRange>,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Constant {
    Nil,
    Number(String),
    Bool(bool),
    String(String),
}

/// Width selected by a machine integer instruction.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum MachineIntWidth {
    W8,
    W16,
    W32,
    W64,
}

impl MachineIntWidth {
    #[allow(non_upper_case_globals)]
    pub const I8: Self = Self::W8;
    #[allow(non_upper_case_globals)]
    pub const I16: Self = Self::W16;
    #[allow(non_upper_case_globals)]
    pub const I32: Self = Self::W32;
    #[allow(non_upper_case_globals)]
    pub const I64: Self = Self::W64;

    pub const fn bits(self) -> u32 {
        match self {
            Self::W8 => 8,
            Self::W16 => 16,
            Self::W32 => 32,
            Self::W64 => 64,
        }
    }

    pub const fn mask(self) -> u64 {
        match self {
            Self::W8 => 0xff,
            Self::W16 => 0xffff,
            Self::W32 => 0xffff_ffff,
            Self::W64 => u64::MAX,
        }
    }

    pub const fn as_str(self) -> &'static str {
        match self {
            Self::W8 => "8",
            Self::W16 => "16",
            Self::W32 => "32",
            Self::W64 => "64",
        }
    }
}

/// Scalar domain and width selected by a typed machine memory operation.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum MachineMemoryType {
    I8,
    I16,
    I32,
    I64,
    F32,
    F64,
    Addr,
}

impl MachineMemoryType {
    /// Alias matching the spelling used by the machine ABI document.
    #[allow(non_upper_case_globals)]
    pub const Address: Self = Self::Addr;
    /// Alias matching the textual `ADDR` type name.
    pub const ADDR: Self = Self::Addr;

    pub const fn size(self) -> usize {
        match self {
            Self::I8 => 1,
            Self::I16 => 2,
            Self::I32 | Self::F32 => 4,
            Self::I64 | Self::F64 | Self::Addr => 8,
        }
    }

    pub const fn alignment(self) -> u64 {
        match self {
            Self::I8 => 1,
            Self::I16 => 2,
            Self::I32 | Self::F32 => 4,
            Self::I64 | Self::F64 | Self::Addr => 8,
        }
    }

    pub const fn as_str(self) -> &'static str {
        match self {
            Self::I8 => "i8",
            Self::I16 => "i16",
            Self::I32 => "i32",
            Self::I64 => "i64",
            Self::F32 => "f32",
            Self::F64 => "f64",
            Self::Addr => "addr",
        }
    }

    pub const fn is_integer(self) -> bool {
        matches!(self, Self::I8 | Self::I16 | Self::I32 | Self::I64)
    }

    pub const fn is_float(self) -> bool {
        matches!(self, Self::F32 | Self::F64)
    }

    pub const fn is_address(self) -> bool {
        matches!(self, Self::Addr)
    }
}

/// Scalar domain used by the register-facing machine call ABI.
///
/// This is deliberately separate from `MachineMemoryType`: memory types carry
/// a load/store width, while ABI scalars describe only the value domain.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum MachineScalarType {
    MachineInt,
    MachineFloat,
    Address,
}

impl MachineScalarType {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::MachineInt => "machine_int",
            Self::MachineFloat => "machine_float",
            Self::Address => "address",
        }
    }
}

/// Maximum number of scalar values in the 0.3 register call boundary.
pub const MACHINE_ABI_MAX_PARAMS: usize = 8;

/// Predicate used by the machine integer comparison instruction.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum MachineIntPredicate {
    Eq,
    Ne,
    Slt,
    Sle,
    Sgt,
    Sge,
    Ult,
    Ule,
    Ugt,
    Uge,
}

impl MachineIntPredicate {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Eq => "eq",
            Self::Ne => "ne",
            Self::Slt => "slt",
            Self::Sle => "sle",
            Self::Sgt => "sgt",
            Self::Sge => "sge",
            Self::Ult => "ult",
            Self::Ule => "ule",
            Self::Ugt => "ugt",
            Self::Uge => "uge",
        }
    }
}

/// Where a function's upvalue comes from. Populated by the closure-conversion
/// phase from explicit `upvalue` descriptors in the artifact.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum UpvalueSource {
    Local(LocalId),
    Upvalue(UpvalueId),
    Global(GlobalId),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UpvalueDesc {
    pub source: UpvalueSource,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Function {
    pub id: FuncId,
    pub name: String,
    pub arity: usize,
    /// Reserved machine stack bytes for one invocation. Zero preserves the
    /// dynamic-only function representation.
    pub machine_frame_size: u64,
    /// Register-facing machine ABI argument domains. An empty vector means
    /// that this function has no scalar ABI parameter metadata.
    pub machine_params: Vec<MachineScalarType>,
    /// Optional register-facing machine ABI result domain.
    pub machine_return: Option<MachineScalarType>,
    /// Number of compiler-assigned local slots (excluding parameters).
    pub local_count: usize,
    /// Explicit upvalue descriptors.
    pub upvalues: Vec<UpvalueDesc>,
    pub params: Vec<String>,
    pub registers: usize,
    pub instructions: Vec<Instruction>,
    pub locations: Vec<Option<DebugLocation>>,
}

impl Function {
    /// Whether this function has an explicit scalar machine call boundary.
    /// Machine frame metadata alone remains compatible with the pre-ABI frame
    /// model used by VM03-06 through VM03-08.
    pub fn has_machine_abi(&self) -> bool {
        !self.machine_params.is_empty() || self.machine_return.is_some()
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum Instruction {
    Constant {
        dest: usize,
        constant: usize,
    },
    MakeFunction {
        dest: usize,
        function: FuncId,
    },
    Array {
        dest: usize,
        elements: Vec<usize>,
    },
    Map {
        dest: usize,
        entries: Vec<(usize, usize)>,
    },
    MakeStruct {
        dest: usize,
        type_id: TypeId,
        elements: Vec<usize>,
    },
    StructGet {
        dest: usize,
        object: usize,
        type_id: TypeId,
        slot: usize,
    },
    StructSet {
        dest: usize,
        object: usize,
        type_id: TypeId,
        slot: usize,
        value: usize,
    },
    MakeVariant {
        dest: usize,
        type_id: TypeId,
        variant_id: VariantId,
        payload: Vec<usize>,
    },
    IsVariant {
        dest: usize,
        value: usize,
        type_id: TypeId,
        variant_id: VariantId,
    },
    VariantGet {
        dest: usize,
        value: usize,
        type_id: TypeId,
        variant_id: VariantId,
        index: usize,
    },
    Move {
        dest: usize,
        source: usize,
    },
    LoadLocal {
        dest: usize,
        slot: usize,
    },
    BindLocal {
        slot: usize,
        value: usize,
    },
    SetLocal {
        slot: usize,
        value: usize,
    },
    LoadUpvalue {
        dest: usize,
        slot: usize,
    },
    SetUpvalue {
        slot: usize,
        value: usize,
    },
    LoadGlobal {
        dest: usize,
        slot: usize,
    },
    InitGlobal {
        slot: usize,
        value: usize,
    },
    SetGlobal {
        slot: usize,
        value: usize,
    },
    Call {
        dest: usize,
        callee: usize,
        arguments: Vec<usize>,
    },
    CallDirect {
        dest: usize,
        function: FuncId,
        arguments: Vec<usize>,
    },
    CallNative {
        dest: usize,
        native: NativeId,
        arguments: Vec<usize>,
    },
    Index {
        dest: usize,
        collection: usize,
        index: usize,
    },
    AssignIndex {
        dest: usize,
        collection: usize,
        index: usize,
        value: usize,
    },
    ArrayGet {
        dest: usize,
        collection: usize,
        index: usize,
    },
    ArraySet {
        dest: usize,
        collection: usize,
        index: usize,
        value: usize,
    },
    MapGet {
        dest: usize,
        collection: usize,
        index: usize,
    },
    MapSet {
        dest: usize,
        collection: usize,
        index: usize,
        value: usize,
    },
    RangeGet {
        dest: usize,
        collection: usize,
        index: usize,
    },
    Field {
        dest: usize,
        object: usize,
        name: usize,
    },
    AssignField {
        dest: usize,
        object: usize,
        name: usize,
        value: usize,
    },
    LenArray {
        dest: usize,
        value: usize,
    },
    LenMap {
        dest: usize,
        value: usize,
    },
    LenRange {
        dest: usize,
        value: usize,
    },
    LenStr {
        dest: usize,
        value: usize,
    },
    Len {
        dest: usize,
        value: usize,
    },
    IterInit {
        dest: usize,
        value: usize,
    },
    IterHas {
        dest: usize,
        value: usize,
    },
    IterNext {
        dest: usize,
        value: usize,
    },
    InitModule {
        module: usize,
    },
    AssertNumber {
        dest: usize,
        value: usize,
        message: usize,
    },
    Return {
        value: usize,
    },
    Negate {
        dest: usize,
        value: usize,
    },
    Not {
        dest: usize,
        value: usize,
    },
    Add {
        dest: usize,
        left: usize,
        right: usize,
    },
    Subtract {
        dest: usize,
        left: usize,
        right: usize,
    },
    Multiply {
        dest: usize,
        left: usize,
        right: usize,
    },
    Divide {
        dest: usize,
        left: usize,
        right: usize,
    },
    Equal {
        dest: usize,
        left: usize,
        right: usize,
    },
    NotEqual {
        dest: usize,
        left: usize,
        right: usize,
    },
    Greater {
        dest: usize,
        left: usize,
        right: usize,
    },
    GreaterEqual {
        dest: usize,
        left: usize,
        right: usize,
    },
    Less {
        dest: usize,
        left: usize,
        right: usize,
    },
    LessEqual {
        dest: usize,
        left: usize,
        right: usize,
    },
    AddNum {
        dest: usize,
        left: usize,
        right: usize,
    },
    SubNum {
        dest: usize,
        left: usize,
        right: usize,
    },
    MulNum {
        dest: usize,
        left: usize,
        right: usize,
    },
    DivNum {
        dest: usize,
        left: usize,
        right: usize,
    },
    NegNum {
        dest: usize,
        value: usize,
    },
    ConcatStr {
        dest: usize,
        left: usize,
        right: usize,
    },
    LessNum {
        dest: usize,
        left: usize,
        right: usize,
    },
    LessEqualNum {
        dest: usize,
        left: usize,
        right: usize,
    },
    GreaterNum {
        dest: usize,
        left: usize,
        right: usize,
    },
    GreaterEqualNum {
        dest: usize,
        left: usize,
        right: usize,
    },
    LessStr {
        dest: usize,
        left: usize,
        right: usize,
    },
    LessEqualStr {
        dest: usize,
        left: usize,
        right: usize,
    },
    GreaterStr {
        dest: usize,
        left: usize,
        right: usize,
    },
    GreaterEqualStr {
        dest: usize,
        left: usize,
        right: usize,
    },
    IConst {
        dest: usize,
        width: MachineIntWidth,
        raw: u64,
    },
    Load {
        dest: usize,
        address: usize,
        memory_type: MachineMemoryType,
    },
    Store {
        address: usize,
        source: usize,
        memory_type: MachineMemoryType,
    },
    Memcpy {
        destination: usize,
        source: usize,
        size: usize,
    },
    Memmove {
        destination: usize,
        source: usize,
        size: usize,
    },
    Memset {
        destination: usize,
        value: usize,
        size: usize,
    },
    FrameAddr {
        dest: usize,
        offset: u64,
    },
    Trunc {
        dest: usize,
        value: usize,
        from_width: MachineIntWidth,
        to_width: MachineIntWidth,
    },
    ZExt {
        dest: usize,
        value: usize,
        from_width: MachineIntWidth,
        to_width: MachineIntWidth,
    },
    SExt {
        dest: usize,
        value: usize,
        from_width: MachineIntWidth,
        to_width: MachineIntWidth,
    },
    IAdd {
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
    },
    ISub {
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
    },
    IMul {
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
    },
    SDiv {
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
    },
    UDiv {
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
    },
    SRem {
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
    },
    URem {
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
    },
    And {
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
    },
    Or {
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
    },
    Xor {
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
    },
    IntNot {
        dest: usize,
        value: usize,
        width: MachineIntWidth,
    },
    Shl {
        dest: usize,
        value: usize,
        amount: usize,
        width: MachineIntWidth,
    },
    LShr {
        dest: usize,
        value: usize,
        amount: usize,
        width: MachineIntWidth,
    },
    AShr {
        dest: usize,
        value: usize,
        amount: usize,
        width: MachineIntWidth,
    },
    ICmp {
        dest: usize,
        left: usize,
        right: usize,
        width: MachineIntWidth,
        predicate: MachineIntPredicate,
    },
    BlockStart {
        id: BlockId,
    },
    Br {
        target: BlockId,
    },
    BrIf {
        condition: usize,
        if_true: BlockId,
        if_false: BlockId,
    },
    ReturnNil,
}
