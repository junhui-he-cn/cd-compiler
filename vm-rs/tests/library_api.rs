use compiler_design_vm::bytecode::{
    Constant, DebugLocation, DebugRange, DebugSource, FuncId, Function, Instruction,
};
use compiler_design_vm::{
    format_artifact, link_modules_checked, link_modules_with_report, parse_artifact,
    parse_artifact_checked, verify_artifact, verify_module_artifact, verify_program_checked,
    Artifact, ArtifactErrorKind, CooperativeDebugHook, CooperativeDebugPause,
    CooperativeProfileReport, CooperativeStep, DebugControl, DebugHook, DebugPause, JoinPoll,
    LinkErrorKind, ModuleArtifact, ModuleDependency, ModuleDependencyKind, Program, ResourceKind,
    RunConfig, RuntimeErrorKind, TaskOutcome, TaskOutputEvent, TaskProfileReport, TaskSpec,
    TaskState, TaskTraceEvent, TraceEventKind, ARTIFACT_FORMAT_FAMILY, ARTIFACT_FORMAT_VERSION,
    LIBRARY_API_VERSION, VM,
};
use std::cell::RefCell;
use std::rc::Rc;

fn print_program() -> Program {
    Program {
        constants: vec![Constant::Number("7".to_string())],
        names: Vec::new(),
        functions: vec![Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
            registers: 1,
            instructions: vec![
                Instruction::Constant {
                    dest: 0,
                    constant: 0,
                },
                Instruction::Print { value: 0 },
            ],
            locations: vec![None, None],
        }],
        entry: FuncId(0),
        debug_sources: Vec::new(),
    }
}

fn cooperative_program() -> Program {
    Program {
        constants: vec![Constant::Number("7".to_string()), Constant::Number("8".to_string())],
        names: Vec::new(),
        functions: vec![Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
            registers: 0,
            instructions: Vec::new(),
            locations: Vec::new(),
        },
            Function {
                id: FuncId(1),
                local_count: 0,
                upvalues: Vec::new(),
                name: "target".to_string(),
                arity: 0,
                registers: 1,
                params: Vec::new(),
                instructions: vec![
                    Instruction::Constant { dest: 0, constant: 0 },
                    Instruction::Return { value: 0 },
                ],
                locations: vec![None; 2],
            },
            Function {
                id: FuncId(2),
                local_count: 0,
                upvalues: Vec::new(),
                name: "waiter".to_string(),
                arity: 0,
                registers: 1,
                params: Vec::new(),
                instructions: vec![
                    Instruction::Constant { dest: 0, constant: 1 },
                    Instruction::Return { value: 0 },
                ],
                locations: vec![None; 2],
            },
        ],
        entry: FuncId(0),
        debug_sources: Vec::new(),
    }
}

fn cooperative_output_program() -> Program {
    Program {
        constants: vec![
            Constant::Number("1".to_string()),
            Constant::Number("2".to_string()),
        ],
        names: Vec::new(),
        functions: vec![Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
            registers: 0,
            instructions: Vec::new(),
            locations: Vec::new(),
        },
            Function {
                id: FuncId(1),
                local_count: 0,
                upvalues: Vec::new(),
                name: "first".to_string(),
                arity: 0,
                registers: 1,
                params: Vec::new(),
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Print { value: 0 },
                    Instruction::Return { value: 0 },
                ],
                locations: vec![None; 3],
            },
            Function {
                id: FuncId(2),
                local_count: 0,
                upvalues: Vec::new(),
                name: "second".to_string(),
                arity: 0,
                registers: 1,
                params: Vec::new(),
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 1,
                    },
                    Instruction::Print { value: 0 },
                    Instruction::Return { value: 0 },
                ],
                locations: vec![None; 3],
            },
        ],
        entry: FuncId(0),
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
        functions: vec![Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
            registers: 4,
            instructions: vec![
                Instruction::MakeFunction { dest: 0, function: FuncId(1) },
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
            Function {
            id: FuncId(1),
            local_count: 0,
            upvalues: Vec::new(),
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
        entry: FuncId(0),
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
        functions: vec![Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
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
        }],
        entry: FuncId(0),
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
        functions: vec![Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
            registers: 2,
            instructions: vec![
                Instruction::MakeFunction { dest: 0, function: FuncId(1) },
                Instruction::Call {
                    dest: 1,
                    callee: 0,
                    arguments: Vec::new(),
                },
            ],
            locations: vec![location(2, 1), location(2, 1)],
        },
            Function {
            id: FuncId(1),
            local_count: 0,
            upvalues: Vec::new(),
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
        entry: FuncId(0),
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
fn library_api_exposes_typed_cooperative_task_results_and_join() {
    let program = cooperative_program();
    let mut session = VM::with_config(&program, RunConfig::unlimited())
        .start_cooperative(1)
        .expect("cooperative session should start");
    let waiter = session
        .spawn(TaskSpec::function(2, Vec::new()))
        .expect("waiter should spawn");
    let target = session
        .spawn(TaskSpec::function(1, Vec::new()))
        .expect("target should spawn");

    assert!(matches!(
        session.join(waiter, target).expect("join should register"),
        JoinPoll::Waiting
    ));
    assert_eq!(session.task_state(waiter), Ok(TaskState::Blocked));
    assert!(matches!(
        session.step().expect("target should yield"),
        CooperativeStep::Dispatched { task_id, .. } if task_id == target
    ));
    assert!(matches!(
        session.step().expect("target should complete"),
        CooperativeStep::Dispatched {
            task_id,
            state: TaskState::Completed,
        } if task_id == target
    ));
    assert!(matches!(
        session.join(waiter, target).expect("join should return target"),
        JoinPoll::Ready(TaskOutcome::Completed(value))
            if matches!(value, compiler_design_vm::value::Value::Number(number) if number == 7.0)
    ));
}

#[test]
fn library_api_exposes_task_attributed_cooperative_output() {
    let program = cooperative_output_program();
    let mut session = VM::with_config(&program, RunConfig::unlimited())
        .start_cooperative(1)
        .expect("cooperative session should start");
    let first = session
        .spawn(TaskSpec::function(1, Vec::new()))
        .expect("first task should spawn");
    let second = session
        .spawn(TaskSpec::function(2, Vec::new()))
        .expect("second task should spawn");

    assert_eq!(
        session
            .run_until_waiting()
            .expect("cooperative output should complete"),
        CooperativeStep::Complete
    );
    assert_eq!(session.take_output(), "1\n2\n");
    assert_eq!(
        session.take_output_events(),
        vec![
            TaskOutputEvent {
                sequence: 0,
                task_id: first,
                text: "1\n".to_string(),
            },
            TaskOutputEvent {
                sequence: 1,
                task_id: second,
                text: "2\n".to_string(),
            },
        ]
    );
}

#[test]
fn library_api_exposes_task_attributed_cooperative_trace() {
    let program = cooperative_output_program();
    let mut session = VM::with_config(&program, RunConfig::unlimited())
        .start_cooperative_trace(1)
        .expect("cooperative trace session should start");
    let first = session
        .spawn(TaskSpec::function(1, Vec::new()))
        .expect("first task should spawn");
    let second = session
        .spawn(TaskSpec::function(2, Vec::new()))
        .expect("second task should spawn");

    session
        .run_until_waiting()
        .expect("cooperative trace should complete");
    let events: Vec<TaskTraceEvent> = session.take_trace_events();
    assert_eq!(
        events
            .iter()
            .map(|event| (event.sequence, event.task_id, event.kind))
            .collect::<Vec<_>>(),
        vec![
            (0, first, TraceEventKind::Enter),
            (1, second, TraceEventKind::Enter),
            (2, first, TraceEventKind::Output),
            (3, second, TraceEventKind::Output),
            (4, first, TraceEventKind::Return),
            (5, first, TraceEventKind::Exit),
            (6, second, TraceEventKind::Return),
            (7, second, TraceEventKind::Exit),
        ]
    );
    assert_eq!(
        session
            .output_events()
            .iter()
            .map(|event| event.sequence)
            .collect::<Vec<_>>(),
        vec![2, 3]
    );
}

#[test]
fn library_api_exposes_task_attributed_cooperative_profile() {
    let program = cooperative_output_program();
    let mut session = VM::with_config(&program, RunConfig::unlimited())
        .start_cooperative_profile(1)
        .expect("cooperative profile session should start");
    let first = session
        .spawn(TaskSpec::function(1, Vec::new()))
        .expect("first task should spawn");
    let second = session
        .spawn(TaskSpec::function(2, Vec::new()))
        .expect("second task should spawn");

    session
        .run_until_waiting()
        .expect("cooperative profile should complete");
    let report: CooperativeProfileReport = session
        .profile_report()
        .expect("profile should be enabled");
    assert_eq!(report.aggregate.instruction_count, 6);
    assert_eq!(report.aggregate.output_bytes, 4);
    let tasks: Vec<TaskProfileReport> = report.tasks;
    assert_eq!(tasks.len(), 2);
    assert_eq!(tasks[0].task_id, first);
    assert_eq!(tasks[0].instruction_count, 3);
    assert_eq!(tasks[0].output_bytes, 2);
    assert_eq!(tasks[1].task_id, second);
    assert_eq!(tasks[1].instruction_count, 3);
    assert_eq!(tasks[1].output_bytes, 2);
}

struct RecordCooperativePauses {
    pauses: Rc<RefCell<Vec<CooperativeDebugPause>>>,
}

impl CooperativeDebugHook for RecordCooperativePauses {
    fn on_instruction(&mut self, pause: CooperativeDebugPause) -> DebugControl {
        self.pauses.borrow_mut().push(pause);
        DebugControl::Continue
    }
}

#[test]
fn library_api_exposes_task_attributed_cooperative_debug_pauses() {
    let program = cooperative_output_program();
    let pauses = Rc::new(RefCell::new(Vec::new()));
    let mut session = VM::with_config(&program, RunConfig::unlimited())
        .start_cooperative_debug(
            1,
            Box::new(RecordCooperativePauses {
                pauses: Rc::clone(&pauses),
            }),
        )
        .expect("cooperative debug session should start");
    let first = session
        .spawn(TaskSpec::function(1, Vec::new()))
        .expect("first task should spawn");
    let second = session
        .spawn(TaskSpec::function(2, Vec::new()))
        .expect("second task should spawn");

    assert!(matches!(
        session.step().expect("first task should execute"),
        CooperativeStep::Dispatched { task_id, .. } if task_id == first
    ));
    let pauses = pauses.borrow();
    assert_eq!(pauses.len(), 1);
    assert_eq!(pauses[0].task_id, first);
    assert_eq!(pauses[0].scheduler.running, first);
    assert_eq!(pauses[0].scheduler.ready, vec![second]);
    assert_eq!(
        pauses[0].scheduler.tasks,
        vec![(first, TaskState::Running), (second, TaskState::Ready)]
    );
    assert!(session.trace_events().is_empty());
}

#[test]
fn library_api_profiles_functions_natives_ranges_and_output() {
    let profiled = VM::with_config(&profile_program(), RunConfig::unlimited()).profile();
    assert_eq!(profiled.result.expect("profiled program should run"), "7\n");
    assert_eq!(profiled.report.instruction_count, 7);
    assert_eq!(profiled.report.output_bytes, 2);
    assert_eq!(profiled.report.tracked_heap_allocations, 6);
    assert_eq!(profiled.report.tracked_heap_peak_live, 6);
    assert!(profiled.report.tracked_heap_estimated_live_bytes > 0);
    assert!(
        profiled.report.tracked_heap_estimated_peak_live_bytes
            >= profiled.report.tracked_heap_estimated_live_bytes
    );
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
    assert!(profiled.report.tracked_heap_estimated_live_bytes > 0);
    assert!(
        profiled.report.tracked_heap_estimated_peak_live_bytes
            >= profiled.report.tracked_heap_estimated_live_bytes
    );
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
        functions: vec![Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
            registers: 1,
            instructions: vec![Instruction::Constant {
                dest: 0,
                constant: 0,
            }],
            locations: vec![None],
        }],
        entry: FuncId(0),
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
