use crate::bytecode::{
    BlockId, Constant, DebugLocation, DebugRange, DebugSource, FuncId, Function, GlobalId,
    Instruction, ModuleInit, NativeId, NativeImport, Program, TypeId, TypeLayout, UpvalueDesc,
    UpvalueSource,
};
use crate::format::{verify_module_artifact, verify_program, ModuleArtifact};
use std::collections::HashMap;
use std::fmt;

/// Machine-readable class for a module-linking failure.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LinkErrorKind {
    InvalidModule,
    DuplicateModuleIdentity,
    EmptyModuleSet,
    MissingEntryModule,
    InvalidEntryOrder,
    MissingDependency,
    InvalidDependency,
    DependencyCycle,
    InvalidInstruction,
    Overflow,
    InvalidLinkedProgram,
}

impl LinkErrorKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::InvalidModule => "invalid_module",
            Self::DuplicateModuleIdentity => "duplicate_module_identity",
            Self::EmptyModuleSet => "empty_module_set",
            Self::MissingEntryModule => "missing_entry_module",
            Self::InvalidEntryOrder => "invalid_entry_order",
            Self::MissingDependency => "missing_dependency",
            Self::InvalidDependency => "invalid_dependency",
            Self::DependencyCycle => "dependency_cycle",
            Self::InvalidInstruction => "invalid_instruction",
            Self::Overflow => "overflow",
            Self::InvalidLinkedProgram => "invalid_linked_program",
        }
    }
}

/// Stable linker error returned by the checked library facade.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LinkError {
    pub kind: LinkErrorKind,
    pub module_identity: Option<String>,
    pub dependency_index: Option<usize>,
    pub message: String,
}

impl LinkError {
    fn new(kind: LinkErrorKind, message: impl Into<String>) -> Self {
        Self {
            kind,
            module_identity: None,
            dependency_index: None,
            message: message.into(),
        }
    }

    fn module(
        kind: LinkErrorKind,
        module_identity: impl Into<String>,
        message: impl Into<String>,
    ) -> Self {
        Self {
            kind,
            module_identity: Some(module_identity.into()),
            dependency_index: None,
            message: message.into(),
        }
    }

    fn dependency(
        kind: LinkErrorKind,
        module_identity: impl Into<String>,
        dependency_index: usize,
        message: impl Into<String>,
    ) -> Self {
        Self {
            kind,
            module_identity: Some(module_identity.into()),
            dependency_index: Some(dependency_index),
            message: message.into(),
        }
    }
}

impl fmt::Display for LinkError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for LinkError {}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LinkReport {
    pub input_module_identities: Vec<String>,
    pub entry_module_identities: Vec<String>,
    pub expanded_module_order: Vec<String>,
    pub input_instruction_count: usize,
    pub input_dependency_count: usize,
    pub linked_instruction_count: usize,
    pub linked_function_count: usize,
    pub linked_constant_count: usize,
    pub linked_name_count: usize,
    pub linked_debug_source_count: usize,
}

#[derive(Clone, Debug, PartialEq)]
pub struct LinkResult {
    pub program: Program,
    pub report: LinkReport,
}

#[derive(Clone, Debug)]
struct ModuleContext {
    constant_base: usize,
    name_base: usize,
    function_base: usize,
    type_remap: Vec<u32>,
    native_remap: Vec<u32>,
    global_remap: Vec<usize>,
    module_remap: Vec<u32>,
    init: FuncId,
    source_base: usize,
}

struct Linker {
    modules: HashMap<String, ModuleArtifact>,
    contexts: HashMap<String, ModuleContext>,
    input_module_identities: Vec<String>,
    entry_module_identities: Vec<String>,
    input_instruction_count: usize,
    input_dependency_count: usize,
    expansion_order: Vec<String>,
    constants: Vec<Constant>,
    names: Vec<String>,
    functions: Vec<Function>,
    debug_sources: Vec<DebugSource>,
    global_names: HashMap<String, usize>,
    linked_types: Vec<TypeLayout>,
    type_names: HashMap<String, u32>,
    linked_native_imports: Vec<NativeImport>,
    native_names: HashMap<String, u32>,
    linked_modules: Vec<ModuleInit>,
    module_indices: HashMap<String, u32>,
}

fn program_instruction_count(program: &Program) -> usize {
    program
        .functions
        .iter()
        .map(|function| function.instructions.len())
        .fold(0usize, usize::saturating_add)
}

impl Linker {
    fn new(modules: Vec<ModuleArtifact>) -> Result<Self, LinkError> {
        let input_instruction_count = modules
            .iter()
            .map(|module| program_instruction_count(&module.program))
            .fold(0usize, usize::saturating_add);
        let input_dependency_count = modules
            .iter()
            .map(|module| module.dependencies.len())
            .fold(0usize, usize::saturating_add);
        let mut by_identity = HashMap::new();
        for module in modules {
            let identity = module.identity.clone();
            verify_module_artifact(&module).map_err(|error| {
                LinkError::module(
                    LinkErrorKind::InvalidModule,
                    identity.clone(),
                    format!("module artifact verification failed: {}", error),
                )
            })?;
            if by_identity.insert(identity, module).is_some() {
                return Err(LinkError::new(
                    LinkErrorKind::DuplicateModuleIdentity,
                    "duplicate module identity",
                ));
            }
        }
        if by_identity.is_empty() {
            return Err(LinkError::new(
                LinkErrorKind::EmptyModuleSet,
                "module product set is empty",
            ));
        }

        let mut entries = by_identity
            .values()
            .filter(|module| module.is_entry)
            .collect::<Vec<_>>();
        entries.sort_by_key(|module| module.entry_order);
        if entries.is_empty() {
            return Err(LinkError::new(
                LinkErrorKind::MissingEntryModule,
                "module product set has no entry module",
            ));
        }
        for (expected, module) in entries.iter().enumerate() {
            if module.entry_order != Some(expected) {
                return Err(LinkError::new(
                    LinkErrorKind::InvalidEntryOrder,
                    "entry module orders must be contiguous from zero",
                ));
            }
        }
        let mut input_module_identities = by_identity.keys().cloned().collect::<Vec<_>>();
        input_module_identities.sort();
        let entry_module_identities = entries
            .iter()
            .map(|module| module.identity.clone())
            .collect();

        for module in by_identity.values() {
            for (index, dependency) in module.dependencies.iter().enumerate() {
                if !by_identity.contains_key(&dependency.identity) {
                    return Err(LinkError::dependency(
                        LinkErrorKind::MissingDependency,
                        module.identity.clone(),
                        index,
                        format!(
                            "module `{}` dependency d{} targets missing module `{}`",
                            module.identity, index, dependency.identity
                        ),
                    ));
                }
            }
        }

        Ok(Self {
            modules: by_identity,
            contexts: HashMap::new(),
            input_module_identities,
            entry_module_identities,
            input_instruction_count,
            input_dependency_count,
            expansion_order: Vec::new(),
            constants: Vec::new(),
            names: Vec::new(),
            functions: Vec::new(),
            debug_sources: Vec::new(),
            global_names: HashMap::new(),
            linked_types: Vec::new(),
            type_names: HashMap::new(),
            linked_native_imports: Vec::new(),
            native_names: HashMap::new(),
            linked_modules: Vec::new(),
            module_indices: HashMap::new(),
        })
    }

    fn allocate_context(&mut self, module: &ModuleArtifact) -> Result<ModuleContext, LinkError> {
        let constant_base = self.constants.len();
        let name_base = self.names.len();
        let source_base = self.debug_sources.len();
        self.constants
            .extend(module.program.constants.iter().cloned());
        self.names.extend(module.program.names.iter().cloned());
        self.debug_sources
            .extend(module.program.debug_sources.iter().cloned());

        let mut global_remap = Vec::with_capacity(module.program.globals.len());
        for (local, name_index) in module.program.globals.iter().enumerate() {
            let linked_name_index = checked_add(*name_index, name_base, "linked name index")
                .map_err(|error| {
                    LinkError::module(
                        LinkErrorKind::Overflow,
                        module.identity.clone(),
                        error.to_string(),
                    )
                })?;
            let Some(name) = self.names.get(linked_name_index) else {
                return Err(LinkError::module(
                    LinkErrorKind::InvalidModule,
                    module.identity.clone(),
                    format!("global g{local} references name n{name_index} out of range"),
                ));
            };
            let slot = match self.global_names.get(name) {
                Some(slot) => *slot,
                None => {
                    let slot = self.global_names.len();
                    self.global_names.insert(name.clone(), slot);
                    slot
                }
            };
            global_remap.push(slot);
        }
        let mut type_remap = Vec::with_capacity(module.program.types.len());
        for layout in &module.program.types {
            let linked = match self.type_names.get(&layout.name) {
                Some(id) => *id,
                None => {
                    let id = self.linked_types.len() as u32;
                    self.type_names.insert(layout.name.clone(), id);
                    self.linked_types.push(layout.clone());
                    id
                }
            };
            type_remap.push(linked);
        }
        let mut native_remap = Vec::with_capacity(module.program.native_imports.len());
        for import in &module.program.native_imports {
            let linked = match self.native_names.get(&import.name) {
                Some(id) => *id,
                None => {
                    let id = self.linked_native_imports.len() as u32;
                    self.native_names.insert(import.name.clone(), id);
                    self.linked_native_imports.push(import.clone());
                    id
                }
            };
            native_remap.push(linked);
        }
        let mut module_remap = Vec::with_capacity(module.dependencies.len());
        for (index, dependency) in module.dependencies.iter().enumerate() {
            let linked = *self.module_indices.get(&dependency.identity).ok_or_else(|| {
                LinkError::dependency(
                    LinkErrorKind::MissingDependency,
                    module.identity.clone(),
                    index,
                    format!(
                        "module `{}` dependency d{} has no merged module index",
                        module.identity, index
                    ),
                )
            })?;
            module_remap.push(linked);
        }
        let function_base = self.functions.len();
        let init = FuncId(
            checked_add(
                checked_add(function_base, module.init, "linked module init index")
                    .map_err(|error| {
                        LinkError::module(
                            LinkErrorKind::Overflow,
                            module.identity.clone(),
                            error.to_string(),
                        )
                    })?,
                1,
                "linked module init function",
            )
            .map_err(|error| {
                LinkError::module(
                    LinkErrorKind::Overflow,
                    module.identity.clone(),
                    error.to_string(),
                )
            })? as u32,
        );
        let context = ModuleContext {
            constant_base,
            name_base,
            function_base,
            type_remap,
            native_remap,
            global_remap,
            module_remap,
            init,
            source_base,
        };
        Ok(context)
    }

    fn expand(&mut self, identity: &str) -> Result<(), LinkError> {
        if self.module_indices.contains_key(identity) {
            return Ok(());
        }
        let module_index = self.linked_modules.len() as u32;
        self.module_indices.insert(identity.to_string(), module_index);
        self.expansion_order.push(identity.to_string());
        self.linked_modules.push(ModuleInit { init: FuncId(0) });

        let module = self.modules.get(identity).cloned().ok_or_else(|| {
            LinkError::new(
                LinkErrorKind::MissingDependency,
                format!("missing module `{}`", identity),
            )
        })?;
        for dependency in &module.dependencies {
            self.expand(&dependency.identity)?;
        }

        let context = self.allocate_context(&module)?;
        self.contexts.insert(identity.to_string(), context.clone());
        self.linked_modules[module_index as usize] = ModuleInit { init: context.init };

        let function_base = context.function_base;
        for (position, function) in module.program.functions.iter().enumerate() {
            self.functions.push(Function {
                id: FuncId((function_base + position + 1) as u32),
                name: function.name.clone(),
                arity: function.arity,
                local_count: function.local_count,
                upvalues: function
                    .upvalues
                    .iter()
                    .map(|descriptor| match &descriptor.source {
                        UpvalueSource::Global(global) => {
                            let linked = *context
                                .global_remap
                                .get(global.0 as usize)
                                .ok_or_else(|| {
                                    LinkError::new(
                                        LinkErrorKind::InvalidInstruction,
                                        format!("global g{} out of range", global.0),
                                    )
                                })?;
                            Ok(UpvalueDesc {
                                source: UpvalueSource::Global(GlobalId(linked as u32)),
                            })
                        }
                        source => Ok(UpvalueDesc {
                            source: source.clone(),
                        }),
                    })
                    .collect::<Result<Vec<_>, _>>()?,
                params: function.params.clone(),
                registers: function.registers,
                instructions: function
                    .instructions
                    .iter()
                    .map(|instruction| map_function_instruction(instruction, &context))
                    .collect::<Result<Vec<_>, _>>()?,
                locations: function
                    .locations
                    .iter()
                    .map(|location| remap_location(location, context.source_base))
                    .collect(),
            });
        }
        Ok(())
    }

    fn finish(mut self) -> Result<LinkResult, LinkError> {
        for identity in self.entry_module_identities.clone() {
            self.expand(&identity)?;
        }
        let mut main = Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
            registers: 0,
            instructions: Vec::new(),
            locations: Vec::new(),
        };
        for identity in &self.entry_module_identities {
            let module = *self.module_indices.get(identity).ok_or_else(|| {
                LinkError::new(
                    LinkErrorKind::MissingDependency,
                    format!("entry module `{}` has no merged module index", identity),
                )
            })?;
            main.instructions.push(Instruction::InitModule {
                module: module as usize,
            });
            let entry_location = self
                .modules
                .get(identity)
                .and_then(|artifact| {
                    artifact
                        .program
                        .functions
                        .get(artifact.init)
                        .and_then(|function| function.locations.first().cloned().flatten())
                })
                .and_then(|location| {
                    self.contexts
                        .get(identity)
                        .map(|context| remap_location(&Some(location), context.source_base))
                })
                .flatten();
            main.locations.push(entry_location);
        }
        main.instructions.push(Instruction::ReturnNil);
        main.locations.push(None);
        let linked_instruction_count = main
            .instructions
            .len()
            .saturating_add(
                self.functions
                    .iter()
                    .map(|function| function.instructions.len())
                    .fold(0usize, usize::saturating_add),
            );

        let report = LinkReport {
            input_module_identities: self.input_module_identities.clone(),
            entry_module_identities: self.entry_module_identities.clone(),
            expanded_module_order: self.expansion_order.clone(),
            input_instruction_count: self.input_instruction_count,
            input_dependency_count: self.input_dependency_count,
            linked_instruction_count,
            linked_function_count: self.functions.len(),
            linked_constant_count: self.constants.len(),
            linked_name_count: self.names.len(),
            linked_debug_source_count: self.debug_sources.len(),
        };
        let mut functions = vec![main];
        functions.extend(self.functions);
        let mut linked_globals = vec![0usize; self.global_names.len()];
        for (name, slot) in &self.global_names {
            let name_index = self
                .names
                .iter()
                .position(|candidate| candidate == name)
                .ok_or_else(|| {
                    LinkError::new(
                        LinkErrorKind::InvalidLinkedProgram,
                        format!("linked global `{}` has no name index", name),
                    )
                })?;
            linked_globals[*slot] = name_index;
        }
        let program = Program {
            constants: self.constants,
            globals: linked_globals,
            types: self.linked_types,
            native_imports: self.linked_native_imports,
            modules: self.linked_modules,
            names: self.names,
            functions,
            entry: FuncId(0),
            debug_sources: self.debug_sources,
        };
        Ok(LinkResult { program, report })
    }
}

/// Link module products and return deterministic size/order metadata with a typed error.
pub fn link_modules_with_report_checked(
    modules: Vec<ModuleArtifact>,
) -> Result<LinkResult, LinkError> {
    let result = Linker::new(modules)?.finish()?;
    verify_program(&result.program).map_err(|error| {
        LinkError::new(
            LinkErrorKind::InvalidLinkedProgram,
            format!("linked program verification failed: {}", error),
        )
    })?;
    Ok(result)
}

pub fn link_modules_with_report(modules: Vec<ModuleArtifact>) -> Result<LinkResult, String> {
    link_modules_with_report_checked(modules).map_err(|error| error.to_string())
}

/// Link module products and return only the linked program with a typed error.
pub fn link_modules_checked(modules: Vec<ModuleArtifact>) -> Result<Program, LinkError> {
    link_modules_with_report_checked(modules).map(|result| result.program)
}

pub fn link_modules(modules: Vec<ModuleArtifact>) -> Result<Program, String> {
    link_modules_with_report(modules).map(|result| result.program)
}

#[cfg(test)]
mod tests {
    use super::{link_modules, link_modules_with_report};
    use crate::bytecode::{FuncId, Function, Program};
    use crate::format::{ModuleArtifact, ModuleDependency, ModuleDependencyKind};

    fn empty_program() -> Program {
        Program {
            constants: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            modules: Vec::new(),
            names: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
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

    fn dependency(identity: &str) -> ModuleDependency {
        ModuleDependency {
            identity: identity.to_string(),
            kind: ModuleDependencyKind::Import,
            requested_path: format!("./{}.cd", identity),
        }
    }

    fn module(
        identity: &str,
        is_entry: bool,
        entry_order: Option<usize>,
        dependencies: Vec<ModuleDependency>,
    ) -> ModuleArtifact {
        ModuleArtifact {
            identity: identity.to_string(),
            path: format!("{}.cd", identity),
            canonical_path: format!("{}.cd", identity),
            is_entry,
            entry_order,
            init: 0,
            dependencies,
            program: empty_program(),
        }
    }

    #[test]
    fn rejects_module_with_empty_identity_before_linking() {
        let module = ModuleArtifact {
            identity: String::new(),
            path: "entry.cd".to_string(),
            canonical_path: "entry.cd".to_string(),
            is_entry: true,
            entry_order: Some(0),
            init: 0,
            dependencies: Vec::new(),
            program: empty_program(),
        };

        let error = link_modules(vec![module]).expect_err("invalid module must be rejected");
        assert!(
            error.contains("identity and paths must be non-empty"),
            "{error}"
        );
    }

    #[test]
    fn reports_deterministic_diamond_expansion_and_link_sizes() {
        let shared = module("shared", false, None, Vec::new());
        let left = module("left", false, None, vec![dependency("shared")]);
        let right = module("right", false, None, vec![dependency("shared")]);
        let entry = module(
            "entry",
            true,
            Some(0),
            vec![dependency("left"), dependency("right")],
        );

        let result = link_modules_with_report(vec![right, shared, entry, left])
            .expect("diamond modules should link");
        assert_eq!(
            result.report.input_module_identities,
            vec!["entry", "left", "right", "shared"]
        );
        assert_eq!(result.report.entry_module_identities, vec!["entry"]);
        assert_eq!(
            result.report.expanded_module_order,
            vec!["entry", "left", "shared", "right"]
        );
        assert_eq!(result.report.input_instruction_count, 0);
        assert_eq!(result.report.input_dependency_count, 4);
        assert_eq!(result.report.linked_instruction_count, 2);
        assert_eq!(result.report.linked_function_count, 4);
        assert_eq!(result.report.linked_constant_count, 0);
        assert_eq!(result.report.linked_name_count, 0);
        assert_eq!(result.report.linked_debug_source_count, 0);
    }

    #[test]
    fn links_module_dependency_cycles_without_stream_splicing() {
        let entry = ModuleArtifact {
            identity: "entry".to_string(),
            path: "entry.cd".to_string(),
            canonical_path: "entry.cd".to_string(),
            is_entry: true,
            entry_order: Some(0),
            init: 0,
            dependencies: vec![dependency("library")],
            program: empty_program(),
        };
        let library = ModuleArtifact {
            identity: "library".to_string(),
            path: "library.cd".to_string(),
            canonical_path: "library.cd".to_string(),
            is_entry: false,
            entry_order: None,
            init: 0,
            dependencies: vec![dependency("entry")],
            program: empty_program(),
        };

        let linked = link_modules(vec![entry, library]).expect("cycle must link");
        assert_eq!(linked.modules.len(), 2);
        assert_eq!(linked.entry, FuncId(0));
        assert_eq!(
            linked.functions[0].instructions.len(),
            2,
            "entry main should init one module and return"
        );
    }
}

fn map_function_instruction(
    instruction: &Instruction,
    context: &ModuleContext,
) -> Result<Instruction, LinkError> {
    map_instruction(
        instruction,
        context,
        0,
        |target| Ok(target),
        |block| Ok(block),
        |block| Ok(block),
    )
}

fn map_instruction(
    instruction: &Instruction,
    context: &ModuleContext,
    register_base: usize,
    map_jump: impl Fn(usize) -> Result<usize, LinkError>,
    map_block: impl Fn(u32) -> Result<u32, LinkError>,
    map_branch: impl Fn(u32) -> Result<u32, LinkError>,
) -> Result<Instruction, LinkError> {
    let register = |value: usize| checked_add(value, register_base, "linked register index");
    let constant =
        |value: usize| checked_add(value, context.constant_base, "linked constant index");
    let name = |value: usize| checked_add(value, context.name_base, "linked name index");
    let function = |value: FuncId| {
        checked_add(
            checked_add(
                value.0 as usize,
                context.function_base,
                "linked function index",
            )?,
            1,
            "linked function index",
        )
        .map(|index| FuncId(index as u32))
    };
    let map_type = |value: TypeId| {
        context
            .type_remap
            .get(value.0 as usize)
            .copied()
            .map(TypeId)
            .ok_or_else(|| {
                LinkError::new(
                    LinkErrorKind::InvalidInstruction,
                    format!("type t{} out of range", value.0),
                )
            })
    };
    Ok(match instruction {
        Instruction::Constant {
            dest,
            constant: value,
        } => Instruction::Constant {
            dest: register(*dest)?,
            constant: constant(*value)?,
        },
        Instruction::MakeFunction {
            dest,
            function: value,
        } => Instruction::MakeFunction {
            dest: register(*dest)?,
            function: function(*value)?,
        },
        Instruction::Array { dest, elements } => Instruction::Array {
            dest: register(*dest)?,
            elements: elements
                .iter()
                .map(|value| register(*value))
                .collect::<Result<Vec<_>, _>>()?,
        },
        Instruction::Map { dest, entries } => Instruction::Map {
            dest: register(*dest)?,
            entries: entries
                .iter()
                .map(|(key, value)| Ok((register(*key)?, register(*value)?)))
                .collect::<Result<Vec<_>, LinkError>>()?,
        },
        Instruction::Struct {
            dest,
            type_name,
            fields,
        } => Instruction::Struct {
            dest: register(*dest)?,
            type_name: type_name.map(name).transpose()?,
            fields: fields
                .iter()
                .map(|(field, value)| Ok((name(*field)?, register(*value)?)))
                .collect::<Result<Vec<_>, LinkError>>()?,
        },
        Instruction::MakeStruct {
            dest,
            type_id,
            elements,
        } => Instruction::MakeStruct {
            dest: register(*dest)?,
            type_id: map_type(*type_id)?,
            elements: elements
                .iter()
                .map(|value| register(*value))
                .collect::<Result<Vec<_>, _>>()?,
        },
        Instruction::StructGet {
            dest,
            object,
            type_id,
            slot,
        } => Instruction::StructGet {
            dest: register(*dest)?,
            object: register(*object)?,
            type_id: map_type(*type_id)?,
            slot: *slot,
        },
        Instruction::StructSet {
            dest,
            object,
            type_id,
            slot,
            value,
        } => Instruction::StructSet {
            dest: register(*dest)?,
            object: register(*object)?,
            type_id: map_type(*type_id)?,
            slot: *slot,
            value: register(*value)?,
        },
        Instruction::Variant {
            dest,
            enum_name,
            variant_name,
            payload,
        } => Instruction::Variant {
            dest: register(*dest)?,
            enum_name: name(*enum_name)?,
            variant_name: name(*variant_name)?,
            payload: payload
                .iter()
                .map(|value| register(*value))
                .collect::<Result<Vec<_>, _>>()?,
        },
        Instruction::MakeVariant {
            dest,
            type_id,
            variant_id,
            payload,
        } => Instruction::MakeVariant {
            dest: register(*dest)?,
            type_id: map_type(*type_id)?,
            variant_id: *variant_id,
            payload: payload
                .iter()
                .map(|value| register(*value))
                .collect::<Result<Vec<_>, _>>()?,
        },
        Instruction::IsVariant {
            dest,
            value,
            type_id,
            variant_id,
        } => Instruction::IsVariant {
            dest: register(*dest)?,
            value: register(*value)?,
            type_id: map_type(*type_id)?,
            variant_id: *variant_id,
        },
        Instruction::VariantGet {
            dest,
            value,
            type_id,
            variant_id,
            index,
        } => Instruction::VariantGet {
            dest: register(*dest)?,
            value: register(*value)?,
            type_id: map_type(*type_id)?,
            variant_id: *variant_id,
            index: *index,
        },
        Instruction::VariantTag {
            dest,
            value,
            enum_name,
            variant_name,
        } => Instruction::VariantTag {
            dest: register(*dest)?,
            value: register(*value)?,
            enum_name: name(*enum_name)?,
            variant_name: name(*variant_name)?,
        },
        Instruction::VariantField { dest, value, index } => Instruction::VariantField {
            dest: register(*dest)?,
            value: register(*value)?,
            index: *index,
        },
        Instruction::Move { dest, source } => Instruction::Move {
            dest: register(*dest)?,
            source: register(*source)?,
        },
        Instruction::LoadVar { dest, name: value } => Instruction::LoadVar {
            dest: register(*dest)?,
            name: name(*value)?,
        },
        Instruction::StoreVar {
            name: value,
            value: source,
        } => Instruction::StoreVar {
            name: name(*value)?,
            value: register(*source)?,
        },
        Instruction::AssignVar {
            name: value,
            value: source,
        } => Instruction::AssignVar {
            name: name(*value)?,
            value: register(*source)?,
        },
        Instruction::LoadLocal { dest, slot } => Instruction::LoadLocal {
            dest: register(*dest)?,
            slot: *slot,
        },
        Instruction::BindLocal { slot, value } => Instruction::BindLocal {
            slot: *slot,
            value: register(*value)?,
        },
        Instruction::SetLocal { slot, value } => Instruction::SetLocal {
            slot: *slot,
            value: register(*value)?,
        },
        Instruction::LoadUpvalue { dest, slot } => Instruction::LoadUpvalue {
            dest: register(*dest)?,
            slot: *slot,
        },
        Instruction::SetUpvalue { slot, value } => Instruction::SetUpvalue {
            slot: *slot,
            value: register(*value)?,
        },
        Instruction::LoadGlobal { dest, slot } => Instruction::LoadGlobal {
            dest: register(*dest)?,
            slot: *context.global_remap.get(*slot).ok_or(LinkError::new(
                LinkErrorKind::InvalidInstruction,
                format!("global g{slot} out of range"),
            ))?,
        },
        Instruction::InitGlobal { slot, value } => Instruction::InitGlobal {
            slot: *context.global_remap.get(*slot).ok_or(LinkError::new(
                LinkErrorKind::InvalidInstruction,
                format!("global g{slot} out of range"),
            ))?,
            value: register(*value)?,
        },
        Instruction::SetGlobal { slot, value } => Instruction::SetGlobal {
            slot: *context.global_remap.get(*slot).ok_or(LinkError::new(
                LinkErrorKind::InvalidInstruction,
                format!("global g{slot} out of range"),
            ))?,
            value: register(*value)?,
        },
        Instruction::Call {
            dest,
            callee,
            arguments,
        } => Instruction::Call {
            dest: register(*dest)?,
            callee: register(*callee)?,
            arguments: arguments
                .iter()
                .map(|value| register(*value))
                .collect::<Result<Vec<_>, _>>()?,
        },
        Instruction::CallDirect {
            dest,
            function: target,
            arguments,
        } => Instruction::CallDirect {
            dest: register(*dest)?,
            function: function(*target)?,
            arguments: arguments
                .iter()
                .map(|argument| register(*argument))
                .collect::<Result<Vec<_>, _>>()?,
        },
        Instruction::InitModule { module } => Instruction::InitModule {
            module: *context.module_remap.get(*module).ok_or_else(|| {
                LinkError::new(
                    LinkErrorKind::InvalidInstruction,
                    format!("module m{} out of range", module),
                )
            })? as usize,
        },
        Instruction::NativeCall {
            dest,
            name: value,
            arguments,
        } => Instruction::NativeCall {
            dest: register(*dest)?,
            name: name(*value)?,
            arguments: arguments
                .iter()
                .map(|argument| register(*argument))
                .collect::<Result<Vec<_>, _>>()?,
        },
        Instruction::CallNative {
            dest,
            native,
            arguments,
        } => Instruction::CallNative {
            dest: register(*dest)?,
            native: NativeId(
                *context
                    .native_remap
                    .get(native.0 as usize)
                    .ok_or_else(|| {
                        LinkError::new(
                            LinkErrorKind::InvalidInstruction,
                            format!("native import i{} out of range", native.0),
                        )
                    })?,
            ),
            arguments: arguments
                .iter()
                .map(|argument| register(*argument))
                .collect::<Result<Vec<_>, _>>()?,
        },
        Instruction::Index {
            dest,
            collection,
            index,
        } => Instruction::Index {
            dest: register(*dest)?,
            collection: register(*collection)?,
            index: register(*index)?,
        },
        Instruction::ArrayGet {
            dest,
            collection,
            index,
        } => Instruction::ArrayGet {
            dest: register(*dest)?,
            collection: register(*collection)?,
            index: register(*index)?,
        },
        Instruction::MapGet {
            dest,
            collection,
            index,
        } => Instruction::MapGet {
            dest: register(*dest)?,
            collection: register(*collection)?,
            index: register(*index)?,
        },
        Instruction::RangeGet {
            dest,
            collection,
            index,
        } => Instruction::RangeGet {
            dest: register(*dest)?,
            collection: register(*collection)?,
            index: register(*index)?,
        },
        Instruction::AssignIndex {
            dest,
            collection,
            index,
            value,
        } => Instruction::AssignIndex {
            dest: register(*dest)?,
            collection: register(*collection)?,
            index: register(*index)?,
            value: register(*value)?,
        },
        Instruction::ArraySet {
            dest,
            collection,
            index,
            value,
        } => Instruction::ArraySet {
            dest: register(*dest)?,
            collection: register(*collection)?,
            index: register(*index)?,
            value: register(*value)?,
        },
        Instruction::MapSet {
            dest,
            collection,
            index,
            value,
        } => Instruction::MapSet {
            dest: register(*dest)?,
            collection: register(*collection)?,
            index: register(*index)?,
            value: register(*value)?,
        },
        Instruction::Field {
            dest,
            object,
            name: value,
        } => Instruction::Field {
            dest: register(*dest)?,
            object: register(*object)?,
            name: name(*value)?,
        },
        Instruction::AssignField {
            dest,
            object,
            name: value,
            value: source,
        } => Instruction::AssignField {
            dest: register(*dest)?,
            object: register(*object)?,
            name: name(*value)?,
            value: register(*source)?,
        },
        Instruction::Len { dest, value } => Instruction::Len {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::LenArray { dest, value } => Instruction::LenArray {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::LenMap { dest, value } => Instruction::LenMap {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::LenRange { dest, value } => Instruction::LenRange {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::LenStr { dest, value } => Instruction::LenStr {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::AssertArray { dest, value } => Instruction::AssertArray {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::IterInit { dest, value } => Instruction::IterInit {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::IterHas { dest, value } => Instruction::IterHas {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::IterNext { dest, value } => Instruction::IterNext {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::AssertNumber {
            dest,
            value,
            message,
        } => Instruction::AssertNumber {
            dest: register(*dest)?,
            value: register(*value)?,
            message: name(*message)?,
        },
        Instruction::Print { value } => Instruction::Print {
            value: register(*value)?,
        },
        Instruction::Return { value } => Instruction::Return {
            value: register(*value)?,
        },
        Instruction::Negate { dest, value } => Instruction::Negate {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::NegNum { dest, value } => Instruction::NegNum {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::Not { dest, value } => Instruction::Not {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::Add { dest, left, right } => Instruction::Add {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::AddNum { dest, left, right } => Instruction::AddNum {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::ConcatStr { dest, left, right } => Instruction::ConcatStr {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::Subtract { dest, left, right } => Instruction::Subtract {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::SubNum { dest, left, right } => Instruction::SubNum {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::Multiply { dest, left, right } => Instruction::Multiply {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::MulNum { dest, left, right } => Instruction::MulNum {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::Divide { dest, left, right } => Instruction::Divide {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::DivNum { dest, left, right } => Instruction::DivNum {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::Equal { dest, left, right } => Instruction::Equal {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::NotEqual { dest, left, right } => Instruction::NotEqual {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::Greater { dest, left, right } => Instruction::Greater {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::GreaterNum { dest, left, right } => Instruction::GreaterNum {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::GreaterStr { dest, left, right } => Instruction::GreaterStr {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::GreaterEqual { dest, left, right } => Instruction::GreaterEqual {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::GreaterEqualNum { dest, left, right } => Instruction::GreaterEqualNum {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::GreaterEqualStr { dest, left, right } => Instruction::GreaterEqualStr {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::Less { dest, left, right } => Instruction::Less {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::LessNum { dest, left, right } => Instruction::LessNum {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::LessStr { dest, left, right } => Instruction::LessStr {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::LessEqual { dest, left, right } => Instruction::LessEqual {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::LessEqualNum { dest, left, right } => Instruction::LessEqualNum {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::LessEqualStr { dest, left, right } => Instruction::LessEqualStr {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::Jump { target } => Instruction::Jump {
            target: map_jump(*target)?,
        },
        Instruction::JumpIfFalse { condition, target } => Instruction::JumpIfFalse {
            condition: register(*condition)?,
            target: map_jump(*target)?,
        },
        Instruction::JumpIfTrue { condition, target } => Instruction::JumpIfTrue {
            condition: register(*condition)?,
            target: map_jump(*target)?,
        },
        Instruction::BlockStart { id } => Instruction::BlockStart {
            id: BlockId(map_block(id.0)?),
        },
        Instruction::Br { target } => Instruction::Br {
            target: BlockId(map_branch(target.0)?),
        },
        Instruction::BrIf {
            condition,
            if_true,
            if_false,
        } => Instruction::BrIf {
            condition: register(*condition)?,
            if_true: BlockId(map_branch(if_true.0)?),
            if_false: BlockId(map_branch(if_false.0)?),
        },
        Instruction::ReturnNil => Instruction::ReturnNil,
    })
}

fn remap_location(location: &Option<DebugLocation>, source_base: usize) -> Option<DebugLocation> {
    location.as_ref().map(|location| DebugLocation {
        source: location.source + source_base,
        line: location.line,
        column: location.column,
        range: location.range.as_ref().map(|range| DebugRange {
            source: range.source + source_base,
            start: range.start,
            end: range.end,
        }),
    })
}

fn checked_add(left: usize, right: usize, description: &str) -> Result<usize, LinkError> {
    left.checked_add(right).ok_or_else(|| {
        LinkError::new(
            LinkErrorKind::Overflow,
            format!("{} out of range", description),
        )
    })
}
