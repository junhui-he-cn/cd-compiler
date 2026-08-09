# M5B LSP follow-up deferral

Status: deferred by user direction on 2026-08-09.

The shipped closed-module `textDocument/definition` and
`textDocument/references` behavior remains part of the supported tooling
boundary. The following follow-up work is intentionally paused:

- C3A closed-module completion;
- C3B workspace rename; and
- C3C incremental workspace analysis.

No existing LSP implementation or test is removed. The deferred work may
resume after explicit reprioritization or when a concrete editor/workspace
consumer justifies the next slice. When resumed, keep the existing
`FrontendSession`, `ModuleGraph`, declaration index, source precedence, and
workspace-root contracts as the implementation boundary.
