//! `.cdbc 0.2` boundary behavior after the 0.1 read-compatibility removal.
//!
//! The VM emits and accepts only `cdbc 0.2`; legacy `cdbc 0.1` inputs are
//! rejected as unsupported versions before any body parsing.

use compiler_design_vm::{parse_program, VM};

#[test]
fn legacy_0_1_artifacts_are_rejected_before_parsing() {
    let error = parse_program(
        r#"cdbc 0.1

constants:

names:

main registers=0:
block b0:
  return_nil
"#,
    )
    .expect_err("legacy header must be rejected");

    assert_eq!(error.line, 1);
    assert!(error.message.contains("expected `cdbc 0.2`"));
}

#[test]
fn minimal_0_2_program_still_round_trips_and_runs() {
    let program = parse_program(
        r#"cdbc 0.2

constants:

names:

main registers=0:
block b0:
  return_nil
"#,
    )
    .expect("minimal 0.2 artifact should parse");

    assert_eq!(VM::new(&program).run().expect("run should succeed"), "");
}
