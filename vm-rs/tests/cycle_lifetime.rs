use compiler_design_vm::bytecode::{FunctionBody, Instruction, Program};
use compiler_design_vm::runtime::{Heap, HeapObjectKind};
use compiler_design_vm::value::Value;
use compiler_design_vm::{RunConfig, VM};
use std::rc::Rc;

fn self_array_program() -> Program {
    Program {
        constants: Vec::new(),
        names: vec!["push".to_string()],
        main: FunctionBody {
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
        },
        functions: Vec::new(),
        debug_sources: Vec::new(),
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

    array_weak
        .upgrade()
        .expect("array cycle should still be reachable through its weak observation")
        .borrow_mut()
        .clear();
    map_weak
        .upgrade()
        .expect("map cycle should still be reachable through its weak observation")
        .borrow_mut()
        .clear();
    self_struct_weak
        .upgrade()
        .expect("self struct cycle should still be reachable through its weak observation")
        .borrow_mut()[0]
        .1 = Value::Nil;
    first_weak
        .upgrade()
        .expect("mutual struct cycle should still be reachable through its weak observation")
        .borrow_mut()[0]
        .1 = Value::Nil;
    cell_weak
        .upgrade()
        .expect("closure cycle should still be reachable through its weak observation")
        .replace(Value::Nil);

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
