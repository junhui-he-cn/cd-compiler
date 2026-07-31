use compiler_design_vm::bytecode::{
    Constant, DebugLocation, DebugRange, DebugSource, Function, FunctionBody, Instruction,
};
use compiler_design_vm::{
    format_artifact, link_modules_checked, link_modules_with_report, parse_artifact,
    parse_artifact_checked, verify_artifact, verify_module_artifact, verify_program_checked,
    Artifact, ArtifactErrorKind, DebugControl, DebugHook, DebugPause, LinkErrorKind,
    ModuleArtifact, ModuleDependency, ModuleDependencyKind, Program, ResourceKind, RunConfig,
    RuntimeErrorKind, TraceEventKind, ARTIFACT_FORMAT_FAMILY,
    ARTIFACT_FORMAT_VERSION, LIBRARY_API_VERSION, VM,
};
use std::cell::RefCell;
use std::rc::Rc;

fn print_program() -> Program {
    Program {
        constants: vec![Constant::Number("7".to_string())],
        names: Vec::new(),
        main: FunctionBody {
            registers: 1,
            instructions: vec![
                Instruction::Constant {
                    dest: 0,
                    constant: 0,
                },
                Instruction::Print { value: 0 },
            ],
            locations: vec![None, None],
        },
        functions: Vec::new(),
        debug_sources: Vec::new(),
    }
}

fn profile_program() -> Program {
    let range = DebugRange {
        source: 0,
        start: 0,
        end: 8,
    };
    let location = || {
        Some(DebugLocation {
            source: 0,
            line: 1,
            column: 1,
            range: Some(range.clone()),
        })
    };
    Program {
        constants: vec![Constant::Number("7".to_string())],
        names: vec!["value".to_string(), "str".to_string()],
        main: FunctionBody {
            registers: 4,
            instructions: vec![
                Instruction::MakeFunction { dest: 0, function: 0 },
                Instruction::Constant { dest: 1, constant: 0 },
                Instruction::Call {
                    dest: 2,
                    callee: 0,
                    arguments: vec![1],
                },
                Instruction::NativeCall {
                    dest: 3,
                    name: 1,
                    arguments: vec![2],
                },
                Instruction::Print { value: 3 },
            ],
            locations: (0..5).map(|_| location()).collect(),
        },
        functions: vec![Function {
            index: 0,
            name: "identity".to_string(),
            arity: 1,
            registers: 1,
            params: vec!["value".to_string()],
            instructions: vec![
                Instruction::LoadVar { dest: 0, name: 0 },
                Instruction::Return { value: 0 },
            ],
            locations: (0..2).map(|_| location()).collect(),
        }],
        debug_sources: vec![DebugSource {
            module: None,
            path: "profile.cd".to_string(),
            text: "print 7;\n".to_string(),
        }],
    }
}

fn profile_failure_program() -> Program {
    Program {
        constants: vec![
            Constant::String("ok".to_string()),
            Constant::Number("1".to_string()),
            Constant::Number("0".to_string()),
        ],
        names: Vec::new(),
        main: FunctionBody {
            registers: 3,
            instructions: vec![
                Instruction::Constant { dest: 0, constant: 0 },
                Instruction::Print { value: 0 },
                Instruction::Constant { dest: 1, constant: 1 },
                Instruction::Constant { dest: 2, constant: 2 },
                Instruction::Divide {
                    dest: 2,
                    left: 1,
                    right: 2,
                },
            ],
            locations: vec![None; 5],
        },
        functions: Vec::new(),
        debug_sources: Vec::new(),
    }
}

fn runtime_diagnostic_program() -> Program {
    let location = |line: usize, column: usize| {
        Some(DebugLocation {
            source: 0,
            line,
            column,
            range: None,
        })
    };
    Program {
        constants: vec![Constant::Number("1".to_string()), Constant::Number("0".to_string())],
        names: Vec::new(),
        main: FunctionBody {
            registers: 2,
            instructions: vec![
                Instruction::MakeFunction { dest: 0, function: 0 },
                Instruction::Call {
                    dest: 1,
                    callee: 0,
                    arguments: Vec::new(),
                },
            ],
            locations: vec![location(2, 1), location(2, 1)],
        },
        functions: vec![Function {
            index: 0,
            name: "fail".to_string(),
            arity: 0,
            registers: 3,
            params: Vec::new(),
            instructions: vec![
                Instruction::Constant { dest: 0, constant: 0 },
                Instruction::Constant { dest: 1, constant: 1 },
                Instruction::Divide {
                    dest: 2,
                    left: 0,
                    right: 1,
                },
                Instruction::Return { value: 2 },
            ],
            locations: vec![location(1, 21), location(1, 25), location(1, 21), location(1, 14)],
        }],
        debug_sources: vec![DebugSource {
            module: None,
            path: "diagnostic.cd".to_string(),
            text: "fun fail() { return 1 / 0; }\nfail();\n".to_string(),
        }],
    }
}

#[test]
fn library_api_parses_verifies_runs_and_traces_programs() {
    let source = format_artifact(&Artifact::Program(print_program()));
    let artifact =
        parse_artifact(&source).expect("library parser should accept its formatter output");
    verify_artifact(&artifact).expect("library verifier should accept parsed artifacts");
    let Artifact::Program(program) = artifact else {
        panic!("expected linked program artifact");
    };

    let output = VM::with_config(&program, RunConfig::unlimited())
        .run()
        .expect("library VM should run the verified program");
    assert_eq!(output, "7\n");

    let trace = VM::with_config(&program, RunConfig::unlimited()).trace();
    assert_eq!(trace.result.expect("library trace should succeed"), "7\n");
    assert!(trace
        .events
        .iter()
        .any(|event| event.kind == TraceEventKind::Output));
}

#[test]
fn library_api_profiles_functions_natives_ranges_and_output() {
    let profiled = VM::with_config(&profile_program(), RunConfig::unlimited()).profile();
    assert_eq!(profiled.result.expect("profiled program should run"), "7\n");
    assert_eq!(profiled.report.instruction_count, 7);
    assert_eq!(profiled.report.output_bytes, 2);
    assert_eq!(profiled.report.tracked_heap_allocations, 6);
    assert_eq!(profiled.report.tracked_heap_peak_live, 6);
    assert_eq!(profiled.report.functions[0].name, "main");
    assert_eq!(profiled.report.functions[0].calls, 1);
    assert_eq!(profiled.report.functions[0].instructions, 5);
    assert_eq!(profiled.report.functions[1].name, "identity");
    assert_eq!(profiled.report.functions[1].calls, 1);
    assert_eq!(profiled.report.functions[1].instructions, 2);
    assert_eq!(
        profiled.report.natives,
        vec![compiler_design_vm::ProfileNative {
            name: "str".to_string(),
            calls: 1,
        }]
    );
    assert_eq!(
        profiled.report.source_ranges,
        vec![compiler_design_vm::ProfileSourceRange {
            range: DebugRange {
                source: 0,
                start: 0,
                end: 8,
            },
            hits: 7,
        }]
    );
}

#[test]
fn library_api_returns_partial_profile_on_runtime_failure() {
    let profiled = VM::with_config(&profile_failure_program(), RunConfig::unlimited()).profile();
    let error = profiled.result.expect_err("profiled program should fail");
    assert_eq!(error.kind, compiler_design_vm::RuntimeErrorKind::Runtime);
    assert_eq!(profiled.report.instruction_count, 5);
    assert_eq!(profiled.report.output_bytes, 3);
    assert_eq!(profiled.report.tracked_heap_allocations, 3);
    assert_eq!(profiled.report.tracked_heap_peak_live, 3);
    assert_eq!(profiled.report.functions[0].calls, 1);
    assert_eq!(profiled.report.functions[0].instructions, 5);
}

#[test]
fn library_api_exposes_stable_diagnostic_kinds_and_runtime_context() {
    for (kind, label) in [
        (ArtifactErrorKind::Parse, "parse"),
        (
            ArtifactErrorKind::UnsupportedVersion,
            "unsupported_version",
        ),
        (ArtifactErrorKind::Verification, "verification"),
    ] {
        assert_eq!(kind.as_str(), label);
    }
    for (kind, label) in [
        (LinkErrorKind::InvalidModule, "invalid_module"),
        (
            LinkErrorKind::DuplicateModuleIdentity,
            "duplicate_module_identity",
        ),
        (LinkErrorKind::EmptyModuleSet, "empty_module_set"),
        (LinkErrorKind::MissingEntryModule, "missing_entry_module"),
        (LinkErrorKind::InvalidEntryOrder, "invalid_entry_order"),
        (LinkErrorKind::MissingDependency, "missing_dependency"),
        (LinkErrorKind::InvalidDependency, "invalid_dependency"),
        (LinkErrorKind::DependencyCycle, "dependency_cycle"),
        (LinkErrorKind::InvalidInstruction, "invalid_instruction"),
        (LinkErrorKind::Overflow, "overflow"),
        (LinkErrorKind::InvalidLinkedProgram, "invalid_linked_program"),
    ] {
        assert_eq!(kind.as_str(), label);
    }
    assert_eq!(RuntimeErrorKind::Runtime.as_str(), "runtime");
    assert_eq!(RuntimeErrorKind::Resource(ResourceKind::OutputBytes).as_str(), "resource");
    assert_eq!(RuntimeErrorKind::Cancelled.as_str(), "cancelled");
    assert_eq!(RuntimeErrorKind::DebuggerQuit.as_str(), "debugger_quit");
    assert_eq!(ResourceKind::OutputBytes.as_str(), "output bytes");

    let error = VM::with_config(&runtime_diagnostic_program(), RunConfig::unlimited())
        .run()
        .expect_err("diagnostic fixture should fail");
    assert_eq!(error.kind, RuntimeErrorKind::Runtime);
    assert_eq!(error.resource_limit, None);
    assert_eq!(error.location.as_ref().map(|location| location.source), Some(0));
    assert_eq!(error.sources[0].path, "diagnostic.cd");
    assert!(error.stack.iter().any(|frame| frame.function == "fail"));
    assert!(error.stack.iter().any(|frame| frame.function == "main"));
    assert!(error.to_string().contains("diagnostic.cd:1:21"));
}

#[test]
fn library_api_links_modules_and_keeps_vm_instances_independent() {
    let module = ModuleArtifact {
        identity: "entry".to_string(),
        path: "entry.cdbc".to_string(),
        canonical_path: "entry.cdbc".to_string(),
        is_entry: true,
        entry_order: Some(0),
        dependencies: Vec::new(),
        program: print_program(),
    };
    verify_module_artifact(&module).expect("library verifier should accept module artifacts");
    let linked =
        link_modules_with_report(vec![module]).expect("library linker should link one entry");
    assert_eq!(linked.report.entry_module_identities, vec!["entry"]);
    assert_eq!(linked.report.input_instruction_count, 2);
    assert_eq!(linked.report.linked_instruction_count, 2);
    let linked = linked.program;

    let first = VM::with_config(&linked, RunConfig::unlimited())
        .run()
        .expect("first VM should run");
    let second = VM::with_config(&linked, RunConfig::unlimited())
        .run()
        .expect("second VM should run independently");
    assert_eq!(first, "7\n");
    assert_eq!(second, first);
}

#[test]
fn library_api_exposes_versions_and_typed_artifact_errors() {
    assert_eq!(LIBRARY_API_VERSION, "0.1");
    assert_eq!(ARTIFACT_FORMAT_FAMILY, "cdbc");
    assert_eq!(ARTIFACT_FORMAT_VERSION, "0.1");

    let version_error = parse_artifact_checked("cdbc 9.9\n").expect_err("version must be rejected");
    assert_eq!(version_error.kind, ArtifactErrorKind::UnsupportedVersion);
    assert_eq!(version_error.line, 1);
    assert!(version_error.message.contains("cdbc 0.1"));

    let invalid_program = Program {
        constants: Vec::new(),
        names: Vec::new(),
        main: FunctionBody {
            registers: 1,
            instructions: vec![Instruction::Constant {
                dest: 0,
                constant: 0,
            }],
            locations: vec![None],
        },
        functions: Vec::new(),
        debug_sources: Vec::new(),
    };
    let verification_error = verify_program_checked(&invalid_program)
        .expect_err("invalid references must be classified as verification errors");
    assert_eq!(verification_error.kind, ArtifactErrorKind::Verification);
    assert_eq!(verification_error.line, 1);
}

#[test]
fn library_api_exposes_typed_link_errors_without_changing_display_text() {
    let entry = ModuleArtifact {
        identity: "entry".to_string(),
        path: "entry.cdbc".to_string(),
        canonical_path: "entry.cdbc".to_string(),
        is_entry: true,
        entry_order: Some(0),
        dependencies: vec![ModuleDependency {
            identity: "missing".to_string(),
            kind: ModuleDependencyKind::Import,
            instruction_offset: 0,
            requested_path: "./missing.cd".to_string(),
        }],
        program: print_program(),
    };

    let error = link_modules_checked(vec![entry]).expect_err("missing dependency must fail");
    assert_eq!(error.kind, LinkErrorKind::MissingDependency);
    assert_eq!(error.module_identity.as_deref(), Some("entry"));
    assert_eq!(error.dependency_index, Some(0));
    assert_eq!(
        error.to_string(),
        "module `entry` dependency d0 targets missing module `missing`"
    );

    let compatibility_error = link_modules_with_report(vec![ModuleArtifact {
        identity: "entry".to_string(),
        path: "entry.cdbc".to_string(),
        canonical_path: "entry.cdbc".to_string(),
        is_entry: true,
        entry_order: Some(0),
        dependencies: vec![ModuleDependency {
            identity: "missing".to_string(),
            kind: ModuleDependencyKind::Import,
            instruction_offset: 0,
            requested_path: "./missing.cd".to_string(),
        }],
        program: print_program(),
    }])
    .expect_err("compatibility linker must retain the rejection");
    assert_eq!(compatibility_error, error.to_string());
}

struct QuitAfterFirstPause {
    pauses: Rc<RefCell<Vec<DebugPause>>>,
}

impl DebugHook for QuitAfterFirstPause {
    fn on_instruction(&mut self, pause: DebugPause) -> DebugControl {
        self.pauses.borrow_mut().push(pause);
        DebugControl::Quit
    }
}

#[test]
fn library_api_debug_hook_observes_live_entry_state_before_execution() {
    let pauses = Rc::new(RefCell::new(Vec::new()));
    let debug = VM::with_config(&print_program(), RunConfig::unlimited()).debug(Box::new(
        QuitAfterFirstPause {
            pauses: Rc::clone(&pauses),
        },
    ));

    assert!(debug.quit);
    assert_eq!(debug.result.expect("debugger quit is not a runtime error"), "");
    let pauses = pauses.borrow();
    assert_eq!(pauses.len(), 1);
    assert_eq!(pauses[0].function, "main");
    assert_eq!(pauses[0].instruction, 0);
    assert_eq!(pauses[0].stack.len(), 1);
}
