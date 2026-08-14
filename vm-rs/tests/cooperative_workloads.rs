use compiler_design_vm::bytecode::{Constant, FuncId, Function, Instruction, Program};
use compiler_design_vm::value::Value;
use compiler_design_vm::{
    CooperativeDebugHook, CooperativeDebugPause, CooperativeProfileReport, CooperativeRun,
    CooperativeStep, DebugControl, TaskId, TaskOutputEvent, TaskSpec, TaskTraceEvent, VM,
};
use std::cell::RefCell;
use std::rc::Rc;

#[derive(Clone, Debug, PartialEq, Eq)]
struct ProfileObservation {
    dispatches: usize,
    output: String,
    output_events: Vec<TaskOutputEvent>,
    report: CooperativeProfileReport,
    outcomes: Vec<(usize, String)>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct TraceObservation {
    dispatches: usize,
    output: String,
    output_events: Vec<TaskOutputEvent>,
    trace_events: Vec<TaskTraceEvent>,
    outcomes: Vec<(usize, String)>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct DebugObservation {
    dispatches: usize,
    output: String,
    output_events: Vec<TaskOutputEvent>,
    pauses: Vec<CooperativeDebugPause>,
    outcomes: Vec<(usize, String)>,
    quit: bool,
}

struct CollectDebugPauses {
    pauses: Rc<RefCell<Vec<CooperativeDebugPause>>>,
}

impl CooperativeDebugHook for CollectDebugPauses {
    fn on_instruction(&mut self, pause: CooperativeDebugPause) -> DebugControl {
        self.pauses.borrow_mut().push(pause);
        DebugControl::Continue
    }
}

fn arithmetic_workload() -> Program {
    Program {
        constants: vec![
            Constant::Number("0".to_string()),
            Constant::Number("1".to_string()),
        ],
        names: vec!["limit".to_string()],
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
            name: "arithmetic_worker".to_string(),
            arity: 1,
            registers: 6,
            params: vec!["limit".to_string()],
            instructions: vec![
                Instruction::Constant {
                    dest: 1,
                    constant: 0,
                },
                Instruction::Constant {
                    dest: 2,
                    constant: 0,
                },
                Instruction::Constant {
                    dest: 5,
                    constant: 1,
                },
                Instruction::LoadVar { dest: 3, name: 0 },
                Instruction::Less {
                    dest: 4,
                    left: 1,
                    right: 3,
                },
                Instruction::JumpIfFalse {
                    condition: 4,
                    target: 9,
                },
                Instruction::Add {
                    dest: 2,
                    left: 2,
                    right: 1,
                },
                Instruction::Add {
                    dest: 1,
                    left: 1,
                    right: 5,
                },
                Instruction::Jump { target: 3 },
                Instruction::Print { value: 2 },
                Instruction::Return { value: 2 },
            ],
            locations: vec![None; 11],
        }],
        entry: FuncId(0),
        debug_sources: Vec::new(),
    }
}

fn callback_workload() -> Program {
    Program {
        constants: vec![
            Constant::Number("1".to_string()),
            Constant::Number("2".to_string()),
        ],
        names: vec!["map".to_string(), "item".to_string()],
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
                name: "callback_worker".to_string(),
                arity: 0,
                registers: 5,
                params: Vec::new(),
                instructions: vec![
                    Instruction::Constant {
                        dest: 0,
                        constant: 0,
                    },
                    Instruction::Constant {
                        dest: 1,
                        constant: 1,
                    },
                    Instruction::Array {
                        dest: 2,
                        elements: vec![0, 1],
                    },
                    Instruction::MakeFunction {
                        dest: 3,
                        function: FuncId(2),
                    },
                    Instruction::NativeCall {
                        dest: 4,
                        name: 0,
                        arguments: vec![2, 3],
                    },
                    Instruction::Print { value: 4 },
                    Instruction::Return { value: 4 },
                ],
                locations: vec![None; 7],
            },
            Function {
                id: FuncId(2),
                local_count: 0,
                upvalues: Vec::new(),
                name: "identity_callback".to_string(),
                arity: 1,
                registers: 1,
                params: vec!["item".to_string()],
                instructions: vec![
                    Instruction::LoadVar { dest: 0, name: 1 },
                    Instruction::Return { value: 0 },
                ],
                locations: vec![None; 2],
            },
        ],
        entry: FuncId(0),
        debug_sources: Vec::new(),
    }
}

fn arithmetic_specs() -> Vec<TaskSpec> {
    [4.0, 5.0, 6.0, 7.0]
        .into_iter()
        .map(|limit| TaskSpec::function(1, vec![Value::number(limit)]))
        .collect()
}

fn callback_specs() -> Vec<TaskSpec> {
    vec![
        TaskSpec::function(1, Vec::new()),
        TaskSpec::function(1, Vec::new()),
    ]
}

fn spawn_all(run: &mut CooperativeRun<'_>, specs: &[TaskSpec]) -> Vec<TaskId> {
    specs
        .iter()
        .cloned()
        .map(|spec| run.spawn(spec).expect("workload task should spawn"))
        .collect()
}

fn drive(run: &mut CooperativeRun<'_>) -> usize {
    let mut dispatches = 0;
    loop {
        match run.step().expect("workload dispatch should succeed") {
            CooperativeStep::Dispatched { .. } => dispatches += 1,
            CooperativeStep::Complete => return dispatches,
            CooperativeStep::Waiting => panic!("workload should not block"),
        }
    }
}

fn outcomes(run: &CooperativeRun<'_>) -> Vec<(usize, String)> {
    run.outcomes()
        .expect("workload outcomes should be available")
        .into_iter()
        .map(|(task_id, outcome)| (task_id.index(), format!("{:?}", outcome)))
        .collect()
}

fn profile_observation(program: Program, specs: &[TaskSpec], quantum: usize) -> ProfileObservation {
    let mut run = VM::new(&program)
        .start_cooperative_profile(quantum)
        .expect("profile workload should start");
    spawn_all(&mut run, specs);
    let dispatches = drive(&mut run);
    ProfileObservation {
        dispatches,
        output: run.take_output(),
        output_events: run.output_events().to_vec(),
        report: run.profile_report().expect("profile should be enabled"),
        outcomes: outcomes(&run),
    }
}

fn trace_observation(program: Program, specs: &[TaskSpec], quantum: usize) -> TraceObservation {
    let mut run = VM::new(&program)
        .start_cooperative_trace(quantum)
        .expect("trace workload should start");
    spawn_all(&mut run, specs);
    let dispatches = drive(&mut run);
    TraceObservation {
        dispatches,
        output: run.take_output(),
        output_events: run.output_events().to_vec(),
        trace_events: run.trace_events().to_vec(),
        outcomes: outcomes(&run),
    }
}

fn debug_observation(program: Program, specs: &[TaskSpec], quantum: usize) -> DebugObservation {
    let pauses = Rc::new(RefCell::new(Vec::new()));
    let mut run = VM::new(&program)
        .start_cooperative_debug(
            quantum,
            Box::new(CollectDebugPauses {
                pauses: Rc::clone(&pauses),
            }),
        )
        .expect("debug workload should start");
    spawn_all(&mut run, specs);
    let dispatches = drive(&mut run);
    let recorded_pauses = pauses.borrow().clone();
    DebugObservation {
        dispatches,
        output: run.take_output(),
        output_events: run.output_events().to_vec(),
        pauses: recorded_pauses,
        outcomes: outcomes(&run),
        quit: run.debug_quit(),
    }
}

#[test]
fn arithmetic_multi_task_workload_is_repeatable_and_quantum_invariant() {
    let specs = arithmetic_specs();
    let first = profile_observation(arithmetic_workload(), &specs, 1);
    let second = profile_observation(arithmetic_workload(), &specs, 1);
    assert_eq!(first, second);
    assert_eq!(first.report.aggregate.functions[1].calls, 4);
    assert!(first.report.aggregate.functions[1].instructions > 0);
    assert_eq!(first.report.tasks.len(), 4);
    assert_eq!(first.output, "6\n10\n15\n21\n");
    assert_eq!(first.output_events.len(), 4);
    assert!(first.dispatches > 4);

    for quantum in [3, 32] {
        let candidate = profile_observation(arithmetic_workload(), &specs, quantum);
        assert_eq!(candidate.report, first.report);
        assert_eq!(candidate.output, first.output);
        assert_eq!(candidate.output_events, first.output_events);
        assert_eq!(candidate.outcomes, first.outcomes);
    }
}

#[test]
fn callback_multi_task_workload_is_repeatable_across_observability_modes() {
    let specs = callback_specs();
    let profile_first = profile_observation(callback_workload(), &specs, 1);
    let profile_second = profile_observation(callback_workload(), &specs, 1);
    assert_eq!(profile_first, profile_second);
    assert_eq!(profile_first.report.aggregate.functions[2].calls, 4);
    assert_eq!(profile_first.report.aggregate.natives.len(), 1);
    assert_eq!(profile_first.output, "[1, 2]\n[1, 2]\n");

    let trace_first = trace_observation(callback_workload(), &specs, 1);
    let trace_second = trace_observation(callback_workload(), &specs, 1);
    assert_eq!(trace_first, trace_second);
    assert_eq!(trace_first.output_events.len(), 2);
    assert!(trace_first
        .trace_events
        .iter()
        .any(|event| event.function == "identity_callback"));

    let debug_first = debug_observation(callback_workload(), &specs, 1);
    let debug_second = debug_observation(callback_workload(), &specs, 1);
    assert_eq!(debug_first, debug_second);
    assert!(!debug_first.quit);
    assert!(debug_first
        .pauses
        .iter()
        .any(|pause| pause.function == "identity_callback"));
    assert!(debug_first
        .pauses
        .iter()
        .all(|pause| pause.scheduler.running == pause.task_id));
}
