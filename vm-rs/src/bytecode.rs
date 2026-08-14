//! Bytecode data model for `.cdbc` artifacts.
//!
//! The 0.2 refactor introduces strong index types so the VM never re-derives
//! language-level identity (locals, upvalues, globals, types, variants, native
//! imports, blocks) from strings. The current text envelope is still
//! `.cdbc 0.1`, so the parser maps the legacy `main` section to `functions[0]`
//! and legacy `fK` sections to `functions[K + 1]`; `Program::entry` names the
//! unified entry function. Operand indexes that later phases will replace with
//! `LocalId`/`GlobalId`/`StringId`/`ConstId` remain `usize` for now.

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
    pub functions: Vec<Function>,
    pub entry: FuncId,
    pub debug_sources: Vec<DebugSource>,
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
    Variant {
        dest: usize,
        enum_name: usize,
        variant_name: usize,
        payload: Vec<usize>,
    },
    VariantTag {
        dest: usize,
        value: usize,
        enum_name: usize,
        variant_name: usize,
    },
    VariantField {
        dest: usize,
        value: usize,
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
}
