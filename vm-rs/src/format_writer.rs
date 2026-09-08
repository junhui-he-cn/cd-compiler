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
