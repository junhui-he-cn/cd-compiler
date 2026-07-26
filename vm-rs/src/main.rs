mod bytecode;
mod format;
mod link;
mod runtime;
mod value;
mod vm;

use std::env;
use std::fs;
use std::path::PathBuf;
use std::process;

const HELP: &str = "compiler-design-vm 0.1.0\n\n\
Usage:\n\
  compiler-design-vm --help\n\
  compiler-design-vm dump <program.cdbc>\n\
  compiler-design-vm run <program.cdbc>\n\
  compiler-design-vm link <module-directory> <output.cdbc>\n\n\
Current phase: .cdbc parsing, canonical dump, and bytecode execution are implemented.\n";

fn help_text() -> &'static str {
    HELP
}

fn dump(path: &str) -> Result<(), String> {
    let source = fs::read_to_string(path)
        .map_err(|error| format!("error: failed to read `{}`: {}", path, error))?;
    let artifact = format::parse_artifact(&source).map_err(|error| format!("error: {}", error))?;
    print!("{}", format::format_artifact(&artifact));
    Ok(())
}

fn run(path: &str) -> Result<(), String> {
    let source = fs::read_to_string(path)
        .map_err(|error| format!("error: failed to read `{}`: {}", path, error))?;
    let artifact = format::parse_artifact(&source).map_err(|error| format!("error: {}", error))?;
    let format::Artifact::Program(program) = artifact else {
        return Err("error: cannot run an unlinked module artifact".to_string());
    };
    let output = vm::VM::new(&program)
        .run()
        .map_err(|error| error.to_string())?;
    print!("{}", output);
    Ok(())
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
        let source = fs::read_to_string(&path)
            .map_err(|error| format!("error: failed to read `{}`: {}", path.display(), error))?;
        let artifact = format::parse_artifact(&source)
            .map_err(|error| format!("error: {}", error))?;
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
        assert!(help.contains("compiler-design-vm link <module-directory> <output.cdbc>"));
        assert!(
            help.contains(".cdbc parsing, canonical dump, and bytecode execution are implemented")
        );
    }
}
