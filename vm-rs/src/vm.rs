#![allow(dead_code)]

use crate::bytecode::{Constant, DebugLocation, DebugSource, FunctionBody, Instruction, Program};
use crate::runtime::{
    new_cell, new_environment, ArrayValue, Cell, FunctionValue, MapValue, RangeValue,
    SharedEnvironment, StructValue,
};
use crate::value::Value;
use std::cell::RefCell;
use std::fmt;
use std::rc::Rc;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StackFrame {
    pub function: String,
    pub location: Option<DebugLocation>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RuntimeError {
    pub message: String,
    pub location: Option<DebugLocation>,
    pub stack: Vec<StackFrame>,
    pub sources: Vec<DebugSource>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bytecode::Function;

    fn empty_program() -> Program {
        Program {
            constants: Vec::new(),
            names: Vec::new(),
            main: FunctionBody {
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            },
            functions: Vec::new(),
            debug_sources: Vec::new(),
        }
    }

    fn array_elements(value: &Value) -> Vec<Value> {
        let Value::Array(array) = value else {
            panic!("expected array");
        };
        array.elements.borrow().clone()
    }

    fn debug_failure_program() -> Program {
        let source = DebugSource {
            module: None,
            path: "demo.cd".to_string(),
            text: "fun fail() { return 1 / 0; }\nfail();\n".to_string(),
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
                locations: vec![
                    Some(DebugLocation { source: 0, line: 2, column: 1 }),
                    Some(DebugLocation { source: 0, line: 2, column: 1 }),
                ],
            },
            functions: vec![Function {
                index: 0,
                name: "fail".to_string(),
                arity: 0,
                registers: 4,
                params: Vec::new(),
                instructions: vec![
                    Instruction::Constant { dest: 0, constant: 0 },
                    Instruction::Constant { dest: 1, constant: 1 },
                    Instruction::Divide { dest: 2, left: 0, right: 1 },
                    Instruction::Return { value: 2 },
                ],
                locations: vec![
                    Some(DebugLocation { source: 0, line: 1, column: 21 }),
                    Some(DebugLocation { source: 0, line: 1, column: 25 }),
                    Some(DebugLocation { source: 0, line: 1, column: 21 }),
                    Some(DebugLocation { source: 0, line: 1, column: 14 }),
                ],
            }],
            debug_sources: vec![source],
        }
    }

    #[test]
    fn native_collection_helpers_query_and_copy_shallowly() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let shared = vm.make_array(vec![Value::number(9.0)]);
        let source = vm.make_array(vec![Value::number(1.0), shared.clone(), Value::number(3.0)]);
        let distinct = vm.make_array(vec![Value::number(9.0)]);

        let contains_shared = vm
            .execute_native_call("contains", vec![source.clone(), shared.clone()])
            .expect("contains succeeds");
        let contains_distinct = vm
            .execute_native_call("contains", vec![source.clone(), distinct])
            .expect("contains succeeds");
        assert!(matches!(contains_shared, Value::Bool(true)));
        assert!(matches!(contains_distinct, Value::Bool(false)));

        let sliced = vm
            .execute_native_call(
                "slice",
                vec![source.clone(), Value::number(1.0), Value::number(2.0)],
            )
            .expect("slice succeeds");
        let copied = vm
            .execute_native_call("copy", vec![source.clone()])
            .expect("copy succeeds");
        let concatenated = vm
            .execute_native_call("concat", vec![sliced.clone(), copied.clone()])
            .expect("concat succeeds");

        assert_eq!(array_elements(&sliced).len(), 2);
        assert_eq!(array_elements(&copied).len(), 3);
        assert_eq!(array_elements(&concatenated).len(), 5);
        assert!(!source.runtime_equals(&copied));
        let source_elements = array_elements(&source);
        let copied_elements = array_elements(&copied);
        assert!(source_elements[1].runtime_equals(&copied_elements[1]));
    }

    #[test]
    fn native_map_invokes_callback_and_returns_fresh_array() {
        let program = Program {
            constants: Vec::new(),
            names: vec!["item".to_string()],
            main: FunctionBody {
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            },
            functions: vec![Function {
                index: 0,
                name: "identity".to_string(),
                arity: 1,
                registers: 1,
                params: vec!["item".to_string()],
                instructions: vec![
                    Instruction::LoadVar { dest: 0, name: 0 },
                    Instruction::Return { value: 0 },
                ],
                locations: vec![None, None],
            }],
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let callback = Value::function(FunctionValue {
            name: "identity".to_string(),
            function_index: 0,
            arity: 1,
            identity: 1,
            closure: new_environment(),
        });

        let mapped = vm
            .execute_native_call("map", vec![source.clone(), callback])
            .expect("map succeeds");

        let elements = array_elements(&mapped);
        assert_eq!(elements.len(), 2);
        assert!(matches!(elements[0], Value::Number(value) if value == 1.0));
        assert!(matches!(elements[1], Value::Number(value) if value == 2.0));
        assert!(!source.runtime_equals(&mapped));
    }

    #[test]
    fn native_flat_map_flattens_one_level_and_returns_fresh_array() {
        let program = Program {
            constants: vec![Constant::Number("10".to_string())],
            names: vec!["item".to_string()],
            main: FunctionBody {
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            },
            functions: vec![
                Function {
                    index: 0,
                    name: "expand".to_string(),
                    arity: 1,
                    registers: 5,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::LoadVar { dest: 0, name: 0 },
                        Instruction::Constant { dest: 1, constant: 0 },
                        Instruction::Add {
                            dest: 2,
                            left: 0,
                            right: 1,
                        },
                        Instruction::Array {
                            dest: 3,
                            elements: vec![0],
                        },
                        Instruction::Array {
                            dest: 4,
                            elements: vec![0, 2, 3],
                        },
                        Instruction::Return { value: 4 },
                    ],
                    locations: vec![None; 6],
                },
                Function {
                    index: 1,
                    name: "identity".to_string(),
                    arity: 1,
                    registers: 1,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::LoadVar { dest: 0, name: 0 },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None; 2],
                },
            ],
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let callback = Value::function(FunctionValue {
            name: "expand".to_string(),
            function_index: 0,
            arity: 1,
            identity: 1,
            closure: new_environment(),
        });

        let flattened = vm
            .execute_native_call("flatMap", vec![source.clone(), callback])
            .expect("flatMap succeeds");

        let elements = array_elements(&flattened);
        assert_eq!(elements.len(), 6);
        assert!(matches!(elements[0], Value::Number(value) if value == 1.0));
        assert!(matches!(elements[1], Value::Number(value) if value == 11.0));
        assert!(matches!(&elements[2], Value::Array(nested) if nested.elements.borrow().len() == 1));
        assert!(matches!(elements[3], Value::Number(value) if value == 2.0));
        assert!(matches!(elements[4], Value::Number(value) if value == 12.0));
        assert!(matches!(&elements[5], Value::Array(nested) if nested.elements.borrow().len() == 1));
        assert!(!source.runtime_equals(&flattened));

        let invalid_callback = Value::function(FunctionValue {
            name: "identity".to_string(),
            function_index: 1,
            arity: 1,
            identity: 2,
            closure: new_environment(),
        });
        assert_eq!(
            vm.execute_native_call("flatMap", vec![source, invalid_callback])
                .unwrap_err()
                .message,
            "flatMap expects callback to return array"
        );
    }

    #[test]
    fn native_filter_invokes_predicate_and_returns_matching_fresh_array() {
        let program = Program {
            constants: vec![Constant::Number("1".to_string())],
            names: vec!["item".to_string()],
            main: FunctionBody {
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            },
            functions: vec![Function {
                index: 0,
                name: "greater_than_one".to_string(),
                arity: 1,
                registers: 3,
                params: vec!["item".to_string()],
                instructions: vec![
                    Instruction::LoadVar { dest: 0, name: 0 },
                    Instruction::Constant { dest: 1, constant: 0 },
                    Instruction::Greater {
                        dest: 2,
                        left: 0,
                        right: 1,
                    },
                    Instruction::Return { value: 2 },
                ],
                locations: vec![None, None, None, None],
            }],
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let source = vm.make_array(vec![Value::number(1.0), Value::number(2.0), Value::number(3.0)]);
        let predicate = Value::function(FunctionValue {
            name: "greater_than_one".to_string(),
            function_index: 0,
            arity: 1,
            identity: 1,
            closure: new_environment(),
        });

        let filtered = vm
            .execute_native_call("filter", vec![source.clone(), predicate])
            .expect("filter succeeds");

        let elements = array_elements(&filtered);
        assert_eq!(elements.len(), 2);
        assert!(matches!(elements[0], Value::Number(value) if value == 2.0));
        assert!(matches!(elements[1], Value::Number(value) if value == 3.0));
        assert!(!source.runtime_equals(&filtered));
    }

    #[test]
    fn native_filter_validates_operands_and_boolean_predicate_results() {
        let program = Program {
            constants: vec![Constant::Nil],
            names: Vec::new(),
            main: FunctionBody {
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            },
            functions: vec![
                Function {
                    index: 0,
                    name: "no_args".to_string(),
                    arity: 0,
                    registers: 0,
                    params: Vec::new(),
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    index: 1,
                    name: "returns_nil".to_string(),
                    arity: 1,
                    registers: 1,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::Constant { dest: 0, constant: 0 },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None, None],
                },
            ],
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let array = vm.make_array(vec![Value::number(1.0)]);
        let no_args = Value::function(FunctionValue {
            name: "no_args".to_string(),
            function_index: 0,
            arity: 0,
            identity: 1,
            closure: new_environment(),
        });
        let returns_nil = Value::function(FunctionValue {
            name: "returns_nil".to_string(),
            function_index: 1,
            arity: 1,
            identity: 2,
            closure: new_environment(),
        });

        assert_eq!(
            vm.execute_native_call("filter", Vec::new())
                .unwrap_err()
                .message,
            "filter expects 2 arguments"
        );
        assert_eq!(
            vm.execute_native_call("filter", vec![Value::number(1.0), no_args.clone()])
                .unwrap_err()
                .message,
            "filter expects array as first argument"
        );
        assert_eq!(
            vm.execute_native_call("filter", vec![array.clone(), Value::number(1.0)])
                .unwrap_err()
                .message,
            "filter expects function as second argument"
        );
        assert_eq!(
            vm.execute_native_call("filter", vec![array.clone(), no_args])
                .unwrap_err()
                .message,
            "filter expects callback with 1 argument"
        );
        assert_eq!(
            vm.execute_native_call("filter", vec![array, returns_nil])
                .unwrap_err()
                .message,
            "filter expects callback to return bool"
        );
    }

    #[test]
    fn native_any_and_all_short_circuit_with_boolean_results() {
        let program = Program {
            constants: vec![Constant::Number("2".to_string())],
            names: vec!["item".to_string()],
            main: FunctionBody {
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            },
            functions: vec![Function {
                index: 0,
                name: "is_two".to_string(),
                arity: 1,
                registers: 3,
                params: vec!["item".to_string()],
                instructions: vec![
                    Instruction::LoadVar { dest: 0, name: 0 },
                    Instruction::Constant { dest: 1, constant: 0 },
                    Instruction::Equal {
                        dest: 2,
                        left: 0,
                        right: 1,
                    },
                    Instruction::Return { value: 2 },
                ],
                locations: vec![None, None, None, None],
            }],
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let predicate = Value::function(FunctionValue {
            name: "is_two".to_string(),
            function_index: 0,
            arity: 1,
            identity: 1,
            closure: new_environment(),
        });
        let any_source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let all_source = vm.make_array(vec![Value::number(2.0), Value::number(2.0)]);
        let count_source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let empty_any_source = vm.make_array(Vec::new());
        let empty_all_source = vm.make_array(Vec::new());

        assert!(matches!(
            vm.execute_native_call("any", vec![any_source, predicate.clone()])
            .unwrap(),
            Value::Bool(true)
        ));
        assert!(matches!(
            vm.execute_native_call("all", vec![all_source, predicate.clone()])
            .unwrap(),
            Value::Bool(true)
        ));
        assert!(matches!(
            vm.execute_native_call("count", vec![count_source, predicate.clone()])
                .unwrap(),
            Value::Number(value) if value == 1.0
        ));
        let find_source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let find_miss_source = vm.make_array(vec![Value::number(1.0)]);
        assert!(matches!(
            vm.execute_native_call("find", vec![find_source, predicate.clone()])
                .unwrap(),
            Value::Number(value) if value == 2.0
        ));
        assert!(matches!(
            vm.execute_native_call("find", vec![find_miss_source, predicate.clone()])
                .unwrap(),
            Value::Nil
        ));
        let find_index_source = vm.make_array(vec![Value::number(1.0), Value::number(2.0)]);
        let find_index_miss_source = vm.make_array(vec![Value::number(1.0)]);
        assert!(matches!(
            vm.execute_native_call("findIndex", vec![find_index_source, predicate.clone()])
                .unwrap(),
            Value::Number(value) if value == 1.0
        ));
        assert!(matches!(
            vm.execute_native_call("findIndex", vec![find_index_miss_source, predicate.clone()])
                .unwrap(),
            Value::Number(value) if value == -1.0
        ));
        assert!(matches!(
            vm.execute_native_call("any", vec![empty_any_source, predicate.clone()])
                .unwrap(),
            Value::Bool(false)
        ));
        assert!(matches!(
            vm.execute_native_call("all", vec![empty_all_source, predicate])
                .unwrap(),
            Value::Bool(true)
        ));
    }

    #[test]
    fn native_any_and_all_validate_operands_and_predicate_results() {
        let program = Program {
            constants: vec![Constant::Nil],
            names: Vec::new(),
            main: FunctionBody {
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            },
            functions: vec![
                Function {
                    index: 0,
                    name: "no_args".to_string(),
                    arity: 0,
                    registers: 0,
                    params: Vec::new(),
                    instructions: Vec::new(),
                    locations: Vec::new(),
                },
                Function {
                    index: 1,
                    name: "returns_nil".to_string(),
                    arity: 1,
                    registers: 1,
                    params: vec!["item".to_string()],
                    instructions: vec![
                        Instruction::Constant { dest: 0, constant: 0 },
                        Instruction::Return { value: 0 },
                    ],
                    locations: vec![None, None],
                },
            ],
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let array = vm.make_array(vec![Value::number(1.0)]);
        let no_args = Value::function(FunctionValue {
            name: "no_args".to_string(),
            function_index: 0,
            arity: 0,
            identity: 1,
            closure: new_environment(),
        });
        let returns_nil = Value::function(FunctionValue {
            name: "returns_nil".to_string(),
            function_index: 1,
            arity: 1,
            identity: 2,
            closure: new_environment(),
        });

        assert_eq!(
            vm.execute_native_call("any", Vec::new())
                .unwrap_err()
                .message,
            "any expects 2 arguments"
        );
        assert_eq!(
            vm.execute_native_call("all", vec![Value::number(1.0), no_args.clone()])
                .unwrap_err()
                .message,
            "all expects array as first argument"
        );
        assert_eq!(
            vm.execute_native_call("any", vec![array.clone(), Value::number(1.0)])
                .unwrap_err()
                .message,
            "any expects function as second argument"
        );
        assert_eq!(
            vm.execute_native_call("all", vec![array.clone(), no_args])
                .unwrap_err()
                .message,
            "all expects callback with 1 argument"
        );
        assert_eq!(
            vm.execute_native_call("any", vec![array, returns_nil])
                .unwrap_err()
                .message,
            "any expects callback to return bool"
        );
    }

    #[test]
    fn native_reduce_threads_accumulator_and_returns_initial_for_empty_arrays() {
        let program = Program {
            constants: Vec::new(),
            names: vec!["acc".to_string(), "item".to_string()],
            main: FunctionBody {
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            },
            functions: vec![Function {
                index: 0,
                name: "add".to_string(),
                arity: 2,
                registers: 3,
                params: vec!["acc".to_string(), "item".to_string()],
                instructions: vec![
                    Instruction::LoadVar { dest: 0, name: 0 },
                    Instruction::LoadVar { dest: 1, name: 1 },
                    Instruction::Add {
                        dest: 2,
                        left: 0,
                        right: 1,
                    },
                    Instruction::Return { value: 2 },
                ],
                locations: vec![None, None, None, None],
            }],
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let source = vm.make_array(vec![Value::number(1.0), Value::number(2.0), Value::number(3.0)]);
        let empty = vm.make_array(Vec::new());
        let callback = Value::function(FunctionValue {
            name: "add".to_string(),
            function_index: 0,
            arity: 2,
            identity: 1,
            closure: new_environment(),
        });

        let total = vm
            .execute_native_call(
                "reduce",
                vec![source, Value::number(0.0), callback.clone()],
            )
            .expect("reduce succeeds");
        assert!(matches!(total, Value::Number(value) if value == 6.0));

        let initial = Value::string("seed");
        let returned = vm
            .execute_native_call("reduce", vec![empty, initial.clone(), callback])
            .expect("empty reduce succeeds");
        assert!(returned.runtime_equals(&initial));
    }

    #[test]
    fn native_reduce_validates_operands_and_callback_arity() {
        let program = Program {
            constants: Vec::new(),
            names: Vec::new(),
            main: FunctionBody {
                registers: 0,
                instructions: Vec::new(),
                locations: Vec::new(),
            },
            functions: vec![Function {
                index: 0,
                name: "one_arg".to_string(),
                arity: 1,
                registers: 0,
                params: vec!["item".to_string()],
                instructions: Vec::new(),
                locations: Vec::new(),
            }],
            debug_sources: Vec::new(),
        };
        let mut vm = VM::new(&program);
        let array = vm.make_array(vec![Value::number(1.0)]);
        let callback = Value::function(FunctionValue {
            name: "one_arg".to_string(),
            function_index: 0,
            arity: 1,
            identity: 1,
            closure: new_environment(),
        });

        assert_eq!(
            vm.execute_native_call("reduce", Vec::new())
                .unwrap_err()
                .message,
            "reduce expects 3 arguments"
        );
        assert_eq!(
            vm.execute_native_call(
                "reduce",
                vec![Value::number(1.0), Value::number(0.0), callback.clone()],
            )
            .unwrap_err()
            .message,
            "reduce expects array as first argument"
        );
        assert_eq!(
            vm.execute_native_call(
                "reduce",
                vec![array.clone(), Value::number(0.0), Value::number(1.0)],
            )
            .unwrap_err()
            .message,
            "reduce expects function as third argument"
        );
        assert_eq!(
            vm.execute_native_call("reduce", vec![array, Value::number(0.0), callback])
                .unwrap_err()
                .message,
            "reduce expects callback with 2 arguments"
        );
    }

    #[test]
    fn map_lookup_update_length_and_contains() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(vec![(Value::string("a"), Value::number(1.0))]);
        let alias = map.clone();

        let updated = vm
            .execute_assign_index(alias, Value::string("b"), Value::number(2.0))
            .expect("map assignment succeeds");
        assert!(matches!(updated, Value::Number(value) if value == 2.0));
        assert!(matches!(
            vm.execute_index(map.clone(), Value::string("b")).unwrap(),
            Value::Number(value) if value == 2.0
        ));
        assert!(matches!(vm.execute_len(map.clone()).unwrap(), Value::Number(value) if value == 2.0));
        assert!(matches!(
            vm.execute_native_call("contains", vec![map.clone(), Value::string("a")]).unwrap(),
            Value::Bool(true)
        ));
        assert!(vm.execute_index(map, Value::string("missing")).is_err());
    }

    #[test]
    fn native_map_keys_and_values_return_ordered_fresh_arrays() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(vec![
            (Value::string("a"), Value::number(1.0)),
            (Value::string("b"), Value::number(2.0)),
        ]);

        let keys = vm
            .execute_native_call("keys", vec![map.clone()])
            .expect("keys succeeds");
        let values = vm
            .execute_native_call("values", vec![map.clone()])
            .expect("values succeeds");
        assert_eq!(keys.to_string(), "[a, b]");
        assert_eq!(values.to_string(), "[1, 2]");

        vm.execute_assign_index(map.clone(), Value::string("c"), Value::number(3.0))
            .expect("map mutation succeeds");
        assert_eq!(keys.to_string(), "[a, b]");
        assert_eq!(values.to_string(), "[1, 2]");

        let fresh_keys = vm
            .execute_native_call("keys", vec![map])
            .expect("second keys succeeds");
        assert_eq!(fresh_keys.to_string(), "[a, b, c]");
        assert!(!keys.runtime_equals(&fresh_keys));
    }

    #[test]
    fn assert_array_snapshots_map_keys_in_insertion_order() {
        let program = Program {
            constants: vec![
                Constant::String("a".to_string()),
                Constant::Number("1".to_string()),
                Constant::String("b".to_string()),
                Constant::Number("2".to_string()),
            ],
            names: Vec::new(),
            main: FunctionBody {
                registers: 6,
                instructions: vec![
                    Instruction::Constant { dest: 0, constant: 0 },
                    Instruction::Constant { dest: 1, constant: 1 },
                    Instruction::Constant { dest: 2, constant: 2 },
                    Instruction::Constant { dest: 3, constant: 3 },
                    Instruction::Map {
                        dest: 4,
                        entries: vec![(0, 1), (2, 3)],
                    },
                    Instruction::AssertArray { dest: 5, value: 4 },
                    Instruction::Print { value: 5 },
                    Instruction::Return { value: 5 },
                ],
                locations: Vec::new(),
            },
            functions: Vec::new(),
            debug_sources: Vec::new(),
        };

        assert_eq!(VM::new(&program).run().expect("map iteration succeeds"), "[a, b]\n");
    }

    #[test]
    fn native_remove_updates_shared_maps_and_returns_removed_value() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(vec![
            (Value::string("a"), Value::number(1.0)),
            (Value::string("b"), Value::Nil),
        ]);
        let alias = map.clone();

        let removed = vm
            .execute_native_call("remove", vec![map.clone(), Value::string("a")])
            .expect("remove succeeds");
        assert!(matches!(removed, Value::Number(value) if value == 1.0));
        assert_eq!(map.to_string(), "map{b: nil}");
        assert!(matches!(
            vm.execute_index(alias.clone(), Value::string("b")).unwrap(),
            Value::Nil
        ));

        let removed_nil = vm
            .execute_native_call("remove", vec![alias.clone(), Value::string("b")])
            .expect("remove can return nil");
        assert!(matches!(removed_nil, Value::Nil));
        assert!(matches!(
            vm.execute_len(alias).unwrap(),
            Value::Number(value) if value == 0.0
        ));
    }

    #[test]
    fn native_clear_empties_shared_map_and_allows_new_insertion() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(vec![
            (Value::string("a"), Value::number(1.0)),
            (Value::string("b"), Value::number(2.0)),
        ]);
        let alias = map.clone();

        let result = vm
            .execute_native_call("clear", vec![map.clone()])
            .expect("clear succeeds");
        assert!(matches!(result, Value::Nil));
        assert_eq!(alias.to_string(), "map{}");
        assert!(matches!(
            vm.execute_len(alias.clone()).unwrap(),
            Value::Number(value) if value == 0.0
        ));

        vm.execute_assign_index(alias.clone(), Value::string("c"), Value::number(3.0))
            .expect("map insertion after clear succeeds");
        assert_eq!(map.to_string(), "map{c: 3}");
    }

    #[test]
    fn native_merge_returns_fresh_ordered_map_and_shares_values() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let shared = vm.make_array(vec![Value::number(7.0)]);
        let left = vm.make_map(vec![
            (Value::string("a"), Value::number(1.0)),
            (Value::string("b"), shared.clone()),
        ]);
        let right = vm.make_map(vec![
            (Value::string("b"), Value::number(2.0)),
            (Value::string("c"), shared.clone()),
        ]);

        let merged = vm
            .execute_native_call("merge", vec![left.clone(), right])
            .expect("merge succeeds");

        assert_eq!(merged.to_string(), "map{a: 1, b: 2, c: [7]}");
        assert_eq!(left.to_string(), "map{a: 1, b: [7]}");
        assert!(!merged.runtime_equals(&left));

        vm.execute_native_call("push", vec![shared, Value::number(8.0)])
            .expect("shared value mutation succeeds");
        assert_eq!(merged.to_string(), "map{a: 1, b: 2, c: [7, 8]}");
        assert_eq!(left.to_string(), "map{a: 1, b: [7, 8]}");
    }

    #[test]
    fn native_merge_validates_arity_and_map_operands() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(Vec::new());

        assert_eq!(
            vm.execute_native_call("merge", Vec::new())
                .unwrap_err()
                .message,
            "merge expects 2 arguments"
        );
        assert_eq!(
            vm.execute_native_call("merge", vec![Value::number(1.0), map.clone()])
                .unwrap_err()
                .message,
            "merge expects map as first argument"
        );
        assert_eq!(
            vm.execute_native_call("merge", vec![map, Value::number(1.0)])
                .unwrap_err()
                .message,
            "merge expects map as second argument"
        );
    }

    #[test]
    fn map_aliases_share_updates_through_cells() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(vec![(Value::string("a"), Value::number(1.0))]);
        let left = new_cell(map.clone());
        let right = new_cell(map);

        vm.execute_assign_index(
            left.borrow().clone(),
            Value::string("b"),
            Value::number(2.0),
        )
        .expect("map assignment succeeds");

        assert!(matches!(
            vm.execute_index(left.borrow().clone(), Value::string("b")).unwrap(),
            Value::Number(value) if value == 2.0
        ));
        assert!(matches!(
            vm.execute_index(right.borrow().clone(), Value::string("b")).unwrap(),
            Value::Number(value) if value == 2.0
        ));
    }

    #[test]
    fn map_rejects_reference_keys() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(Vec::new());
        let key = vm.make_array(vec![Value::number(1.0)]);

        let error = vm
            .execute_assign_index(map, key, Value::number(1.0))
            .expect_err("array key should fail");
        assert_eq!(error.message, "map key must be nil, number, bool, or string");
    }

    #[test]
    fn map_preserves_order_and_identity_equality() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let map = vm.make_map(vec![
            (Value::string("a"), Value::number(1.0)),
            (Value::string("b"), Value::number(2.0)),
            (Value::string("a"), Value::number(3.0)),
        ]);
        let distinct = vm.make_map(vec![
            (Value::string("a"), Value::number(3.0)),
            (Value::string("b"), Value::number(2.0)),
        ]);

        assert_eq!(map.to_string(), "map{a: 3, b: 2}");
        assert!(map.runtime_equals(&map));
        assert!(!map.runtime_equals(&distinct));
    }

    #[test]
    fn native_ranges_are_indexable_and_iterable_values() {
        let program = empty_program();
        let mut vm = VM::new(&program);

        let ascending = vm
            .execute_native_call("range", vec![Value::number(1.0), Value::number(6.0)])
            .expect("range succeeds");
        let equivalent = vm
            .execute_native_call("range", vec![Value::number(1.0), Value::number(6.0)])
            .expect("equivalent range succeeds");
        let different = vm
            .execute_native_call("range", vec![Value::number(1.0), Value::number(7.0)])
            .expect("different range succeeds");
        assert_eq!(ascending.to_string(), "range(1, 6, 1)");
        assert_eq!(ascending.type_name(), "range");
        assert!(ascending.runtime_equals(&equivalent));
        assert!(!ascending.runtime_equals(&different));
        assert!(matches!(
            vm.execute_len(ascending.clone()).unwrap(),
            Value::Number(value) if value == 5.0
        ));
        assert!(matches!(
            vm.execute_index(ascending.clone(), Value::number(2.0)).unwrap(),
            Value::Number(value) if value == 3.0
        ));
        assert!(matches!(
            vm.execute_native_call("contains", vec![ascending.clone(), Value::number(5.0)]).unwrap(),
            Value::Bool(true)
        ));
        assert!(matches!(
            vm.execute_native_call("contains", vec![ascending.clone(), Value::number(5.5)]).unwrap(),
            Value::Bool(false)
        ));

        let descending = vm
            .execute_native_call(
                "range",
                vec![Value::number(5.0), Value::number(0.0), Value::number(-2.0)],
            )
            .expect("descending range succeeds");
        assert!(matches!(
            vm.execute_len(descending.clone()).unwrap(),
            Value::Number(value) if value == 3.0
        ));
        assert!(matches!(
            vm.execute_index(descending, Value::number(1.0)).unwrap(),
            Value::Number(value) if value == 3.0
        ));

        let empty = vm
            .execute_native_call("range", vec![Value::number(5.0), Value::number(0.0)])
            .expect("empty range succeeds");
        assert!(matches!(
            vm.execute_len(empty).unwrap(),
            Value::Number(value) if value == 0.0
        ));
    }

    #[test]
    fn native_ranges_validate_bounds_and_indexes() {
        let program = empty_program();
        let mut vm = VM::new(&program);

        let zero_step = vm
            .execute_native_call(
                "range",
                vec![Value::number(0.0), Value::number(3.0), Value::number(0.0)],
            )
            .expect_err("zero step should fail");
        assert_eq!(zero_step.message, "range step must not be zero");

        let fractional = vm
            .execute_native_call("range", vec![Value::number(0.0), Value::number(3.0), Value::number(1.5)])
            .expect_err("fractional step should fail");
        assert_eq!(fractional.message, "range expects integer as third argument");

        let range = vm
            .execute_native_call("range", vec![Value::number(3.0)])
            .expect("range succeeds");
        let wrong_type = vm
            .execute_index(range.clone(), Value::boolean(true))
            .expect_err("bool index should fail");
        assert_eq!(wrong_type.message, "range index must be number");
        let out_of_bounds = vm
            .execute_index(range, Value::number(3.0))
            .expect_err("out-of-bounds index should fail");
        assert_eq!(out_of_bounds.message, "range index out of bounds");
    }

    #[test]
    fn native_collection_helpers_validate_slice_boundaries() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let source = vm.make_array(vec![Value::number(1.0)]);

        let empty = vm
            .execute_native_call(
                "slice",
                vec![source.clone(), Value::number(1.0), Value::number(0.0)],
            )
            .expect("empty end slice succeeds");
        assert!(array_elements(&empty).is_empty());

        for (start, length, expected) in [
            (f64::NAN, 0.0, "slice expects integer start offset"),
            (-1.0, 0.0, "slice start offset out of bounds"),
            (0.0, f64::INFINITY, "slice expects integer length"),
            (0.0, 2.0, "slice length out of bounds"),
        ] {
            let error = vm
                .execute_native_call(
                    "slice",
                    vec![source.clone(), Value::number(start), Value::number(length)],
                )
                .expect_err("slice should fail");
            assert_eq!(error.message, expected);
        }
    }

    #[test]
    fn native_collection_helpers_validate_arity_and_types() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        assert_eq!(
            vm.execute_native_call("contains", vec![])
                .unwrap_err()
                .message,
            "contains expects 2 arguments"
        );
        assert_eq!(
            vm.execute_native_call("slice", vec![]).unwrap_err().message,
            "slice expects 3 arguments"
        );
        assert_eq!(
            vm.execute_native_call("copy", vec![]).unwrap_err().message,
            "copy expects 1 argument"
        );
        assert_eq!(
            vm.execute_native_call("concat", vec![])
                .unwrap_err()
                .message,
            "concat expects 2 arguments"
        );
        assert_eq!(
            vm.execute_native_call("copy", vec![Value::number(1.0)])
                .unwrap_err()
                .message,
            "copy expects array as first argument"
        );
        assert_eq!(
            vm.execute_native_call("concat", vec![Value::Nil, Value::Nil])
                .unwrap_err()
                .message,
            "concat expects array as first argument"
        );
    }

    #[test]
    fn native_string_helpers_use_unicode_scalar_boundaries() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let source = Value::string("你🙂e\u{301}");

        let length = vm.execute_len(source.clone()).expect("len succeeds");
        assert!(matches!(length, Value::Number(value) if value == 4.0));

        let sliced = vm
            .execute_native_call(
                "substr",
                vec![source.clone(), Value::number(1.0), Value::number(2.0)],
            )
            .expect("substr succeeds");
        assert!(matches!(sliced, Value::String(value) if value == "🙂e"));

        let combined = vm
            .execute_native_call(
                "substr",
                vec![source.clone(), Value::number(2.0), Value::number(2.0)],
            )
            .expect("combining scalar slice succeeds");
        assert!(matches!(combined, Value::String(value) if value == "e\u{301}"));

        let character = vm
            .execute_native_call("charAt", vec![source, Value::number(1.0)])
            .expect("charAt succeeds");
        assert!(matches!(character, Value::String(value) if value == "🙂"));
    }

    #[test]
    fn native_string_helpers_validate_scalar_boundaries() {
        let program = empty_program();
        let mut vm = VM::new(&program);
        let source = Value::string("你🙂");

        let empty = vm
            .execute_native_call(
                "substr",
                vec![source.clone(), Value::number(2.0), Value::number(0.0)],
            )
            .expect("empty end slice succeeds");
        assert!(matches!(empty, Value::String(value) if value.is_empty()));

        for (start, length, expected) in [
            (3.0, 0.0, "substr start offset out of bounds"),
            (1.0, 2.0, "substr length out of bounds"),
            (-1.0, 0.0, "substr start offset out of bounds"),
            (1.5, 0.0, "substr expects integer start offset"),
        ] {
            let error = vm
                .execute_native_call(
                    "substr",
                    vec![source.clone(), Value::number(start), Value::number(length)],
                )
                .expect_err("substr should fail");
            assert_eq!(error.message, expected);
        }

        for (index, expected) in [
            (2.0, "charAt index out of bounds"),
            (1.5, "charAt expects integer index"),
        ] {
            let error = vm
                .execute_native_call("charAt", vec![source.clone(), Value::number(index)])
                .expect_err("charAt should fail");
            assert_eq!(error.message, expected);
        }
    }

    #[test]
    fn runtime_error_reports_inner_location_then_outer_call_site() {
        let error = VM::new(&debug_failure_program()).run().unwrap_err();
        assert_eq!(error.location.as_ref().unwrap().line, 1);
        assert_eq!(error.stack[0].function, "fail");
        assert_eq!(error.stack[1].function, "main");
        assert!(error.to_string().contains("Call stack:\n"));
        assert!(error.to_string().contains("  fun fail() { return 1 / 0; }\n"));
    }

    #[test]
    fn native_failure_uses_native_call_location() {
        let program = Program {
            constants: vec![Constant::Nil],
            names: vec!["sqrt".to_string()],
            main: FunctionBody {
                registers: 2,
                instructions: vec![
                    Instruction::Constant { dest: 0, constant: 0 },
                    Instruction::NativeCall {
                        dest: 1,
                        name: 0,
                        arguments: vec![0],
                    },
                ],
                locations: vec![
                    Some(DebugLocation { source: 0, line: 1, column: 7 }),
                    Some(DebugLocation { source: 0, line: 1, column: 1 }),
                ],
            },
            functions: Vec::new(),
            debug_sources: vec![DebugSource {
                module: None,
                path: "native.cd".to_string(),
                text: "sqrt(nil);\n".to_string(),
            }],
        };
        let error = VM::new(&program).run().unwrap_err();
        assert_eq!(error.location.as_ref().unwrap().column, 1);
        assert!(error.to_string().contains("sqrt(nil);"));
    }

    #[test]
    fn metadata_free_runtime_errors_keep_legacy_format() {
        let error = VM::new(&empty_program()).execute_native_call("sqrt", vec![Value::Nil]).unwrap_err();
        assert_eq!(error.to_string(), "Runtime error: sqrt expects number");
    }

    #[test]
    fn invalid_debug_source_lookup_does_not_panic() {
        let mut program = debug_failure_program();
        program.functions[0].locations[2] = Some(DebugLocation { source: 99, line: 1, column: 1 });
        let error = VM::new(&program).run().unwrap_err();
        assert!(error.to_string().starts_with("Runtime error: "));
    }
}

impl RuntimeError {
    fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
            location: None,
            stack: Vec::new(),
            sources: Vec::new(),
        }
    }

    fn push_frame(&mut self, function: String, location: Option<DebugLocation>) {
        self.stack.push(StackFrame { function, location });
    }

    fn source_location(&self, location: &DebugLocation) -> Option<(&DebugSource, &str)> {
        let source = self.sources.get(location.source)?;
        if location.line == 0 || location.column == 0 {
            return None;
        }
        let line = source.text.split('\n').nth(location.line - 1)?;
        let line = line.strip_suffix('\r').unwrap_or(line);
        if location.column > line.len() + 1 {
            return None;
        }
        Some((source, line))
    }
}

impl fmt::Display for RuntimeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut output = String::new();
        if let Some(location) = self.location.as_ref() {
            if let Some((source, line)) = self.source_location(location) {
                output.push_str(&format!(
                    "Runtime error at {}:{}:{}: {}\n",
                    source.path, location.line, location.column, self.message
                ));
                output.push_str(&format!("  {}\n", line));
                output.push_str(&format!(
                    "  {}^",
                    " ".repeat(location.column.saturating_sub(1))
                ));
            } else {
                output.push_str(&format!("Runtime error: {}", self.message));
            }
        } else {
            output.push_str(&format!("Runtime error: {}", self.message));
        }

        let valid_frames = self
            .stack
            .iter()
            .filter_map(|frame| frame.location.as_ref().map(|location| (frame, location)))
            .filter(|(_, location)| self.source_location(location).is_some())
            .collect::<Vec<_>>();
        if !valid_frames.is_empty() {
            output.push_str("\nCall stack:\n");
            for (index, (frame, location)) in valid_frames.iter().enumerate() {
                let source = self
                    .sources
                    .get(location.source)
                    .expect("validated debug source");
                if index != 0 {
                    output.push('\n');
                }
                output.push_str(&format!(
                    "  at {} ({}:{}:{})",
                    frame.function, source.path, location.line, location.column
                ));
            }
        }
        write!(f, "{}", output)
    }
}

struct Frame {
    ip: usize,
    registers: Vec<Value>,
    locals: SharedEnvironment,
    closure: SharedEnvironment,
    is_main: bool,
    function: String,
    function_index: Option<usize>,
}

pub struct VM<'a> {
    program: &'a Program,
    globals: SharedEnvironment,
    output: String,
    next_function_identity: usize,
    next_array_identity: usize,
    next_map_identity: usize,
    next_struct_identity: usize,
}

impl<'a> VM<'a> {
    pub fn new(program: &'a Program) -> Self {
        Self {
            program,
            globals: new_environment(),
            output: String::new(),
            next_function_identity: 1,
            next_array_identity: 1,
            next_map_identity: 1,
            next_struct_identity: 1,
        }
    }

    pub fn run(mut self) -> Result<String, RuntimeError> {
        let mut frame = Frame {
            ip: 0,
            registers: vec![Value::Nil; self.program.main.registers],
            locals: new_environment(),
            closure: new_environment(),
            is_main: true,
            function: "main".to_string(),
            function_index: None,
        };
        let main = FunctionBody {
            registers: self.program.main.registers,
            instructions: self.program.main.instructions.clone(),
            locations: self.program.main.locations.clone(),
        };
        match self.execute_body(&main, &mut frame) {
            Ok(_) => Ok(self.output),
            Err(mut error) => {
                if error.sources.is_empty() {
                    error.sources = self.program.debug_sources.clone();
                }
                Err(error)
            }
        }
    }

    fn execute_body(
        &mut self,
        body: &FunctionBody,
        frame: &mut Frame,
    ) -> Result<Option<Value>, RuntimeError> {
        frame.ip = 0;
        while frame.ip < body.instructions.len() {
            let instruction = &body.instructions[frame.ip];
            let mut jumped = false;
            let result = (|| -> Result<Option<Value>, RuntimeError> {
                match instruction {
                Instruction::Constant { dest, constant } => {
                    let value = self.constant_value(*constant)?;
                    self.write_register(frame, *dest, value)?;
                }
                Instruction::Print { value } => {
                    let value = self.read_register(frame, *value)?;
                    self.output.push_str(&value.to_string());
                    self.output.push('\n');
                }
                Instruction::MakeFunction { dest, function } => {
                    let value = self.make_function(*function, frame)?;
                    self.write_register(frame, *dest, value)?;
                }
                Instruction::Array { dest, elements } => {
                    let mut values = Vec::with_capacity(elements.len());
                    for element in elements {
                        values.push(self.read_register(frame, *element)?);
                    }
                    let value = self.make_array(values);
                    self.write_register(frame, *dest, value)?;
                }
                Instruction::Map { dest, entries } => {
                    let mut values = Vec::with_capacity(entries.len());
                    for (key_register, value_register) in entries {
                        let key = self.read_register(frame, *key_register)?;
                        self.validate_map_key(&key)?;
                        let value = self.read_register(frame, *value_register)?;
                        values.push((key, value));
                    }
                    let value = self.make_map(values);
                    self.write_register(frame, *dest, value)?;
                }
                Instruction::Struct {
                    dest,
                    type_name,
                    fields,
                } => {
                    let type_name = type_name.map(|index| self.read_name(index)).transpose()?;
                    let value = self.make_struct(frame, type_name, fields)?;
                    self.write_register(frame, *dest, value)?;
                }
                Instruction::Variant {
                    dest,
                    enum_name,
                    variant_name,
                    payload,
                } => {
                    let enum_name = self.read_name(*enum_name)?;
                    let variant_name = self.read_name(*variant_name)?;
                    let mut fields = Vec::with_capacity(payload.len());
                    for register in payload {
                        fields.push(self.read_register(frame, *register)?);
                    }
                    self.write_register(
                        frame,
                        *dest,
                        Value::variant(crate::runtime::VariantValue {
                            enum_name,
                            variant_name,
                            fields,
                        }),
                    )?;
                }
                Instruction::VariantTag {
                    dest,
                    value,
                    enum_name,
                    variant_name,
                } => {
                    let input = self.read_register(frame, *value)?;
                    let enum_name = self.read_name(*enum_name)?;
                    let variant_name = self.read_name(*variant_name)?;
                    let matched = matches!(
                        input,
                        Value::Variant(ref variant)
                            if variant.enum_name == enum_name
                                && variant.variant_name == variant_name
                    );
                    self.write_register(frame, *dest, Value::boolean(matched))?;
                }
                Instruction::VariantField { dest, value, index } => {
                    let input = self.read_register(frame, *value)?;
                    let Value::Variant(variant) = input else {
                        return Err(RuntimeError::new("can only access fields on enum variants"));
                    };
                    let field = variant
                        .fields
                        .get(*index)
                        .cloned()
                        .ok_or_else(|| RuntimeError::new("enum variant field index out of bounds"))?;
                    self.write_register(frame, *dest, field)?;
                }
                Instruction::Move { dest, source } => {
                    let value = self.read_register(frame, *source)?;
                    self.write_register(frame, *dest, value)?;
                }
                Instruction::LoadVar { dest, name } => {
                    let name = self.read_name(*name)?;
                    let value = self.load_variable(frame, &name)?;
                    self.write_register(frame, *dest, value)?;
                }
                Instruction::StoreVar { name, value } => {
                    let name = self.read_name(*name)?;
                    let value = self.read_register(frame, *value)?;
                    self.store_variable(frame, name, value);
                }
                Instruction::AssignVar { name, value } => {
                    let name = self.read_name(*name)?;
                    let value = self.read_register(frame, *value)?;
                    self.assign_variable(frame, &name, value)?;
                }
                Instruction::Call {
                    dest,
                    callee,
                    arguments,
                } => {
                    let callee = self.read_register(frame, *callee)?;
                    let Value::Function(function) = callee else {
                        return Err(RuntimeError::new("can only call functions"));
                    };
                    let mut values = Vec::with_capacity(arguments.len());
                    for argument in arguments {
                        values.push(self.read_register(frame, *argument)?);
                    }
                    let call_site = body.locations.get(frame.ip).cloned().flatten();
                    let result = self.call_function(
                        function,
                        values,
                        frame.function.clone(),
                        call_site,
                    )?;
                    self.write_register(frame, *dest, result)?;
                }
                Instruction::NativeCall {
                    dest,
                    name,
                    arguments,
                } => {
                    let name = self.read_name(*name)?;
                    let mut values = Vec::with_capacity(arguments.len());
                    for argument in arguments {
                        values.push(self.read_register(frame, *argument)?);
                    }
                    let call_site = body.locations.get(frame.ip).cloned().flatten();
                    let result = self.execute_native_call_at(
                        &name,
                        values,
                        frame.function.clone(),
                        call_site,
                    )?;
                    self.write_register(frame, *dest, result)?;
                }
                Instruction::Negate { dest, value } => {
                    let input = self.expect_number(frame, *value, "negate")?;
                    self.write_register(frame, *dest, Value::number(-input))?;
                }
                Instruction::Not { dest, value } => {
                    let result = !self.read_register(frame, *value)?.is_truthy();
                    self.write_register(frame, *dest, Value::boolean(result))?;
                }
                Instruction::Add { dest, left, right } => {
                    let left_value = self.read_register(frame, *left)?;
                    let right_value = self.read_register(frame, *right)?;
                    let result = match (left_value, right_value) {
                        (Value::Number(left), Value::Number(right)) => Value::number(left + right),
                        (Value::String(left), Value::String(right)) => {
                            Value::string(format!("{}{}", left, right))
                        }
                        _ => {
                            return Err(RuntimeError::new("add expects two numbers or two strings"))
                        }
                    };
                    self.write_register(frame, *dest, result)?;
                }
                Instruction::Subtract { dest, left, right } => {
                    let (left, right) =
                        self.expect_two_numbers(frame, *left, *right, "subtract")?;
                    self.write_register(frame, *dest, Value::number(left - right))?;
                }
                Instruction::Multiply { dest, left, right } => {
                    let (left, right) =
                        self.expect_two_numbers(frame, *left, *right, "multiply")?;
                    self.write_register(frame, *dest, Value::number(left * right))?;
                }
                Instruction::Divide { dest, left, right } => {
                    let (left, right) = self.expect_two_numbers(frame, *left, *right, "divide")?;
                    if right == 0.0 {
                        return Err(RuntimeError::new("division by zero"));
                    }
                    self.write_register(frame, *dest, Value::number(left / right))?;
                }
                Instruction::Equal { dest, left, right } => {
                    let result = self
                        .read_register(frame, *left)?
                        .runtime_equals(&self.read_register(frame, *right)?);
                    self.write_register(frame, *dest, Value::boolean(result))?;
                }
                Instruction::NotEqual { dest, left, right } => {
                    let result = !self
                        .read_register(frame, *left)?
                        .runtime_equals(&self.read_register(frame, *right)?);
                    self.write_register(frame, *dest, Value::boolean(result))?;
                }
                Instruction::Greater { dest, left, right } => {
                    self.compare(frame, *dest, *left, *right, "greater", |l, r| l > r)?
                }
                Instruction::GreaterEqual { dest, left, right } => {
                    self.compare(frame, *dest, *left, *right, "greater_equal", |l, r| l >= r)?
                }
                Instruction::Less { dest, left, right } => {
                    self.compare(frame, *dest, *left, *right, "less", |l, r| l < r)?
                }
                Instruction::LessEqual { dest, left, right } => {
                    self.compare(frame, *dest, *left, *right, "less_equal", |l, r| l <= r)?
                }
                Instruction::Jump { target } => {
                    self.validate_jump_target(*target, body.instructions.len())?;
                    frame.ip = *target;
                    jumped = true;
                    return Ok(None);
                }
                Instruction::JumpIfFalse { condition, target } => {
                    self.validate_jump_target(*target, body.instructions.len())?;
                    if !self.read_register(frame, *condition)?.is_truthy() {
                        frame.ip = *target;
                        jumped = true;
                        return Ok(None);
                    }
                }
                Instruction::JumpIfTrue { condition, target } => {
                    self.validate_jump_target(*target, body.instructions.len())?;
                    if self.read_register(frame, *condition)?.is_truthy() {
                        frame.ip = *target;
                        jumped = true;
                        return Ok(None);
                    }
                }
                Instruction::Index {
                    dest,
                    collection,
                    index,
                } => {
                    let collection = self.read_register(frame, *collection)?;
                    let index = self.read_register(frame, *index)?;
                    let value = self.execute_index(collection, index)?;
                    self.write_register(frame, *dest, value)?;
                }
                Instruction::AssignIndex {
                    dest,
                    collection,
                    index,
                    value,
                } => {
                    let collection = self.read_register(frame, *collection)?;
                    let index = self.read_register(frame, *index)?;
                    let value = self.read_register(frame, *value)?;
                    let assigned = self.execute_assign_index(collection, index, value)?;
                    self.write_register(frame, *dest, assigned)?;
                }
                Instruction::Field { dest, object, name } => {
                    let object = self.read_register(frame, *object)?;
                    let name = self.read_name(*name)?;
                    let value = self.execute_field(object, &name)?;
                    self.write_register(frame, *dest, value)?;
                }
                Instruction::AssignField {
                    dest,
                    object,
                    name,
                    value,
                } => {
                    let object = self.read_register(frame, *object)?;
                    let name = self.read_name(*name)?;
                    let value = self.read_register(frame, *value)?;
                    let assigned = self.execute_assign_field(object, &name, value)?;
                    self.write_register(frame, *dest, assigned)?;
                }
                Instruction::Len { dest, value } => {
                    let value = self.read_register(frame, *value)?;
                    let length = self.execute_len(value)?;
                    self.write_register(frame, *dest, length)?;
                }
                Instruction::AssertArray { dest, value } => {
                    let input = self.read_register(frame, *value)?;
                    let iterable = match input {
                        Value::Array(_) | Value::Range(_) => input,
                        Value::Map(map) => {
                            let keys = map
                                .entries
                                .borrow()
                                .iter()
                                .map(|(key, _)| key.clone())
                                .collect();
                            self.make_array(keys)
                        }
                        _ => {
                            return Err(RuntimeError::new(
                                "for-in expects array, range, or map",
                            ));
                        }
                    };
                    self.write_register(frame, *dest, iterable)?;
                }
                Instruction::AssertNumber {
                    dest,
                    value,
                    message,
                } => {
                    let input = self.read_register(frame, *value)?;
                    if !matches!(input, Value::Number(_)) {
                        let message = self.read_name(*message)?;
                        return Err(RuntimeError::new(message));
                    }
                    self.write_register(frame, *dest, input)?;
                }
                Instruction::Return { value } => {
                    return Ok(Some(self.read_register(frame, *value)?))
                }
                }
                Ok(None)
            })();
            match result {
                Ok(Some(value)) => return Ok(Some(value)),
                Ok(None) => {
                    if !jumped {
                        frame.ip += 1;
                    }
                }
                Err(mut error) => {
                    let location = body.locations.get(frame.ip).cloned().flatten();
                    if error.location.is_none() {
                        error.location = location.clone();
                    }
                    if error.stack.is_empty() {
                        error.push_frame(frame.function.clone(), location);
                    }
                    return Err(error);
                }
            }
        }
        Ok(None)
    }

    fn capture_environment(&self, frame: &Frame) -> SharedEnvironment {
        let captured = new_environment();
        {
            let mut target = captured.borrow_mut();
            for (name, cell) in frame.closure.borrow().iter() {
                target.insert(name.clone(), cell.clone());
            }
            for (name, cell) in frame.locals.borrow().iter() {
                target.insert(name.clone(), cell.clone());
            }
        }
        captured
    }

    fn make_function(
        &mut self,
        function_index: usize,
        frame: &Frame,
    ) -> Result<Value, RuntimeError> {
        let function = self
            .program
            .functions
            .get(function_index)
            .ok_or_else(|| RuntimeError::new("function index out of range"))?;
        let identity = self.next_function_identity;
        self.next_function_identity += 1;
        Ok(Value::function(FunctionValue {
            name: function.name.clone(),
            function_index,
            arity: function.params.len(),
            identity,
            closure: self.capture_environment(frame),
        }))
    }

    fn call_function(
        &mut self,
        function: FunctionValue,
        arguments: Vec<Value>,
        caller: String,
        call_site: Option<DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        let Some(bytecode_function) = self.program.functions.get(function.function_index) else {
            let mut error = RuntimeError::new("function index out of range");
            error.location = call_site.clone();
            error.push_frame(caller, call_site);
            return Err(error);
        };
        let params = bytecode_function.params.clone();
        let registers = bytecode_function.registers;
        let instructions = bytecode_function.instructions.clone();

        if arguments.len() != params.len() {
            let mut error = RuntimeError::new(format!(
                "expected {} arguments but got {}",
                params.len(),
                arguments.len()
            ));
            error.location = call_site.clone();
            error.push_frame(caller, call_site);
            return Err(error);
        }

        let mut frame = Frame {
            ip: 0,
            registers: vec![Value::Nil; registers],
            locals: new_environment(),
            closure: function.closure.clone(),
            is_main: false,
            function: bytecode_function.name.clone(),
            function_index: Some(function.function_index),
        };

        for (index, argument) in arguments.into_iter().enumerate() {
            frame
                .locals
                .borrow_mut()
                .insert(params[index].clone(), new_cell(argument));
        }

        let body = FunctionBody {
            registers,
            instructions,
            locations: bytecode_function.locations.clone(),
        };
        match self.execute_body(&body, &mut frame) {
            Ok(result) => Ok(result.unwrap_or(Value::Nil)),
            Err(mut error) => {
                if error.location.is_none() {
                    error.location = call_site.clone();
                }
                error.push_frame(caller, call_site);
                Err(error)
            }
        }
    }

    fn make_array(&mut self, elements: Vec<Value>) -> Value {
        let identity = self.next_array_identity;
        self.next_array_identity += 1;
        Value::array(ArrayValue {
            identity,
            elements: Rc::new(RefCell::new(elements)),
        })
    }

    fn make_map(&mut self, entries: Vec<(Value, Value)>) -> Value {
        let identity = self.next_map_identity;
        self.next_map_identity += 1;
        let mut ordered: Vec<(Value, Value)> = Vec::with_capacity(entries.len());
        for (key, value) in entries {
            if let Some((_, existing)) = ordered
                .iter_mut()
                .find(|(existing_key, _)| existing_key.runtime_equals(&key))
            {
                *existing = value;
            } else {
                ordered.push((key, value));
            }
        }
        Value::map(MapValue {
            identity,
            entries: Rc::new(RefCell::new(ordered)),
        })
    }

    fn range_bound(value: &Value, position: usize) -> Result<i64, RuntimeError> {
        let ordinal = match position {
            0 => "first",
            1 => "second",
            _ => "third",
        };
        let Value::Number(number) = value else {
            return Err(RuntimeError::new(format!(
                "range expects number as {} argument",
                ordinal
            )));
        };
        if !number.is_finite() || number.fract() != 0.0 {
            return Err(RuntimeError::new(format!(
                "range expects integer as {} argument",
                ordinal
            )));
        }
        const MIN: f64 = -9223372036854775808.0;
        const MAX_EXCLUSIVE: f64 = 9223372036854775808.0;
        if *number < MIN || *number >= MAX_EXCLUSIVE {
            return Err(RuntimeError::new("range bound out of range"));
        }
        Ok(*number as i64)
    }

    fn range_length(start: i64, stop: i64, step: i64) -> Result<usize, RuntimeError> {
        let start = start as i128;
        let stop = stop as i128;
        let step = step as i128;
        let length = if step > 0 && start < stop {
            ((stop - start - 1) / step + 1) as u128
        } else if step < 0 && start > stop {
            ((start - stop - 1) / (-step) + 1) as u128
        } else {
            0
        };
        if length > usize::MAX as u128 {
            return Err(RuntimeError::new("range length out of bounds"));
        }
        Ok(length as usize)
    }

    fn execute_native_range(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.is_empty() || arguments.len() > 3 {
            return Err(RuntimeError::new("range expects 1 to 3 arguments"));
        }
        let (start, stop, step) = match arguments.len() {
            1 => (0, Self::range_bound(&arguments[0], 0)?, 1),
            2 => (
                Self::range_bound(&arguments[0], 0)?,
                Self::range_bound(&arguments[1], 1)?,
                1,
            ),
            3 => (
                Self::range_bound(&arguments[0], 0)?,
                Self::range_bound(&arguments[1], 1)?,
                Self::range_bound(&arguments[2], 2)?,
            ),
            _ => unreachable!(),
        };
        if step == 0 {
            return Err(RuntimeError::new("range step must not be zero"));
        }
        let length = Self::range_length(start, stop, step)?;
        Ok(Value::range(RangeValue {
            start,
            stop,
            step,
            length,
        }))
    }

    fn validate_map_key(&self, key: &Value) -> Result<(), RuntimeError> {
        if matches!(key, Value::Nil | Value::Number(_) | Value::Bool(_) | Value::String(_)) {
            Ok(())
        } else {
            Err(RuntimeError::new("map key must be nil, number, bool, or string"))
        }
    }

    fn make_struct(
        &mut self,
        frame: &Frame,
        type_name: Option<String>,
        fields: &[(usize, usize)],
    ) -> Result<Value, RuntimeError> {
        let identity = self.next_struct_identity;
        self.next_struct_identity += 1;
        let mut values = Vec::with_capacity(fields.len());
        for (name_index, register) in fields {
            values.push((
                self.read_name(*name_index)?,
                self.read_register(frame, *register)?,
            ));
        }
        Ok(Value::structure(StructValue {
            identity,
            type_name,
            fields: Rc::new(RefCell::new(values)),
        }))
    }

    fn checked_array_index(&self, index_value: Value) -> Result<usize, RuntimeError> {
        let Value::Number(number) = index_value else {
            return Err(RuntimeError::new("array index must be number"));
        };
        let integer = number.trunc();
        if integer != number {
            return Err(RuntimeError::new("array index must be integer"));
        }
        if integer < 0.0 {
            return Err(RuntimeError::new("array index out of range"));
        }
        Ok(integer as usize)
    }

    fn execute_index(&self, collection: Value, index: Value) -> Result<Value, RuntimeError> {
        match collection {
            Value::Array(array) => {
                let position = self.checked_array_index(index)?;
                let elements = array.elements.borrow();
                elements
                    .get(position)
                    .cloned()
                    .ok_or_else(|| RuntimeError::new("array index out of range"))
            }
            Value::Map(map) => {
                self.validate_map_key(&index)?;
                map.entries
                    .borrow()
                    .iter()
                    .find(|(key, _)| key.runtime_equals(&index))
                    .map(|(_, value)| value.clone())
                    .ok_or_else(|| RuntimeError::new("map key not found"))
            }
            Value::Range(range) => {
                if range.length == 0 {
                    return Err(RuntimeError::new("range index out of bounds"));
                }
                let position = Self::checked_integer_index(
                    match index {
                        Value::Number(value) => value,
                        _ => return Err(RuntimeError::new("range index must be number")),
                    },
                    "range index must be integer",
                    "range index out of bounds",
                    range.length.saturating_sub(1),
                )?;
                let value = range.start as i128 + range.step as i128 * position as i128;
                Ok(Value::number(value as i64 as f64))
            }
            _ => Err(RuntimeError::new("can only index arrays, maps, or ranges")),
        }
    }

    fn execute_assign_index(
        &self,
        collection: Value,
        index: Value,
        value: Value,
    ) -> Result<Value, RuntimeError> {
        match collection {
            Value::Array(array) => {
                let position = self.checked_array_index(index)?;
                let mut elements = array.elements.borrow_mut();
                if position >= elements.len() {
                    return Err(RuntimeError::new("array index out of range"));
                }
                elements[position] = value.clone();
                Ok(value)
            }
            Value::Map(map) => {
                self.validate_map_key(&index)?;
                let mut entries = map.entries.borrow_mut();
                if let Some((_, existing)) = entries
                    .iter_mut()
                    .find(|(key, _)| key.runtime_equals(&index))
                {
                    *existing = value.clone();
                } else {
                    entries.push((index, value.clone()));
                }
                Ok(value)
            }
            Value::Range(_) => Err(RuntimeError::new("cannot assign range elements")),
            _ => Err(RuntimeError::new("can only assign array elements, map entries, or range elements")),
        }
    }

    fn execute_field(&self, object: Value, name: &str) -> Result<Value, RuntimeError> {
        let Value::Struct(value) = object else {
            return Err(RuntimeError::new("can only access fields on structs"));
        };
        for (field_name, field_value) in value.fields.borrow().iter() {
            if field_name == name {
                return Ok(field_value.clone());
            }
        }
        Err(RuntimeError::new(format!("undefined field `{}`", name)))
    }

    fn execute_assign_field(
        &self,
        object: Value,
        name: &str,
        value: Value,
    ) -> Result<Value, RuntimeError> {
        let Value::Struct(struct_value) = object else {
            return Err(RuntimeError::new("can only assign fields on structs"));
        };
        let mut fields = struct_value.fields.borrow_mut();
        for (field_name, field_value) in fields.iter_mut() {
            if field_name == name {
                *field_value = value.clone();
                return Ok(value);
            }
        }
        Err(RuntimeError::new(format!("undefined field `{}`", name)))
    }

    fn execute_len(&self, value: Value) -> Result<Value, RuntimeError> {
        match value {
            Value::Array(array) => Ok(Value::number(array.elements.borrow().len() as f64)),
            Value::Map(map) => Ok(Value::number(map.entries.borrow().len() as f64)),
            Value::Range(range) => Ok(Value::number(range.length as f64)),
            Value::String(value) => Ok(Value::number(value.chars().count() as f64)),
            _ => Err(RuntimeError::new("len expects array, string, map, or range")),
        }
    }

    fn execute_native_call(
        &mut self,
        name: &str,
        arguments: Vec<Value>,
    ) -> Result<Value, RuntimeError> {
        self.execute_native_call_at(name, arguments, "<native>".to_string(), None)
    }

    fn execute_native_call_at(
        &mut self,
        name: &str,
        arguments: Vec<Value>,
        caller: String,
        call_site: Option<DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        match name {
            "push" => self.execute_native_push(arguments),
            "pop" => self.execute_native_pop(arguments),
            "remove" => self.execute_native_remove(arguments),
            "clear" => self.execute_native_clear(arguments),
            "merge" => self.execute_native_merge(arguments),
            "keys" => self.execute_native_keys(arguments),
            "values" => self.execute_native_values(arguments),
            "floor" => self.execute_native_floor(arguments),
            "ceil" => self.execute_native_ceil(arguments),
            "sqrt" => self.execute_native_sqrt(arguments),
            "str" => self.execute_native_str(arguments),
            "substr" => self.execute_native_substr(arguments),
            "charAt" => self.execute_native_char_at(arguments),
            "typeOf" => self.execute_native_type_of(arguments),
            "contains" => self.execute_native_contains(arguments),
            "slice" => self.execute_native_slice(arguments),
            "copy" => self.execute_native_copy(arguments),
            "concat" => self.execute_native_concat(arguments),
            "map" => self.execute_native_map(arguments, caller, call_site),
            "filter" => self.execute_native_filter(arguments, caller, call_site),
            "flatMap" => self.execute_native_flat_map(arguments, caller, call_site),
            "any" => self.execute_native_any_all(arguments, caller, call_site, true),
            "all" => self.execute_native_any_all(arguments, caller, call_site, false),
            "count" => self.execute_native_count(arguments, caller, call_site),
            "find" => self.execute_native_find(arguments, caller, call_site),
            "findIndex" => self.execute_native_find_index(arguments, caller, call_site),
            "reduce" => self.execute_native_reduce(arguments, caller, call_site),
            "range" => self.execute_native_range(arguments),
            _ => Err(RuntimeError::new(format!(
                "unknown native stdlib function `{}`",
                name
            ))),
        }
    }

    fn execute_native_map(
        &mut self,
        arguments: Vec<Value>,
        caller: String,
        call_site: Option<DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        if arguments.len() != 2 {
            return Err(RuntimeError::new("map expects 2 arguments"));
        }
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("map expects array as first argument"));
        };
        let Value::Function(callback) = &arguments[1] else {
            return Err(RuntimeError::new("map expects function as second argument"));
        };
        if callback.arity != 1 {
            return Err(RuntimeError::new("map expects callback with 1 argument"));
        }

        let elements = array.elements.borrow().clone();
        let mut mapped = Vec::with_capacity(elements.len());
        for element in elements {
            mapped.push(self.call_function(
                callback.clone(),
                vec![element],
                caller.clone(),
                call_site.clone(),
            )?);
        }
        Ok(self.make_array(mapped))
    }

    fn execute_native_filter(
        &mut self,
        arguments: Vec<Value>,
        caller: String,
        call_site: Option<DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        if arguments.len() != 2 {
            return Err(RuntimeError::new("filter expects 2 arguments"));
        }
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("filter expects array as first argument"));
        };
        let Value::Function(predicate) = &arguments[1] else {
            return Err(RuntimeError::new("filter expects function as second argument"));
        };
        if predicate.arity != 1 {
            return Err(RuntimeError::new("filter expects callback with 1 argument"));
        }

        let elements = array.elements.borrow().clone();
        let mut filtered = Vec::with_capacity(elements.len());
        for element in elements {
            let keep = self.call_function(
                predicate.clone(),
                vec![element.clone()],
                caller.clone(),
                call_site.clone(),
            )?;
            match keep {
                Value::Bool(true) => filtered.push(element),
                Value::Bool(false) => {}
                _ => return Err(RuntimeError::new("filter expects callback to return bool")),
            }
        }
        Ok(self.make_array(filtered))
    }

    fn execute_native_flat_map(
        &mut self,
        arguments: Vec<Value>,
        caller: String,
        call_site: Option<DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        if arguments.len() != 2 {
            return Err(RuntimeError::new("flatMap expects 2 arguments"));
        }
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("flatMap expects array as first argument"));
        };
        let Value::Function(callback) = &arguments[1] else {
            return Err(RuntimeError::new("flatMap expects function as second argument"));
        };
        if callback.arity != 1 {
            return Err(RuntimeError::new("flatMap expects callback with 1 argument"));
        }

        let elements = array.elements.borrow().clone();
        let mut flattened = Vec::new();
        for element in elements {
            let result = self.call_function(
                callback.clone(),
                vec![element],
                caller.clone(),
                call_site.clone(),
            )?;
            let Value::Array(mapped) = result else {
                return Err(RuntimeError::new("flatMap expects callback to return array"));
            };
            flattened.extend(mapped.elements.borrow().iter().cloned());
        }
        Ok(self.make_array(flattened))
    }

    fn execute_native_any_all(
        &mut self,
        arguments: Vec<Value>,
        caller: String,
        call_site: Option<DebugLocation>,
        any: bool,
    ) -> Result<Value, RuntimeError> {
        let name = if any { "any" } else { "all" };
        if arguments.len() != 2 {
            return Err(RuntimeError::new(format!("{} expects 2 arguments", name)));
        }
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new(format!(
                "{} expects array as first argument",
                name
            )));
        };
        let Value::Function(predicate) = &arguments[1] else {
            return Err(RuntimeError::new(format!(
                "{} expects function as second argument",
                name
            )));
        };
        if predicate.arity != 1 {
            return Err(RuntimeError::new(format!(
                "{} expects callback with 1 argument",
                name
            )));
        }

        let elements = array.elements.borrow().clone();
        for element in elements {
            let result = self.call_function(
                predicate.clone(),
                vec![element],
                caller.clone(),
                call_site.clone(),
            )?;
            let Value::Bool(result) = result else {
                return Err(RuntimeError::new(format!(
                    "{} expects callback to return bool",
                    name
                )));
            };
            if (any && result) || (!any && !result) {
                return Ok(Value::boolean(result));
            }
        }
        Ok(Value::boolean(!any))
    }

    fn execute_native_count(
        &mut self,
        arguments: Vec<Value>,
        caller: String,
        call_site: Option<DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        if arguments.len() != 2 {
            return Err(RuntimeError::new("count expects 2 arguments"));
        }
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("count expects array as first argument"));
        };
        let Value::Function(predicate) = &arguments[1] else {
            return Err(RuntimeError::new("count expects function as second argument"));
        };
        if predicate.arity != 1 {
            return Err(RuntimeError::new("count expects callback with 1 argument"));
        }

        let elements = array.elements.borrow().clone();
        let mut count = 0usize;
        for element in elements {
            let result = self.call_function(
                predicate.clone(),
                vec![element],
                caller.clone(),
                call_site.clone(),
            )?;
            match result {
                Value::Bool(true) => count += 1,
                Value::Bool(false) => {}
                _ => return Err(RuntimeError::new("count expects callback to return bool")),
            }
        }
        Ok(Value::number(count as f64))
    }

    fn execute_native_find(
        &mut self,
        arguments: Vec<Value>,
        caller: String,
        call_site: Option<DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        if arguments.len() != 2 {
            return Err(RuntimeError::new("find expects 2 arguments"));
        }
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("find expects array as first argument"));
        };
        let Value::Function(predicate) = &arguments[1] else {
            return Err(RuntimeError::new("find expects function as second argument"));
        };
        if predicate.arity != 1 {
            return Err(RuntimeError::new("find expects callback with 1 argument"));
        }

        let elements = array.elements.borrow().clone();
        for element in elements {
            let result = self.call_function(
                predicate.clone(),
                vec![element.clone()],
                caller.clone(),
                call_site.clone(),
            )?;
            match result {
                Value::Bool(true) => return Ok(element),
                Value::Bool(false) => {}
                _ => return Err(RuntimeError::new("find expects callback to return bool")),
            }
        }
        Ok(Value::Nil)
    }

    fn execute_native_find_index(
        &mut self,
        arguments: Vec<Value>,
        caller: String,
        call_site: Option<DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        if arguments.len() != 2 {
            return Err(RuntimeError::new("findIndex expects 2 arguments"));
        }
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("findIndex expects array as first argument"));
        };
        let Value::Function(predicate) = &arguments[1] else {
            return Err(RuntimeError::new("findIndex expects function as second argument"));
        };
        if predicate.arity != 1 {
            return Err(RuntimeError::new("findIndex expects callback with 1 argument"));
        }

        let elements = array.elements.borrow().clone();
        for (index, element) in elements.into_iter().enumerate() {
            let result = self.call_function(
                predicate.clone(),
                vec![element],
                caller.clone(),
                call_site.clone(),
            )?;
            match result {
                Value::Bool(true) => return Ok(Value::number(index as f64)),
                Value::Bool(false) => {}
                _ => return Err(RuntimeError::new("findIndex expects callback to return bool")),
            }
        }
        Ok(Value::number(-1.0))
    }

    fn execute_native_reduce(
        &mut self,
        arguments: Vec<Value>,
        caller: String,
        call_site: Option<DebugLocation>,
    ) -> Result<Value, RuntimeError> {
        if arguments.len() != 3 {
            return Err(RuntimeError::new("reduce expects 3 arguments"));
        }
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("reduce expects array as first argument"));
        };
        let Value::Function(callback) = &arguments[2] else {
            return Err(RuntimeError::new("reduce expects function as third argument"));
        };
        if callback.arity != 2 {
            return Err(RuntimeError::new("reduce expects callback with 2 arguments"));
        }

        let elements = array.elements.borrow().clone();
        let mut accumulator = arguments[1].clone();
        for element in elements {
            accumulator = self.call_function(
                callback.clone(),
                vec![accumulator, element],
                caller.clone(),
                call_site.clone(),
            )?;
        }
        Ok(accumulator)
    }

    fn execute_native_push(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 2 {
            return Err(RuntimeError::new("push expects 2 arguments"));
        }
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("push expects array as first argument"));
        };
        array.elements.borrow_mut().push(arguments[1].clone());
        Ok(Value::Nil)
    }

    fn execute_native_pop(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 1 {
            return Err(RuntimeError::new("pop expects 1 arguments"));
        }
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("pop expects array as first argument"));
        };
        array
            .elements
            .borrow_mut()
            .pop()
            .ok_or_else(|| RuntimeError::new("cannot pop from empty array"))
    }

    fn execute_native_remove(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 2 {
            return Err(RuntimeError::new("remove expects 2 arguments"));
        }
        let Value::Map(map) = &arguments[0] else {
            return Err(RuntimeError::new("remove expects map as first argument"));
        };
        self.validate_map_key(&arguments[1])?;
        let mut entries = map.entries.borrow_mut();
        let position = entries
            .iter()
            .position(|(key, _)| key.runtime_equals(&arguments[1]))
            .ok_or_else(|| RuntimeError::new("map key not found"))?;
        Ok(entries.remove(position).1)
    }

    fn execute_native_clear(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 1 {
            return Err(RuntimeError::new("clear expects 1 argument"));
        }
        let Value::Map(map) = &arguments[0] else {
            return Err(RuntimeError::new("clear expects map as first argument"));
        };
        map.entries.borrow_mut().clear();
        Ok(Value::Nil)
    }

    fn execute_native_merge(&mut self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 2 {
            return Err(RuntimeError::new("merge expects 2 arguments"));
        }
        let Value::Map(left) = &arguments[0] else {
            return Err(RuntimeError::new("merge expects map as first argument"));
        };
        let Value::Map(right) = &arguments[1] else {
            return Err(RuntimeError::new("merge expects map as second argument"));
        };
        let mut entries = left.entries.borrow().clone();
        entries.extend(right.entries.borrow().iter().cloned());
        Ok(self.make_map(entries))
    }

    fn execute_native_keys(&mut self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 1 {
            return Err(RuntimeError::new("keys expects 1 argument"));
        }
        let Value::Map(map) = &arguments[0] else {
            return Err(RuntimeError::new("keys expects map as first argument"));
        };
        let elements = map
            .entries
            .borrow()
            .iter()
            .map(|(key, _)| key.clone())
            .collect();
        Ok(self.make_array(elements))
    }

    fn execute_native_values(&mut self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 1 {
            return Err(RuntimeError::new("values expects 1 argument"));
        }
        let Value::Map(map) = &arguments[0] else {
            return Err(RuntimeError::new("values expects map as first argument"));
        };
        let elements = map
            .entries
            .borrow()
            .iter()
            .map(|(_, value)| value.clone())
            .collect();
        Ok(self.make_array(elements))
    }

    fn execute_native_floor(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 1 {
            return Err(RuntimeError::new("floor expects 1 arguments"));
        }
        let Value::Number(value) = &arguments[0] else {
            return Err(RuntimeError::new("floor expects number"));
        };
        Ok(Value::number(value.floor()))
    }

    fn execute_native_ceil(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 1 {
            return Err(RuntimeError::new("ceil expects 1 arguments"));
        }
        let Value::Number(value) = &arguments[0] else {
            return Err(RuntimeError::new("ceil expects number"));
        };
        Ok(Value::number(value.ceil()))
    }

    fn execute_native_sqrt(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 1 {
            return Err(RuntimeError::new("sqrt expects 1 arguments"));
        }
        let Value::Number(value) = &arguments[0] else {
            return Err(RuntimeError::new("sqrt expects number"));
        };
        if *value < 0.0 {
            return Err(RuntimeError::new("sqrt expects non-negative number"));
        }
        Ok(Value::number(value.sqrt()))
    }

    fn checked_integer_index(
        value: f64,
        integer_message: &'static str,
        bounds_message: &'static str,
        upper_bound_inclusive: usize,
    ) -> Result<usize, RuntimeError> {
        if !value.is_finite() || value.floor() != value {
            return Err(RuntimeError::new(integer_message));
        }
        if value < 0.0 || value > upper_bound_inclusive as f64 {
            return Err(RuntimeError::new(bounds_message));
        }
        Ok(value as usize)
    }

    fn string_scalar_offsets(text: &str) -> Vec<usize> {
        let mut offsets = text
            .char_indices()
            .map(|(offset, _)| offset)
            .collect::<Vec<_>>();
        offsets.push(text.len());
        offsets
    }

    fn execute_native_str(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 1 {
            return Err(RuntimeError::new("str expects 1 arguments"));
        }
        Ok(Value::string(arguments[0].to_string()))
    }

    fn execute_native_substr(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 3 {
            return Err(RuntimeError::new("substr expects 3 arguments"));
        }
        let Value::String(text) = &arguments[0] else {
            return Err(RuntimeError::new("substr expects string as first argument"));
        };
        let Value::Number(start_value) = &arguments[1] else {
            return Err(RuntimeError::new(
                "substr expects number as second argument",
            ));
        };
        let Value::Number(length_value) = &arguments[2] else {
            return Err(RuntimeError::new("substr expects number as third argument"));
        };

        let offsets = Self::string_scalar_offsets(text);
        let scalar_count = offsets.len() - 1;
        let start = Self::checked_integer_index(
            *start_value,
            "substr expects integer start offset",
            "substr start offset out of bounds",
            scalar_count,
        )?;
        let length = Self::checked_integer_index(
            *length_value,
            "substr expects integer length",
            "substr length out of bounds",
            scalar_count,
        )?;
        if length > scalar_count - start {
            return Err(RuntimeError::new("substr length out of bounds"));
        }
        let begin = offsets[start];
        let end = offsets[start + length];
        Ok(Value::string(text[begin..end].to_string()))
    }

    fn execute_native_char_at(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 2 {
            return Err(RuntimeError::new("charAt expects 2 arguments"));
        }
        let Value::String(text) = &arguments[0] else {
            return Err(RuntimeError::new("charAt expects string as first argument"));
        };
        let Value::Number(index_value) = &arguments[1] else {
            return Err(RuntimeError::new(
                "charAt expects number as second argument",
            ));
        };
        let offsets = Self::string_scalar_offsets(text);
        let scalar_count = offsets.len() - 1;
        if scalar_count == 0 {
            return Err(RuntimeError::new("charAt index out of bounds"));
        }
        let index = Self::checked_integer_index(
            *index_value,
            "charAt expects integer index",
            "charAt index out of bounds",
            scalar_count - 1,
        )?;
        let begin = offsets[index];
        let end = offsets[index + 1];
        Ok(Value::string(text[begin..end].to_string()))
    }

    fn execute_native_type_of(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 1 {
            return Err(RuntimeError::new("typeOf expects 1 arguments"));
        }
        Ok(Value::string(arguments[0].type_name()))
    }

    fn execute_native_contains(&self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 2 {
            return Err(RuntimeError::new("contains expects 2 arguments"));
        }
        match &arguments[0] {
            Value::Array(array) => {
                let found = array
                    .elements
                    .borrow()
                    .iter()
                    .any(|element| element.runtime_equals(&arguments[1]));
                Ok(Value::boolean(found))
            }
            Value::Map(map) => {
                self.validate_map_key(&arguments[1])?;
                let found = map
                    .entries
                    .borrow()
                    .iter()
                    .any(|(key, _)| key.runtime_equals(&arguments[1]));
                Ok(Value::boolean(found))
            }
            Value::Range(range) => {
                let Value::Number(number) = &arguments[1] else {
                    return Err(RuntimeError::new("contains expects number for range"));
                };
                if !number.is_finite() || number.fract() != 0.0 {
                    return Ok(Value::boolean(false));
                }
                const MIN: f64 = -9223372036854775808.0;
                const MAX_EXCLUSIVE: f64 = 9223372036854775808.0;
                if *number < MIN || *number >= MAX_EXCLUSIVE {
                    return Ok(Value::boolean(false));
                }
                let candidate = *number as i64;
                let in_bounds = if range.step > 0 {
                    candidate >= range.start && candidate < range.stop
                } else {
                    candidate <= range.start && candidate > range.stop
                };
                let offset = candidate as i128 - range.start as i128;
                Ok(Value::boolean(in_bounds && offset % range.step as i128 == 0))
            }
            _ => Err(RuntimeError::new(
                "contains expects array, map, or range as first argument",
            )),
        }
    }

    fn execute_native_slice(&mut self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 3 {
            return Err(RuntimeError::new("slice expects 3 arguments"));
        }
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("slice expects array as first argument"));
        };
        let Value::Number(start_value) = &arguments[1] else {
            return Err(RuntimeError::new("slice expects number as second argument"));
        };
        let Value::Number(length_value) = &arguments[2] else {
            return Err(RuntimeError::new("slice expects number as third argument"));
        };

        let source_len = array.elements.borrow().len();
        let start = Self::checked_integer_index(
            *start_value,
            "slice expects integer start offset",
            "slice start offset out of bounds",
            source_len,
        )?;
        let length = Self::checked_integer_index(
            *length_value,
            "slice expects integer length",
            "slice length out of bounds",
            source_len,
        )?;
        if length > source_len - start {
            return Err(RuntimeError::new("slice length out of bounds"));
        }
        let elements = array.elements.borrow()[start..start + length].to_vec();
        Ok(self.make_array(elements))
    }

    fn execute_native_copy(&mut self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 1 {
            return Err(RuntimeError::new("copy expects 1 argument"));
        }
        let Value::Array(array) = &arguments[0] else {
            return Err(RuntimeError::new("copy expects array as first argument"));
        };
        let elements = array.elements.borrow().clone();
        Ok(self.make_array(elements))
    }

    fn execute_native_concat(&mut self, arguments: Vec<Value>) -> Result<Value, RuntimeError> {
        if arguments.len() != 2 {
            return Err(RuntimeError::new("concat expects 2 arguments"));
        }
        let Value::Array(left) = &arguments[0] else {
            return Err(RuntimeError::new("concat expects array as first argument"));
        };
        let Value::Array(right) = &arguments[1] else {
            return Err(RuntimeError::new("concat expects array as second argument"));
        };
        let mut elements = left.elements.borrow().clone();
        elements.extend(right.elements.borrow().iter().cloned());
        Ok(self.make_array(elements))
    }

    fn read_name(&self, index: usize) -> Result<String, RuntimeError> {
        self.program
            .names
            .get(index)
            .cloned()
            .ok_or_else(|| RuntimeError::new("name index out of range"))
    }

    fn find_cell(&self, frame: &Frame, name: &str) -> Option<Cell> {
        if let Some(cell) = frame.locals.borrow().get(name) {
            return Some(cell.clone());
        }
        if let Some(cell) = frame.closure.borrow().get(name) {
            return Some(cell.clone());
        }
        self.globals.borrow().get(name).cloned()
    }

    fn load_variable(&self, frame: &Frame, name: &str) -> Result<Value, RuntimeError> {
        let cell = self
            .find_cell(frame, name)
            .ok_or_else(|| RuntimeError::new(format!("undefined variable `{}`", name)))?;
        let value = cell.borrow().clone();
        Ok(value)
    }

    fn store_variable(&self, frame: &mut Frame, name: String, value: Value) {
        let cell = new_cell(value);
        if frame.is_main {
            self.globals.borrow_mut().insert(name, cell);
        } else {
            frame.locals.borrow_mut().insert(name, cell);
        }
    }

    fn assign_variable(&self, frame: &Frame, name: &str, value: Value) -> Result<(), RuntimeError> {
        let cell = self
            .find_cell(frame, name)
            .ok_or_else(|| RuntimeError::new(format!("undefined variable `{}`", name)))?;
        *cell.borrow_mut() = value;
        Ok(())
    }

    fn validate_jump_target(
        &self,
        target: usize,
        instruction_count: usize,
    ) -> Result<(), RuntimeError> {
        if target > instruction_count {
            Err(RuntimeError::new("jump target out of range"))
        } else {
            Ok(())
        }
    }

    fn constant_value(&self, index: usize) -> Result<Value, RuntimeError> {
        let constant = self
            .program
            .constants
            .get(index)
            .ok_or_else(|| RuntimeError::new("constant index out of range"))?;
        match constant {
            Constant::Nil => Ok(Value::Nil),
            Constant::Number(value) => value
                .parse::<f64>()
                .map(Value::number)
                .map_err(|_| RuntimeError::new("invalid number constant")),
            Constant::Bool(value) => Ok(Value::boolean(*value)),
            Constant::String(value) => Ok(Value::string(value.clone())),
        }
    }

    fn read_register(&self, frame: &Frame, index: usize) -> Result<Value, RuntimeError> {
        frame
            .registers
            .get(index)
            .cloned()
            .ok_or_else(|| RuntimeError::new("register index out of range"))
    }

    fn write_register(
        &self,
        frame: &mut Frame,
        index: usize,
        value: Value,
    ) -> Result<(), RuntimeError> {
        let slot = frame
            .registers
            .get_mut(index)
            .ok_or_else(|| RuntimeError::new("register index out of range"))?;
        *slot = value;
        Ok(())
    }

    fn expect_number(
        &self,
        frame: &Frame,
        value: usize,
        op_name: &str,
    ) -> Result<f64, RuntimeError> {
        match self.read_register(frame, value)? {
            Value::Number(value) => Ok(value),
            other => Err(RuntimeError::new(format!(
                "{} expects number, got {}",
                op_name,
                other.type_name()
            ))),
        }
    }

    fn expect_two_numbers(
        &self,
        frame: &Frame,
        left: usize,
        right: usize,
        op_name: &str,
    ) -> Result<(f64, f64), RuntimeError> {
        match (
            self.read_register(frame, left)?,
            self.read_register(frame, right)?,
        ) {
            (Value::Number(left), Value::Number(right)) => Ok((left, right)),
            _ => Err(RuntimeError::new(format!("{} expects numbers", op_name))),
        }
    }

    fn compare(
        &self,
        frame: &mut Frame,
        dest: usize,
        left: usize,
        right: usize,
        op_name: &str,
        operation: fn(f64, f64) -> bool,
    ) -> Result<(), RuntimeError> {
        let (left, right) = self.expect_two_numbers(frame, left, right, op_name)?;
        self.write_register(frame, dest, Value::boolean(operation(left, right)))
    }
}
