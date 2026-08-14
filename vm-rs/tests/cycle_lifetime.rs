use compiler_design_vm::bytecode::{Constant, FuncId, Function, Instruction, Program};
use compiler_design_vm::runtime::{Heap, HeapObjectKind, VariantValue};
use compiler_design_vm::value::Value;
use compiler_design_vm::{CancellationToken, DebugControl, DebugHook, DebugPause, RunConfig, VM};
use std::rc::Rc;

fn self_array_program() -> Program {
    Program {
        constants: Vec::new(),
        names: vec!["push".to_string()],
        functions: vec![Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
            registers: 2,
            instructions: vec![
                Instruction::Array {
                    dest: 0,
                    elements: Vec::new(),
                },
                Instruction::NativeCall {
                    dest: 1,
                    name: 0,
                    arguments: vec![0, 0],
                },
                Instruction::Print { value: 0 },
            ],
            locations: vec![None; 3],
        }],
        entry: FuncId(0),
        debug_sources: Vec::new(),
    }
}

fn callback_cycle_program() -> Program {
    Program {
        constants: vec![Constant::Number("1".to_string())],
        names: vec!["map".to_string(), "push".to_string()],
        functions: vec![Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
            registers: 4,
            instructions: vec![
                Instruction::Constant {
                    dest: 0,
                    constant: 0,
                },
                Instruction::Array {
                    dest: 1,
                    elements: vec![0],
                },
                Instruction::MakeFunction {
                    dest: 2,
                    function: FuncId(1),
                },
                Instruction::NativeCall {
                    dest: 3,
                    name: 0,
                    arguments: vec![1, 2],
                },
                Instruction::Return { value: 3 },
            ],
            locations: vec![None; 5],
        },
            Function {
            id: FuncId(1),
            local_count: 0,
            upvalues: Vec::new(),
            name: "make_cycle".to_string(),
            arity: 1,
            registers: 2,
            params: vec!["item".to_string()],
            instructions: vec![
                Instruction::Array {
                    dest: 0,
                    elements: Vec::new(),
                },
                Instruction::NativeCall {
                    dest: 1,
                    name: 1,
                    arguments: vec![0, 0],
                },
                Instruction::Return { value: 0 },
            ],
            locations: vec![None; 3],
        }],
        entry: FuncId(0),
        debug_sources: Vec::new(),
    }
}

fn nested_cycle_program() -> Program {
    Program {
        constants: Vec::new(),
        names: vec!["push".to_string()],
        functions: vec![Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
            registers: 2,
            instructions: vec![
                Instruction::MakeFunction {
                    dest: 0,
                    function: FuncId(2),
                },
                Instruction::Call {
                    dest: 1,
                    callee: 0,
                    arguments: Vec::new(),
                },
                Instruction::Return { value: 1 },
            ],
            locations: vec![None; 3],
        },
            Function {
                id: FuncId(1),
                local_count: 0,
                upvalues: Vec::new(),
                name: "inner".to_string(),
                arity: 0,
                registers: 2,
                params: Vec::new(),
                instructions: vec![
                    Instruction::Array {
                        dest: 0,
                        elements: Vec::new(),
                    },
                    Instruction::NativeCall {
                        dest: 1,
                        name: 0,
                        arguments: vec![0, 0],
                    },
                    Instruction::Return { value: 0 },
                ],
                locations: vec![None; 3],
            },
            Function {
                id: FuncId(2),
                local_count: 0,
                upvalues: Vec::new(),
                name: "outer".to_string(),
                arity: 0,
                registers: 2,
                params: Vec::new(),
                instructions: vec![
                    Instruction::MakeFunction {
                        dest: 0,
                        function: FuncId(1),
                    },
                    Instruction::Call {
                        dest: 1,
                        callee: 0,
                        arguments: Vec::new(),
                    },
                    Instruction::Return { value: 1 },
                ],
                locations: vec![None; 3],
            },
        ],
        entry: FuncId(0),
        debug_sources: Vec::new(),
    }
}

fn cycle_until_pause_program() -> Program {
    Program {
        constants: Vec::new(),
        names: vec!["push".to_string()],
        functions: vec![Function {
            id: FuncId(0),
            name: "main".to_string(),
            arity: 0,
            local_count: 0,
            upvalues: Vec::new(),
            params: Vec::new(),
            registers: 2,
            instructions: vec![
                Instruction::Array {
                    dest: 0,
                    elements: Vec::new(),
                },
                Instruction::NativeCall {
                    dest: 1,
                    name: 0,
                    arguments: vec![0, 0],
                },
                Instruction::Jump { target: 2 },
            ],
            locations: vec![None; 3],
        }],
        entry: FuncId(0),
        debug_sources: Vec::new(),
    }
}

struct CancelAtInstruction {
    token: CancellationToken,
    instruction: usize,
}

impl DebugHook for CancelAtInstruction {
    fn on_instruction(&mut self, pause: DebugPause) -> DebugControl {
        if pause.instruction == self.instruction {
            self.token.cancel();
        }
        DebugControl::Continue
    }
}

struct QuitAtInstruction {
    instruction: usize,
}

impl DebugHook for QuitAtInstruction {
    fn on_instruction(&mut self, pause: DebugPause) -> DebugControl {
        if pause.instruction == self.instruction {
            DebugControl::Quit
        } else {
            DebugControl::Continue
        }
    }
}

#[test]
fn cycle_storage_is_observed_after_roots_are_dropped_and_released_after_replacement() {
    let mut heap = Heap::new();
    let stats = heap.stats();

    let array = heap
        .allocate_array(Vec::new())
        .expect("array identity should be available");
    let array_weak = match &array {
        Value::Array(value) => {
            value.elements.borrow_mut().push(array.clone());
            Rc::downgrade(&value.elements)
        }
        _ => panic!("expected array cycle"),
    };

    let map = heap
        .allocate_map(Vec::new())
        .expect("map identity should be available");
    let map_weak = match &map {
        Value::Map(value) => {
            value
                .entries
                .borrow_mut()
                .push((Value::string("self"), map.clone()));
            Rc::downgrade(&value.entries)
        }
        _ => panic!("expected map cycle"),
    };

    let self_struct = heap
        .allocate_struct(Some("SelfNode".to_string()), vec![("next".to_string(), Value::Nil)])
        .expect("self struct identity should be available");
    let self_struct_weak = match &self_struct {
        Value::Struct(value) => {
            value.fields.borrow_mut()[0].1 = self_struct.clone();
            Rc::downgrade(&value.fields)
        }
        _ => panic!("expected self struct cycle"),
    };

    let first = heap
        .allocate_struct(Some("Node".to_string()), vec![("next".to_string(), Value::Nil)])
        .expect("first mutual struct identity should be available");
    let second = heap
        .allocate_struct(
            Some("Node".to_string()),
            vec![("next".to_string(), first.clone())],
        )
        .expect("second mutual struct identity should be available");
    let first_weak = match &first {
        Value::Struct(value) => {
            value.fields.borrow_mut()[0].1 = second.clone();
            Rc::downgrade(&value.fields)
        }
        _ => panic!("expected first mutual struct"),
    };

    let environment = heap.new_environment();
    let cell = heap.new_cell(Value::Nil);
    environment
        .borrow_mut()
        .insert("closure".to_string(), cell.clone());
    let function = heap
        .allocate_function("cycle", 0, 0, environment.clone())
        .expect("function identity should be available");
    *cell.borrow_mut() = function;
    let cell_weak = Rc::downgrade(&cell);
    drop(environment);

    drop(array);
    drop(map);
    drop(self_struct);
    drop(first);
    drop(second);
    drop(cell);

    let retained = stats.snapshot();
    assert_eq!(retained.for_kind(HeapObjectKind::Array).live, 1);
    assert_eq!(retained.for_kind(HeapObjectKind::Map).live, 1);
    assert_eq!(retained.for_kind(HeapObjectKind::Struct).live, 3);
    assert_eq!(retained.for_kind(HeapObjectKind::Environment).live, 1);
    assert_eq!(retained.for_kind(HeapObjectKind::Cell).live, 1);
    assert_eq!(retained.total_dead, 0);
    assert_eq!(retained.total_live, 7);
    assert_eq!(retained.peak_live, 7);
    assert!(retained.estimated_live_bytes > 0);
    assert!(retained.estimated_peak_live_bytes >= retained.estimated_live_bytes);

    assert!(array_weak.upgrade().is_some());
    assert!(map_weak.upgrade().is_some());
    assert!(self_struct_weak.upgrade().is_some());
    assert!(first_weak.upgrade().is_some());
    assert!(cell_weak.upgrade().is_some());

    assert_eq!(heap.collect_garbage(), 5);

    let released = stats.snapshot();
    assert_eq!(released.total_live, 0);
    assert_eq!(released.total_dead, 7);
    assert_eq!(released.peak_live, retained.peak_live);
    assert_eq!(released.estimated_live_bytes, 0);
    assert_eq!(
        released.estimated_peak_live_bytes,
        retained.estimated_peak_live_bytes
    );
}

#[test]
fn rooted_cycle_survives_collection_until_the_external_root_is_dropped() {
    let mut heap = Heap::new();
    let stats = heap.stats();
    let root = heap
        .allocate_array(Vec::new())
        .expect("array identity should be available");
    let root_pointer = if let Value::Array(value) = &root {
        value.elements.borrow_mut().push(root.clone());
        Rc::as_ptr(&value.elements) as usize
    } else {
        panic!("expected array cycle");
    };

    assert_eq!(heap.collect_garbage(), 0);
    assert_eq!(stats.snapshot().total_live, 1);
    if let Value::Array(value) = &root {
        assert_eq!(Rc::as_ptr(&value.elements) as usize, root_pointer);
    }

    drop(root);
    assert_eq!(heap.collect_garbage(), 1);
    let released = stats.snapshot();
    assert_eq!(released.total_live, 0);
    assert_eq!(released.total_dead, 1);
}

#[test]
fn recursive_variant_payload_edges_are_traced_without_moving_the_array() {
    let mut heap = Heap::new();
    let stats = heap.stats();
    let array = heap
        .allocate_array(Vec::new())
        .expect("array identity should be available");
    let array_weak = match &array {
        Value::Array(value) => {
            let variant = Value::variant(VariantValue {
                enum_name: "Loop".to_string(),
                variant_name: "Node".to_string(),
                fields: vec![array.clone()],
            });
            value.elements.borrow_mut().push(variant.clone());
            drop(variant);
            Rc::downgrade(&value.elements)
        }
        _ => panic!("expected array payload"),
    };

    drop(array);
    assert_eq!(stats.snapshot().total_live, 1);
    assert_eq!(heap.collect_garbage(), 1);
    assert!(array_weak.upgrade().is_none());
    assert_eq!(stats.snapshot().total_live, 0);
}

#[test]
fn native_callback_cycles_are_reclaimed_at_the_top_level_safepoint() {
    let program = callback_cycle_program();
    let vm = VM::with_config(&program, RunConfig::unlimited());
    let profile = vm.profile();

    assert_eq!(profile.result.expect("callback cycle should run"), "");
    assert!(profile.report.tracked_heap_estimated_peak_live_bytes > 0);
    assert!(
        profile.report.tracked_heap_estimated_live_bytes
            < profile.report.tracked_heap_estimated_peak_live_bytes
    );
}

#[test]
fn nested_call_cycles_are_reclaimed_after_returned_values_are_dropped() {
    let program = nested_cycle_program();
    let vm = VM::with_config(&program, RunConfig::unlimited());
    let profile = vm.profile();

    assert_eq!(profile.result.expect("nested cycle should run"), "");
    assert!(profile.report.tracked_heap_estimated_peak_live_bytes > 0);
    assert!(
        profile.report.tracked_heap_estimated_live_bytes
            < profile.report.tracked_heap_estimated_peak_live_bytes
    );
}

#[test]
fn cancellation_and_debugger_quit_collect_after_the_cycle_frame_ends() {
    let token = CancellationToken::new();
    let program = cycle_until_pause_program();
    let vm = VM::with_config(
        &program,
        RunConfig::unlimited().with_cancellation(token.clone()),
    );
    let debug = vm.debug(Box::new(CancelAtInstruction {
        token,
        instruction: 2,
    }));
    let error = debug
        .result
        .expect_err("cancellation should stop the cycle loop");
    assert_eq!(error.kind, compiler_design_vm::RuntimeErrorKind::Cancelled);
    assert!(!debug.quit);

    let program = cycle_until_pause_program();
    let vm = VM::with_config(&program, RunConfig::unlimited());
    let debug = vm.debug(Box::new(QuitAtInstruction { instruction: 2 }));
    assert!(debug.quit);
    assert_eq!(
        debug.result.expect("debugger quit is a successful stop"),
        ""
    );
}

#[test]
fn repeated_in_process_vm_instances_keep_cycle_profiles_isolated_and_deterministic() {
    let program = self_array_program();
    let first = VM::with_config(&program, RunConfig::unlimited()).profile();
    let expected_output = first.result.expect("self-array VM should run");
    let expected_report = first.report;
    assert_eq!(expected_output, "[<cycle>]\n");
    assert!(expected_report.tracked_heap_allocations > 0);
    assert!(expected_report.tracked_heap_peak_live > 0);
    assert!(
        expected_report.tracked_heap_estimated_peak_live_bytes
            >= expected_report.tracked_heap_estimated_live_bytes
    );

    for _ in 0..16 {
        let run = VM::with_config(&program, RunConfig::unlimited()).profile();
        assert_eq!(run.result.expect("repeated self-array VM should run"), expected_output);
        assert_eq!(run.report, expected_report);
    }
}
