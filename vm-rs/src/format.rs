use crate::bytecode::{
    BlockId, Constant, DebugLocation, DebugRange, DebugSource, FuncId, Function, GlobalId,
    Instruction, LocalId, NativeId, NativeImport, Program, TypeId, TypeLayout, UpvalueDesc,
    UpvalueId, UpvalueSource, VariantId, VariantLayout,
};
use crate::vm::native_arity_bounds;
use std::collections::BTreeSet;
use std::fmt;

/// Stable artifact family accepted and emitted by this VM.
pub const ARTIFACT_FORMAT_FAMILY: &str = "cdbc";
/// Stable artifact version accepted and emitted by this VM.
pub const ARTIFACT_FORMAT_VERSION: &str = "0.2";
/// Canonical header for the current artifact family and version.
pub const ARTIFACT_HEADER: &str = "cdbc 0.2";
/// Legacy header still accepted for `.cdbc 0.1` compatibility inputs.
pub const LEGACY_ARTIFACT_HEADER: &str = "cdbc 0.1";

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ParseError {
    pub line: usize,
    pub message: String,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "parse error at line {}: {}", self.line, self.message)
    }
}

/// Machine-readable class for an artifact loading or validation failure.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArtifactErrorKind {
    Parse,
    UnsupportedVersion,
    Verification,
}

impl ArtifactErrorKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Parse => "parse",
            Self::UnsupportedVersion => "unsupported_version",
            Self::Verification => "verification",
        }
    }
}

/// Stable artifact error returned by the checked library facade.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ArtifactError {
    pub kind: ArtifactErrorKind,
    pub line: usize,
    pub message: String,
}

impl ArtifactError {
    fn from_parse(source: &str, error: ParseError) -> Self {
        let family_prefix = format!("{} ", ARTIFACT_FORMAT_FAMILY);
        let first_line = source.lines().map(str::trim).find(|line| !line.is_empty());
        let expected_header = format!("expected `{}`", ARTIFACT_HEADER);
        let kind = if error.message == expected_header
            && first_line.is_some_and(|line| line.starts_with(&family_prefix))
        {
            ArtifactErrorKind::UnsupportedVersion
        } else {
            ArtifactErrorKind::Parse
        };
        Self {
            kind,
            line: error.line,
            message: error.message,
        }
    }

    fn from_verification(error: ParseError) -> Self {
        Self {
            kind: ArtifactErrorKind::Verification,
            line: error.line,
            message: error.message,
        }
    }
}

impl fmt::Display for ArtifactError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let kind = match self.kind {
            ArtifactErrorKind::Parse => "parse error",
            ArtifactErrorKind::UnsupportedVersion => "unsupported artifact version",
            ArtifactErrorKind::Verification => "artifact verification error",
        };
        write!(f, "{} at line {}: {}", kind, self.line, self.message)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModuleDependency {
    pub identity: String,
    pub kind: ModuleDependencyKind,
    pub instruction_offset: usize,
    pub requested_path: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ModuleDependencyKind {
    Import,
    ReExport,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ModuleArtifact {
    pub identity: String,
    pub path: String,
    pub canonical_path: String,
    pub is_entry: bool,
    pub entry_order: Option<usize>,
    pub dependencies: Vec<ModuleDependency>,
    pub program: Program,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Artifact {
    Program(Program),
    Module(ModuleArtifact),
}

struct ParsedModuleHeader {
    identity: String,
    path: String,
    canonical_path: String,
    is_entry: bool,
    entry_order: Option<usize>,
    dependencies: Vec<ModuleDependency>,
}

struct Parser<'a> {
    lines: Vec<(usize, &'a str)>,
    current: usize,
}

impl<'a> Parser<'a> {
    fn new(source: &'a str) -> Self {
        let lines = source
            .lines()
            .enumerate()
            .filter_map(|(index, line)| {
                let trimmed = line.trim();
                if trimmed.is_empty() {
                    None
                } else {
                    Some((index + 1, trimmed))
                }
            })
            .collect();
        Self { lines, current: 0 }
    }

    fn peek(&self) -> Option<(usize, &'a str)> {
        self.lines.get(self.current).copied()
    }

    fn advance(&mut self) -> Option<(usize, &'a str)> {
        let line = self.peek()?;
        self.current += 1;
        Some(line)
    }

    fn require_line(&mut self, expected: &str) -> Result<(), ParseError> {
        let (line_number, line) = self.advance().ok_or_else(|| ParseError {
            line: self.last_line(),
            message: format!("expected `{}`", expected),
        })?;
        if line == expected {
            Ok(())
        } else {
            Err(ParseError {
                line: line_number,
                message: format!("expected `{}`", expected),
            })
        }
    }

    fn last_line(&self) -> usize {
        self.lines.last().map(|(line, _)| *line).unwrap_or(1)
    }

    fn parse_module_header(&mut self) -> Result<ParsedModuleHeader, ParseError> {
        self.require_line("artifact: module")?;
        self.require_line("module:")?;
        let identity = self.parse_module_string_field("identity")?;
        let path = self.parse_module_string_field("path")?;
        let canonical_path = self.parse_module_string_field("canonical_path")?;
        let is_entry = self.parse_module_bool_field("entry")?;
        let entry_order = if self
            .peek()
            .map(|(_, line)| line.starts_with("entry_order = "))
            .unwrap_or(false)
        {
            Some(self.parse_module_usize_field("entry_order")?)
        } else {
            None
        };
        self.require_line("dependencies:")?;

        let mut dependencies = Vec::new();
        while let Some((line_number, line)) = self.peek() {
            if line == "constants:" {
                break;
            }
            self.advance();
            let (dependency_ref, rest) = split_once(line_number, line, " target=")?;
            let index = parse_prefixed(line_number, dependency_ref, 'd', "dependency reference")?;
            if index != dependencies.len() {
                return Err(ParseError {
                    line: line_number,
                    message: format!("expected dependency d{}", dependencies.len()),
                });
            }
            let (identity, rest) = parse_string_prefix(line_number, rest)?;
            let rest = rest.strip_prefix(" kind=").ok_or_else(|| ParseError {
                line: line_number,
                message: "expected dependency kind".to_string(),
            })?;
            let (kind_text, rest) = split_once(line_number, rest, " at=")?;
            let kind = match kind_text {
                "import" => ModuleDependencyKind::Import,
                "re_export" => ModuleDependencyKind::ReExport,
                _ => {
                    return Err(ParseError {
                        line: line_number,
                        message: "expected dependency kind import or re_export".to_string(),
                    })
                }
            };
            let (offset_text, rest) = split_once(line_number, rest, " requested=")?;
            let instruction_offset =
                parse_usize(line_number, offset_text, "dependency instruction offset")?;
            let requested_path = parse_string_full(line_number, rest)?;
            dependencies.push(ModuleDependency {
                identity,
                kind,
                instruction_offset,
                requested_path,
            });
        }

        Ok(ParsedModuleHeader {
            identity,
            path,
            canonical_path,
            is_entry,
            entry_order,
            dependencies,
        })
    }

    fn parse_module_string_field(&mut self, field: &str) -> Result<String, ParseError> {
        let (line_number, line) = self.advance().ok_or_else(|| ParseError {
            line: self.last_line(),
            message: format!("expected module field `{}`", field),
        })?;
        let prefix = format!("{} = ", field);
        let value = line.strip_prefix(&prefix).ok_or_else(|| ParseError {
            line: line_number,
            message: format!("expected module field `{}`", field),
        })?;
        parse_string_full(line_number, value)
    }

    fn parse_module_bool_field(&mut self, field: &str) -> Result<bool, ParseError> {
        let (line_number, line) = self.advance().ok_or_else(|| ParseError {
            line: self.last_line(),
            message: format!("expected module field `{}`", field),
        })?;
        let prefix = format!("{} = ", field);
        let value = line.strip_prefix(&prefix).ok_or_else(|| ParseError {
            line: line_number,
            message: format!("expected module field `{}`", field),
        })?;
        match value {
            "true" => Ok(true),
            "false" => Ok(false),
            _ => Err(ParseError {
                line: line_number,
                message: format!("expected boolean module field `{}`", field),
            }),
        }
    }

    fn parse_module_usize_field(&mut self, field: &str) -> Result<usize, ParseError> {
        let (line_number, line) = self.advance().ok_or_else(|| ParseError {
            line: self.last_line(),
            message: format!("expected module field `{}`", field),
        })?;
        let prefix = format!("{} = ", field);
        let value = line.strip_prefix(&prefix).ok_or_else(|| ParseError {
            line: line_number,
            message: format!("expected module field `{}`", field),
        })?;
        parse_usize(line_number, value, "module field value")
    }

    fn parse_constants(&mut self) -> Result<Vec<Constant>, ParseError> {
        self.require_line("constants:")?;
        let mut constants = Vec::new();
        while let Some((line_number, line)) = self.peek() {
            if line == "names:" {
                break;
            }
            self.advance();
            let (left, right) = split_once(line_number, line, " = ")?;
            let index = parse_prefixed(line_number, left, 'c', "constant reference")?;
            if index != constants.len() {
                return Err(ParseError {
                    line: line_number,
                    message: format!("expected constant c{}", constants.len()),
                });
            }
            constants.push(parse_constant(line_number, right)?);
        }
        Ok(constants)
    }

    fn parse_names(&mut self) -> Result<Vec<String>, ParseError> {
        self.require_line("names:")?;
        let mut names = Vec::new();
        while let Some((line_number, line)) = self.peek() {
            if line.starts_with("main registers=")
                || line == "globals:"
                || line == "types:"
                || line == "native_imports:"
            {
                break;
            }
            self.advance();
            let (left, right) = split_once(line_number, line, " = ")?;
            let index = parse_prefixed(line_number, left, 'n', "name reference")?;
            if index != names.len() {
                return Err(ParseError {
                    line: line_number,
                    message: format!("expected name n{}", names.len()),
                });
            }
            names.push(parse_string_full(line_number, right)?);
        }
        Ok(names)
    }

    fn parse_main(&mut self) -> Result<Function, ParseError> {
        let (line_number, line) = self.advance().ok_or_else(|| ParseError {
            line: self.last_line(),
            message: "expected main section".to_string(),
        })?;
        let registers =
            parse_wrapped_usize(line_number, line, "main registers=", ":", "main section")?;
        let instructions = self.parse_instructions_until_function()?;
        let instruction_count = instructions.len();
        Ok(Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
            registers,
            instructions,
            locations: vec![None; instruction_count],
        })
    }

    fn parse_functions(&mut self) -> Result<Vec<Function>, ParseError> {
        let mut functions = Vec::new();
        while let Some((_, line)) = self.peek() {
            if line == "debug_sources:" || line == "debug_locations:" || line == "debug_ranges:" {
                break;
            }
            let (line_number, line) = self.advance().expect("checked end");
            let (index, name, arity, registers) = parse_function_header(line_number, line)?;
            if index != functions.len() {
                return Err(ParseError {
                    line: line_number,
                    message: format!("expected function f{}", functions.len()),
                });
            }

            let mut params = Vec::new();
            while let Some((param_line, candidate)) = self.peek() {
                if !candidate.starts_with("param ") {
                    break;
                }
                self.advance();
                let (param_index, param_name) = parse_param(param_line, candidate)?;
                if param_index != params.len() {
                    return Err(ParseError {
                        line: param_line,
                        message: format!("expected param {}", params.len()),
                    });
                }
                params.push(param_name);
            }
            if params.len() != arity {
                return Err(ParseError {
                    line: line_number,
                    message: format!(
                        "function f{} expected {} params, found {}",
                        index,
                        arity,
                        params.len()
                    ),
                });
            }
            let mut upvalues = Vec::new();
            while let Some((upvalue_line, candidate)) = self.peek() {
                if !candidate.starts_with("upvalue ") {
                    break;
                }
                self.advance();
                let (upvalue_text, source_text) =
                    split_once(upvalue_line, candidate, " = ")?;
                let upvalue_index_text = upvalue_text.strip_prefix("upvalue ").ok_or_else(|| {
                    ParseError {
                        line: upvalue_line,
                        message: "expected upvalue reference".to_string(),
                    }
                })?;
                let upvalue_index =
                    parse_prefixed(upvalue_line, upvalue_index_text, 'u', "upvalue reference")?;
                if upvalue_index != upvalues.len() {
                    return Err(ParseError {
                        line: upvalue_line,
                        message: format!("expected upvalue u{}", upvalues.len()),
                    });
                }
                let (kind, source_index) = split_once(upvalue_line, source_text, " ")?;
                match kind {
                    "local" => upvalues.push(UpvalueDesc {
                        source: UpvalueSource::Local(LocalId(parse_prefixed(
                            upvalue_line,
                            source_index,
                            'l',
                            "local reference",
                        )? as u32)),
                    }),
                    "upvalue" => upvalues.push(UpvalueDesc {
                        source: UpvalueSource::Upvalue(UpvalueId(parse_prefixed(
                            upvalue_line,
                            source_index,
                            'u',
                            "upvalue reference",
                        )? as u32)),
                    }),
                    "global" => upvalues.push(UpvalueDesc {
                        source: UpvalueSource::Global(GlobalId(parse_prefixed(
                            upvalue_line,
                            source_index,
                            'g',
                            "global reference",
                        )? as u32)),
                    }),
                    _ => {
                        return Err(ParseError {
                            line: upvalue_line,
                            message: "expected upvalue source local or upvalue".to_string(),
                        })
                    }
                }
            }
            let instructions = self.parse_instructions_until_function()?;
            let instruction_count = instructions.len();
            let max_local_slot = instructions
                .iter()
                .filter_map(|instruction| match instruction {
                    Instruction::LoadLocal { slot, .. }
                    | Instruction::BindLocal { slot, .. }
                    | Instruction::SetLocal { slot, .. } => Some(*slot),
                    _ => None,
                })
                .max()
                .unwrap_or(0);
            functions.push(Function {
                id: FuncId((index + 1) as u32),
                name,
                arity,
                local_count: arity.max(max_local_slot.saturating_add(1)),
                upvalues,
                params,
                registers,
                instructions,
                locations: vec![None; instruction_count],
            });
        }
        Ok(functions)
    }

    fn parse_instructions_until_function(&mut self) -> Result<Vec<Instruction>, ParseError> {
        let mut instructions = Vec::new();
        let mut block_count = 0usize;
        while let Some((line_number, line)) = self.peek() {
            if line.starts_with("function ")
                || line == "debug_sources:"
                || line == "debug_locations:"
                || line == "debug_ranges:"
            {
                break;
            }
            if line.starts_with("block ") {
                self.advance();
                let header = line
                    .strip_prefix("block ")
                    .and_then(|rest| rest.strip_suffix(':'))
                    .ok_or_else(|| ParseError {
                        line: line_number,
                        message: "expected block header".to_string(),
                    })?;
                let id = parse_prefixed(line_number, header, 'b', "block reference")?;
                if id != block_count {
                    return Err(ParseError {
                        line: line_number,
                        message: format!("expected block b{}", block_count),
                    });
                }
                block_count += 1;
                instructions.push(Instruction::BlockStart {
                    id: BlockId(id as u32),
                });
                continue;
            }
            self.advance();
            instructions.push(parse_instruction(line_number, line)?);
        }
        Ok(instructions)
    }

    fn parse_debug_sources(&mut self) -> Result<Vec<DebugSource>, ParseError> {
        let Some((line_number, line)) = self.peek() else {
            return Ok(Vec::new());
        };
        if line != "debug_sources:" {
            return Ok(Vec::new());
        }
        self.advance();
        let mut sources = Vec::new();
        while let Some((line_number, line)) = self.peek() {
            if line == "debug_locations:" || line == "debug_ranges:" {
                break;
            }
            self.advance();
            let (source_ref, rest) = split_once(line_number, line, " ")?;
            let index = parse_prefixed(line_number, source_ref, 's', "source reference")?;
            if index != sources.len() {
                return Err(ParseError {
                    line: line_number,
                    message: format!("expected source s{}", sources.len()),
                });
            }
            let (module, rest) = if let Some(module_text) = rest.strip_prefix("module=") {
                let (module, rest) = parse_string_prefix(line_number, module_text)?;
                let rest = rest.strip_prefix(" path=").ok_or_else(|| ParseError {
                    line: line_number,
                    message: "expected source path".to_string(),
                })?;
                (Some(module), rest)
            } else {
                let rest = rest.strip_prefix("path=").ok_or_else(|| ParseError {
                    line: line_number,
                    message: "expected source path".to_string(),
                })?;
                (None, rest)
            };
            let (path, rest) = parse_string_prefix(line_number, rest)?;
            let rest = rest.strip_prefix(" text=").ok_or_else(|| ParseError {
                line: line_number,
                message: "expected source text".to_string(),
            })?;
            let text = parse_string_full(line_number, rest)?;
            sources.push(DebugSource { module, path, text });
        }
        if self.peek().is_some()
            && self.peek().unwrap().1 != "debug_locations:"
            && self.peek().unwrap().1 != "debug_ranges:"
        {
            return Err(ParseError {
                line: line_number,
                message: "expected debug_locations or debug_ranges section".to_string(),
            });
        }
        Ok(sources)
    }

    fn parse_debug_locations(&mut self, program: &mut Program) -> Result<(), ParseError> {
        let Some((line_number, line)) = self.peek() else {
            return Ok(());
        };
        if line != "debug_locations:" {
            return Err(ParseError {
                line: line_number,
                message: format!("unexpected section `{}`", line),
            });
        }
        self.advance();
        while let Some((line_number, line)) = self.peek() {
            if line == "debug_ranges:" {
                break;
            }
            self.advance();
            let (left, location_text) = split_once(line_number, line, " = ")?;
            let (section, instruction_text) = split_location_target(line_number, left)?;
            let instruction = parse_usize(line_number, instruction_text, "instruction index")?;
            let location = parse_debug_location(line_number, location_text)?;
            if location.source >= program.debug_sources.len() {
                return Err(ParseError {
                    line: line_number,
                    message: "debug location source index out of range".to_string(),
                });
            }

            let locations = match section {
                DebugSection::Main => &mut program.functions[0].locations,
                DebugSection::Function(index) => {
                    let Some(function) = program.functions.get_mut(index + 1) else {
                        return Err(ParseError {
                            line: line_number,
                            message: "debug location function index out of range".to_string(),
                        });
                    };
                    &mut function.locations
                }
            };
            let Some(slot) = locations.get_mut(instruction) else {
                return Err(ParseError {
                    line: line_number,
                    message: "debug location instruction index out of range".to_string(),
                });
            };
            if slot.is_some() {
                return Err(ParseError {
                    line: line_number,
                    message: "duplicate debug location".to_string(),
                });
            }
            *slot = Some(location);
        }
        Ok(())
    }

    fn parse_debug_ranges(&mut self, program: &mut Program) -> Result<(), ParseError> {
        let Some((_, line)) = self.peek() else {
            return Ok(());
        };
        if line != "debug_ranges:" {
            return Ok(());
        }
        self.advance();
        while let Some((line_number, line)) = self.peek() {
            self.advance();
            let (left, range_text) = split_once(line_number, line, " = ")?;
            let (section, instruction_text) = split_location_target(line_number, left)?;
            let instruction = parse_usize(line_number, instruction_text, "instruction index")?;
            let range = parse_debug_range(line_number, range_text)?;
            if range.source >= program.debug_sources.len() {
                return Err(ParseError {
                    line: line_number,
                    message: "debug range source index out of range".to_string(),
                });
            }

            let locations = match section {
                DebugSection::Main => &mut program.functions[0].locations,
                DebugSection::Function(index) => {
                    let Some(function) = program.functions.get_mut(index + 1) else {
                        return Err(ParseError {
                            line: line_number,
                            message: "debug range function index out of range".to_string(),
                        });
                    };
                    &mut function.locations
                }
            };
            let Some(slot) = locations.get_mut(instruction) else {
                return Err(ParseError {
                    line: line_number,
                    message: "debug range instruction index out of range".to_string(),
                });
            };
            let Some(location) = slot.as_mut() else {
                return Err(ParseError {
                    line: line_number,
                    message: "debug range requires a matching debug location".to_string(),
                });
            };
            if location.range.is_some() {
                return Err(ParseError {
                    line: line_number,
                    message: "duplicate debug range".to_string(),
                });
            }
            if location.source != range.source {
                return Err(ParseError {
                    line: line_number,
                    message: "debug range source does not match debug location".to_string(),
                });
            }
            location.range = Some(range);
        }
        Ok(())
    }
}

enum DebugSection {
    Main,
    Function(usize),
}

fn split_location_target(line: usize, text: &str) -> Result<(DebugSection, &str), ParseError> {
    let parts = text.split_whitespace().collect::<Vec<_>>();
    match parts.as_slice() {
        ["main", instruction] => Ok((DebugSection::Main, instruction)),
        ["function", function_ref, instruction] => Ok((
            DebugSection::Function(parse_function_ref(line, function_ref)?),
            instruction,
        )),
        _ => Err(ParseError {
            line,
            message: "expected debug location target".to_string(),
        }),
    }
}

fn parse_debug_location(line: usize, text: &str) -> Result<DebugLocation, ParseError> {
    let parts = text.split(':').collect::<Vec<_>>();
    if parts.len() != 3 {
        return Err(ParseError {
            line,
            message: "expected source:line:column location".to_string(),
        });
    }
    let source = parse_prefixed(line, parts[0], 's', "source reference")?;
    let location_line = parse_usize(line, parts[1], "source line")?;
    let column = parse_usize(line, parts[2], "source column")?;
    if location_line == 0 || column == 0 {
        return Err(ParseError {
            line,
            message: "source line and column must be positive".to_string(),
        });
    }
    Ok(DebugLocation {
        source,
        line: location_line,
        column,
        range: None,
    })
}

fn parse_debug_range(line: usize, text: &str) -> Result<DebugRange, ParseError> {
    let parts = text.split(':').collect::<Vec<_>>();
    if parts.len() != 3 {
        return Err(ParseError {
            line,
            message: "expected source:start:end range".to_string(),
        });
    }
    let source = parse_prefixed(line, parts[0], 's', "source reference")?;
    let start = parse_usize(line, parts[1], "range start")?;
    let end = parse_usize(line, parts[2], "range end")?;
    if start > end {
        return Err(ParseError {
            line,
            message: "debug range start must not exceed end".to_string(),
        });
    }
    Ok(DebugRange { source, start, end })
}

fn parse_program_body_with_globals(parser: &mut Parser<'_>) -> Result<Program, ParseError> {
    let constants = parser.parse_constants()?;
    let names = parser.parse_names()?;
    let mut globals = Vec::new();
    if parser
        .peek()
        .map(|(_, line)| line == "globals:")
        .unwrap_or(false)
    {
        parser.advance();
        while let Some((line_number, line)) = parser.peek() {
            if !line.starts_with("g") || !line.contains(" = ") {
                break;
            }
            parser.advance();
            let (global_ref, name_ref) = split_once(line_number, line, " = ")?;
            let global_index =
                parse_prefixed(line_number, global_ref, 'g', "global reference")?;
            if global_index != globals.len() {
                return Err(ParseError {
                    line: line_number,
                    message: format!("expected global g{}", globals.len()),
                });
            }
            globals.push(parse_name_ref(line_number, name_ref)?);
        }
    }
    let mut types = Vec::new();
    if parser
        .peek()
        .map(|(_, line)| line == "types:")
        .unwrap_or(false)
    {
        parser.advance();
        while let Some((line_number, line)) = parser.peek() {
            if !line.starts_with("t") || !line.contains(" = ") {
                break;
            }
            parser.advance();
            let (type_ref, rest) = split_once(line_number, line, " = ")?;
            let type_index = parse_prefixed(line_number, type_ref, 't', "type reference")?;
            if type_index != types.len() {
                return Err(ParseError {
                    line: line_number,
                    message: format!("expected type t{}", types.len()),
                });
            }
            let mut layout = TypeLayout {
                is_enum: false,
                name: String::new(),
                field_names: Vec::new(),
                variants: Vec::new(),
            };
            let parts: Vec<&str> = rest.split_whitespace().collect();
            if parts.is_empty() || (parts[0] != "struct" && parts[0] != "enum") {
                return Err(ParseError {
                    line: line_number,
                    message: "expected struct or enum type declaration".to_string(),
                });
            }
            layout.is_enum = parts[0] == "enum";
            layout.name = parse_string_full(line_number, parts[1])?;
            if layout.is_enum {
                let mut index = 2;
                while index < parts.len() {
                    let (variant_ref, variant_name) =
                        split_once(line_number, parts[index], "=")?;
                    let variant_index = parse_prefixed(
                        line_number,
                        variant_ref,
                        'v',
                        "variant reference",
                    )?;
                    if variant_index != layout.variants.len() {
                        return Err(ParseError {
                            line: line_number,
                            message: format!("expected variant v{}", layout.variants.len()),
                        });
                    }
                    index += 1;
                    if index >= parts.len() {
                        return Err(ParseError {
                            line: line_number,
                            message: "expected variant payload count".to_string(),
                        });
                    }
                    let (payload_label, count) =
                        split_once(line_number, parts[index], "=")?;
                    if payload_label != "payload" {
                        return Err(ParseError {
                            line: line_number,
                            message: "expected `payload=`".to_string(),
                        });
                    }
                    layout.variants.push(VariantLayout {
                        name: parse_string_full(line_number, variant_name)?,
                        payload_count: parse_usize(line_number, count, "variant payload count")?,
                    });
                    index += 1;
                }
            } else {
                for part in &parts[2..] {
                    let (field_ref, field_name) = split_once(line_number, part, "=")?;
                    let field_number = field_ref.strip_prefix("field").ok_or_else(|| {
                        ParseError {
                            line: line_number,
                            message: "expected field reference".to_string(),
                        }
                    })?;
                    let field_index =
                        parse_usize(line_number, field_number, "field index")?;
                    if field_index != layout.field_names.len() {
                        return Err(ParseError {
                            line: line_number,
                            message: format!("expected field f{}", layout.field_names.len()),
                        });
                    }
                    layout
                        .field_names
                        .push(parse_string_full(line_number, field_name)?);
                }
            }
            types.push(layout);
        }
    }
    let mut native_imports = Vec::new();
    if parser
        .peek()
        .map(|(_, line)| line == "native_imports:")
        .unwrap_or(false)
    {
        parser.advance();
        while let Some((line_number, line)) = parser.peek() {
            if !line.starts_with("i") || !line.contains(" = ") {
                break;
            }
            parser.advance();
            let (import_ref, rest) = split_once(line_number, line, " = ")?;
            let import_index =
                parse_prefixed(line_number, import_ref, 'i', "native import reference")?;
            if import_index != native_imports.len() {
                return Err(ParseError {
                    line: line_number,
                    message: format!("expected native import i{}", native_imports.len()),
                });
            }
            let (name, rest) = parse_string_prefix(line_number, rest)?;
            let abi_text = rest.strip_prefix(" abi=").ok_or_else(|| ParseError {
                line: line_number,
                message: "expected native import abi version".to_string(),
            })?;
            let abi = parse_usize(line_number, abi_text, "native import abi version")?;
            native_imports.push(NativeImport {
                name,
                abi: abi as u32,
            });
        }
    }
    let main = parser.parse_main()?;
    let mut functions = parser.parse_functions()?;
    functions.insert(0, main);
    let debug_sources = parser.parse_debug_sources()?;
    let mut program = Program {
        constants,
        names,
        globals,
        types,
        native_imports,
        functions,
        entry: FuncId(0),
        debug_sources,
    };
    parser.parse_debug_locations(&mut program)?;
    parser.parse_debug_ranges(&mut program)?;
    Ok(program)
}

fn parse_artifact_unverified(source: &str) -> Result<(Artifact, usize), ParseError> {
    let mut parser = Parser::new(source);
    let (line_number, line) = parser.advance().ok_or_else(|| ParseError {
        line: parser.last_line(),
        message: format!("expected `{}`", ARTIFACT_HEADER),
    })?;
    if line != ARTIFACT_HEADER && line != LEGACY_ARTIFACT_HEADER {
        return Err(ParseError {
            line: line_number,
            message: format!("expected `{}`", ARTIFACT_HEADER),
        });
    }
    let module = if parser
        .peek()
        .map(|(_, line)| line == "artifact: module")
        .unwrap_or(false)
    {
        Some(parser.parse_module_header()?)
    } else {
        None
    };
    let program = parse_program_body_with_globals(&mut parser)?;
    let artifact = match module {
        Some(module) => Artifact::Module(ModuleArtifact {
            identity: module.identity,
            path: module.path,
            canonical_path: module.canonical_path,
            is_entry: module.is_entry,
            entry_order: module.entry_order,
            dependencies: module.dependencies,
            program,
        }),
        None => Artifact::Program(program),
    };
    Ok((artifact, parser.last_line()))
}

pub fn parse_artifact(source: &str) -> Result<Artifact, ParseError> {
    let (artifact, line) = parse_artifact_unverified(source)?;
    verify_artifact_at_line(&artifact, line)?;
    Ok(artifact)
}

fn validate_module_envelope(artifact: &ModuleArtifact, line: usize) -> Result<(), ParseError> {
    if artifact.identity.is_empty()
        || artifact.path.is_empty()
        || artifact.canonical_path.is_empty()
    {
        return Err(ParseError {
            line,
            message: "module identity and paths must be non-empty".to_string(),
        });
    }
    if artifact.is_entry != artifact.entry_order.is_some() {
        return Err(ParseError {
            line,
            message: "module entry_order must be present exactly for entry modules".to_string(),
        });
    }

    let mut previous_offset = 0;
    for (index, dependency) in artifact.dependencies.iter().enumerate() {
        if dependency.identity.is_empty() || dependency.requested_path.is_empty() {
            return Err(ParseError {
                line,
                message: format!("module dependency d{} has an empty identity or path", index),
            });
        }
        if dependency.instruction_offset > artifact.program.functions[0].instructions.len() {
            return Err(ParseError {
                line,
                message: format!(
                    "module dependency d{} instruction offset out of range",
                    index
                ),
            });
        }
        if index != 0 && dependency.instruction_offset < previous_offset {
            return Err(ParseError {
                line,
                message: "module dependency offsets must be nondecreasing".to_string(),
            });
        }
        previous_offset = dependency.instruction_offset;
    }
    Ok(())
}

const SUPPORTED_NATIVE_FUNCTIONS: &[&str] = &[
    "push",
    "pop",
    "remove",
    "clear",
    "merge",
    "keys",
    "values",
    "floor",
    "ceil",
    "sqrt",
    "str",
    "substr",
    "charAt",
    "typeOf",
    "hash",
    "contains",
    "slice",
    "copy",
    "concat",
    "map",
    "filter",
    "flatMap",
    "any",
    "all",
    "count",
    "find",
    "findIndex",
    "reduce",
    "range",
    "print",
];

fn validation_error(line: usize, message: impl Into<String>) -> ParseError {
    ParseError {
        line,
        message: message.into(),
    }
}

pub fn verify_artifact(artifact: &Artifact) -> Result<(), ParseError> {
    verify_artifact_at_line(artifact, 1)
}

/// Parse and verify an in-memory artifact with a typed failure boundary.
pub fn parse_artifact_checked(source: &str) -> Result<Artifact, ArtifactError> {
    let (artifact, line) = parse_artifact_unverified(source)
        .map_err(|error| ArtifactError::from_parse(source, error))?;
    verify_artifact_at_line(&artifact, line).map_err(ArtifactError::from_verification)?;
    Ok(artifact)
}

/// Verify an in-memory linked or module artifact with a typed failure boundary.
pub fn verify_artifact_checked(artifact: &Artifact) -> Result<(), ArtifactError> {
    verify_artifact(artifact).map_err(ArtifactError::from_verification)
}

/// Verify an in-memory linked program with a typed failure boundary.
pub fn verify_program_checked(program: &Program) -> Result<(), ArtifactError> {
    verify_program(program).map_err(ArtifactError::from_verification)
}

/// Verify an in-memory module product with a typed failure boundary.
pub fn verify_module_artifact_checked(artifact: &ModuleArtifact) -> Result<(), ArtifactError> {
    verify_module_artifact(artifact).map_err(ArtifactError::from_verification)
}

pub fn verify_program(program: &Program) -> Result<(), ParseError> {
    validate_program(program, 1)
}

pub fn verify_module_artifact(artifact: &ModuleArtifact) -> Result<(), ParseError> {
    verify_module_artifact_at_line(artifact, 1)
}

fn verify_artifact_at_line(artifact: &Artifact, line: usize) -> Result<(), ParseError> {
    match artifact {
        Artifact::Program(program) => validate_program(program, line),
        Artifact::Module(module) => verify_module_artifact_at_line(module, line),
    }
}

fn verify_module_artifact_at_line(
    artifact: &ModuleArtifact,
    line: usize,
) -> Result<(), ParseError> {
    validate_program(&artifact.program, line)?;
    validate_module_envelope(artifact, line)
}

fn validate_program(program: &Program, line: usize) -> Result<(), ParseError> {
    for (index, name) in program.globals.iter().enumerate() {
        if *name >= program.names.len() {
            return Err(validation_error(
                line,
                format!("global g{index} references name n{name} out of range"),
            ));
        }
    }
    for (index, source) in program.debug_sources.iter().enumerate() {
        if source.module.as_ref().is_some_and(String::is_empty) {
            return Err(validation_error(
                line,
                format!("debug source s{} has an empty module identity", index),
            ));
        }
    }
    for (index, constant) in program.constants.iter().enumerate() {
        if let Constant::Number(value) = constant {
            let parsed = value.parse::<f64>().map_err(|_| {
                validation_error(
                    line,
                    format!("constant c{} is not a valid number literal", index),
                )
            })?;
            if !parsed.is_finite() {
                return Err(validation_error(
                    line,
                    format!("constant c{} must be a finite number literal", index),
                ));
            }
        }
    }
    let mut native_names = BTreeSet::new();
    for (index, import) in program.native_imports.iter().enumerate() {
        if import.name.is_empty() {
            return Err(validation_error(
                line,
                format!("native import i{} has an empty name", index),
            ));
        }
        if import.abi != 1 {
            return Err(validation_error(
                line,
                format!("native import i{} must declare abi=1", index),
            ));
        }
        if !SUPPORTED_NATIVE_FUNCTIONS.contains(&import.name.as_str()) {
            return Err(validation_error(
                line,
                format!(
                    "native import i{} references unsupported native function `{}`",
                    index, import.name
                ),
            ));
        }
        if !native_names.insert(import.name.as_str()) {
            return Err(validation_error(
                line,
                format!("native import i{} duplicates native function `{}`", index, import.name),
            ));
        }
    }

    if program.functions.get(program.entry.0 as usize).is_none() {
        return Err(validation_error(
            line,
            format!("entry function f{} is out of range", program.entry.0),
        ));
    }
    if program.entry != FuncId(0) {
        return Err(validation_error(
            line,
            format!(
                "entry must be f0, found f{}",
                program.entry.0
            ),
        ));
    }
    for (index, function) in program.functions.iter().enumerate() {
        if function.id != FuncId(index as u32) {
            return Err(validation_error(
                line,
                format!(
                    "function table entry {} has id f{}, expected f{}",
                    index, function.id.0, index
                ),
            ));
        }
        if function.params.len() != function.arity {
            return Err(validation_error(
                line,
                format!(
                    "function f{} declares arity {}, but has {} parameters",
                    index,
                    function.arity,
                    function.params.len()
                ),
            ));
        }
        for (upvalue_index, upvalue) in function.upvalues.iter().enumerate() {
            if let UpvalueSource::Global(global) = upvalue.source {
                if global.0 as usize >= program.globals.len() {
                    return Err(validation_error(
                        line,
                        format!(
                            "function f{} upvalue u{} global g{} out of range",
                            index.saturating_sub(1),
                            upvalue_index,
                            global.0
                        ),
                    ));
                }
            }
        }
        let context = if index == 0 {
            "main".to_string()
        } else {
            format!("function f{}", index - 1)
        };
        validate_body(
            &context,
            function.registers,
            function.arity,
            function.local_count,
            function.upvalues.len(),
            &function.instructions,
            &function.locations,
            program,
            line,
        )?;
    }
    Ok(())
}

fn validate_body(
    context: &str,
    registers: usize,
    arity: usize,
    local_count: usize,
    upvalue_count: usize,
    instructions: &[Instruction],
    locations: &[Option<DebugLocation>],
    program: &Program,
    line: usize,
) -> Result<(), ParseError> {
    if locations.len() != instructions.len() {
        return Err(validation_error(
            line,
            format!(
                "{} has {} debug locations for {} instructions",
                context,
                locations.len(),
                instructions.len()
            ),
        ));
    }
    for (instruction_index, location) in locations.iter().enumerate() {
        let Some(location) = location else {
            continue;
        };
        let Some(range) = &location.range else {
            continue;
        };
        let Some(source) = program.debug_sources.get(range.source) else {
            return Err(validation_error(
                line,
                format!(
                    "{} instruction {} debug range source index out of range",
                    context, instruction_index
                ),
            ));
        };
        if range.start > range.end {
            return Err(validation_error(
                line,
                format!(
                    "{} instruction {} debug range start exceeds end",
                    context, instruction_index
                ),
            ));
        }
        if range.end > source.text.len() {
            return Err(validation_error(
                line,
                format!(
                    "{} instruction {} debug range exceeds source length",
                    context, instruction_index
                ),
            ));
        }
        if range.source != location.source {
            return Err(validation_error(
                line,
                format!(
                    "{} instruction {} debug range source does not match location",
                    context, instruction_index
                ),
            ));
        }
    }
    for (instruction_index, instruction) in instructions.iter().enumerate() {
        validate_instruction(
            context,
            instruction_index,
            registers,
            local_count,
            upvalue_count,
            instructions.len(),
            instruction,
            program,
            line,
        )?;
    }
    if instructions
        .iter()
        .any(|instruction| matches!(instruction, Instruction::BlockStart { .. }))
    {
        validate_block_body(
            context,
            registers,
            arity,
            local_count,
            upvalue_count,
            instructions,
            program,
            line,
        )?;
    }
    Ok(())
}

fn is_block_terminator(instruction: &Instruction) -> bool {
    matches!(
        instruction,
        Instruction::Br { .. }
            | Instruction::BrIf { .. }
            | Instruction::Return { .. }
            | Instruction::ReturnNil
    )
}

fn instruction_register_def(instruction: &Instruction) -> Option<usize> {
    match instruction {
        Instruction::Constant { dest, .. }
        | Instruction::MakeFunction { dest, .. }
        | Instruction::Array { dest, .. }
        | Instruction::Map { dest, .. }
        | Instruction::Struct { dest, .. }
        | Instruction::Variant { dest, .. }
        | Instruction::VariantTag { dest, .. }
        | Instruction::VariantField { dest, .. }
        | Instruction::Move { dest, .. }
        | Instruction::LoadVar { dest, .. }
        | Instruction::LoadLocal { dest, .. }
        | Instruction::LoadUpvalue { dest, .. }
        | Instruction::LoadGlobal { dest, .. }
        | Instruction::Call { dest, .. }
        | Instruction::NativeCall { dest, .. }
        | Instruction::CallNative { dest, .. }
        | Instruction::Index { dest, .. }
        | Instruction::AssignIndex { dest, .. }
        | Instruction::Field { dest, .. }
        | Instruction::AssignField { dest, .. }
        | Instruction::Len { dest, .. }
        | Instruction::AssertArray { dest, .. }
        | Instruction::AssertNumber { dest, .. }
        | Instruction::MakeStruct { dest, .. }
        | Instruction::StructGet { dest, .. }
        | Instruction::StructSet { dest, .. }
        | Instruction::MakeVariant { dest, .. }
        | Instruction::IsVariant { dest, .. }
        | Instruction::VariantGet { dest, .. }
        | Instruction::Negate { dest, .. }
        | Instruction::Not { dest, .. }
        | Instruction::Add { dest, .. }
        | Instruction::Subtract { dest, .. }
        | Instruction::Multiply { dest, .. }
        | Instruction::Divide { dest, .. }
        | Instruction::Equal { dest, .. }
        | Instruction::NotEqual { dest, .. }
        | Instruction::Greater { dest, .. }
        | Instruction::GreaterEqual { dest, .. }
        | Instruction::Less { dest, .. }
        | Instruction::LessEqual { dest, .. } => Some(*dest),
        _ => None,
    }
}

fn instruction_register_reads(instruction: &Instruction) -> Vec<usize> {
    match instruction {
        Instruction::Array { elements, .. } => elements.clone(),
        Instruction::Map { entries, .. } => entries.iter().flat_map(|(k, v)| [*k, *v]).collect(),
        Instruction::Struct { fields, .. } => fields.iter().map(|(_, v)| *v).collect(),
        Instruction::Variant { payload, .. } => payload.clone(),
        Instruction::VariantTag { value, .. } | Instruction::VariantField { value, .. } => vec![*value],
        Instruction::Move { source, .. } => vec![*source],
        Instruction::StoreVar { value, .. }
        | Instruction::AssignVar { value, .. }
        | Instruction::BindLocal { value, .. }
        | Instruction::SetLocal { value, .. }
        | Instruction::SetUpvalue { value, .. }
        | Instruction::InitGlobal { value, .. }
        | Instruction::SetGlobal { value, .. }
        | Instruction::Print { value }
        | Instruction::Return { value }
        | Instruction::Negate { value, .. }
        | Instruction::Not { value, .. }
        | Instruction::Len { value, .. }
        | Instruction::AssertArray { value, .. }
        | Instruction::AssertNumber { value, .. } => vec![*value],
        Instruction::Call {
            callee, arguments, ..
        } => {
            let mut reads = vec![*callee];
            reads.extend(arguments.iter().copied());
            reads
        }
        Instruction::NativeCall { arguments, .. } => arguments.clone(),
        Instruction::CallNative { arguments, .. } => arguments.clone(),
        Instruction::MakeStruct { elements, .. } => elements.clone(),
        Instruction::StructGet { object, .. } => vec![*object],
        Instruction::StructSet {
            object, value, ..
        } => vec![*object, *value],
        Instruction::MakeVariant { payload, .. } => payload.clone(),
        Instruction::IsVariant { value, .. } => vec![*value],
        Instruction::VariantGet { value, .. } => vec![*value],
        Instruction::Index {
            collection, index, ..
        } => vec![*collection, *index],
        Instruction::AssignIndex {
            collection,
            index,
            value,
            ..
        } => vec![*collection, *index, *value],
        Instruction::Field { object, .. } => vec![*object],
        Instruction::AssignField { object, value, .. } => vec![*object, *value],
        Instruction::JumpIfFalse { condition, .. }
        | Instruction::JumpIfTrue { condition, .. }
        | Instruction::BrIf { condition, .. } => vec![*condition],
        Instruction::Add {
            left, right, ..
        }
        | Instruction::Subtract {
            left, right, ..
        }
        | Instruction::Multiply {
            left, right, ..
        }
        | Instruction::Divide {
            left, right, ..
        }
        | Instruction::Equal {
            left, right, ..
        }
        | Instruction::NotEqual {
            left, right, ..
        }
        | Instruction::Greater {
            left, right, ..
        }
        | Instruction::GreaterEqual {
            left, right, ..
        }
        | Instruction::Less {
            left, right, ..
        }
        | Instruction::LessEqual {
            left, right, ..
        } => vec![*left, *right],
        _ => Vec::new(),
    }
}

fn validate_block_body(
    context: &str,
    registers: usize,
    arity: usize,
    local_count: usize,
    upvalue_count: usize,
    instructions: &[Instruction],
    program: &Program,
    line: usize,
) -> Result<(), ParseError> {
    let mut blocks: Vec<(usize, usize)> = Vec::new();
    for (index, instruction) in instructions.iter().enumerate() {
        match instruction {
            Instruction::BlockStart { id } => {
                if id.0 as usize != blocks.len() {
                    return Err(validation_error(
                        line,
                        format!(
                            "{} block b{} is not sequential (expected b{})",
                            context,
                            id.0,
                            blocks.len()
                        ),
                    ));
                }
                blocks.push((index, index + 1));
            }
            _ => {
                if let Some(last) = blocks.last_mut() {
                    last.1 = index + 1;
                }
            }
        }
    }
    if blocks.is_empty() || blocks[0].0 != 0 {
        return Err(validation_error(
            line,
            format!("{} block body must begin with block b0", context),
        ));
    }

    let mut successors: Vec<Vec<usize>> = vec![Vec::new(); blocks.len()];
    for (index, (_start, end)) in blocks.iter().enumerate() {
        let terminator = &instructions[end - 1];
        if !is_block_terminator(terminator) {
            return Err(validation_error(
                line,
                format!("{} block b{} is missing a terminator", context, index),
            ));
        }
        match terminator {
            Instruction::Br { target } => successors[index].push(target.0 as usize),
            Instruction::BrIf {
                if_true, if_false, ..
            } => {
                successors[index].push(if_true.0 as usize);
                successors[index].push(if_false.0 as usize);
            }
            _ => {}
        }
    }
    for (index, targets) in successors.iter().enumerate() {
        for target in targets {
            if *target >= blocks.len() {
                return Err(validation_error(
                    line,
                    format!(
                        "{} block b{} branches to invalid block b{}",
                        context, index, target
                    ),
                ));
            }
        }
    }

    let mut predecessors: Vec<Vec<usize>> = vec![Vec::new(); blocks.len()];
    for (index, targets) in successors.iter().enumerate() {
        for target in targets {
            predecessors[*target].push(index);
        }
    }

    let universe: BTreeSet<usize> = (0..registers).collect();
    let mut defs: Vec<BTreeSet<usize>> = Vec::with_capacity(blocks.len());
    let mut exposed: Vec<BTreeSet<usize>> = Vec::with_capacity(blocks.len());
    for (start, end) in &blocks {
        let mut block_defs = BTreeSet::new();
        let mut block_exposed = BTreeSet::new();
        let mut defined = BTreeSet::new();
        for instruction in &instructions[*start..end - 1] {
            if matches!(instruction, Instruction::BlockStart { .. }) {
                continue;
            }
            for read in instruction_register_reads(instruction) {
                if read < registers && !defined.contains(&read) {
                    block_exposed.insert(read);
                }
            }
            if let Some(def) = instruction_register_def(instruction) {
                if def < registers {
                    defined.insert(def);
                    block_defs.insert(def);
                }
            }
        }
        let terminator = &instructions[end - 1];
        for read in instruction_register_reads(terminator) {
            if read < registers && !defined.contains(&read) {
                block_exposed.insert(read);
            }
        }
        defs.push(block_defs);
        exposed.push(block_exposed);
    }

    let mut ins: Vec<BTreeSet<usize>> = Vec::with_capacity(blocks.len());
    ins.push(BTreeSet::new());
    for _ in 1..blocks.len() {
        ins.push(universe.clone());
    }
    loop {
        let mut changed = false;
        let outs: Vec<BTreeSet<usize>> = ins
            .iter()
            .zip(defs.iter())
            .map(|(input, def)| input.union(def).copied().collect())
            .collect();
        for block in 1..blocks.len() {
            let next: BTreeSet<usize> = if predecessors[block].is_empty() {
                universe.clone()
            } else {
                predecessors[block]
                    .iter()
                    .map(|pred| &outs[*pred])
                    .fold(universe.clone(), |acc, out| {
                        acc.intersection(out).copied().collect()
                    })
            };
            if next != ins[block] {
                ins[block] = next;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    for (block, (start, end)) in blocks.iter().enumerate() {
        let mut defined = ins[block].clone();
        for instruction in &instructions[*start..end - 1] {
            for read in instruction_register_reads(instruction) {
                if read < registers && !defined.contains(&read) {
                    return Err(validation_error(
                        line,
                        format!(
                            "{} block b{} reads undefined register r{}",
                            context, block, read
                        ),
                    ));
                }
            }
            if let Some(def) = instruction_register_def(instruction) {
                if def < registers {
                    defined.insert(def);
                }
            }
        }
        let terminator = &instructions[end - 1];
        for read in instruction_register_reads(terminator) {
            if read < registers && !defined.contains(&read) {
                return Err(validation_error(
                    line,
                    format!(
                        "{} block b{} reads undefined register r{}",
                        context, block, read
                    ),
                ));
            }
        }
    }

    // Local definite-binding mirrors the register analysis; parameters are
    // bound on entry.
    if local_count > 0 {
        let local_universe: BTreeSet<usize> = (0..local_count).collect();
        let initial_params: BTreeSet<usize> = (0..arity.min(local_count)).collect();
        let mut local_defs: Vec<BTreeSet<usize>> = Vec::with_capacity(blocks.len());
        let mut local_exposed: Vec<BTreeSet<usize>> = Vec::with_capacity(blocks.len());
        for (start, end) in &blocks {
            let mut block_defs = BTreeSet::new();
            let mut block_exposed = BTreeSet::new();
            let mut bound = BTreeSet::new();
            for instruction in &instructions[*start..end - 1] {
                if let Instruction::BindLocal { slot, .. } = instruction {
                    if *slot < local_count {
                        bound.insert(*slot);
                        block_defs.insert(*slot);
                    }
                } else if let Instruction::LoadLocal { slot, .. }
                | Instruction::SetLocal { slot, .. } = instruction
                {
                    if *slot < local_count && !bound.contains(slot) {
                        block_exposed.insert(*slot);
                    }
                }
            }
            local_defs.push(block_defs);
            local_exposed.push(block_exposed);
        }
        let mut local_ins: Vec<BTreeSet<usize>> = Vec::with_capacity(blocks.len());
        local_ins.push(initial_params);
        for _ in 1..blocks.len() {
            local_ins.push(local_universe.clone());
        }
        loop {
            let mut changed = false;
            let outs: Vec<BTreeSet<usize>> = local_ins
                .iter()
                .zip(local_defs.iter())
                .map(|(input, def)| input.union(def).copied().collect())
                .collect();
            for block in 1..blocks.len() {
                let next: BTreeSet<usize> = if predecessors[block].is_empty() {
                    local_universe.clone()
                } else {
                    predecessors[block]
                        .iter()
                        .map(|pred| &outs[*pred])
                        .fold(local_universe.clone(), |acc, out| {
                            acc.intersection(out).copied().collect()
                        })
                };
                if next != local_ins[block] {
                    local_ins[block] = next;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        for (block, (start, end)) in blocks.iter().enumerate() {
            let mut bound = local_ins[block].clone();
            for instruction in &instructions[*start..*end] {
                if let Instruction::BindLocal { slot, .. } = instruction {
                    if *slot < local_count {
                        bound.insert(*slot);
                    }
                } else if let Instruction::LoadLocal { slot, .. }
                | Instruction::SetLocal { slot, .. } = instruction
                {
                    if *slot < local_count && !bound.contains(slot) {
                        return Err(validation_error(
                            line,
                            format!(
                                "{} block b{} reads unbound local l{}",
                                context, block, slot
                            ),
                        ));
                    }
                }
            }
        }
    }

    for instruction in instructions {
        if let Instruction::NativeCall {
            name, arguments, ..
        } = instruction
        {
            if let Some(native_name) = program.names.get(*name) {
                if let Some((min_arity, max_arity)) = native_arity_bounds(native_name) {
                    if arguments.len() < min_arity || arguments.len() > max_arity {
                        return Err(validation_error(
                            line,
                            format!(
                                "{} native `{}` expects {} to {} arguments, got {}",
                                context,
                                native_name,
                                min_arity,
                                max_arity,
                                arguments.len()
                            ),
                        ));
                    }
                }
            }
        }
        if let Instruction::CallNative {
            native, arguments, ..
        } = instruction
        {
            if let Some(import) = program.native_imports.get(native.0 as usize) {
                if let Some((min_arity, max_arity)) = native_arity_bounds(&import.name) {
                    if arguments.len() < min_arity || arguments.len() > max_arity {
                        return Err(validation_error(
                            line,
                            format!(
                                "{} native `{}` expects {} to {} arguments, got {}",
                                context,
                                import.name,
                                min_arity,
                                max_arity,
                                arguments.len()
                            ),
                        ));
                    }
                }
            }
        }
        if matches!(
            instruction,
            Instruction::Jump { .. }
                | Instruction::JumpIfFalse { .. }
                | Instruction::JumpIfTrue { .. }
        ) {
            return Err(validation_error(
                line,
                format!("{} block body contains a legacy jump", context),
            ));
        }
    }
    let _ = upvalue_count;
    Ok(())
}

fn validate_instruction(
    context: &str,
    instruction_index: usize,
    registers: usize,
    local_count: usize,
    upvalue_count: usize,
    instruction_count: usize,
    instruction: &Instruction,
    program: &Program,
    line: usize,
) -> Result<(), ParseError> {
    let register = |index: usize, role: &str| {
        if index >= registers {
            Err(validation_error(
                line,
                format!(
                    "{} instruction {} {} register r{} out of range (register count {})",
                    context, instruction_index, role, index, registers
                ),
            ))
        } else {
            Ok(())
        }
    };
    let constant = |index: usize| {
        if index >= program.constants.len() {
            Err(validation_error(
                line,
                format!(
                    "{} instruction {} constant c{} out of range (constant count {})",
                    context,
                    instruction_index,
                    index,
                    program.constants.len()
                ),
            ))
        } else {
            Ok(())
        }
    };
    let name = |index: usize, role: &str| {
        if index >= program.names.len() {
            Err(validation_error(
                line,
                format!(
                    "{} instruction {} {} name n{} out of range (name count {})",
                    context,
                    instruction_index,
                    role,
                    index,
                    program.names.len()
                ),
            ))
        } else {
            Ok(())
        }
    };
    let function = |index: FuncId| {
        if index.0 as usize >= program.functions.len() {
            Err(validation_error(
                line,
                format!(
                    "{} instruction {} function f{} out of range (function count {})",
                    context,
                    instruction_index,
                    index.0.saturating_sub(1),
                    program.functions.len()
                ),
            ))
        } else {
            Ok(())
        }
    };
    let jump = |target: usize| {
        if target > instruction_count {
            Err(validation_error(
                line,
                format!(
                    "{} instruction {} jump target {} out of range (instruction count {})",
                    context, instruction_index, target, instruction_count
                ),
            ))
        } else {
            Ok(())
        }
    };
    let registers = |values: &[usize], role: &str| {
        for (index, value) in values.iter().enumerate() {
            register(*value, &format!("{} {}", role, index))?;
        }
        Ok(())
    };
    let local_slot = |index: usize| {
        if index >= local_count {
            Err(validation_error(
                line,
                format!(
                    "{} instruction {} local l{} out of range (local count {})",
                    context, instruction_index, index, local_count
                ),
            ))
        } else {
            Ok(())
        }
    };
    let upvalue_slot = |index: usize| {
        if index >= upvalue_count {
            Err(validation_error(
                line,
                format!(
                    "{} instruction {} upvalue u{} out of range (upvalue count {})",
                    context, instruction_index, index, upvalue_count
                ),
            ))
        } else {
            Ok(())
        }
    };

    match instruction {
        Instruction::Constant {
            dest,
            constant: value,
        } => {
            register(*dest, "destination")?;
            constant(*value)?;
        }
        Instruction::MakeFunction {
            dest,
            function: value,
        } => {
            register(*dest, "destination")?;
            function(*value)?;
        }
        Instruction::Array { dest, elements } => {
            register(*dest, "destination")?;
            registers(elements, "array element")?;
        }
        Instruction::Map { dest, entries } => {
            register(*dest, "destination")?;
            for (index, (key, value)) in entries.iter().enumerate() {
                register(*key, &format!("map entry {} key", index))?;
                register(*value, &format!("map entry {} value", index))?;
            }
        }
        Instruction::Struct {
            dest,
            type_name,
            fields,
        } => {
            register(*dest, "destination")?;
            if let Some(type_name) = type_name {
                name(*type_name, "struct type")?;
            }
            for (index, (field, value)) in fields.iter().enumerate() {
                name(*field, &format!("struct field {}", index))?;
                register(*value, &format!("struct field {} value", index))?;
            }
        }
        Instruction::MakeStruct {
            dest,
            type_id,
            elements,
        } => {
            register(*dest, "destination")?;
            let layout = program.types.get(type_id.0 as usize).ok_or_else(|| {
                validation_error(line, format!("type t{} out of range", type_id.0))
            })?;
            if layout.is_enum || elements.len() != layout.field_names.len() {
                return Err(validation_error(
                    line,
                    format!(
                        "make_struct t{} expects {} fields, got {}",
                        type_id.0,
                        layout.field_names.len(),
                        elements.len()
                    ),
                ));
            }
            registers(elements, "struct element")?;
        }
        Instruction::StructGet {
            dest,
            object,
            type_id,
            slot,
        } => {
            register(*dest, "destination")?;
            register(*object, "struct object")?;
            let layout = program.types.get(type_id.0 as usize).ok_or_else(|| {
                validation_error(line, format!("type t{} out of range", type_id.0))
            })?;
            if layout.is_enum || *slot >= layout.field_names.len() {
                return Err(validation_error(
                    line,
                    format!(
                        "struct_get t{} field slot {} out of range",
                        type_id.0, slot
                    ),
                ));
            }
        }
        Instruction::StructSet {
            dest,
            object,
            type_id,
            slot,
            value,
        } => {
            register(*dest, "destination")?;
            register(*object, "struct object")?;
            register(*value, "field value")?;
            let layout = program.types.get(type_id.0 as usize).ok_or_else(|| {
                validation_error(line, format!("type t{} out of range", type_id.0))
            })?;
            if layout.is_enum || *slot >= layout.field_names.len() {
                return Err(validation_error(
                    line,
                    format!(
                        "struct_set t{} field slot {} out of range",
                        type_id.0, slot
                    ),
                ));
            }
        }
        Instruction::Variant {
            dest,
            enum_name,
            variant_name,
            payload,
        } => {
            register(*dest, "destination")?;
            name(*enum_name, "variant enum")?;
            name(*variant_name, "variant name")?;
            registers(payload, "variant payload")?;
        }
        Instruction::MakeVariant {
            dest,
            type_id,
            variant_id,
            payload,
        } => {
            register(*dest, "destination")?;
            registers(payload, "variant payload")?;
            let layout = program.types.get(type_id.0 as usize).ok_or_else(|| {
                validation_error(line, format!("type t{} out of range", type_id.0))
            })?;
            if !layout.is_enum || variant_id.0 as usize >= layout.variants.len() {
                return Err(validation_error(
                    line,
                    format!(
                        "make_variant t{} variant v{} out of range",
                        type_id.0, variant_id.0
                    ),
                ));
            }
        }
        Instruction::IsVariant {
            dest,
            value,
            type_id,
            variant_id,
        } => {
            register(*dest, "destination")?;
            register(*value, "variant value")?;
            let layout = program.types.get(type_id.0 as usize).ok_or_else(|| {
                validation_error(line, format!("type t{} out of range", type_id.0))
            })?;
            if !layout.is_enum || variant_id.0 as usize >= layout.variants.len() {
                return Err(validation_error(
                    line,
                    format!(
                        "is_variant t{} variant v{} out of range",
                        type_id.0, variant_id.0
                    ),
                ));
            }
        }
        Instruction::VariantGet {
            dest,
            value,
            type_id,
            variant_id,
            index,
        } => {
            register(*dest, "destination")?;
            register(*value, "variant value")?;
            let layout = program.types.get(type_id.0 as usize).ok_or_else(|| {
                validation_error(line, format!("type t{} out of range", type_id.0))
            })?;
            let variant = layout.variants.get(variant_id.0 as usize).ok_or_else(|| {
                validation_error(line, format!("variant v{} out of range", variant_id.0))
            })?;
            if *index >= variant.payload_count {
                return Err(validation_error(
                    line,
                    format!(
                        "variant_get t{} v{} payload index {} out of range",
                        type_id.0, variant_id.0, index
                    ),
                ));
            }
        }
        Instruction::VariantTag {
            dest,
            value,
            enum_name,
            variant_name,
        } => {
            register(*dest, "destination")?;
            register(*value, "variant tag value")?;
            name(*enum_name, "variant enum")?;
            name(*variant_name, "variant name")?;
        }
        Instruction::VariantField { dest, value, .. } => {
            register(*dest, "destination")?;
            register(*value, "variant field value")?;
        }
        Instruction::Move { dest, source } => {
            register(*dest, "destination")?;
            register(*source, "source")?;
        }
        Instruction::LoadVar { dest, name: value } => {
            register(*dest, "destination")?;
            name(*value, "variable")?;
        }
        Instruction::StoreVar {
            name: value,
            value: source,
        }
        | Instruction::AssignVar {
            name: value,
            value: source,
        } => {
            name(*value, "variable")?;
            register(*source, "value")?;
        }
        Instruction::LoadLocal { dest, slot } => {
            register(*dest, "destination")?;
            local_slot(*slot)?;
        }
        Instruction::BindLocal { slot, value } => {
            local_slot(*slot)?;
            register(*value, "local value")?;
        }
        Instruction::SetLocal { slot, value } => {
            local_slot(*slot)?;
            register(*value, "local value")?;
        }
        Instruction::LoadUpvalue { dest, slot } => {
            register(*dest, "destination")?;
            upvalue_slot(*slot)?;
        }
        Instruction::SetUpvalue { slot, value } => {
            upvalue_slot(*slot)?;
            register(*value, "upvalue value")?;
        }
        Instruction::LoadGlobal { dest, slot } => {
            register(*dest, "destination")?;
            let _ = slot;
        }
        Instruction::InitGlobal { slot, value } => {
            let _ = slot;
            register(*value, "global value")?;
        }
        Instruction::SetGlobal { slot, value } => {
            let _ = slot;
            register(*value, "global value")?;
        }
        Instruction::Call {
            dest,
            callee,
            arguments,
        } => {
            register(*dest, "destination")?;
            register(*callee, "callee")?;
            registers(arguments, "call argument")?;
        }
        Instruction::NativeCall {
            dest,
            name: value,
            arguments,
        } => {
            register(*dest, "destination")?;
            name(*value, "native function")?;
            if !SUPPORTED_NATIVE_FUNCTIONS.contains(&program.names[*value].as_str()) {
                return Err(validation_error(
                    line,
                    format!(
                        "{} instruction {} unsupported native function `{}`",
                        context, instruction_index, program.names[*value]
                    ),
                ));
            }
            registers(arguments, "native argument")?;
        }
        Instruction::CallNative {
            dest,
            native,
            arguments,
        } => {
            register(*dest, "destination")?;
            if native.0 as usize >= program.native_imports.len() {
                return Err(validation_error(
                    line,
                    format!(
                        "{} instruction {} native import i{} out of range (import count {})",
                        context,
                        instruction_index,
                        native.0,
                        program.native_imports.len()
                    ),
                ));
            }
            registers(arguments, "native argument")?;
        }
        Instruction::Index {
            dest,
            collection,
            index,
        } => {
            register(*dest, "destination")?;
            register(*collection, "index collection")?;
            register(*index, "index value")?;
        }
        Instruction::AssignIndex {
            dest,
            collection,
            index,
            value,
        } => {
            register(*dest, "destination")?;
            register(*collection, "index collection")?;
            register(*index, "index value")?;
            register(*value, "assigned value")?;
        }
        Instruction::Field {
            dest,
            object,
            name: value,
        } => {
            register(*dest, "destination")?;
            register(*object, "field object")?;
            name(*value, "field")?;
        }
        Instruction::AssignField {
            dest,
            object,
            name: field,
            value,
        } => {
            register(*dest, "destination")?;
            register(*object, "field object")?;
            name(*field, "field")?;
            register(*value, "assigned value")?;
        }
        Instruction::Len { dest, value }
        | Instruction::AssertArray { dest, value }
        | Instruction::Negate { dest, value }
        | Instruction::Not { dest, value } => {
            register(*dest, "destination")?;
            register(*value, "value")?;
        }
        Instruction::AssertNumber {
            dest,
            value,
            message,
        } => {
            register(*dest, "destination")?;
            register(*value, "value")?;
            name(*message, "assertion message")?;
        }
        Instruction::Print { value } | Instruction::Return { value } => {
            register(*value, "value")?;
        }
        Instruction::Add { dest, left, right }
        | Instruction::Subtract { dest, left, right }
        | Instruction::Multiply { dest, left, right }
        | Instruction::Divide { dest, left, right }
        | Instruction::Equal { dest, left, right }
        | Instruction::NotEqual { dest, left, right }
        | Instruction::Greater { dest, left, right }
        | Instruction::GreaterEqual { dest, left, right }
        | Instruction::Less { dest, left, right }
        | Instruction::LessEqual { dest, left, right } => {
            register(*dest, "destination")?;
            register(*left, "left operand")?;
            register(*right, "right operand")?;
        }
        Instruction::Jump { target } => jump(*target)?,
        Instruction::JumpIfFalse { condition, target }
        | Instruction::JumpIfTrue { condition, target } => {
            register(*condition, "jump condition")?;
            jump(*target)?;
        }
        Instruction::BlockStart { id } => {
            let _ = id;
        }
        Instruction::Br { target } => {
            let _ = target;
        }
        Instruction::BrIf {
            condition,
            if_true,
            if_false,
        } => {
            register(*condition, "branch condition")?;
            let _ = (if_true, if_false);
        }
        Instruction::ReturnNil => {}
    }
    Ok(())
}

#[allow(dead_code)]
pub fn parse_program(source: &str) -> Result<Program, ParseError> {
    match parse_artifact(source)? {
        Artifact::Program(program) => Ok(program),
        Artifact::Module(_) => Err(ParseError {
            line: 3,
            message: "module artifact requires a module-aware loader".to_string(),
        }),
    }
}

pub fn format_program(program: &Program) -> String {
    let mut out = String::with_capacity(format_program_capacity_hint(program));
    out.push_str(program_header(program));
    out.push_str("\n\n");
    format_program_sections(&mut out, program);
    out
}

fn program_header(program: &Program) -> &'static str {
    let uses_legacy_variables = program.functions.iter().any(|function| {
        function.instructions.iter().any(|instruction| {
            matches!(
                instruction,
                Instruction::LoadVar { .. }
                    | Instruction::StoreVar { .. }
                    | Instruction::AssignVar { .. }
            )
        })
    });
    if uses_legacy_variables {
        LEGACY_ARTIFACT_HEADER
    } else {
        ARTIFACT_HEADER
    }
}

pub fn format_artifact(artifact: &Artifact) -> String {
    match artifact {
        Artifact::Program(program) => format_program(program),
        Artifact::Module(module) => {
            let mut out = String::with_capacity(
                format_program_capacity_hint(&module.program)
                    .saturating_add(module.identity.len())
                    .saturating_add(module.path.len())
                    .saturating_add(module.canonical_path.len())
                    .saturating_add(
                        module
                            .dependencies
                            .iter()
                            .map(|dependency| {
                                dependency.identity.len() + dependency.requested_path.len()
                            })
                            .sum(),
                    ),
            );
            out.push_str(program_header(&module.program));
            out.push_str("\n\n");
            out.push_str("artifact: module\n\n");
            out.push_str("module:\n");
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
            out.push_str("  dependencies:\n");
            for (index, dependency) in module.dependencies.iter().enumerate() {
                let kind = match dependency.kind {
                    ModuleDependencyKind::Import => "import",
                    ModuleDependencyKind::ReExport => "re_export",
                };
                out.push_str(&format!(
                    "    d{} target={} kind={} at={} requested={}\n",
                    index,
                    quote_string(&dependency.identity),
                    kind,
                    dependency.instruction_offset,
                    quote_string(&dependency.requested_path)
                ));
            }
            out.push('\n');
            format_program_sections(&mut out, &module.program);
            out
        }
    }
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

fn format_program_sections(out: &mut String, program: &Program) {
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
    let entry = &program.functions[program.entry.0 as usize];
    out.push_str(&format!("\nmain registers={}:\n", entry.registers));
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
        out.push_str(&format!(
            "\nfunction f{} name={} arity={} registers={}:\n",
            function_index,
            quote_string(&function.name),
            function.arity,
            function.registers
        ));
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
            if let Some(range) = location.as_ref().and_then(|location| location.range.as_ref()) {
                out.push_str(&format_debug_range("main", index, range));
            }
        }
        let mut function_index = 0usize;
        for (position, function) in program.functions.iter().enumerate() {
            if position as u32 == program.entry.0 {
                continue;
            }
            for (instruction, location) in function.locations.iter().enumerate() {
                if let Some(range) = location.as_ref().and_then(|location| location.range.as_ref()) {
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

fn parse_constant(line: usize, text: &str) -> Result<Constant, ParseError> {
    if text == "nil" {
        Ok(Constant::Nil)
    } else if let Some(number) = text.strip_prefix("number ") {
        if number.is_empty() {
            Err(ParseError {
                line,
                message: "expected number literal".to_string(),
            })
        } else {
            Ok(Constant::Number(number.to_string()))
        }
    } else if let Some(value) = text.strip_prefix("bool ") {
        match value {
            "true" => Ok(Constant::Bool(true)),
            "false" => Ok(Constant::Bool(false)),
            _ => Err(ParseError {
                line,
                message: "expected bool literal".to_string(),
            }),
        }
    } else if let Some(value) = text.strip_prefix("string ") {
        Ok(Constant::String(parse_string_full(line, value)?))
    } else {
        Err(ParseError {
            line,
            message: "expected constant value".to_string(),
        })
    }
}

fn format_constant(constant: &Constant) -> String {
    match constant {
        Constant::Nil => "nil".to_string(),
        Constant::Number(value) => format!("number {}", value),
        Constant::Bool(value) => format!("bool {}", if *value { "true" } else { "false" }),
        Constant::String(value) => format!("string {}", quote_string(value)),
    }
}

fn parse_instruction(line: usize, text: &str) -> Result<Instruction, ParseError> {
    if let Some((dest_text, rest)) = text.split_once(" = ") {
        let dest = parse_register(line, dest_text)?;
        let (opcode, operands) = split_opcode(rest);
        match opcode {
            "constant" => Ok(Instruction::Constant {
                dest,
                constant: parse_constant_ref(line, operands)?,
            }),
            "make_function" => Ok(Instruction::MakeFunction {
                dest,
                function: FuncId((parse_function_ref(line, operands)? + 1) as u32),
            }),
            "array" => Ok(Instruction::Array {
                dest,
                elements: parse_register_list(line, operands)?,
            }),
            "map" => {
                let entries = parse_map_entries(line, operands)?;
                Ok(Instruction::Map { dest, entries })
            }
            "struct" => {
                let (type_name, field_text) = parse_optional_struct_type_name(line, operands)?;
                Ok(Instruction::Struct {
                    dest,
                    type_name,
                    fields: parse_struct_fields(line, field_text)?,
                })
            }
            "make_struct" => {
                let (type_text, elements) = split_once(line, operands, " ")?;
                Ok(Instruction::MakeStruct {
                    dest,
                    type_id: TypeId(parse_prefixed(line, type_text, 't', "type reference")? as u32),
                    elements: parse_register_list(line, elements)?,
                })
            }
            "struct_get" => {
                let parts = split_comma_parts(operands);
                if parts.len() != 3 {
                    return Err(ParseError {
                        line,
                        message: "struct_get expects three operands".to_string(),
                    });
                }
                Ok(Instruction::StructGet {
                    dest,
                    object: parse_register(line, parts[0])?,
                    type_id: TypeId(parse_prefixed(line, parts[1], 't', "type reference")? as u32),
                    slot: parse_usize(line, parts[2], "field slot")?,
                })
            }
            "struct_set" => {
                let parts = split_comma_parts(operands);
                if parts.len() != 4 {
                    return Err(ParseError {
                        line,
                        message: "struct_set expects four operands".to_string(),
                    });
                }
                Ok(Instruction::StructSet {
                    dest,
                    object: parse_register(line, parts[0])?,
                    type_id: TypeId(parse_prefixed(line, parts[1], 't', "type reference")? as u32),
                    slot: parse_usize(line, parts[2], "field slot")?,
                    value: parse_register(line, parts[3])?,
                })
            }
            "variant" => {
                let (variant_text, payload_text) = split_once(line, operands, " ")?;
                let (enum_name, variant_name) = split_once(line, variant_text, ".")?;
                Ok(Instruction::Variant {
                    dest,
                    enum_name: parse_name_ref(line, enum_name)?,
                    variant_name: parse_name_ref(line, variant_name)?,
                    payload: parse_register_list(line, &payload_text)?,
                })
            }
            "make_variant" => {
                let (header, payload_text) = split_once(line, operands, " [")?;
                let (type_text, variant_text) = split_once(line, header, ", ")?;
                let payload_text = format!("[{}", payload_text);
                Ok(Instruction::MakeVariant {
                    dest,
                    type_id: TypeId(parse_prefixed(line, type_text, 't', "type reference")? as u32),
                    variant_id: VariantId(parse_prefixed(line, variant_text, 'v', "variant reference")? as u32),
                    payload: parse_register_list(line, &payload_text)?,
                })
            }
            "is_variant" => {
                let parts = split_comma_parts(operands);
                if parts.len() != 3 {
                    return Err(ParseError {
                        line,
                        message: "is_variant expects three operands".to_string(),
                    });
                }
                Ok(Instruction::IsVariant {
                    dest,
                    value: parse_register(line, parts[0])?,
                    type_id: TypeId(parse_prefixed(line, parts[1], 't', "type reference")? as u32),
                    variant_id: VariantId(parse_prefixed(line, parts[2], 'v', "variant reference")? as u32),
                })
            }
            "variant_tag" => {
                let (value, variant_text) = split_once(line, operands, " ")?;
                let (enum_name, variant_name) = split_once(line, variant_text, ".")?;
                Ok(Instruction::VariantTag {
                    dest,
                    value: parse_register(line, value)?,
                    enum_name: parse_name_ref(line, enum_name)?,
                    variant_name: parse_name_ref(line, variant_name)?,
                })
            }
            "variant_field" => {
                let (value, index) = split_once(line, operands, " ")?;
                Ok(Instruction::VariantField {
                    dest,
                    value: parse_register(line, value)?,
                    index: parse_usize(line, index, "variant field index")?,
                })
            }
            "variant_get" => {
                let parts = split_comma_parts(operands);
                if parts.len() != 4 {
                    return Err(ParseError {
                        line,
                        message: "variant_get expects four operands".to_string(),
                    });
                }
                Ok(Instruction::VariantGet {
                    dest,
                    value: parse_register(line, parts[0])?,
                    type_id: TypeId(parse_prefixed(line, parts[1], 't', "type reference")? as u32),
                    variant_id: VariantId(parse_prefixed(line, parts[2], 'v', "variant reference")? as u32),
                    index: parse_usize(line, parts[3], "payload index")?,
                })
            }
            "move" => Ok(Instruction::Move {
                dest,
                source: parse_register(line, operands)?,
            }),
            "load_var" => Ok(Instruction::LoadVar {
                dest,
                name: parse_name_ref(line, operands)?,
            }),
            "load_local" => Ok(Instruction::LoadLocal {
                dest,
                slot: parse_prefixed(line, operands, 'l', "local reference")?,
            }),
            "load_upvalue" => Ok(Instruction::LoadUpvalue {
                dest,
                slot: parse_prefixed(line, operands, 'u', "upvalue reference")?,
            }),
            "load_global" => Ok(Instruction::LoadGlobal {
                dest,
                slot: parse_prefixed(line, operands, 'g', "global reference")?,
            }),
            "call" => {
                let (callee, args) = split_once(line, operands, " ")?;
                Ok(Instruction::Call {
                    dest,
                    callee: parse_register(line, callee)?,
                    arguments: parse_register_list(line, args)?,
                })
            }
            "native_call" => {
                let (name, args) = split_once(line, operands, " ")?;
                Ok(Instruction::NativeCall {
                    dest,
                    name: parse_name_ref(line, name)?,
                    arguments: parse_register_list(line, args)?,
                })
            }
            "call_native" => {
                let (native, args) = split_once(line, operands, " ")?;
                Ok(Instruction::CallNative {
                    dest,
                    native: NativeId(parse_prefixed(
                        line,
                        native,
                        'i',
                        "native import reference",
                    )? as u32),
                    arguments: parse_register_list(line, args)?,
                })
            }
            "index" => {
                let (collection, index) = parse_two_registers(line, operands)?;
                Ok(Instruction::Index {
                    dest,
                    collection,
                    index,
                })
            }
            "assign_index" => {
                let parts = split_comma_parts(operands);
                if parts.len() != 3 {
                    return Err(ParseError {
                        line,
                        message: "assign_index expects three operands".to_string(),
                    });
                }
                Ok(Instruction::AssignIndex {
                    dest,
                    collection: parse_register(line, parts[0])?,
                    index: parse_register(line, parts[1])?,
                    value: parse_register(line, parts[2])?,
                })
            }
            "field" => {
                let (object, name) = split_once(line, operands, ", ")?;
                Ok(Instruction::Field {
                    dest,
                    object: parse_register(line, object)?,
                    name: parse_name_ref(line, name)?,
                })
            }
            "assign_field" => {
                let parts = split_comma_parts(operands);
                if parts.len() != 3 {
                    return Err(ParseError {
                        line,
                        message: "assign_field expects three operands".to_string(),
                    });
                }
                Ok(Instruction::AssignField {
                    dest,
                    object: parse_register(line, parts[0])?,
                    name: parse_name_ref(line, parts[1])?,
                    value: parse_register(line, parts[2])?,
                })
            }
            "len" => Ok(Instruction::Len {
                dest,
                value: parse_register(line, operands)?,
            }),
            "assert_array" => Ok(Instruction::AssertArray {
                dest,
                value: parse_register(line, operands)?,
            }),
            "assert_number" => {
                let (value, message) = split_once(line, operands, ", ")?;
                Ok(Instruction::AssertNumber {
                    dest,
                    value: parse_register(line, value)?,
                    message: parse_name_ref(line, message)?,
                })
            }
            "negate" => Ok(Instruction::Negate {
                dest,
                value: parse_register(line, operands)?,
            }),
            "not" => Ok(Instruction::Not {
                dest,
                value: parse_register(line, operands)?,
            }),
            "add" => parse_binary(line, dest, operands, "add"),
            "subtract" => parse_binary(line, dest, operands, "subtract"),
            "multiply" => parse_binary(line, dest, operands, "multiply"),
            "divide" => parse_binary(line, dest, operands, "divide"),
            "equal" => parse_binary(line, dest, operands, "equal"),
            "not_equal" => parse_binary(line, dest, operands, "not_equal"),
            "greater" => parse_binary(line, dest, operands, "greater"),
            "greater_equal" => parse_binary(line, dest, operands, "greater_equal"),
            "less" => parse_binary(line, dest, operands, "less"),
            "less_equal" => parse_binary(line, dest, operands, "less_equal"),
            unknown => Err(ParseError {
                line,
                message: format!("unknown opcode `{}`", unknown),
            }),
        }
    } else {
        let (opcode, operands) = split_opcode(text);
        match opcode {
            "store_var" => {
                let (name, value) = split_once(line, operands, ", ")?;
                Ok(Instruction::StoreVar {
                    name: parse_name_ref(line, name)?,
                    value: parse_register(line, value)?,
                })
            }
            "assign_var" => {
                let (name, value) = split_once(line, operands, ", ")?;
                Ok(Instruction::AssignVar {
                    name: parse_name_ref(line, name)?,
                    value: parse_register(line, value)?,
                })
            }
            "bind_local" => {
                let (slot, value) = split_once(line, operands, ", ")?;
                Ok(Instruction::BindLocal {
                    slot: parse_prefixed(line, slot, 'l', "local reference")?,
                    value: parse_register(line, value)?,
                })
            }
            "set_local" => {
                let (slot, value) = split_once(line, operands, ", ")?;
                Ok(Instruction::SetLocal {
                    slot: parse_prefixed(line, slot, 'l', "local reference")?,
                    value: parse_register(line, value)?,
                })
            }
            "set_upvalue" => {
                let (slot, value) = split_once(line, operands, ", ")?;
                Ok(Instruction::SetUpvalue {
                    slot: parse_prefixed(line, slot, 'u', "upvalue reference")?,
                    value: parse_register(line, value)?,
                })
            }
            "init_global" => {
                let (slot, value) = split_once(line, operands, ", ")?;
                Ok(Instruction::InitGlobal {
                    slot: parse_prefixed(line, slot, 'g', "global reference")?,
                    value: parse_register(line, value)?,
                })
            }
            "set_global" => {
                let (slot, value) = split_once(line, operands, ", ")?;
                Ok(Instruction::SetGlobal {
                    slot: parse_prefixed(line, slot, 'g', "global reference")?,
                    value: parse_register(line, value)?,
                })
            }
            "print" => Ok(Instruction::Print {
                value: parse_register(line, operands)?,
            }),
            "return" => Ok(Instruction::Return {
                value: parse_register(line, operands)?,
            }),
            "jump" => Ok(Instruction::Jump {
                target: parse_usize(line, operands, "jump target")?,
            }),
            "jump_if_false" => {
                let (condition, target) = split_once(line, operands, ", ")?;
                Ok(Instruction::JumpIfFalse {
                    condition: parse_register(line, condition)?,
                    target: parse_usize(line, target, "jump target")?,
                })
            }
            "jump_if_true" => {
                let (condition, target) = split_once(line, operands, ", ")?;
                Ok(Instruction::JumpIfTrue {
                    condition: parse_register(line, condition)?,
                    target: parse_usize(line, target, "jump target")?,
                })
            }
            "br" => Ok(Instruction::Br {
                target: BlockId(parse_prefixed(line, operands, 'b', "block reference")? as u32),
            }),
            "br_if" => {
                let parts = split_comma_parts(operands);
                if parts.len() != 3 {
                    return Err(ParseError {
                        line,
                        message: "br_if expects three operands".to_string(),
                    });
                }
                Ok(Instruction::BrIf {
                    condition: parse_register(line, parts[0])?,
                    if_true: BlockId(parse_prefixed(line, parts[1], 'b', "block reference")? as u32),
                    if_false: BlockId(parse_prefixed(line, parts[2], 'b', "block reference")? as u32),
                })
            }
            "return_nil" => Ok(Instruction::ReturnNil),
            unknown => Err(ParseError {
                line,
                message: format!("unknown opcode `{}`", unknown),
            }),
        }
    }
}

fn format_instruction(instruction: &Instruction) -> String {
    match instruction {
        Instruction::Constant { dest, constant } => format!("r{} = constant c{}", dest, constant),
        Instruction::MakeFunction { dest, function } => {
            format!("r{} = make_function f{}", dest, function.0 - 1)
        }
        Instruction::Array { dest, elements } => {
            format!("r{} = array {}", dest, format_register_list(elements))
        }
        Instruction::Map { dest, entries } => {
            let parts = entries
                .iter()
                .map(|(key, value)| format!("r{}: r{}", key, value))
                .collect::<Vec<_>>()
                .join(", ");
            format!("r{} = map [{}]", dest, parts)
        }
        Instruction::Struct {
            dest,
            type_name,
            fields,
        } => {
            let parts = fields
                .iter()
                .map(|(name, value)| format!("n{}: r{}", name, value))
                .collect::<Vec<_>>()
                .join(", ");
            match type_name {
                Some(type_name) => format!("r{} = struct n{} {{{}}}", dest, type_name, parts),
                None => format!("r{} = struct {{{}}}", dest, parts),
            }
        }
        Instruction::MakeStruct {
            dest,
            type_id,
            elements,
        } => format!(
            "r{} = make_struct t{} {}",
            dest,
            type_id.0,
            format_register_list(elements)
        ),
        Instruction::StructGet {
            dest,
            object,
            type_id,
            slot,
        } => format!("r{} = struct_get r{}, t{}, {}", dest, object, type_id.0, slot),
        Instruction::StructSet {
            dest,
            object,
            type_id,
            slot,
            value,
        } => format!(
            "r{} = struct_set r{}, t{}, {}, r{}",
            dest, object, type_id.0, slot, value
        ),
        Instruction::Variant {
            dest,
            enum_name,
            variant_name,
            payload,
        } => format!(
            "r{} = variant n{}.n{} {}",
            dest,
            enum_name,
            variant_name,
            format_register_list(payload)
        ),
        Instruction::MakeVariant {
            dest,
            type_id,
            variant_id,
            payload,
        } => format!(
            "r{} = make_variant t{}, v{} {}",
            dest,
            type_id.0,
            variant_id.0,
            format_register_list(payload)
        ),
        Instruction::IsVariant {
            dest,
            value,
            type_id,
            variant_id,
        } => format!(
            "r{} = is_variant r{}, t{}, v{}",
            dest, value, type_id.0, variant_id.0
        ),
        Instruction::VariantGet {
            dest,
            value,
            type_id,
            variant_id,
            index,
        } => format!(
            "r{} = variant_get r{}, t{}, v{}, {}",
            dest, value, type_id.0, variant_id.0, index
        ),
        Instruction::VariantTag {
            dest,
            value,
            enum_name,
            variant_name,
        } => format!(
            "r{} = variant_tag r{} n{}.n{}",
            dest, value, enum_name, variant_name
        ),
        Instruction::VariantField { dest, value, index } => {
            format!("r{} = variant_field r{} {}", dest, value, index)
        }
        Instruction::Move { dest, source } => format!("r{} = move r{}", dest, source),
        Instruction::LoadVar { dest, name } => format!("r{} = load_var n{}", dest, name),
        Instruction::StoreVar { name, value } => format!("store_var n{}, r{}", name, value),
        Instruction::AssignVar { name, value } => format!("assign_var n{}, r{}", name, value),
        Instruction::LoadLocal { dest, slot } => format!("r{} = load_local l{}", dest, slot),
        Instruction::BindLocal { slot, value } => format!("bind_local l{}, r{}", slot, value),
        Instruction::SetLocal { slot, value } => format!("set_local l{}, r{}", slot, value),
        Instruction::LoadUpvalue { dest, slot } => format!("r{} = load_upvalue u{}", dest, slot),
        Instruction::SetUpvalue { slot, value } => format!("set_upvalue u{}, r{}", slot, value),
        Instruction::LoadGlobal { dest, slot } => format!("r{} = load_global g{}", dest, slot),
        Instruction::InitGlobal { slot, value } => format!("init_global g{}, r{}", slot, value),
        Instruction::SetGlobal { slot, value } => format!("set_global g{}, r{}", slot, value),
        Instruction::Call {
            dest,
            callee,
            arguments,
        } => format!(
            "r{} = call r{} {}",
            dest,
            callee,
            format_register_list(arguments)
        ),
        Instruction::NativeCall {
            dest,
            name,
            arguments,
        } => format!(
            "r{} = native_call n{} {}",
            dest,
            name,
            format_register_list(arguments)
        ),
        Instruction::CallNative {
            dest,
            native,
            arguments,
        } => format!(
            "r{} = call_native i{} {}",
            dest,
            native.0,
            format_register_list(arguments)
        ),
        Instruction::Index {
            dest,
            collection,
            index,
        } => format!("r{} = index r{}, r{}", dest, collection, index),
        Instruction::AssignIndex {
            dest,
            collection,
            index,
            value,
        } => format!(
            "r{} = assign_index r{}, r{}, r{}",
            dest, collection, index, value
        ),
        Instruction::Field { dest, object, name } => {
            format!("r{} = field r{}, n{}", dest, object, name)
        }
        Instruction::AssignField {
            dest,
            object,
            name,
            value,
        } => format!(
            "r{} = assign_field r{}, n{}, r{}",
            dest, object, name, value
        ),
        Instruction::Len { dest, value } => format!("r{} = len r{}", dest, value),
        Instruction::AssertArray { dest, value } => format!("r{} = assert_array r{}", dest, value),
        Instruction::AssertNumber {
            dest,
            value,
            message,
        } => {
            format!("r{} = assert_number r{}, n{}", dest, value, message)
        }
        Instruction::Print { value } => format!("print r{}", value),
        Instruction::Return { value } => format!("return r{}", value),
        Instruction::Negate { dest, value } => format!("r{} = negate r{}", dest, value),
        Instruction::Not { dest, value } => format!("r{} = not r{}", dest, value),
        Instruction::Add { dest, left, right } => format!("r{} = add r{}, r{}", dest, left, right),
        Instruction::Subtract { dest, left, right } => {
            format!("r{} = subtract r{}, r{}", dest, left, right)
        }
        Instruction::Multiply { dest, left, right } => {
            format!("r{} = multiply r{}, r{}", dest, left, right)
        }
        Instruction::Divide { dest, left, right } => {
            format!("r{} = divide r{}, r{}", dest, left, right)
        }
        Instruction::Equal { dest, left, right } => {
            format!("r{} = equal r{}, r{}", dest, left, right)
        }
        Instruction::NotEqual { dest, left, right } => {
            format!("r{} = not_equal r{}, r{}", dest, left, right)
        }
        Instruction::Greater { dest, left, right } => {
            format!("r{} = greater r{}, r{}", dest, left, right)
        }
        Instruction::GreaterEqual { dest, left, right } => {
            format!("r{} = greater_equal r{}, r{}", dest, left, right)
        }
        Instruction::Less { dest, left, right } => {
            format!("r{} = less r{}, r{}", dest, left, right)
        }
        Instruction::LessEqual { dest, left, right } => {
            format!("r{} = less_equal r{}, r{}", dest, left, right)
        }
        Instruction::Jump { target } => format!("jump {}", target),
        Instruction::JumpIfFalse { condition, target } => {
            format!("jump_if_false r{}, {}", condition, target)
        }
        Instruction::JumpIfTrue { condition, target } => {
            format!("jump_if_true r{}, {}", condition, target)
        }
        Instruction::BlockStart { id } => format!("block b{}:\n", id.0),
        Instruction::Br { target } => format!("br b{}", target.0),
        Instruction::BrIf {
            condition,
            if_true,
            if_false,
        } => format!("br_if r{}, b{}, b{}", condition, if_true.0, if_false.0),
        Instruction::ReturnNil => "return_nil".to_string(),
    }
}

fn parse_binary(
    line: usize,
    dest: usize,
    operands: &str,
    opcode: &str,
) -> Result<Instruction, ParseError> {
    let (left, right) = parse_two_registers(line, operands)?;
    match opcode {
        "add" => Ok(Instruction::Add { dest, left, right }),
        "subtract" => Ok(Instruction::Subtract { dest, left, right }),
        "multiply" => Ok(Instruction::Multiply { dest, left, right }),
        "divide" => Ok(Instruction::Divide { dest, left, right }),
        "equal" => Ok(Instruction::Equal { dest, left, right }),
        "not_equal" => Ok(Instruction::NotEqual { dest, left, right }),
        "greater" => Ok(Instruction::Greater { dest, left, right }),
        "greater_equal" => Ok(Instruction::GreaterEqual { dest, left, right }),
        "less" => Ok(Instruction::Less { dest, left, right }),
        "less_equal" => Ok(Instruction::LessEqual { dest, left, right }),
        _ => unreachable!("validated binary opcode"),
    }
}

fn parse_two_registers(line: usize, text: &str) -> Result<(usize, usize), ParseError> {
    let (left, right) = split_once(line, text, ", ")?;
    Ok((parse_register(line, left)?, parse_register(line, right)?))
}

fn parse_register(line: usize, text: &str) -> Result<usize, ParseError> {
    parse_prefixed(line, text, 'r', "register reference")
}

fn parse_constant_ref(line: usize, text: &str) -> Result<usize, ParseError> {
    parse_prefixed(line, text, 'c', "constant reference")
}

fn parse_name_ref(line: usize, text: &str) -> Result<usize, ParseError> {
    parse_prefixed(line, text, 'n', "name reference")
}

fn parse_function_ref(line: usize, text: &str) -> Result<usize, ParseError> {
    parse_prefixed(line, text, 'f', "function reference")
}

fn parse_prefixed(
    line: usize,
    text: &str,
    prefix: char,
    description: &str,
) -> Result<usize, ParseError> {
    let Some(rest) = text.strip_prefix(prefix) else {
        return Err(ParseError {
            line,
            message: format!("expected {}", description),
        });
    };
    parse_usize(line, rest, description)
}

fn parse_usize(line: usize, text: &str, description: &str) -> Result<usize, ParseError> {
    if text.is_empty() {
        return Err(ParseError {
            line,
            message: format!("expected {}", description),
        });
    }
    text.parse::<usize>().map_err(|_| ParseError {
        line,
        message: format!("expected {}", description),
    })
}

fn parse_register_list(line: usize, text: &str) -> Result<Vec<usize>, ParseError> {
    if !text.starts_with('[') || !text.ends_with(']') {
        return Err(ParseError {
            line,
            message: "expected register list".to_string(),
        });
    }
    let inner = &text[1..text.len() - 1];
    if inner.is_empty() {
        return Ok(Vec::new());
    }
    inner
        .split(", ")
        .map(|part| parse_register(line, part))
        .collect()
}

fn parse_map_entries(line: usize, text: &str) -> Result<Vec<(usize, usize)>, ParseError> {
    if !text.starts_with('[') || !text.ends_with(']') {
        return Err(ParseError {
            line,
            message: "expected map entry list".to_string(),
        });
    }
    let inner = &text[1..text.len() - 1];
    if inner.is_empty() {
        return Ok(Vec::new());
    }
    let mut entries = Vec::new();
    for part in split_comma_parts(inner) {
        let (key, value) = split_once(line, part, ": ")?;
        entries.push((parse_register(line, key)?, parse_register(line, value)?));
    }
    Ok(entries)
}

fn parse_struct_fields(line: usize, text: &str) -> Result<Vec<(usize, usize)>, ParseError> {
    if !text.starts_with('{') || !text.ends_with('}') {
        return Err(ParseError {
            line,
            message: "struct fields must be wrapped in braces".to_string(),
        });
    }
    let inner = &text[1..text.len() - 1];
    if inner.is_empty() {
        return Ok(Vec::new());
    }
    let mut fields = Vec::new();
    for part in split_comma_parts(inner) {
        let (name, value) = split_once(line, part, ": ")?;
        fields.push((parse_name_ref(line, name)?, parse_register(line, value)?));
    }
    Ok(fields)
}

fn parse_optional_struct_type_name<'a>(
    line: usize,
    text: &'a str,
) -> Result<(Option<usize>, &'a str), ParseError> {
    let trimmed = text.trim();
    if trimmed.starts_with('{') {
        return Ok((None, trimmed));
    }
    let Some((name_text, rest)) = trimmed.split_once(' ') else {
        return Err(ParseError {
            line,
            message: "struct expects fields".to_string(),
        });
    };
    if !rest.trim_start().starts_with('{') {
        return Err(ParseError {
            line,
            message: "struct type name must be followed by fields".to_string(),
        });
    }
    Ok((Some(parse_name_ref(line, name_text)?), rest.trim_start()))
}

fn format_register_list(registers: &[usize]) -> String {
    let inner = registers
        .iter()
        .map(|reg| format!("r{}", reg))
        .collect::<Vec<_>>()
        .join(", ");
    format!("[{}]", inner)
}

fn split_once<'a>(
    line: usize,
    text: &'a str,
    delimiter: &str,
) -> Result<(&'a str, &'a str), ParseError> {
    text.split_once(delimiter).ok_or_else(|| ParseError {
        line,
        message: format!("expected `{}`", delimiter.trim()),
    })
}

fn split_opcode(text: &str) -> (&str, &str) {
    text.split_once(' ').unwrap_or((text, ""))
}

fn split_comma_parts(text: &str) -> Vec<&str> {
    text.split(", ").collect()
}

fn parse_wrapped_usize(
    line: usize,
    text: &str,
    prefix: &str,
    suffix: &str,
    description: &str,
) -> Result<usize, ParseError> {
    let Some(rest) = text.strip_prefix(prefix) else {
        return Err(ParseError {
            line,
            message: format!("expected {}", description),
        });
    };
    let Some(value) = rest.strip_suffix(suffix) else {
        return Err(ParseError {
            line,
            message: format!("expected {}", description),
        });
    };
    parse_usize(line, value, description)
}

fn parse_function_header(
    line: usize,
    text: &str,
) -> Result<(usize, String, usize, usize), ParseError> {
    let Some(rest) = text.strip_prefix("function ") else {
        return Err(ParseError {
            line,
            message: "expected function section".to_string(),
        });
    };
    let (function_ref, rest) = split_once(line, rest, " ")?;
    let index = parse_function_ref(line, function_ref)?;
    let Some(rest) = rest.strip_prefix("name=") else {
        return Err(ParseError {
            line,
            message: "expected function name".to_string(),
        });
    };
    let (name, rest) = parse_string_prefix(line, rest)?;
    let Some(rest) = rest.strip_prefix(" arity=") else {
        return Err(ParseError {
            line,
            message: "expected function arity".to_string(),
        });
    };
    let (arity_text, rest) = split_once(line, rest, " ")?;
    let arity = parse_usize(line, arity_text, "function arity")?;
    let registers = parse_wrapped_usize(line, rest, "registers=", ":", "function registers")?;
    Ok((index, name, arity, registers))
}

fn parse_param(line: usize, text: &str) -> Result<(usize, String), ParseError> {
    let Some(rest) = text.strip_prefix("param ") else {
        return Err(ParseError {
            line,
            message: "expected param".to_string(),
        });
    };
    let (index, value) = split_once(line, rest, " = ")?;
    Ok((
        parse_usize(line, index, "param index")?,
        parse_string_full(line, value)?,
    ))
}

fn parse_string_full(line: usize, text: &str) -> Result<String, ParseError> {
    let (value, rest) = parse_string_prefix(line, text)?;
    if rest.is_empty() {
        Ok(value)
    } else {
        Err(ParseError {
            line,
            message: "unexpected characters after string".to_string(),
        })
    }
}

fn parse_string_prefix<'a>(line: usize, text: &'a str) -> Result<(String, &'a str), ParseError> {
    let mut chars = text.char_indices();
    if chars.next().map(|(_, ch)| ch) != Some('"') {
        return Err(ParseError {
            line,
            message: "expected string literal".to_string(),
        });
    }
    let mut value = String::new();
    let mut escaped = false;
    for (index, ch) in chars {
        if escaped {
            match ch {
                '\\' => value.push('\\'),
                '"' => value.push('"'),
                'n' => value.push('\n'),
                'r' => value.push('\r'),
                't' => value.push('\t'),
                other => {
                    return Err(ParseError {
                        line,
                        message: format!("unsupported escape `\\{}`", other),
                    })
                }
            }
            escaped = false;
        } else if ch == '\\' {
            escaped = true;
        } else if ch == '"' {
            return Ok((value, &text[index + ch.len_utf8()..]));
        } else {
            value.push(ch);
        }
    }
    Err(ParseError {
        line,
        message: "unterminated string literal".to_string(),
    })
}

fn quote_string(value: &str) -> String {
    let mut out = String::from("\"");
    for ch in value.chars() {
        match ch {
            '\\' => out.push_str("\\\\"),
            '"' => out.push_str("\\\""),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            other => out.push(other),
        }
    }
    out.push('"');
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_minimal_program() {
        let source = "cdbc 0.1\n\nconstants:\n\nnames:\n\nmain registers=1:\n  print r0\n";
        let program = parse_program(source).expect("parse minimal program");
        assert_eq!(
            format_program(&program),
            "cdbc 0.2\n\nconstants:\n\nnames:\n\nmain registers=1:\n  print r0\n"
        );
    }

    #[test]
    fn parses_and_formats_string_escapes() {
        let source = "cdbc 0.1\n\nconstants:\n  c0 = string \"a\\\\b\\\"c\\n\\r\\t\"\n\nnames:\n  n0 = \"x\\\\y\"\n\nmain registers=1:\n  r0 = constant c0\n";
        let program = parse_program(source).expect("parse escaped strings");
        assert_eq!(
            format_program(&program),
            "cdbc 0.2\n\nconstants:\n  c0 = string \"a\\\\b\\\"c\\n\\r\\t\"\n\nnames:\n  n0 = \"x\\\\y\"\n\nmain registers=1:\n  r0 = constant c0\n"
        );
    }

    #[test]
    fn rejects_bad_header() {
        let error = parse_program("nope\n").expect_err("bad header should fail");
        assert_eq!(error.line, 1);
    }

    #[test]
    fn rejects_unknown_opcode() {
        let source = "cdbc 0.1\n\nconstants:\n\nnames:\n\nmain registers=2:\n  r0 = mystery r1\n";
        let error = parse_program(source).expect_err("unknown opcode should fail");
        assert!(error.message.contains("unknown opcode `mystery`"));
    }

    #[test]
    fn rejects_invalid_bytecode_references_before_execution() {
        let cases = [
            (
                "destination register",
                "cdbc 0.1\n\nconstants:\n  c0 = nil\n\nnames:\n\nmain registers=1:\n  r1 = constant c0\n",
                "destination register r1 out of range",
            ),
            (
                "constant reference",
                "cdbc 0.1\n\nconstants:\n  c0 = nil\n\nnames:\n\nmain registers=1:\n  r0 = constant c1\n",
                "constant c1 out of range",
            ),
            (
                "name reference",
                "cdbc 0.1\n\nconstants:\n\nnames:\n\nmain registers=1:\n  r0 = load_var n0\n",
                "name n0 out of range",
            ),
            (
                "function reference",
                "cdbc 0.1\n\nconstants:\n\nnames:\n\nmain registers=1:\n  r0 = make_function f0\n",
                "function f0 out of range",
            ),
            (
                "jump target",
                "cdbc 0.1\n\nconstants:\n\nnames:\n\nmain registers=1:\n  jump 2\n",
                "jump target 2 out of range",
            ),
            (
                "native capability",
                "cdbc 0.1\n\nconstants:\n\nnames:\n  n0 = \"not_native\"\n\nmain registers=1:\n  r0 = native_call n0 []\n",
                "unsupported native function `not_native`",
            ),
            (
                "number constant",
                "cdbc 0.1\n\nconstants:\n  c0 = number not-a-number\n\nnames:\n\nmain registers=0:\n",
                "constant c0 is not a valid number literal",
            ),
            (
                "empty debug module identity",
                "cdbc 0.1\n\nconstants:\n\nnames:\n\nmain registers=0:\n\ndebug_sources:\n  s0 module=\"\" path=\"demo.cd\" text=\"\"\n",
                "debug source s0 has an empty module identity",
            ),
        ];
        for (name, source, expected) in cases {
            let error = parse_program(source).expect_err(name);
            assert!(
                error.message.contains(expected),
                "{}: {}",
                name,
                error.message
            );
        }
    }

    #[test]
    fn rejects_malformed_block_bodies_before_execution() {
        let cases = [
            (
                "undefined register",
                "cdbc 0.2\n\nconstants:\n\nnames:\n\nmain registers=2:\nblock b0:\n  print r1\n  return_nil\n",
                "reads undefined register r1",
            ),
            (
                "unbound local",
                "cdbc 0.2\n\nconstants:\n\nnames:\n\nmain registers=0:\nblock b0:\n  return_nil\n\nfunction f0 name=\"f\" arity=0 registers=1:\nblock b0:\n  r0 = load_local l0\n  return r0\n",
                "reads unbound local l0",
            ),
            (
                "invalid block target",
                "cdbc 0.2\n\nconstants:\n  c0 = number 1\n\nnames:\n\nmain registers=1:\nblock b0:\n  r0 = constant c0\n  br b3\n",
                "branches to invalid block b3",
            ),
            (
                "missing terminator",
                "cdbc 0.2\n\nconstants:\n  c0 = number 1\n\nnames:\n\nmain registers=1:\nblock b0:\n  r0 = constant c0\n",
                "block b0 is missing a terminator",
            ),
            (
                "invalid global upvalue source",
                "cdbc 0.2\n\nconstants:\n\nnames:\n\nmain registers=0:\nblock b0:\n  return_nil\n\nfunction f0 name=\"f\" arity=0 registers=0:\n  upvalue u0 = global g9\nblock b0:\n  return_nil\n",
                "upvalue u0 global g9 out of range",
            ),
            (
                "invalid native arity",
                "cdbc 0.2\n\nconstants:\n  c0 = number 1\n\nnames:\n  n0 = \"push\"\n\nmain registers=2:\nblock b0:\n  r0 = constant c0\n  r1 = native_call n0 [r0]\n  return_nil\n",
                "native `push` expects 2 to 2 arguments, got 1",
            ),
            (
                "invalid function id",
                "cdbc 0.2\n\nconstants:\n\nnames:\n\nmain registers=1:\nblock b0:\n  r0 = make_function f1\n  return_nil\n",
                "function f1 out of range",
            ),
            (
                "legacy jump in block body",
                "cdbc 0.2\n\nconstants:\n\nnames:\n\nmain registers=0:\nblock b0:\n  jump 0\n  return_nil\n",
                "block body contains a legacy jump",
            ),
        ];
        for (name, source, expected) in cases {
            let error = parse_program(source).expect_err(name);
            assert!(
                error.message.contains(expected),
                "{}: expected `{}`, got: {}",
                name,
                expected,
                error.message
            );
        }
    }

    #[test]
    fn public_verifier_rejects_invalid_in_memory_program() {
        let artifact = Artifact::Program(Program {
            constants: vec![Constant::Nil],
            names: Vec::new(),
            globals: Vec::new(),
            types: Vec::new(),
            native_imports: Vec::new(),
            functions: vec![Function {
                id: FuncId(0),
                name: "main".to_string(),
                arity: 0,
                local_count: 0,
                upvalues: Vec::new(),
                params: Vec::new(),
                registers: 1,
                instructions: vec![Instruction::Constant {
                    dest: 1,
                    constant: 0,
                }],
                locations: vec![None],
            }],
            entry: FuncId(0),
            debug_sources: Vec::new(),
        });

        let error = verify_artifact(&artifact).expect_err("invalid program must be rejected");
        assert_eq!(error.line, 1);
        assert!(error
            .message
            .contains("destination register r1 out of range"));
    }

    #[test]
    fn parses_all_opcode_shapes() {
        let source = "cdbc 0.1\n\nconstants:\n  c0 = nil\n  c1 = number 1.5\n  c2 = bool true\n  c3 = string \"hello\"\n\nnames:\n  n0 = \"x#0\"\n  n1 = \"Box\"\n\nmain registers=38:\n  r0 = constant c0\n  r1 = make_function f0\n  r2 = array [r0, r1]\n  r3 = map [r0: r1, r1: r2]\n  r4 = struct {n0: r1}\n  r5 = struct n1 {n0: r1}\n  r6 = move r2\n  r7 = load_var n0\n  store_var n0, r6\n  assign_var n0, r6\n  r8 = call r1 [r0, r2]\n  r9 = index r2, r0\n  r10 = assign_index r2, r0, r1\n  r11 = len r2\n  print r11\n  return r11\n  r12 = negate r11\n  r13 = not r11\n  r14 = add r11, r12\n  r15 = subtract r11, r12\n  r16 = multiply r11, r12\n  r17 = divide r11, r12\n  r18 = equal r11, r12\n  r19 = not_equal r11, r12\n  r20 = greater r11, r12\n  r21 = greater_equal r11, r12\n  r22 = less r11, r12\n  r23 = less_equal r11, r12\n  jump 30\n  jump_if_false r23, 31\n  jump_if_true r23, 32\n\nfunction f0 name=\"id\" arity=1 registers=1:\n  param 0 = \"arg#0\"\n  return r0\n";
        let source = source.replace("jump_if_true r23, 32", "jump_if_true r23, 31");
        let program = parse_program(&source).expect("parse all opcode shapes");
        assert_eq!(format_program(&program), source);
    }

    #[test]
    fn parses_debug_sources_and_locations() {
        let source = r#"cdbc 0.1

constants:
  c0 = number 1
  c1 = number 0

names:

main registers=3:
  r0 = constant c0
  r1 = constant c1
  r2 = divide r0, r1
  print r2

debug_sources:
  s0 path="demo.cd" text="print 1 / 0;\n"

debug_locations:
  main 2 = s0:1:7

debug_ranges:
  main 2 = s0:6:11
"#;
        let program = parse_program(source).expect("valid debug artifact");
        assert_eq!(program.debug_sources[0].module, None);
        assert_eq!(program.debug_sources[0].path, "demo.cd");
        assert_eq!(program.debug_sources[0].text, "print 1 / 0;\n");
        assert_eq!(program.functions[0].locations[2].as_ref().unwrap().line, 1);
        assert_eq!(program.functions[0].locations[2].as_ref().unwrap().column, 7);
        assert_eq!(
            program.functions[0].locations[2]
                .as_ref()
                .and_then(|location| location.range.as_ref()),
            Some(&DebugRange {
                source: 0,
                start: 6,
                end: 11,
            })
        );
        assert_eq!(
            format_program(&program),
            source.replace("cdbc 0.1", "cdbc 0.2")
        );
    }

    #[test]
    fn rejects_debug_range_outside_source() {
        let source = r#"cdbc 0.1

constants:

names:

main registers=1:
  print r0

debug_sources:
  s0 path="demo.cd" text="print 1;\n"

debug_locations:
  main 0 = s0:1:1

debug_ranges:
  main 0 = s0:0:999
"#;
        let error = parse_program(source).expect_err("out-of-bounds debug range should fail");
        assert!(error.message.contains("debug range exceeds source length"));
    }

    #[test]
    fn parses_and_formats_debug_source_module_identity() {
        let source = r#"cdbc 0.1

constants:

names:

main registers=0:

debug_sources:
  s0 module="/workspace/demo.cd" path="demo.cd" text="print 1;\n"
"#;
        let program = parse_program(source).expect("valid module-aware debug artifact");
        assert_eq!(
            program.debug_sources[0].module.as_deref(),
            Some("/workspace/demo.cd")
        );
        assert_eq!(
            format_program(&program),
            source.replace("cdbc 0.1", "cdbc 0.2")
        );
    }

    #[test]
    fn rejects_location_for_unknown_instruction() {
        let source = r#"cdbc 0.1

constants:

names:

main registers=1:
  print r0

debug_sources:
  s0 path="demo.cd" text="print 1;\n"

debug_locations:
  main 9 = s0:1:1
"#;
        let error = parse_program(source).expect_err("invalid instruction mapping should fail");
        assert!(error.message.contains("instruction"));
    }

    #[test]
    fn round_trips_module_artifact_envelope() {
        let source = r#"cdbc 0.1

artifact: module

module:
  identity = "/tmp/lib.cd"
  path = "/tmp/lib.cd"
  canonical_path = "/tmp/lib.cd"
  entry = false
  dependencies:

constants:
  c0 = number 1

names:

main registers=1:
  r0 = constant c0
  print r0
"#;
        let artifact = parse_artifact(source).expect("valid module artifact");
        let Artifact::Module(module) = &artifact else {
            panic!("expected module artifact");
        };
        assert_eq!(module.identity, "/tmp/lib.cd");
        assert!(!module.is_entry);
        assert!(module.dependencies.is_empty());
        assert_eq!(
            format_artifact(&artifact),
            source.replace("cdbc 0.1", "cdbc 0.2")
        );
    }

    #[test]
    fn parses_module_dependency_markers() {
        let source = r#"cdbc 0.1

artifact: module

module:
  identity = "entry"
  path = "entry.cd"
  canonical_path = "entry"
  entry = true
  entry_order = 0
  dependencies:
    d0 target="lib" kind=import at=2 requested="./lib.cd"
    d1 target="api" kind=re_export at=4 requested="./api.cd"

constants:

names:

main registers=0:
  print r0
  print r0
  print r0
  print r0
"#;
        let source = source.replace("main registers=0:", "main registers=1:");
        let artifact = parse_artifact(&source).expect("valid dependency markers");
        let Artifact::Module(module) = artifact else {
            panic!("expected module artifact");
        };
        assert_eq!(module.entry_order, Some(0));
        assert_eq!(module.dependencies.len(), 2);
        assert_eq!(module.dependencies[0].kind, ModuleDependencyKind::Import);
        assert_eq!(module.dependencies[0].instruction_offset, 2);
        assert_eq!(module.dependencies[1].kind, ModuleDependencyKind::ReExport);
        assert_eq!(module.dependencies[1].requested_path, "./api.cd");
    }

    #[test]
    fn rejects_module_dependency_offset_out_of_range() {
        let source = r#"cdbc 0.1

artifact: module

module:
  identity = "entry"
  path = "entry.cd"
  canonical_path = "entry"
  entry = true
  entry_order = 0
  dependencies:
    d0 target="lib" kind=import at=1 requested="./lib.cd"

constants:

names:

main registers=0:
"#;
        let error = parse_artifact(source).expect_err("offset should be rejected");
        assert!(error.message.contains("offset out of range"));
    }

    #[test]
    fn parses_and_formats_native_import_table() {
        let source = r#"cdbc 0.2

constants:

names:

native_imports:
  i0 = "print" abi=1
  i1 = "str" abi=1

main registers=2:
  r0 = call_native i0 [r1]
"#;
        let program = parse_program(source).expect("parse native import table");
        assert_eq!(program.native_imports.len(), 2);
        assert_eq!(program.native_imports[0].name, "print");
        assert_eq!(program.native_imports[0].abi, 1);
        assert_eq!(format_program(&program), source);
    }

    #[test]
    fn rejects_invalid_native_import_metadata() {
        let cases = [
            (
                "out of range index",
                "cdbc 0.2\n\nconstants:\n\nnames:\n\nnative_imports:\n  i0 = \"print\" abi=1\n\nmain registers=1:\n  r0 = call_native i1 []\n",
                "native import i1 out of range",
            ),
            (
                "unsupported name",
                "cdbc 0.2\n\nconstants:\n\nnames:\n\nnative_imports:\n  i0 = \"not_native\" abi=1\n\nmain registers=0:\n",
                "unsupported native function `not_native`",
            ),
            (
                "wrong abi version",
                "cdbc 0.2\n\nconstants:\n\nnames:\n\nnative_imports:\n  i0 = \"print\" abi=2\n\nmain registers=0:\n",
                "must declare abi=1",
            ),
            (
                "duplicate name",
                "cdbc 0.2\n\nconstants:\n\nnames:\n\nnative_imports:\n  i0 = \"print\" abi=1\n  i1 = \"print\" abi=1\n\nmain registers=0:\n",
                "duplicates native function `print`",
            ),
            (
                "arity violation",
                "cdbc 0.2\n\nconstants:\n  c0 = nil\n\nnames:\n\nnative_imports:\n  i0 = \"print\" abi=1\n\nmain registers=3:\nblock b0:\n  r0 = constant c0\n  r1 = constant c0\n  r2 = call_native i0 [r0, r1]\n  return_nil\n",
                "native `print` expects 1 to 1 arguments, got 2",
            ),
        ];
        for (name, source, expected) in cases {
            let error = parse_program(source).expect_err(name);
            assert!(
                error.message.contains(expected),
                "{}: {}",
                name,
                error.message
            );
        }
    }
}
