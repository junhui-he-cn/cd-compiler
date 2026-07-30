use compiler_design_vm::bytecode::{Constant, FunctionBody, Instruction};
use compiler_design_vm::{
    format_artifact, link_modules_with_report, parse_artifact, verify_artifact,
    verify_module_artifact, Artifact, ModuleArtifact, Program, RunConfig, TraceEventKind, VM,
};

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
