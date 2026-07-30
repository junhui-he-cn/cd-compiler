//! Embeddable Compiler Design `.cdbc` parsing, linking, and execution APIs.
//!
//! The `compiler-design-vm` binary is the CLI boundary over this library. The
//! library does not perform file IO, choose process exit codes, or format CLI
//! trace lines; callers can use the typed artifact, verifier, linker, and VM
//! APIs directly.

pub mod bytecode;
pub mod format;
pub mod link;
pub mod runtime;
pub mod value;
pub mod vm;

pub use bytecode::Program;
pub use format::{
    format_artifact, format_program, parse_artifact, parse_program, verify_artifact,
    verify_module_artifact, verify_program, Artifact, ModuleArtifact, ModuleDependency,
    ModuleDependencyKind, ParseError,
};
pub use link::{link_modules, link_modules_with_report, LinkReport, LinkResult};
pub use vm::{
    CancellationToken, ResourceKind, RunConfig, RuntimeError, RuntimeErrorKind, StackFrame,
    TraceEvent, TraceEventKind, TraceRun, VM,
};
