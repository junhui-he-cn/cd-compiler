#![allow(dead_code)]

use crate::bytecode::{Instruction, Program};
use cranelift_codegen::ir::{
    types, AbiParam, ExtFuncData, ExternalName, Function, InstBuilder, Signature, UserExternalName,
    UserFuncName, Value,
};
use cranelift_codegen::isa::{CallConv, TargetFrontendConfig};
use cranelift_codegen::{settings, verifier::verify_function};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext};
use std::collections::{BTreeMap, BTreeSet};
use target_lexicon::PointerWidth;

/// The first V6C cache budget is an internal admission bound. It is not a
/// runtime-element or artifact budget, and it is not exposed through the
/// public VM configuration until a concrete code representation exists.
pub(crate) const DEFAULT_CODE_CACHE_BYTES: usize = 64 * 1024;

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
    Reserved { function_index: usize, bytes: usize },
    Cached { function_index: usize, bytes: usize },
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
    entries: BTreeMap<usize, CachedJitUnit>,
}

#[derive(Debug)]
struct CachedJitUnit {
    bytes: usize,
    ir: CraneliftIrUnit,
}

enum CacheReservation {
    Inserted,
    Existing(usize),
    Exhausted { remaining: usize },
}

impl JitCodeCache {
    fn new(capacity: usize) -> Self {
        Self {
            capacity,
            used: 0,
            entries: BTreeMap::new(),
        }
    }

    fn reserve(&mut self, function_index: usize, bytes: usize) -> CacheReservation {
        if let Some(existing) = self.entries.get(&function_index) {
            return CacheReservation::Existing(existing.bytes);
        }

        let remaining = self.capacity.saturating_sub(self.used);
        if bytes > remaining {
            return CacheReservation::Exhausted { remaining };
        }

        CacheReservation::Inserted
    }

    fn insert(&mut self, function_index: usize, bytes: usize, ir: CraneliftIrUnit) {
        debug_assert!(!self.entries.contains_key(&function_index));
        self.entries
            .insert(function_index, CachedJitUnit { bytes, ir });
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

        if let Some(bytes) = self.cache.cached_bytes(function_index) {
            return JitAdmission::Cached {
                function_index,
                bytes,
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
            CacheReservation::Inserted => {
                self.cache.insert(function_index, estimated_code_bytes, ir);
                JitAdmission::Reserved {
                    function_index,
                    bytes: estimated_code_bytes,
                }
            }
            CacheReservation::Existing(bytes) => JitAdmission::Cached {
                function_index,
                bytes,
            },
            CacheReservation::Exhausted { remaining } => {
                JitAdmission::Fallback(JitFallbackReason::CodeCacheExhausted {
                    requested: estimated_code_bytes,
                    remaining,
                })
            }
        }
    }

    pub(crate) fn clear_cache(&mut self) {
        self.cache.clear();
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
    fn cached_bytes(&self, function_index: usize) -> Option<usize> {
        self.entries.get(&function_index).map(|entry| entry.bytes)
    }
}

#[repr(u32)]
#[derive(Clone, Copy)]
enum RuntimeHelper {
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
    let mut signature = Signature::new(CallConv::SystemV);
    signature.params.push(AbiParam::new(types::I64));
    signature.params.extend(std::iter::repeat_n(
        AbiParam::new(types::I64),
        arguments.len(),
    ));
    signature.returns.push(AbiParam::new(types::I64));
    let signature = builder.import_signature(signature);
    let user_name = builder
        .func
        .declare_imported_user_function(UserExternalName::new(1, helper as u32));
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

        assert_eq!(
            state.admit(&program, Some(0), JitExecutionMode::Ordinary, 5),
            JitAdmission::Reserved {
                function_index: 0,
                bytes: 5,
            }
        );
        let ir = state
            .cached_ir(0)
            .expect("admission should retain verified Cranelift IR");
        assert_eq!(ir.function_index, 0);
        assert!(ir.display().contains("call"));
        assert!(ir.display().contains("i64"));
        assert_eq!(
            state.admit(&program, Some(0), JitExecutionMode::Ordinary, 7),
            JitAdmission::Cached {
                function_index: 0,
                bytes: 5,
            }
        );
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
        assert_eq!(
            state.cache_stats(),
            JitCacheStats {
                capacity: 8,
                used: 0,
                entries: 0,
            }
        );
    }
}
