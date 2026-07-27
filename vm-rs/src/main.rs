mod bytecode;
mod format;
mod link;
mod runtime;
mod value;
mod vm;

use crate::bytecode::{DebugLocation, Program};
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
Current phase: .cdbc parsing, canonical dump, bytecode execution, and source tracing are implemented.\n";

fn help_text() -> &'static str {
    HELP
}

fn read_artifact(path: impl AsRef<Path>) -> Result<format::Artifact, String> {
    let path = path.as_ref();
    let source = fs::read_to_string(path)
        .map_err(|error| format!("error: failed to read `{}`: {}", path.display(), error))?;
    format::parse_artifact(&source).map_err(|error| format!("error: {}", error))
}

fn read_program(path: impl AsRef<Path>) -> Result<Program, String> {
    match read_artifact(path)? {
        format::Artifact::Program(program) => Ok(program),
        format::Artifact::Module(_) => {
            Err("error: cannot run an unlinked module artifact".to_string())
        }
    }
}

fn dump(path: &str) -> Result<(), String> {
    let artifact = read_artifact(path)?;
    print!("{}", format::format_artifact(&artifact));
    Ok(())
}

fn run(path: &str) -> Result<(), String> {
    let program = read_program(path)?;
    let output = vm::VM::new(&program)
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

fn trace(path: &str) -> Result<(), String> {
    let program = read_program(path)?;
    let traced = vm::VM::new(&program).trace();
    for event in &traced.events {
        println!("{}", format_trace_event(&program, event));
    }
    traced.result.map(|_| ()).map_err(|error| error.to_string())
}

fn link(directory: &str, output_path: &str) -> Result<(), String> {
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

    let mut modules = Vec::new();
    for path in paths {
        let artifact = read_artifact(&path)?;
        match artifact {
            format::Artifact::Module(module) => modules.push(module),
            format::Artifact::Program(_) => {
                return Err(format!(
                    "error: `{}` is a linked program, expected a module artifact",
                    path.display()
                ))
            }
        }
    }

    let program = link::link_modules(modules).map_err(|error| format!("error: {}", error))?;
    fs::write(output_path, format::format_program(&program))
        .map_err(|error| format!("error: failed to write `{}`: {}", output_path, error))?;
    Ok(())
}

fn main() {
    let mut args = env::args().skip(1);
    match args.next().as_deref() {
        None | Some("-h") | Some("--help") => {
            print!("{}", help_text());
        }
        Some("dump") => {
            let Some(path) = args.next() else {
                eprintln!("error: dump expects <program.cdbc>");
                eprintln!();
                eprint!("{}", help_text());
                process::exit(64);
            };
            if args.next().is_some() {
                eprintln!("error: dump expects exactly one input file");
                eprintln!();
                eprint!("{}", help_text());
                process::exit(64);
            }
            if let Err(error) = dump(&path) {
                eprintln!("{}", error);
                process::exit(1);
            }
        }
        Some("run") => {
            let Some(path) = args.next() else {
                eprintln!("error: run expects <program.cdbc>");
                eprintln!();
                eprint!("{}", help_text());
                process::exit(64);
            };
            if args.next().is_some() {
                eprintln!("error: run expects exactly one input file");
                eprintln!();
                eprint!("{}", help_text());
                process::exit(64);
            }
            if let Err(error) = run(&path) {
                eprintln!("{}", error);
                process::exit(1);
            }
        }
        Some("trace") => {
            let Some(path) = args.next() else {
                eprintln!("error: trace expects <program.cdbc>");
                eprintln!();
                eprint!("{}", help_text());
                process::exit(64);
            };
            if args.next().is_some() {
                eprintln!("error: trace expects exactly one input file");
                eprintln!();
                eprint!("{}", help_text());
                process::exit(64);
            }
            if let Err(error) = trace(&path) {
                eprintln!("{}", error);
                process::exit(1);
            }
        }
        Some("link") => {
            let Some(directory) = args.next() else {
                eprintln!("error: link expects <module-directory> <output.cdbc>");
                eprintln!();
                eprint!("{}", help_text());
                process::exit(64);
            };
            let Some(output) = args.next() else {
                eprintln!("error: link expects <module-directory> <output.cdbc>");
                eprintln!();
                eprint!("{}", help_text());
                process::exit(64);
            };
            if args.next().is_some() {
                eprintln!("error: link expects exactly two input paths");
                eprintln!();
                eprint!("{}", help_text());
                process::exit(64);
            }
            if let Err(error) = link(&directory, &output) {
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
    use super::help_text;

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
    }
}
