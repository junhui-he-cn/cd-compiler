//! Bytecode data model for `.cdbc` artifacts.
//!
//! The 0.2 refactor introduces strong index types so the VM never re-derives
//! language-level identity (locals, upvalues, globals, types, variants, native
//! imports, blocks) from strings. The parser maps the legacy `main` section to
//! `functions[0]` and legacy `fK` sections to `functions[K + 1]`;
//! `Program::entry` names the unified entry function.

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
    pub types: Vec<TypeLayout>,
    pub native_imports: Vec<NativeImport>,
    pub functions: Vec<Function>,
    pub entry: FuncId,
    pub debug_sources: Vec<DebugSource>,
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

/// Where a function's upvalue comes from. Populated by the closure-conversion
/// phase; the current `.cdbc 0.1` emitter always produces an empty vector.
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
    /// Number of compiler-assigned local slots (excluding parameters).
    /// Still zero for `.cdbc 0.1` artifacts; populated by the variable
    /// lowering phase.
    pub local_count: usize,
    /// Explicit upvalue descriptors. Still empty for `.cdbc 0.1` artifacts.
    pub upvalues: Vec<UpvalueDesc>,
    pub params: Vec<String>,
    pub registers: usize,
    pub instructions: Vec<Instruction>,
    pub locations: Vec<Option<DebugLocation>>,
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
    Struct {
        dest: usize,
        type_name: Option<usize>,
        fields: Vec<(usize, usize)>,
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
    Variant {
        dest: usize,
        enum_name: usize,
        variant_name: usize,
        payload: Vec<usize>,
    },
    MakeVariant {
        dest: usize,
        type_id: TypeId,
        variant_id: VariantId,
        payload: Vec<usize>,
    },
    VariantTag {
        dest: usize,
        value: usize,
        enum_name: usize,
        variant_name: usize,
    },
    IsVariant {
        dest: usize,
        value: usize,
        type_id: TypeId,
        variant_id: VariantId,
    },
    VariantField {
        dest: usize,
        value: usize,
        index: usize,
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
    LoadVar {
        dest: usize,
        name: usize,
    },
    StoreVar {
        name: usize,
        value: usize,
    },
    AssignVar {
        name: usize,
        value: usize,
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
    NativeCall {
        dest: usize,
        name: usize,
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
    Len {
        dest: usize,
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
    AssertArray {
        dest: usize,
        value: usize,
    },
    AssertNumber {
        dest: usize,
        value: usize,
        message: usize,
    },
    Print {
        value: usize,
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
    Jump {
        target: usize,
    },
    JumpIfFalse {
        condition: usize,
        target: usize,
    },
    JumpIfTrue {
        condition: usize,
        target: usize,
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
