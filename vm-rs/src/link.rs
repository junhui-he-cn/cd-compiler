use crate::bytecode::{
    Constant, DebugLocation, DebugRange, DebugSource, Function, FunctionBody, Instruction, Program,
};
use crate::format::{verify_module_artifact, verify_program, ModuleArtifact};
use std::collections::{HashMap, HashSet};

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
    constants: Vec<Constant>,
    names: Vec<String>,
    main: FunctionBody,
    functions: Vec<Function>,
    debug_sources: Vec<DebugSource>,
}

impl Linker {
    fn new(modules: Vec<ModuleArtifact>) -> Result<Self, String> {
        let mut by_identity = HashMap::new();
        for module in modules {
            verify_module_artifact(&module)
                .map_err(|error| format!("module artifact verification failed: {}", error))?;
            if by_identity
                .insert(module.identity.clone(), module)
                .is_some()
            {
                return Err("duplicate module identity".to_string());
            }
        }
        if by_identity.is_empty() {
            return Err("module product set is empty".to_string());
        }

        let mut entries = by_identity
            .values()
            .filter(|module| module.is_entry)
            .collect::<Vec<_>>();
        entries.sort_by_key(|module| module.entry_order);
        if entries.is_empty() {
            return Err("module product set has no entry module".to_string());
        }
        for (expected, module) in entries.iter().enumerate() {
            if module.entry_order != Some(expected) {
                return Err("entry module orders must be contiguous from zero".to_string());
            }
        }

        for module in by_identity.values() {
            let mut previous_offset = 0;
            for (index, dependency) in module.dependencies.iter().enumerate() {
                if !by_identity.contains_key(&dependency.identity) {
                    return Err(format!(
                        "module `{}` dependency d{} targets missing module `{}`",
                        module.identity, index, dependency.identity
                    ));
                }
                if dependency.instruction_offset > module.program.main.instructions.len() {
                    return Err(format!(
                        "module `{}` dependency d{} instruction offset out of range",
                        module.identity, index
                    ));
                }
                if index != 0 && dependency.instruction_offset < previous_offset {
                    return Err(format!(
                        "module `{}` dependency offsets are not ordered",
                        module.identity
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

    fn allocate_context(&mut self, module: &ModuleArtifact) -> Result<ModuleContext, String> {
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
        )?;
        Ok(context)
    }

    fn expand(&mut self, identity: &str) -> Result<(), String> {
        if self.expanded.contains(identity) {
            return Ok(());
        }
        if !self.visiting.insert(identity.to_string()) {
            return Err(format!("module dependency cycle at `{}`", identity));
        }

        let module = self
            .modules
            .get(identity)
            .cloned()
            .ok_or_else(|| format!("missing module `{}`", identity))?;
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

    fn finish(mut self) -> Result<Program, String> {
        let mut entries = self
            .modules
            .values()
            .filter(|module| module.is_entry)
            .map(|module| {
                (
                    module.entry_order.expect("validated entry order"),
                    module.identity.clone(),
                )
            })
            .collect::<Vec<_>>();
        entries.sort_by_key(|(order, _)| *order);
        for (_, identity) in entries {
            self.expand(&identity)?;
        }
        Ok(Program {
            constants: self.constants,
            names: self.names,
            main: self.main,
            functions: self.functions,
            debug_sources: self.debug_sources,
        })
    }
}

pub fn link_modules(modules: Vec<ModuleArtifact>) -> Result<Program, String> {
    let program = Linker::new(modules)?.finish()?;
    verify_program(&program)
        .map_err(|error| format!("linked program verification failed: {}", error))?;
    Ok(program)
}

#[cfg(test)]
mod tests {
    use super::link_modules;
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
    fn rejects_module_dependency_cycle_deterministically() {
        let dependency = |identity: &str| ModuleDependency {
            identity: identity.to_string(),
            kind: ModuleDependencyKind::Import,
            instruction_offset: 0,
            requested_path: format!("./{}.cd", identity),
        };
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
) -> Result<Instruction, String> {
    map_instruction(instruction, context, 0, |target| Ok(target))
}

fn map_main_instruction(
    instruction: &Instruction,
    context: &ModuleContext,
    local_to_global: &[usize],
) -> Result<Instruction, String> {
    map_instruction(instruction, context, context.main_register_base, |target| {
        local_to_global
            .get(target)
            .copied()
            .ok_or_else(|| "jump target out of range while linking".to_string())
    })
}

fn map_instruction(
    instruction: &Instruction,
    context: &ModuleContext,
    register_base: usize,
    map_jump: impl Fn(usize) -> Result<usize, String>,
) -> Result<Instruction, String> {
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
                .collect::<Result<Vec<_>, String>>()?,
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
                .collect::<Result<Vec<_>, String>>()?,
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

fn checked_add(left: usize, right: usize, description: &str) -> Result<usize, String> {
    left.checked_add(right)
        .ok_or_else(|| format!("{} out of range", description))
}
