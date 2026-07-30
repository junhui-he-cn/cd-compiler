use compiler_design_vm::bytecode::{Constant, FunctionBody, Instruction};
use compiler_design_vm::{
    format_artifact, link_modules_checked, link_modules_with_report, parse_artifact,
    parse_artifact_checked, verify_artifact, verify_module_artifact, verify_program_checked,
    Artifact, ArtifactErrorKind, DebugControl, DebugHook, DebugPause, LinkErrorKind,
    ModuleArtifact, ModuleDependency, ModuleDependencyKind, Program, RunConfig, TraceEventKind,
    ARTIFACT_FORMAT_FAMILY, ARTIFACT_FORMAT_VERSION, LIBRARY_API_VERSION, VM,
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
