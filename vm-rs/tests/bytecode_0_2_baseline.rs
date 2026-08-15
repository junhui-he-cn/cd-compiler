//! Phase 0 behavior baseline for the planned `.cdbc 0.2` refactor.
//!
//! Each case locks current `.cdbc 0.1` VM behavior before the ISA changes and
//! documents the behavior a later Phase is expected to change. See
//! `docs/cd-compiler-bytecode-0.2-execution-plan.md` and
//! `docs/cd-compiler-bytecode-0.2-baseline.md`.

use compiler_design_vm::bytecode::Constant;
use compiler_design_vm::{parse_program, VM};

#[test]
fn map_duplicate_keys_last_write_wins_without_moving_position() {
    let program = parse_program(
        r#"cdbc 0.1

constants:
  c0 = string "a"
  c1 = number 1
  c2 = string "b"
  c3 = number 2
  c4 = number 3
  c5 = number 0
  c6 = number 1

names:
  n0 = "keys"

main registers=12:
  r0 = constant c0
  r1 = constant c1
  r2 = constant c2
  r3 = constant c3
  r4 = constant c4
  r5 = map [r0: r1, r2: r3, r0: r4]
  r6 = index r5, r0
  print r6
  r7 = native_call n0 [r5]
  r8 = constant c5
  r9 = index r7, r8
  print r9
  r10 = constant c6
  r11 = index r7, r10
  print r11
"#,
    )
    .expect("baseline artifact should parse");

    let output = VM::new(&program).run().expect("baseline run should succeed");
    // The duplicate "a" collapses to one entry at the first position with the
    // last value; keys preserve insertion order [a, b].
    assert_eq!(output, "3\na\nb\n");
}

#[test]
fn f64_text_parser_preserves_seventeen_significant_digits() {
    let program = parse_program(
        r#"cdbc 0.1

constants:
  c0 = number 0.30000000000000004
  c1 = number 1.0000000000000002

names:

main registers=0:
"#,
    )
    .expect("baseline artifact should parse");

    let Constant::Number(first) = &program.constants[0] else {
        panic!("expected a number constant");
    };
    let Constant::Number(second) = &program.constants[1] else {
        panic!("expected a number constant");
    };
    assert_eq!(first.parse::<f64>().unwrap(), 0.30000000000000004);
    assert_eq!(second.parse::<f64>().unwrap(), 1.0000000000000002);
    // The text format itself is lossless. The precision loss documented by
    // the C++ emitter baseline test happens before this parser sees the text.
}

#[test]
fn body_without_return_instruction_implicitly_returns_nil() {
    let program = parse_program(
        r#"cdbc 0.1

constants:
  c0 = number 1

names:

main registers=1:
  r0 = constant c0
"#,
    )
    .expect("baseline artifact should parse");

    let output = VM::new(&program).run().expect("baseline run should succeed");
    assert_eq!(output, "");
}

#[test]
fn jump_to_instruction_end_is_valid_and_returns_nil() {
    let program = parse_program(
        r#"cdbc 0.1

constants:
  c0 = number 1

names:

main registers=1:
  r0 = constant c0
  jump 2
"#,
    )
    .expect("baseline artifact should parse");

    let output = VM::new(&program).run().expect("baseline run should succeed");
    assert_eq!(output, "");
}

#[test]
fn unverified_program_reads_uninitialized_register_as_nil() {
    let program = parse_program(
        r#"cdbc 0.1

constants:

names:

main registers=1:
  print r0
"#,
    )
    .expect("baseline artifact should parse");

    let output = VM::new(&program).run().expect("baseline run should succeed");
    assert_eq!(output, "nil\n");
}

#[test]
fn undefined_variable_is_deferred_to_runtime_in_0_1() {
    let program = parse_program(
        r#"cdbc 0.1

constants:

names:
  n0 = "x"

main registers=1:
  r0 = load_var n0
"#,
    )
    .expect("baseline artifact should parse");

    let error = VM::new(&program)
        .run()
        .expect_err("cdbc 0.1 defers undefined variables to execution");
    assert!(
        error.to_string().contains("undefined variable `x`"),
        "unexpected error: {error}"
    );
}
