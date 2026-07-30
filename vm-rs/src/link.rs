use crate::bytecode::{
    Constant, DebugLocation, DebugRange, DebugSource, Function, FunctionBody, Instruction, Program,
};
use crate::format::{verify_module_artifact, verify_program, ModuleArtifact};
use std::collections::{HashMap, HashSet};
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
    main_register_base: usize,
    source_base: usize,
}

struct Linker {
    modules: HashMap<String, ModuleArtifact>,
    contexts: HashMap<String, ModuleContext>,
    visiting: HashSet<String>,
    expanded: HashSet<String>,
    input_module_identities: Vec<String>,
    entry_module_identities: Vec<String>,
    input_instruction_count: usize,
    input_dependency_count: usize,
    expansion_order: Vec<String>,
    constants: Vec<Constant>,
    names: Vec<String>,
    main: FunctionBody,
    functions: Vec<Function>,
    debug_sources: Vec<DebugSource>,
}

fn program_instruction_count(program: &Program) -> usize {
    program.main.instructions.len().saturating_add(
        program
            .functions
            .iter()
            .map(|function| function.instructions.len())
            .fold(0usize, usize::saturating_add),
    )
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
            let mut previous_offset = 0;
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
                if dependency.instruction_offset > module.program.main.instructions.len() {
                    return Err(LinkError::dependency(
                        LinkErrorKind::InvalidDependency,
                        module.identity.clone(),
                        index,
                        format!(
                            "module `{}` dependency d{} instruction offset out of range",
                            module.identity, index
                        ),
                    ));
                }
                if index != 0 && dependency.instruction_offset < previous_offset {
                    return Err(LinkError::module(
                        LinkErrorKind::InvalidDependency,
                        module.identity.clone(),
                        format!(
                            "module `{}` dependency offsets are not ordered",
                            module.identity
                        ),
                    ));
                }
                previous_offset = dependency.instruction_offset;
            }
        }

        Ok(Self {
            modules: by_identity,
            contexts: HashMap::new(),
            visiting: HashSet::new(),
            expanded: HashSet::new(),
            input_module_identities,
            entry_module_identities,
            input_instruction_count,
            input_dependency_count,
            expansion_order: Vec::new(),
            constants: Vec::new(),
            names: Vec::new(),
            main: FunctionBody {
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            },
            functions: Vec::new(),
            debug_sources: Vec::new(),
        })
    }

    fn allocate_context(&mut self, module: &ModuleArtifact) -> Result<ModuleContext, LinkError> {
        let context = ModuleContext {
            constant_base: self.constants.len(),
            name_base: self.names.len(),
            function_base: self.functions.len(),
            main_register_base: self.main.registers,
            source_base: self.debug_sources.len(),
        };

        self.constants
            .extend(module.program.constants.iter().cloned());
        self.names.extend(module.program.names.iter().cloned());
        self.debug_sources
            .extend(module.program.debug_sources.iter().cloned());
        self.main.registers = checked_add(
            self.main.registers,
            module.program.main.registers,
            "linked main register count",
        )
        .map_err(|error| {
            LinkError::module(
                LinkErrorKind::Overflow,
                module.identity.clone(),
                error.to_string(),
            )
        })?;
        Ok(context)
    }

    fn expand(&mut self, identity: &str) -> Result<(), LinkError> {
        if self.expanded.contains(identity) {
            return Ok(());
        }
        if !self.visiting.insert(identity.to_string()) {
            return Err(LinkError::module(
                LinkErrorKind::DependencyCycle,
                identity,
                format!("module dependency cycle at `{}`", identity),
            ));
        }
        self.expansion_order.push(identity.to_string());

        let module = self.modules.get(identity).cloned().ok_or_else(|| {
            LinkError::new(
                LinkErrorKind::MissingDependency,
                format!("missing module `{}`", identity),
            )
        })?;
        let context = self.allocate_context(&module)?;
        self.contexts.insert(identity.to_string(), context.clone());

        let function_base = context.function_base;
        for (index, function) in module.program.functions.iter().enumerate() {
            self.functions.push(Function {
                index: function_base + index,
                name: function.name.clone(),
                arity: function.arity,
                registers: function.registers,
                params: function.params.clone(),
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

        let mut local_to_global = vec![0; module.program.main.instructions.len() + 1];
        let mut pending = Vec::new();
        let mut local_offset = 0;
        for dependency in &module.dependencies {
            while local_offset < dependency.instruction_offset {
                emit_pending_main(
                    &mut self.main,
                    &mut pending,
                    local_offset,
                    &module.program.main,
                    context.source_base,
                );
                local_to_global[local_offset] = self.main.instructions.len() - 1;
                local_offset += 1;
            }
            self.expand(&dependency.identity)?;
            local_to_global[local_offset] = self.main.instructions.len();
        }
        while local_offset < module.program.main.instructions.len() {
            emit_pending_main(
                &mut self.main,
                &mut pending,
                local_offset,
                &module.program.main,
                context.source_base,
            );
            local_to_global[local_offset] = self.main.instructions.len() - 1;
            local_offset += 1;
        }
        local_to_global[module.program.main.instructions.len()] = self.main.instructions.len();

        for (global_index, _local_index, instruction) in pending {
            self.main.instructions[global_index] =
                map_main_instruction(&instruction, &context, &local_to_global)?;
        }

        self.visiting.remove(identity);
        self.expanded.insert(identity.to_string());
        Ok(())
    }

    fn finish(mut self) -> Result<LinkResult, LinkError> {
        for identity in self.entry_module_identities.clone() {
            self.expand(&identity)?;
        }

        let report = LinkReport {
            input_module_identities: self.input_module_identities.clone(),
            entry_module_identities: self.entry_module_identities.clone(),
            expanded_module_order: self.expansion_order.clone(),
            input_instruction_count: self.input_instruction_count,
            input_dependency_count: self.input_dependency_count,
            linked_instruction_count: self.main.instructions.len(),
            linked_function_count: self.functions.len(),
            linked_constant_count: self.constants.len(),
            linked_name_count: self.names.len(),
            linked_debug_source_count: self.debug_sources.len(),
        };
        let program = Program {
            constants: self.constants,
            names: self.names,
            main: self.main,
            functions: self.functions,
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
    use crate::bytecode::{FunctionBody, Program};
    use crate::format::{ModuleArtifact, ModuleDependency, ModuleDependencyKind};

    fn empty_program() -> Program {
        Program {
            constants: Vec::new(),
            names: Vec::new(),
            main: FunctionBody {
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            },
            functions: Vec::new(),
            debug_sources: Vec::new(),
        }
    }

    fn dependency(identity: &str) -> ModuleDependency {
        ModuleDependency {
            identity: identity.to_string(),
            kind: ModuleDependencyKind::Import,
            instruction_offset: 0,
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
        assert_eq!(result.report.linked_instruction_count, 0);
        assert_eq!(result.report.linked_function_count, 0);
        assert_eq!(result.report.linked_constant_count, 0);
        assert_eq!(result.report.linked_name_count, 0);
        assert_eq!(result.report.linked_debug_source_count, 0);
    }

    #[test]
    fn rejects_module_dependency_cycle_deterministically() {
        let entry = ModuleArtifact {
            identity: "entry".to_string(),
            path: "entry.cd".to_string(),
            canonical_path: "entry.cd".to_string(),
            is_entry: true,
            entry_order: Some(0),
            dependencies: vec![dependency("library")],
            program: empty_program(),
        };
        let library = ModuleArtifact {
            identity: "library".to_string(),
            path: "library.cd".to_string(),
            canonical_path: "library.cd".to_string(),
            is_entry: false,
            entry_order: None,
            dependencies: vec![dependency("entry")],
            program: empty_program(),
        };

        let error = link_modules(vec![entry, library]).expect_err("cycle must be rejected");
        assert_eq!(error, "module dependency cycle at `entry`");
    }
}

fn emit_pending_main(
    main: &mut FunctionBody,
    pending: &mut Vec<(usize, usize, Instruction)>,
    local_index: usize,
    source: &FunctionBody,
    source_base: usize,
) {
    let global_index = main.instructions.len();
    main.instructions
        .push(source.instructions[local_index].clone());
    main.locations
        .push(remap_location(&source.locations[local_index], source_base));
    pending.push((
        global_index,
        local_index,
        source.instructions[local_index].clone(),
    ));
}

fn map_function_instruction(
    instruction: &Instruction,
    context: &ModuleContext,
) -> Result<Instruction, LinkError> {
    map_instruction(instruction, context, 0, |target| Ok(target))
}

fn map_main_instruction(
    instruction: &Instruction,
    context: &ModuleContext,
    local_to_global: &[usize],
) -> Result<Instruction, LinkError> {
    map_instruction(instruction, context, context.main_register_base, |target| {
        local_to_global.get(target).copied().ok_or_else(|| {
            LinkError::new(
                LinkErrorKind::InvalidInstruction,
                "jump target out of range while linking",
            )
        })
    })
}

fn map_instruction(
    instruction: &Instruction,
    context: &ModuleContext,
    register_base: usize,
    map_jump: impl Fn(usize) -> Result<usize, LinkError>,
) -> Result<Instruction, LinkError> {
    let register = |value: usize| checked_add(value, register_base, "linked register index");
    let constant =
        |value: usize| checked_add(value, context.constant_base, "linked constant index");
    let name = |value: usize| checked_add(value, context.name_base, "linked name index");
    let function =
        |value: usize| checked_add(value, context.function_base, "linked function index");
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
        Instruction::Index {
            dest,
            collection,
            index,
        } => Instruction::Index {
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
        Instruction::AssertArray { dest, value } => Instruction::AssertArray {
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
        Instruction::Not { dest, value } => Instruction::Not {
            dest: register(*dest)?,
            value: register(*value)?,
        },
        Instruction::Add { dest, left, right } => Instruction::Add {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::Subtract { dest, left, right } => Instruction::Subtract {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::Multiply { dest, left, right } => Instruction::Multiply {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::Divide { dest, left, right } => Instruction::Divide {
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
        Instruction::GreaterEqual { dest, left, right } => Instruction::GreaterEqual {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::Less { dest, left, right } => Instruction::Less {
            dest: register(*dest)?,
            left: register(*left)?,
            right: register(*right)?,
        },
        Instruction::LessEqual { dest, left, right } => Instruction::LessEqual {
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
