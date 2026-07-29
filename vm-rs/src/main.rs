mod bytecode;
mod format;
mod link;
mod runtime;
mod value;
mod vm;

use crate::bytecode::{DebugLocation, Program};
use crate::vm::RunConfig;
use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process;

const HELP: &str = "compiler-design-vm 0.1.0\n\n\
Usage:\n\
  compiler-design-vm --help\n\
  compiler-design-vm dump <program.cdbc>\n\
  compiler-design-vm run <program.cdbc>\n\
  compiler-design-vm trace <program.cdbc>\n\
  compiler-design-vm link <module-directory> <output.cdbc>\n\n\
Current phase: .cdbc parsing, canonical dump, bytecode execution, and source tracing are implemented.\nResource options: --max-steps N, --max-call-depth N, --max-elements N, --max-output-bytes N, --max-artifact-bytes N, --max-modules N, --max-module-instructions N, --unlimited (0 disables an individual limit).\n";

fn help_text() -> &'static str {
    HELP
}

fn resource_limit_error(kind: &str, limit: usize) -> String {
    format!("error: resource limit exceeded: {} (limit {})", kind, limit)
}

fn enforce_size(path: &Path, config: &RunConfig) -> Result<(), String> {
    let Some(limit) = config.max_artifact_bytes else {
        return Ok(());
    };
    let size = fs::metadata(path)
        .map_err(|error| format!("error: failed to read `{}`: {}", path.display(), error))?
        .len();
    if size > limit as u64 {
        return Err(resource_limit_error("artifact bytes", limit));
    }
    Ok(())
}

fn read_artifact(path: impl AsRef<Path>, config: &RunConfig) -> Result<format::Artifact, String> {
    let path = path.as_ref();
    enforce_size(path, config)?;
    let source = fs::read_to_string(path)
        .map_err(|error| format!("error: failed to read `{}`: {}", path.display(), error))?;
    if let Some(limit) = config.max_artifact_bytes {
        if source.as_bytes().len() > limit {
            return Err(resource_limit_error("artifact bytes", limit));
        }
    }
    let artifact = format::parse_artifact(&source).map_err(|error| format!("error: {}", error))?;
    format::verify_artifact(&artifact).map_err(|error| format!("error: {}", error))?;
    Ok(artifact)
}

fn read_program(path: impl AsRef<Path>, config: &RunConfig) -> Result<Program, String> {
    match read_artifact(path, config)? {
        format::Artifact::Program(program) => Ok(program),
        format::Artifact::Module(_) => {
            Err("error: cannot run an unlinked module artifact".to_string())
        }
    }
}

fn dump(path: &str, config: &RunConfig) -> Result<(), String> {
    let artifact = read_artifact(path, config)?;
    print!("{}", format::format_artifact(&artifact));
    Ok(())
}

fn run(path: &str, config: &RunConfig) -> Result<(), String> {
    let program = read_program(path, config)?;
    let output = vm::VM::with_config(&program, config.clone())
        .run()
        .map_err(|error| error.to_string())?;
    print!("{}", output);
    Ok(())
}

fn trace_location(program: &Program, location: Option<&DebugLocation>) -> String {
    let Some(location) = location else {
        return "<unknown>".to_string();
    };
    let Some(source) = program.debug_sources.get(location.source) else {
        return format!(
            "<invalid-source:{}:{}:{}>",
            location.source, location.line, location.column
        );
    };
    format!("{}:{}:{}", source.path, location.line, location.column)
}

fn trace_quote(text: &str) -> String {
    let mut quoted = String::from("\"");
    for character in text.chars() {
        match character {
            '\\' => quoted.push_str("\\\\"),
            '"' => quoted.push_str("\\\""),
            '\n' => quoted.push_str("\\n"),
            '\r' => quoted.push_str("\\r"),
            '\t' => quoted.push_str("\\t"),
            character => quoted.push(character),
        }
    }
    quoted.push('"');
    quoted
}

fn source_local_name(name: &str) -> String {
    let Some((base, suffix)) = name.rsplit_once('#') else {
        return name.to_string();
    };
    if !base.is_empty() && !suffix.is_empty() && suffix.chars().all(|character| character.is_ascii_digit()) {
        base.to_string()
    } else {
        name.to_string()
    }
}

fn format_trace_event(program: &Program, event: &vm::TraceEvent) -> String {
    let location = trace_location(program, event.location.as_ref());
    let stack = event
        .stack
        .iter()
        .map(|frame| {
            format!(
                "{}@{}",
                frame.function,
                trace_location(program, frame.location.as_ref())
            )
        })
        .collect::<Vec<_>>()
        .join(">");
    let display_names = event
        .locals
        .iter()
        .map(|(name, _)| source_local_name(name))
        .collect::<Vec<_>>();
    let mut name_counts = BTreeMap::new();
    for name in &display_names {
        *name_counts.entry(name.clone()).or_insert(0usize) += 1;
    }
    let locals = event
        .locals
        .iter()
        .zip(display_names)
        .map(|((raw_name, value), display_name)| {
            let name = if name_counts.get(&display_name).copied().unwrap_or(0) > 1 {
                raw_name
            } else {
                &display_name
            };
            format!("{}={}", name, trace_quote(value))
        })
        .collect::<Vec<_>>()
        .join(",");
    let mut output = format!(
        "trace {} kind={} function={} location={} stack={} locals={{{}}}",
        event.sequence,
        event.kind.as_str(),
        event.function,
        location,
        stack,
        locals,
    );
    if let Some(instruction) = event.instruction {
        output.push_str(&format!(" instruction={}", instruction));
    }
    if let Some(range) = event
        .location
        .as_ref()
        .and_then(|location| location.range.as_ref())
    {
        output.push_str(&format!(" range=s{}:{}:{}", range.source, range.start, range.end));
    }
    if let Some(value) = &event.value {
        output.push_str(&format!(" value={}", trace_quote(value)));
    }
    output
}

fn trace(path: &str, config: &RunConfig) -> Result<(), String> {
    let program = read_program(path, config)?;
    let traced = vm::VM::with_config(&program, config.clone()).trace();
    for event in &traced.events {
        println!("{}", format_trace_event(&program, event));
    }
    traced.result.map(|_| ()).map_err(|error| error.to_string())
}

fn program_instruction_count(program: &Program) -> usize {
    program
        .main
        .instructions
        .len()
        .saturating_add(program.functions.iter().fold(0usize, |total, function| {
            total.saturating_add(function.instructions.len())
        }))
}

fn enforce_limit(kind: &str, actual: usize, limit: Option<usize>) -> Result<(), String> {
    if let Some(limit) = limit {
        if actual > limit {
            return Err(resource_limit_error(kind, limit));
        }
    }
    Ok(())
}

fn link(directory: &str, output_path: &str, config: &RunConfig) -> Result<(), String> {
    let mut paths = fs::read_dir(directory)
        .map_err(|error| format!("error: failed to read module directory `{}`: {}", directory, error))?
        .map(|entry| entry.map(|entry| entry.path()))
        .collect::<Result<Vec<PathBuf>, _>>()
        .map_err(|error| format!("error: failed to read module directory `{}`: {}", directory, error))?;
    paths.retain(|path| path.extension().and_then(|extension| extension.to_str()) == Some("cdbc"));
    paths.sort();
    if paths.is_empty() {
        return Err("error: module directory contains no .cdbc products".to_string());
    }
    enforce_limit("module count", paths.len(), config.max_module_count)?;

    let mut modules = Vec::new();
    let mut module_instructions = 0usize;
    for path in paths {
        let artifact = read_artifact(&path, config)?;
        match artifact {
            format::Artifact::Module(module) => {
                module_instructions = module_instructions.saturating_add(
                    program_instruction_count(&module.program),
                );
                enforce_limit(
                    "module instructions",
                    module_instructions,
                    config.max_module_instructions,
                )?;
                modules.push(module);
            }
            format::Artifact::Program(_) => {
                return Err(format!(
                    "error: `{}` is a linked program, expected a module artifact",
                    path.display()
                ))
            }
        }
    }

    let program = link::link_modules(modules).map_err(|error| format!("error: {}", error))?;
    enforce_limit(
        "module instructions",
        program_instruction_count(&program),
        config.max_module_instructions,
    )?;
    let output = format::format_program(&program);
    if let Some(limit) = config.max_artifact_bytes {
        if output.as_bytes().len() > limit {
            return Err(resource_limit_error("artifact bytes", limit));
        }
    }
    fs::write(output_path, output)
        .map_err(|error| format!("error: failed to write `{}`: {}", output_path, error))?;
    Ok(())
}

fn parse_limit_value(option: &str, value: &str) -> Result<Option<usize>, String> {
    let parsed = value.parse::<usize>().map_err(|_| {
        format!(
            "error: option `{}` expects a non-negative integer, got `{}`",
            option, value
        )
    })?;
    Ok((parsed != 0).then_some(parsed))
}

fn set_limit(config: &mut RunConfig, option: &str, value: Option<usize>) -> Result<(), String> {
    match option {
        "--max-steps" => config.max_instruction_steps = value,
        "--max-call-depth" => config.max_call_depth = value,
        "--max-elements" => config.max_runtime_elements = value,
        "--max-output-bytes" => config.max_output_bytes = value,
        "--max-artifact-bytes" => config.max_artifact_bytes = value,
        "--max-modules" => config.max_module_count = value,
        "--max-module-instructions" => config.max_module_instructions = value,
        _ => return Err(format!("error: unknown option `{}`", option)),
    }
    Ok(())
}

fn parse_command_args(command: &str, args: Vec<String>) -> Result<(Vec<String>, RunConfig), String> {
    let mut positionals = Vec::new();
    let mut config = RunConfig::default();
    let mut index = 0;
    while index < args.len() {
        let argument = &args[index];
        if argument == "--unlimited" {
            config = RunConfig::unlimited();
            index += 1;
            continue;
        }
        if argument.starts_with("--") {
            if !matches!(
                argument.as_str(),
                "--max-steps"
                    | "--max-call-depth"
                    | "--max-elements"
                    | "--max-output-bytes"
                    | "--max-artifact-bytes"
                    | "--max-modules"
                    | "--max-module-instructions"
            ) {
                return Err(format!("error: unknown option `{}`", argument));
            }
            let value = args.get(index + 1).ok_or_else(|| {
                format!("error: option `{}` expects a value", argument)
            })?;
            let parsed = parse_limit_value(argument, value)?;
            set_limit(&mut config, argument, parsed)?;
            index += 2;
            continue;
        }
        positionals.push(argument.clone());
        index += 1;
    }
    if positionals.is_empty() && command.is_empty() {
        return Err("error: missing command arguments".to_string());
    }
    Ok((positionals, config))
}

fn parse_single_path(command: &str, args: Vec<String>) -> Result<(String, RunConfig), String> {
    let (positionals, config) = parse_command_args(command, args)?;
    let Some(path) = positionals.first() else {
        return Err(format!("error: {} expects <program.cdbc>", command));
    };
    if positionals.len() != 1 {
        return Err(format!("error: {} expects exactly one input file", command));
    }
    Ok((path.clone(), config))
}

fn parse_link_paths(args: Vec<String>) -> Result<((String, String), RunConfig), String> {
    let (positionals, config) = parse_command_args("link", args)?;
    if positionals.len() < 2 {
        return Err("error: link expects <module-directory> <output.cdbc>".to_string());
    }
    if positionals.len() != 2 {
        return Err("error: link expects exactly two input paths".to_string());
    }
    Ok(((positionals[0].clone(), positionals[1].clone()), config))
}

fn usage_error(message: String) -> ! {
    eprintln!("{}", message);
    eprintln!();
    eprint!("{}", help_text());
    process::exit(64);
}

fn main() {
    let mut args = env::args().skip(1);
    let command = args.next();
    let remaining = args.collect::<Vec<_>>();
    match command.as_deref() {
        None | Some("-h") | Some("--help") => {
            print!("{}", help_text());
        }
        Some("dump") => {
            let (path, config) =
                parse_single_path("dump", remaining).unwrap_or_else(|error| usage_error(error));
            if let Err(error) = dump(&path, &config) {
                eprintln!("{}", error);
                process::exit(1);
            }
        }
        Some("run") => {
            let (path, config) =
                parse_single_path("run", remaining).unwrap_or_else(|error| usage_error(error));
            if let Err(error) = run(&path, &config) {
                eprintln!("{}", error);
                process::exit(1);
            }
        }
        Some("trace") => {
            let (path, config) =
                parse_single_path("trace", remaining).unwrap_or_else(|error| usage_error(error));
            if let Err(error) = trace(&path, &config) {
                eprintln!("{}", error);
                process::exit(1);
            }
        }
        Some("link") => {
            let ((directory, output), config) =
                parse_link_paths(remaining).unwrap_or_else(|error| usage_error(error));
            if let Err(error) = link(&directory, &output, &config) {
                eprintln!("{}", error);
                process::exit(1);
            }
        }
        Some(command) => {
            eprintln!("error: unknown command `{}`", command);
            eprintln!();
            eprint!("{}", help_text());
            process::exit(64);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{help_text, parse_single_path};

    #[test]
    fn help_mentions_dump_and_run_scope() {
        let help = help_text();
        assert!(help.contains("compiler-design-vm dump <program.cdbc>"));
        assert!(help.contains("compiler-design-vm run <program.cdbc>"));
        assert!(help.contains("compiler-design-vm trace <program.cdbc>"));
        assert!(help.contains("compiler-design-vm link <module-directory> <output.cdbc>"));
        assert!(
            help.contains(
                ".cdbc parsing, canonical dump, bytecode execution, and source tracing are implemented",
            )
        );
        assert!(help.contains("--max-steps N"));
        assert!(help.contains("--unlimited"));
    }

    #[test]
    fn parses_resource_overrides_without_changing_positional_paths() {
        let (path, config) = parse_single_path(
            "run",
            vec![
                "program.cdbc".to_string(),
                "--max-steps".to_string(),
                "7".to_string(),
                "--max-output-bytes".to_string(),
                "0".to_string(),
            ],
        )
        .expect("resource options should parse");
        assert_eq!(path, "program.cdbc");
        assert_eq!(config.max_instruction_steps, Some(7));
        assert_eq!(config.max_output_bytes, None);
    }

    #[test]
    fn unlimited_flag_disables_all_resource_limits() {
        let (_, config) = parse_single_path(
            "trace",
            vec!["--max-steps".to_string(), "2".to_string(), "--unlimited".to_string(), "trace.cdbc".to_string()],
        )
        .expect("unlimited should parse");
        assert!(config.max_instruction_steps.is_none());
        assert!(config.max_call_depth.is_none());
        assert!(config.max_runtime_elements.is_none());
        assert!(config.max_output_bytes.is_none());
        assert!(config.max_artifact_bytes.is_none());
        assert!(config.max_module_count.is_none());
        assert!(config.max_module_instructions.is_none());
    }
}
