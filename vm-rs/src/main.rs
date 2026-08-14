use compiler_design_vm::bytecode::{DebugLocation, DebugSource, Program};
use compiler_design_vm::format;
use compiler_design_vm::link;
use compiler_design_vm::vm::{self, RunConfig};
use compiler_design_vm::{DebugControl, DebugHook, DebugPause};
use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process;

const HELP: &str = "compiler-design-vm 0.2.0\n\n\
Usage:\n\
  compiler-design-vm --help\n\
  compiler-design-vm verify <program.cdbc>\n\
  compiler-design-vm dump <program.cdbc>\n\
  compiler-design-vm run <program.cdbc>\n\
  compiler-design-vm trace <program.cdbc>\n\
  compiler-design-vm debug <program.cdbc>\n\
  compiler-design-vm profile <program.cdbc>\n\
  compiler-design-vm link <module-directory> <output.cdbc>\n\n\
Current phase: .cdbc parsing, artifact verification, canonical dump, bytecode execution, source tracing, interactive debugging, and deterministic execution profiling are implemented.\nResource options: --max-steps N, --max-call-depth N, --max-elements N, --max-output-bytes N, --max-artifact-bytes N, --max-modules N, --max-module-instructions N, --unlimited (0 disables an individual limit).\n";

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

fn read_artifact_unverified(
    path: impl AsRef<Path>,
    config: &RunConfig,
) -> Result<format::Artifact, String> {
    let path = path.as_ref();
    enforce_size(path, config)?;
    let source = fs::read_to_string(path)
        .map_err(|error| format!("error: failed to read `{}`: {}", path.display(), error))?;
    if let Some(limit) = config.max_artifact_bytes {
        if source.as_bytes().len() > limit {
            return Err(resource_limit_error("artifact bytes", limit));
        }
    }
    format::parse_artifact(&source).map_err(|error| format!("error: {}", error))
}

fn read_artifact(path: impl AsRef<Path>, config: &RunConfig) -> Result<format::Artifact, String> {
    let artifact = read_artifact_unverified(path, config)?;
    format::verify_artifact(&artifact).map_err(|error| format!("error: {}", error))?;
    Ok(artifact)
}

fn read_program(path: impl AsRef<Path>, config: &RunConfig) -> Result<Program, String> {
    match read_artifact_unverified(path, config)? {
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

fn verify(path: &str, config: &RunConfig) -> Result<(), String> {
    read_artifact(path, config)?;
    Ok(())
}

fn run(path: &str, config: &RunConfig) -> Result<(), String> {
    let program = read_program(path, config)?;
    let output = vm::VM::with_config_verified(&program, config.clone())
        .map_err(|error| format!("error: {}", error))?
        .run()
        .map_err(|error| error.to_string())?;
    print!("{}", output);
    Ok(())
}

fn trace_location(program: &Program, location: Option<&DebugLocation>) -> String {
    source_location(&program.debug_sources, location)
}

fn source_location(sources: &[DebugSource], location: Option<&DebugLocation>) -> String {
    let Some(location) = location else {
        return "<unknown>".to_string();
    };
    let Some(source) = sources.get(location.source) else {
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

fn normalize_debug_path(path: &str) -> String {
    Path::new(path)
        .canonicalize()
        .map(|path| path.to_string_lossy().into_owned())
        .unwrap_or_else(|_| path.to_string())
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum Breakpoint {
    Line {
        id: usize,
        path: String,
        line: usize,
    },
    Range {
        id: usize,
        path: String,
        start: usize,
        end: usize,
    },
}

impl Breakpoint {
    fn id(&self) -> usize {
        match self {
            Self::Line { id, .. } | Self::Range { id, .. } => *id,
        }
    }

    fn matches(&self, sources: &[DebugSource], pause: &DebugPause) -> bool {
        let Some(location) = pause.location.as_ref() else {
            return false;
        };
        let Some(source) = sources.get(location.source) else {
            return false;
        };
        let source_path = normalize_debug_path(&source.path);
        match self {
            Self::Line { path, line, .. } => source_path == *path && location.line == *line,
            Self::Range {
                path, start, end, ..
            } => {
                let Some(range) = location.range.as_ref() else {
                    return false;
                };
                source_path == *path
                    && range.start < *end
                    && *start < range.end
                    && range.source == location.source
            }
        }
    }

    fn description(&self) -> String {
        match self {
            Self::Line { path, line, .. } => format!("{}:{}", path, line),
            Self::Range {
                path, start, end, ..
            } => format!("{}:{}-{}", path, start, end),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct DebugKey {
    function: String,
    instruction: usize,
    depth: usize,
}

impl DebugKey {
    fn from_pause(pause: &DebugPause) -> Self {
        Self {
            function: pause.function.clone(),
            instruction: pause.instruction,
            depth: pause.stack.len(),
        }
    }
}

enum ResumeMode {
    Entry,
    Continue,
    Step(DebugKey),
    Next { key: DebugKey, depth: usize },
}

struct InteractiveDebugger {
    sources: Vec<DebugSource>,
    input: io::Stdin,
    breakpoints: Vec<Breakpoint>,
    next_breakpoint_id: usize,
    mode: ResumeMode,
    suppressed_breakpoints: Vec<usize>,
    paused_breakpoints: Vec<usize>,
}

impl InteractiveDebugger {
    fn new(sources: Vec<DebugSource>) -> Self {
        Self {
            sources,
            input: io::stdin(),
            breakpoints: Vec::new(),
            next_breakpoint_id: 1,
            mode: ResumeMode::Entry,
            suppressed_breakpoints: Vec::new(),
            paused_breakpoints: Vec::new(),
        }
    }

    fn pause_reason(&mut self, pause: &DebugPause) -> Option<&'static str> {
        let key = DebugKey::from_pause(pause);
        if matches!(self.mode, ResumeMode::Entry) {
            return Some("entry");
        }
        let mode_reason = match &self.mode {
            ResumeMode::Step(previous) if key != *previous => Some("step"),
            ResumeMode::Next { key: previous, depth }
                if key != *previous && pause.stack.len() <= *depth => Some("next"),
            _ => None,
        };
        if let Some(reason) = mode_reason {
            return Some(reason);
        }
        if !self.suppressed_breakpoints.is_empty() {
            let still_suppressed = self.breakpoints.iter().any(|breakpoint| {
                self.suppressed_breakpoints.contains(&breakpoint.id())
                    && breakpoint.matches(&self.sources, pause)
            });
            if still_suppressed {
                return None;
            }
            self.suppressed_breakpoints.clear();
        }
        if self
            .breakpoints
            .iter()
            .any(|breakpoint| breakpoint.matches(&self.sources, pause))
        {
            return Some("breakpoint");
        }
        None
    }

    fn parse_line_breakpoint(spec: &str) -> Result<(String, usize), String> {
        let Some((path, line)) = spec.rsplit_once(':') else {
            return Err("break expects <path>:<line>".to_string());
        };
        if path.is_empty() {
            return Err("break path must not be empty".to_string());
        }
        let line = line
            .parse::<usize>()
            .map_err(|_| "break line must be a positive integer".to_string())?;
        if line == 0 {
            return Err("break line must be a positive integer".to_string());
        }
        Ok((path.to_string(), line))
    }

    fn parse_range_breakpoint(spec: &str) -> Result<(String, usize, usize), String> {
        let Some((path, range)) = spec.rsplit_once(':') else {
            return Err("break-range expects <path>:<start>-<end>".to_string());
        };
        let Some((start, end)) = range.split_once('-') else {
            return Err("break-range expects <path>:<start>-<end>".to_string());
        };
        if path.is_empty() {
            return Err("break-range path must not be empty".to_string());
        }
        let start = start
            .parse::<usize>()
            .map_err(|_| "break-range start must be a non-negative integer".to_string())?;
        let end = end
            .parse::<usize>()
            .map_err(|_| "break-range end must be a non-negative integer".to_string())?;
        if start >= end {
            return Err("break-range end must be greater than start".to_string());
        }
        Ok((path.to_string(), start, end))
    }

    fn add_line_breakpoint(&mut self, spec: &str) -> Result<(), String> {
        let (path, line) = Self::parse_line_breakpoint(spec)?;
        let breakpoint = Breakpoint::Line {
            id: self.next_breakpoint_id,
            path: normalize_debug_path(&path),
            line,
        };
        self.next_breakpoint_id += 1;
        println!(
            "debug breakpoint id={} spec={}",
            breakpoint.id(),
            breakpoint.description()
        );
        self.breakpoints.push(breakpoint);
        Ok(())
    }

    fn add_range_breakpoint(&mut self, spec: &str) -> Result<(), String> {
        let (path, start, end) = Self::parse_range_breakpoint(spec)?;
        let breakpoint = Breakpoint::Range {
            id: self.next_breakpoint_id,
            path: normalize_debug_path(&path),
            start,
            end,
        };
        self.next_breakpoint_id += 1;
        println!(
            "debug breakpoint id={} spec={}",
            breakpoint.id(),
            breakpoint.description()
        );
        self.breakpoints.push(breakpoint);
        Ok(())
    }

    fn print_help() {
        println!(
            "debug help: break <path>:<line> | break-range <path>:<start>-<end> | continue | step | next | delete <id> | quit"
        );
    }

    fn suppress_current_breakpoints(&mut self) {
        self.suppressed_breakpoints = self.paused_breakpoints.clone();
    }

    fn command(
        &mut self,
        command: &str,
        pause: &DebugPause,
    ) -> Option<DebugControl> {
        if command == "continue" || command == "c" {
            self.mode = ResumeMode::Continue;
            self.suppress_current_breakpoints();
            println!("debug resumed command=continue");
            return Some(DebugControl::Continue);
        }
        if command == "step" || command == "s" {
            self.mode = ResumeMode::Step(DebugKey::from_pause(pause));
            self.suppress_current_breakpoints();
            println!("debug resumed command=step");
            return Some(DebugControl::Continue);
        }
        if command == "next" || command == "n" {
            self.mode = ResumeMode::Next {
                key: DebugKey::from_pause(pause),
                depth: pause.stack.len(),
            };
            self.suppress_current_breakpoints();
            println!("debug resumed command=next");
            return Some(DebugControl::Continue);
        }
        if command == "quit" || command == "q" {
            println!("debug quit");
            return Some(DebugControl::Quit);
        }
        if command == "help" {
            Self::print_help();
            return None;
        }
        if let Some(spec) = command.strip_prefix("break-range ") {
            if let Err(error) = self.add_range_breakpoint(spec) {
                println!("debug error message={}", error);
            }
            return None;
        }
        if let Some(spec) = command.strip_prefix("break ") {
            if let Err(error) = self.add_line_breakpoint(spec) {
                println!("debug error message={}", error);
            }
            return None;
        }
        if let Some(id) = command.strip_prefix("delete ") {
            match id.parse::<usize>() {
                Ok(id) => {
                    let before = self.breakpoints.len();
                    self.breakpoints.retain(|breakpoint| breakpoint.id() != id);
                    if self.breakpoints.len() == before {
                        println!("debug error message=unknown breakpoint id {}", id);
                    } else {
                        println!("debug breakpoint-deleted id={}", id);
                    }
                }
                Err(_) => println!("debug error message=delete expects a breakpoint id"),
            }
            return None;
        }
        if !command.is_empty() {
            println!("debug error message=unknown command `{}`", command);
        }
        None
    }

    fn pause(&mut self, pause: &DebugPause, reason: &str) -> DebugControl {
        self.paused_breakpoints = self
            .breakpoints
            .iter()
            .filter(|breakpoint| breakpoint.matches(&self.sources, pause))
            .map(Breakpoint::id)
            .collect();
        println!("{}", format_debug_pause(&self.sources, pause, reason));
        let _ = io::stdout().flush();
        loop {
            let mut command = String::new();
            match self.input.read_line(&mut command) {
                Ok(0) => return DebugControl::Quit,
                Ok(_) => {
                    let command = command.trim();
                    if let Some(control) = self.command(command, pause) {
                        return control;
                    }
                    let _ = io::stdout().flush();
                }
                Err(error) => {
                    println!("debug error message=failed to read command: {}", error);
                    return DebugControl::Quit;
                }
            }
        }
    }
}

impl DebugHook for InteractiveDebugger {
    fn on_instruction(&mut self, pause: DebugPause) -> DebugControl {
        let Some(reason) = self.pause_reason(&pause) else {
            return DebugControl::Continue;
        };
        self.pause(&pause, reason)
    }

    fn on_error(&mut self, pause: DebugPause, _error: &vm::RuntimeError) -> DebugControl {
        self.pause(&pause, "error")
    }
}

fn format_debug_pause(sources: &[DebugSource], pause: &DebugPause, reason: &str) -> String {
    let (module, range) = pause
        .location
        .as_ref()
        .and_then(|location| sources.get(location.source).map(|source| (source, location)))
        .map(|(source, location)| {
            let range = location.range.as_ref().map(|range| {
                format!(" range=s{}:{}:{}", range.source, range.start, range.end)
            });
            (
                source.module.as_deref().unwrap_or("none"),
                range.unwrap_or_default(),
            )
        })
        .unwrap_or(("none", String::new()));
    let stack = pause
        .stack
        .iter()
        .map(|frame| {
            format!(
                "{}@{}",
                frame.function,
                source_location(sources, frame.location.as_ref())
            )
        })
        .collect::<Vec<_>>()
        .join(">");
    let display_names = pause
        .locals
        .iter()
        .map(|(name, _)| source_local_name(name))
        .collect::<Vec<_>>();
    let mut name_counts = BTreeMap::new();
    for name in &display_names {
        *name_counts.entry(name.clone()).or_insert(0usize) += 1;
    }
    let locals = pause
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
    format!(
        "pause reason={} function={} instruction={} module={} location={} stack={} locals={{{}}}{}",
        reason,
        pause.function,
        pause.instruction,
        module,
        source_location(sources, pause.location.as_ref()),
        stack,
        locals,
        range,
    )
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
    let traced = vm::VM::with_config_verified(&program, config.clone())
        .map_err(|error| format!("error: {}", error))?
        .trace();
    for event in &traced.events {
        println!("{}", format_trace_event(&program, event));
    }
    traced.result.map(|_| ()).map_err(|error| error.to_string())
}

fn format_profile_status(result: &Result<String, vm::RuntimeError>) -> String {
    match result {
        Ok(_) => "profile status=ok".to_string(),
        Err(error) => {
            let mut status = format!("profile status=error kind={}", error.kind.as_str());
            if let vm::RuntimeErrorKind::Resource(resource) = error.kind {
                status.push_str(&format!(" resource={}", trace_quote(resource.as_str())));
            }
            status
        }
    }
}

fn print_profile_report(program: &Program, profiled: &vm::ProfileRun) {
    println!("{}", format_profile_status(&profiled.result));
    println!(
        "profile instruction_count={} output_bytes={}",
        profiled.report.instruction_count, profiled.report.output_bytes
    );
    println!(
        concat!(
            "profile heap tracked_heap_allocations={} tracked_heap_peak_live={} ",
            "tracked_heap_estimated_live_bytes={} tracked_heap_estimated_peak_live_bytes={}"
        ),
        profiled.report.tracked_heap_allocations,
        profiled.report.tracked_heap_peak_live,
        profiled.report.tracked_heap_estimated_live_bytes,
        profiled.report.tracked_heap_estimated_peak_live_bytes
    );
    for function in &profiled.report.functions {
        let index = function
            .index
            .map(|index| format!("f{}", index))
            .unwrap_or_else(|| "main".to_string());
        println!(
            "profile function index={} name={} calls={} instructions={}",
            index,
            trace_quote(&function.name),
            function.calls,
            function.instructions
        );
    }
    for native in &profiled.report.natives {
        println!(
            "profile native name={} calls={}",
            trace_quote(&native.name),
            native.calls
        );
    }
    for source_range in &profiled.report.source_ranges {
        let source_index = source_range.range.source;
        let path = program
            .debug_sources
            .get(source_index)
            .map(|source| source.path.as_str())
            .unwrap_or("<invalid-source>");
        println!(
            "profile source_range source=s{} path={} start={} end={} hits={}",
            source_index,
            trace_quote(path),
            source_range.range.start,
            source_range.range.end,
            source_range.hits
        );
    }
}

fn profile(path: &str, config: &RunConfig) -> Result<(), String> {
    let program = read_program(path, config)?;
    let profiled = vm::VM::with_config_verified(&program, config.clone())
        .map_err(|error| format!("error: {}", error))?
        .profile();
    print_profile_report(&program, &profiled);
    profiled.result.map(|_| ()).map_err(|error| error.to_string())
}

fn debug(path: &str, config: &RunConfig) -> Result<(), String> {
    let program = read_program(path, config)?;
    let sources = program.debug_sources.clone();
    let session = vm::VM::with_config_verified(&program, config.clone())
        .map_err(|error| format!("error: {}", error))?
        .debug(Box::new(InteractiveDebugger::new(sources)));
    if session.quit {
        return Ok(());
    }
    session
        .result
        .map(|output| print!("{}", output))
        .map_err(|error| error.to_string())
}

fn program_instruction_count(program: &Program) -> usize {
    program
        .functions
        .iter()
        .fold(0usize, |total, function| {
            total.saturating_add(function.instructions.len())
        })
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
        Some("verify") => {
            let (path, config) =
                parse_single_path("verify", remaining).unwrap_or_else(|error| usage_error(error));
            if let Err(error) = verify(&path, &config) {
                eprintln!("{}", error);
                process::exit(1);
            }
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
        Some("debug") => {
            let (path, config) =
                parse_single_path("debug", remaining).unwrap_or_else(|error| usage_error(error));
            if let Err(error) = debug(&path, &config) {
                eprintln!("{}", error);
                process::exit(1);
            }
        }
        Some("profile") => {
            let (path, config) =
                parse_single_path("profile", remaining).unwrap_or_else(|error| usage_error(error));
            if let Err(error) = profile(&path, &config) {
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
        assert!(help.contains("compiler-design-vm verify <program.cdbc>"));
        assert!(help.contains("compiler-design-vm dump <program.cdbc>"));
        assert!(help.contains("compiler-design-vm run <program.cdbc>"));
        assert!(help.contains("compiler-design-vm trace <program.cdbc>"));
        assert!(help.contains("compiler-design-vm debug <program.cdbc>"));
        assert!(help.contains("compiler-design-vm profile <program.cdbc>"));
        assert!(help.contains("compiler-design-vm link <module-directory> <output.cdbc>"));
        assert!(
            help.contains(
                ".cdbc parsing, artifact verification, canonical dump, bytecode execution, source tracing, interactive debugging, and deterministic execution profiling are implemented",
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
