use super::*;

fn reject_unsupported_machine_instructions(program: &Program) -> Result<(), FormatError> {
    if !program.data_segments.is_empty() {
        return Err(FormatError::UnsupportedMachineDataSegment { segment: 0 });
    }
    if !program.symbols.is_empty() {
        return Err(FormatError::UnsupportedMachineSymbols { symbol: 0 });
    }
    if !program.relocations.is_empty() {
        return Err(FormatError::UnsupportedMachineRelocations { relocation: 0 });
    }
    for (function, body) in program.functions.iter().enumerate() {
        for (instruction, operation) in body.instructions.iter().enumerate() {
            if let Some(opcode) = machine_instruction_opcode(operation) {
                return Err(FormatError::UnsupportedMachineInstruction {
                    function,
                    instruction,
                    opcode,
                });
            }
        }
        if body.has_machine_abi() {
            return Err(FormatError::UnsupportedMachineAbi { function });
        }
    }
    Ok(())
}

fn machine_instruction_opcode(instruction: &Instruction) -> Option<&'static str> {
    Some(match instruction {
        Instruction::IConst { .. } => "iconst",
        Instruction::Trunc { .. } => "trunc",
        Instruction::ZExt { .. } => "zext",
        Instruction::SExt { .. } => "sext",
        Instruction::FConst { .. } => "fconst",
        Instruction::FAdd { .. } => "fadd",
        Instruction::FSub { .. } => "fsub",
        Instruction::FMul { .. } => "fmul",
        Instruction::FDiv { .. } => "fdiv",
        Instruction::FNeg { .. } => "fneg",
        Instruction::FCmp { .. } => "fcmp",
        Instruction::SIToFp { .. } => "sitofp",
        Instruction::UIToFp { .. } => "uitofp",
        Instruction::FPToSI { .. } => "fptosi",
        Instruction::FPToUI { .. } => "fptoui",
        Instruction::FPExt { .. } => "fpext",
        Instruction::FPTrunc { .. } => "fptrunc",
        Instruction::IAdd { .. } => "iadd",
        Instruction::ISub { .. } => "isub",
        Instruction::IMul { .. } => "imul",
        Instruction::SDiv { .. } => "sdiv",
        Instruction::UDiv { .. } => "udiv",
        Instruction::SRem { .. } => "srem",
        Instruction::URem { .. } => "urem",
        Instruction::And { .. } => "and",
        Instruction::Or { .. } => "or",
        Instruction::Xor { .. } => "xor",
        Instruction::IntNot { .. } => "not_int",
        Instruction::Shl { .. } => "shl",
        Instruction::LShr { .. } => "lshr",
        Instruction::AShr { .. } => "ashr",
        Instruction::ICmp { .. } => "icmp",
        Instruction::Load { .. } => "load",
        Instruction::Store { .. } => "store",
        Instruction::Memcpy { .. } => "memcpy",
        Instruction::Memmove { .. } => "memmove",
        Instruction::Memset { .. } => "memset",
        Instruction::FrameAddr { .. } => "frame_addr",
        _ => return None,
    })
}

fn format_program_capacity_hint(program: &Program) -> usize {
    let instruction_count = program
        .functions
        .iter()
        .map(|function| function.instructions.len())
        .sum::<usize>();
    let string_bytes = program
        .constants
        .iter()
        .filter_map(|constant| match constant {
            Constant::String(value) => Some(value.len()),
            _ => None,
        })
        .sum::<usize>()
        + program.names.iter().map(String::len).sum::<usize>()
        + program
            .native_imports
            .iter()
            .map(|import| import.name.len())
            .sum::<usize>()
        + program
            .debug_sources
            .iter()
            .map(|source| source.path.len() + source.text.len())
            .sum::<usize>();
    128usize
        .saturating_add(instruction_count.saturating_mul(128))
        .saturating_add(program.constants.len().saturating_mul(32))
        .saturating_add(program.names.len().saturating_mul(32))
        .saturating_add(program.native_imports.len().saturating_mul(32))
        .saturating_add(string_bytes)
}

fn format_segment_initial(segment: &DataSegment) -> String {
    let Some(initial) = segment.initial.as_ref() else {
        return "zero".to_string();
    };
    let mut text = String::from("hex:");
    for byte in initial {
        text.push_str(&format!("{:02x}", byte));
    }
    text
}

fn format_machine_function_ref(function: FuncId) -> String {
    if function.0 == 0 {
        "main".to_string()
    } else {
        format!("f{}", function.0.saturating_sub(1))
    }
}

fn format_symbol_target(target: &SymbolTarget) -> String {
    match target {
        SymbolTarget::Function(function) => {
            format!("function {}", format_machine_function_ref(*function))
        }
        SymbolTarget::Data { segment, offset } => {
            format!("data d{} offset={}", segment, offset)
        }
    }
}

fn format_relocation_target(target: &RelocationTarget) -> String {
    match target {
        RelocationTarget::Data { segment, offset } => {
            format!("data d{} offset={}", segment, offset)
        }
        RelocationTarget::CallDirect {
            function,
            instruction,
        } => format!(
            "call {} instruction={}",
            format_machine_function_ref(*function),
            instruction
        ),
    }
}

fn format_machine_scalar_list(values: &[MachineScalarType]) -> String {
    let inner = values
        .iter()
        .map(|value| value.as_str())
        .collect::<Vec<_>>()
        .join(", ");
    format!("[{}]", inner)
}

fn format_machine_scalar_return(value: Option<MachineScalarType>) -> &'static str {
    value.map_or("none", MachineScalarType::as_str)
}

fn format_program_sections(out: &mut String, program: &Program, include_machine: bool) {
    out.push_str("constants:\n");
    for (index, constant) in program.constants.iter().enumerate() {
        out.push_str(&format!("  c{} = {}\n", index, format_constant(constant)));
    }
    out.push_str("\nnames:\n");
    for (index, name) in program.names.iter().enumerate() {
        out.push_str(&format!("  n{} = {}\n", index, quote_string(name)));
    }
    if !program.globals.is_empty() {
        out.push_str("\nglobals:\n");
        for (index, name) in program.globals.iter().enumerate() {
            out.push_str(&format!("  g{} = n{}\n", index, name));
        }
    }
    if include_machine && !program.data_segments.is_empty() {
        out.push_str("\ndata_segments:\n");
        for (index, segment) in program.data_segments.iter().enumerate() {
            out.push_str(&format!(
                "  d{} = {} alignment={} size={} initial={}\n",
                index,
                segment.kind,
                segment.alignment,
                segment.size,
                format_segment_initial(segment)
            ));
        }
    }
    if include_machine && !program.symbols.is_empty() {
        out.push_str("\nsymbols:\n");
        for (index, symbol) in program.symbols.iter().enumerate() {
            out.push_str(&format!(
                "  s{} = {} {}\n",
                index,
                quote_string(&symbol.name),
                format_symbol_target(&symbol.target)
            ));
        }
    }
    if include_machine && !program.relocations.is_empty() {
        out.push_str("\nrelocations:\n");
        for (index, relocation) in program.relocations.iter().enumerate() {
            out.push_str(&format!(
                "  r{} = {} symbol={} addend={} target={}\n",
                index,
                relocation.kind.as_str(),
                quote_string(&relocation.symbol),
                relocation.addend,
                format_relocation_target(&relocation.target)
            ));
        }
    }
    if !program.types.is_empty() {
        out.push_str("\ntypes:\n");
        for (index, layout) in program.types.iter().enumerate() {
            if layout.is_enum {
                out.push_str(&format!(
                    "  t{} = enum {}",
                    index,
                    quote_string(&layout.name)
                ));
                for (variant, item) in layout.variants.iter().enumerate() {
                    out.push_str(&format!(
                        " v{}={} payload={}",
                        variant,
                        quote_string(&item.name),
                        item.payload_count
                    ));
                }
                out.push('\n');
            } else {
                out.push_str(&format!(
                    "  t{} = struct {}",
                    index,
                    quote_string(&layout.name)
                ));
                for (field, name) in layout.field_names.iter().enumerate() {
                    out.push_str(&format!(" field{}={}", field, quote_string(name)));
                }
                out.push('\n');
            }
        }
    }
    if !program.native_imports.is_empty() {
        out.push_str("\nnative_imports:\n");
        for (index, import) in program.native_imports.iter().enumerate() {
            out.push_str(&format!(
                "  i{} = {} abi={}\n",
                index,
                quote_string(&import.name),
                import.abi
            ));
        }
    }
    if !program.modules.is_empty() {
        out.push_str("\nmodules:\n");
        for (index, module) in program.modules.iter().enumerate() {
            out.push_str(&format!(
                "  m{} = f{}\n",
                index,
                module.init.0.saturating_sub(1)
            ));
        }
    }
    let entry = &program.functions[program.entry.0 as usize];
    if include_machine {
        out.push_str(&format!(
            "\nmain registers={} frame_size={} machine_params={} machine_return={}:\n",
            entry.registers,
            entry.machine_frame_size,
            format_machine_scalar_list(&entry.machine_params),
            format_machine_scalar_return(entry.machine_return)
        ));
    } else {
        out.push_str(&format!("\nmain registers={}:\n", entry.registers));
    }
    for instruction in &entry.instructions {
        if matches!(instruction, Instruction::BlockStart { .. }) {
            out.push_str(&format_instruction(instruction));
        } else {
            out.push_str("  ");
            out.push_str(&format_instruction(instruction));
            out.push('\n');
        }
    }
    let mut function_index = 0usize;
    for (position, function) in program.functions.iter().enumerate() {
        if position as u32 == program.entry.0 {
            continue;
        }
        if include_machine {
            out.push_str(&format!(
                "\nfunction f{} name={} arity={} registers={} frame_size={} machine_params={} machine_return={}:\n",
                function_index,
                quote_string(&function.name),
                function.arity,
                function.registers,
                function.machine_frame_size,
                format_machine_scalar_list(&function.machine_params),
                format_machine_scalar_return(function.machine_return)
            ));
        } else {
            out.push_str(&format!(
                "\nfunction f{} name={} arity={} registers={}:\n",
                function_index,
                quote_string(&function.name),
                function.arity,
                function.registers
            ));
        }
        for (index, param) in function.params.iter().enumerate() {
            out.push_str(&format!("  param {} = {}\n", index, quote_string(param)));
        }
        for (index, upvalue) in function.upvalues.iter().enumerate() {
            let source = match upvalue.source {
                UpvalueSource::Local(local) => format!("local l{}", local.0),
                UpvalueSource::Upvalue(upvalue) => format!("upvalue u{}", upvalue.0),
                UpvalueSource::Global(global) => format!("global g{}", global.0),
            };
            out.push_str(&format!("  upvalue u{} = {}\n", index, source));
        }
        for instruction in &function.instructions {
            if matches!(instruction, Instruction::BlockStart { .. }) {
                out.push_str(&format_instruction(instruction));
            } else {
                out.push_str("  ");
                out.push_str(&format_instruction(instruction));
                out.push('\n');
            }
        }
        function_index += 1;
    }

    if !program.debug_sources.is_empty() {
        out.push_str("\ndebug_sources:\n");
        for (index, source) in program.debug_sources.iter().enumerate() {
            out.push_str(&format!("  s{} ", index));
            if let Some(module) = &source.module {
                out.push_str(&format!("module={} ", quote_string(module)));
            }
            out.push_str(&format!(
                "path={} text={}\n",
                quote_string(&source.path),
                quote_string(&source.text)
            ));
        }
    }

    let has_debug_locations = program
        .functions
        .iter()
        .any(|function| function.locations.iter().any(Option::is_some));
    if has_debug_locations {
        out.push_str("\ndebug_locations:\n");
        let entry = &program.functions[program.entry.0 as usize];
        for (index, location) in entry.locations.iter().enumerate() {
            if let Some(location) = location {
                out.push_str(&format_debug_location("main", index, location));
            }
        }
        let mut function_index = 0usize;
        for (position, function) in program.functions.iter().enumerate() {
            if position as u32 == program.entry.0 {
                continue;
            }
            for (instruction, location) in function.locations.iter().enumerate() {
                if let Some(location) = location {
                    out.push_str(&format_debug_location(
                        &format!("function f{}", function_index),
                        instruction,
                        location,
                    ));
                }
            }
            function_index += 1;
        }
    }

    let has_debug_ranges = program.functions.iter().any(|function| {
        function.locations.iter().any(|location| {
            location
                .as_ref()
                .and_then(|location| location.range.as_ref())
                .is_some()
        })
    });
    if has_debug_ranges {
        out.push_str("\ndebug_ranges:\n");
        let entry = &program.functions[program.entry.0 as usize];
        for (index, location) in entry.locations.iter().enumerate() {
            if let Some(range) = location
                .as_ref()
                .and_then(|location| location.range.as_ref())
            {
                out.push_str(&format_debug_range("main", index, range));
            }
        }
        let mut function_index = 0usize;
        for (position, function) in program.functions.iter().enumerate() {
            if position as u32 == program.entry.0 {
                continue;
            }
            for (instruction, location) in function.locations.iter().enumerate() {
                if let Some(range) = location
                    .as_ref()
                    .and_then(|location| location.range.as_ref())
                {
                    out.push_str(&format_debug_range(
                        &format!("function f{}", function_index),
                        instruction,
                        range,
                    ));
                }
            }
            function_index += 1;
        }
    }
}

fn format_debug_location(section: &str, instruction: usize, location: &DebugLocation) -> String {
    format!(
        "  {} {} = s{}:{}:{}\n",
        section, instruction, location.source, location.line, location.column
    )
}

fn format_debug_range(section: &str, instruction: usize, range: &DebugRange) -> String {
    format!(
        "  {} {} = s{}:{}:{}\n",
        section, instruction, range.source, range.start, range.end
    )
}

pub fn format_program(program: &Program) -> String {
    format_program_checked(program)
        .expect("cdbc 0.2 formatter received an unsupported machine instruction")
}

/// Format a program only when it is representable by the cdbc 0.2 writer.
pub fn format_program_checked(program: &Program) -> Result<String, FormatError> {
    reject_unsupported_machine_instructions(program)?;
    let mut out = String::with_capacity(format_program_capacity_hint(program));
    out.push_str(ARTIFACT_HEADER);
    out.push_str("\n\n");
    format_program_sections(&mut out, program, false);
    Ok(out)
}

pub fn format_artifact(artifact: &Artifact) -> String {
    format_artifact_checked(artifact)
        .expect("cdbc 0.2 formatter received an unsupported machine instruction")
}

/// Format an artifact only when it is representable by the cdbc 0.2 writer.
pub fn format_artifact_checked(artifact: &Artifact) -> Result<String, FormatError> {
    match artifact {
        Artifact::Program(program) => format_program_checked(program),
        Artifact::Module(module) => {
            reject_unsupported_machine_instructions(&module.program)?;
            Ok(format_module_artifact(module, ARTIFACT_HEADER, false))
        }
    }
}

/// Format an artifact for the VM's human-readable `dump` command. Legacy
/// artifacts retain the cdbc 0.2 text, while machine-aware artifacts use the
/// cdbc 0.3 sections instead of being rejected by the legacy writer.
pub fn format_artifact_for_dump(artifact: &Artifact) -> Result<String, FormatError> {
    match format_artifact_checked(artifact) {
        Ok(output) => Ok(output),
        Err(_) => format_artifact_v03_checked(artifact),
    }
}

/// Format a program using the machine-aware cdbc 0.3 writer.
pub fn format_program_v03(program: &Program) -> String {
    format_program_v03_checked(program).expect("cdbc 0.3 formatter failed")
}

/// Format a program using the machine-aware cdbc 0.3 writer.
pub fn format_program_v03_checked(program: &Program) -> Result<String, FormatError> {
    let mut out = String::with_capacity(format_program_capacity_hint(program));
    out.push_str(MACHINE_ARTIFACT_HEADER);
    out.push_str("\n\n");
    format_program_sections(&mut out, program, true);
    Ok(out)
}

/// Format a linked or module artifact using the machine-aware cdbc 0.3 writer.
pub fn format_artifact_v03(artifact: &Artifact) -> String {
    format_artifact_v03_checked(artifact).expect("cdbc 0.3 formatter failed")
}

/// Format a linked or module artifact using the machine-aware cdbc 0.3 writer.
pub fn format_artifact_v03_checked(artifact: &Artifact) -> Result<String, FormatError> {
    match artifact {
        Artifact::Program(program) => format_program_v03_checked(program),
        Artifact::Module(module) => Ok(format_module_artifact(
            module,
            MACHINE_ARTIFACT_HEADER,
            true,
        )),
    }
}

fn format_module_artifact(module: &ModuleArtifact, header: &str, machine: bool) -> String {
    let mut out = String::with_capacity(
        format_program_capacity_hint(&module.program)
            .saturating_add(module.identity.len())
            .saturating_add(module.path.len())
            .saturating_add(module.canonical_path.len())
            .saturating_add(
                module
                    .dependencies
                    .iter()
                    .map(|dependency| dependency.identity.len() + dependency.requested_path.len())
                    .sum(),
            ),
    );
    out.push_str(header);
    out.push_str("\n\nartifact: module\n\nmodule:\n");
    out.push_str(&format!(
        "  identity = {}\n",
        quote_string(&module.identity)
    ));
    out.push_str(&format!("  path = {}\n", quote_string(&module.path)));
    out.push_str(&format!(
        "  canonical_path = {}\n",
        quote_string(&module.canonical_path)
    ));
    out.push_str(&format!(
        "  entry = {}\n",
        if module.is_entry { "true" } else { "false" }
    ));
    if let Some(entry_order) = module.entry_order {
        out.push_str(&format!("  entry_order = {}\n", entry_order));
    }
    out.push_str(&format!("  init = f{}\n", module.init.saturating_sub(1)));
    out.push_str("  dependencies:\n");
    for (index, dependency) in module.dependencies.iter().enumerate() {
        let kind = match dependency.kind {
            ModuleDependencyKind::Import => "import",
            ModuleDependencyKind::ReExport => "re_export",
        };
        out.push_str(&format!(
            "    d{} target={} kind={} requested={}\n",
            index,
            quote_string(&dependency.identity),
            kind,
            quote_string(&dependency.requested_path)
        ));
    }
    out.push('\n');
    format_program_sections(&mut out, &module.program, machine);
    out
}
