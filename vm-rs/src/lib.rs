//! Embeddable Compiler Design `.cdbc` parsing, linking, and execution APIs.
//!
//! The `compiler-design-vm` binary is the CLI boundary over this library. The
//! library does not perform file IO, choose process exit codes, or format CLI
//! trace lines; callers can use the typed artifact, verifier, linker, and VM
//! APIs directly.

/// Additive version of the embeddable top-level API facade.
pub const LIBRARY_API_VERSION: &str = "0.1";

pub mod bytecode;
pub mod format;
mod jit;
pub mod link;
pub mod runtime;
mod scheduler;
pub mod value;
pub mod vm;

pub use bytecode::Program;
pub use format::{
    format_artifact, format_program, parse_artifact, parse_artifact_checked, parse_program,
    verify_artifact, verify_artifact_checked, verify_module_artifact,
    verify_module_artifact_checked, verify_program, verify_program_checked, Artifact,
    ArtifactError, ArtifactErrorKind, ModuleArtifact, ModuleDependency, ModuleDependencyKind,
    ParseError, ARTIFACT_FORMAT_FAMILY, ARTIFACT_FORMAT_VERSION, ARTIFACT_HEADER,
};
pub use link::{
    link_modules, link_modules_checked, link_modules_with_report, link_modules_with_report_checked,
    LinkError, LinkErrorKind, LinkReport, LinkResult,
};
pub use vm::{
    CancellationToken, CooperativeDebugHook, CooperativeDebugPause, CooperativeDebugState,
    CooperativeProfileReport, CooperativeRun, CooperativeStep, DebugControl, DebugHook,
    DebugPause, DebugRun, JoinPoll, ProfileFunction, ProfileNative, ProfileReport, ProfileRun,
    ProfileSourceRange, ResourceKind, RunConfig, RuntimeError, RuntimeErrorKind, StackFrame,
    TaskControlError, TaskId, TaskOutcome, TaskOutputEvent, TaskProfileReport, TaskSpec, TaskState,
    TaskTraceEvent, TraceEvent, TraceEventKind, TraceRun, VM,
};
