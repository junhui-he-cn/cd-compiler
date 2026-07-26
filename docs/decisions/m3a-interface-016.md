# M3A-INTERFACE-016: cached interfaces are the dependency semantic boundary

Status: implemented after the complete imported inventory and the scoped
interface-cache policy in `M3A-INTERFACE-015`.

## Decision

Treat a validated imported `.cdi` plus its paired module product as the
dependency's semantic input for the current snapshot:

- dependency-order scheduling visits the dependency graph first, then marks a
  preloaded interface node complete without entering `TypeChecker::checkModule`;
- `checkModule` is source-body checking only and rejects an accidental
  preloaded-module entry as an internal error;
- source-backed entry modules, cold module-product builds, and repair/fallback
  paths continue to check their parsed bodies normally; and
- direct single-file and ordered direct-multi-file programs remain on their
  existing non-module path.

The importer and re-export consumers already read public shape from
`ModuleInterface`; this slice removes the remaining scheduler-level
body-checking compatibility branch rather than changing public language
semantics or the `cdi 0.1`/`cdbc 0.1` formats.

## Migration and deletion boundary

The TypeChecker exposes a snapshot-local body-check trace for migration
evidence. A complete three-module cache hit must record only the source-backed
entry module. A partial dependency fallback must record the source-backed
modules while omitting every still-valid preloaded dependency. A cold or
invalid-sidecar build must retain the existing source-body checks and
diagnostics.

This does not remove source fallback for module-product cold builds or repairs,
does not make entry modules cache-backed, and does not remove the empty
`ModuleStmt` transport node used by the current graph snapshot. Removing those
paths requires a later independently reviewed product/content and diagnostic
decision.

## Quantitative gate

`frontend_session_tests` checks the complete-hit, source-fallback, and partial
dependency-reuse paths. It validates zero module-interface mismatches and the
expected body-check ID set for each path. The focused test remains part of the
canonical CTest and verification inventory.

## Verification

```sh
cmake --build build --target frontend_session_tests
ctest --test-dir build --output-on-failure -R '^frontend_session$'
python3 tests/verification_inventory.py
git diff --check
```
