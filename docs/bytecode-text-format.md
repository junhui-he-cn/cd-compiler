# Compiler Design ByteCode Text Format

This document describes the stable text artifact format for Compiler Design bytecode files.
An instruction-by-instruction reference in Chinese is available in
[`bytecode-instructions-zh.md`](bytecode-instructions-zh.md).

The file extension is `.cdbc`, short for Compiler Design ByteCode.

This format is not the same as the current `--bytecode` debug print. The debug print is for humans inspecting compiler output. The `.cdbc` format is the compiler/VM contract: stable, versioned, and parseable by the Rust VM.

## Phase Status

This format is the text artifact contract at the compiler/VM boundary. The C++ compiler can emit `.cdbc` files with `--emit-bytecode` or independent module products with `--emit-module-bytecode`; the Rust VM can parse, canonicalize, link, and execute them with `dump`, `link`, and `run`.

```sh
compiler_design --emit-bytecode output.cdbc input.cd
compiler_design --emit-module-bytecode module-products input.cd
compiler_design --emit-module-bytecode module-products --module-cache module-cache --module-rebuild-report rebuild.json input.cd
compiler-design-vm dump output.cdbc
compiler-design-vm link module-products output.cdbc
compiler-design-vm run output.cdbc
compiler-design-vm debug output.cdbc
```

## Header

Every file starts with a format identifier and version:

```text
cdbc 0.2
```

The VM accepts `cdbc 0.1` inputs for read compatibility: legacy artifacts carry
name-driven `load_var/store_var/assign_var`, `native_call`, and `print`
instructions and are lowered to the VM's legacy name-resolution path at
construction time. Emitted artifacts are `cdbc 0.2`.
Future format changes must either remain backward-compatible with `0.2` or use a
new version number.

## Artifact kinds

The `cdbc 0.2` envelope has two strict artifact kinds:

- A linked program has no `artifact` declaration and is the existing output of
  `--emit-bytecode`. It may be passed to the VM `run` command.
- An independently compiled module starts with `artifact: module` and a
  `module:` metadata section. It is emitted by
  `--emit-module-bytecode <directory>` as one `module-<graph-id>.cdbc` file per
  graph node. Module products are valid inputs to `dump`, but are not final
  executable programs; the VM rejects them until `link` produces a linked
  program artifact.

A module artifact has this envelope before the ordinary bytecode sections:

```text
artifact: module

module:
  identity = "/workspace/lib.cd"
  path = "lib.cd"
  canonical_path = "/workspace/lib.cd"
  entry = false
  dependencies:
    d0 target="/workspace/shared.cd" kind=import at=2 requested="./shared.cd"
```

`identity` is the graph canonical path and is the product-set key. `path` is
the source display path. Entry modules additionally carry a zero-based
`entry_order`; non-entry modules omit it. Dependency records are ordered by
source occurrence. `kind` is `import` or `re_export`, and `at` is the local
`main` instruction offset before which a linker expands that dependency. The
offset may equal the local instruction count. The module `main` and function
sections contain only the module's own statements; import and re-export bodies
are represented by these markers rather than recursively lowered into the
product.

The link/load rule is deterministic: `compiler-design-vm link <directory>
<output.cdbc>` selects entry products by `entry_order`, walks dependency
markers in their recorded order, expands each module identity at most once,
and preserves each marker's local insertion offset while rebasing register,
constant, name, function, jump, and debug references into the final linked
program. Missing identities, duplicate identities, non-contiguous entry order,
cycles, and invalid offsets are rejected before writing the linked artifact.
The linker does not introduce a new artifact version. The optional module
product cache is separate from VM artifacts: `--module-cache <directory>`
stores a `cdbc-cache 0.2` manifest (internal schema 4) and content-addressed product files, while
`--module-rebuild-report <report.json>` records per-module reuse/rebuild
reasons. Keys include the canonical module identity, exact source bytes,
canonical public interface shape, optimization level, optimizer pipeline
fingerprint, entry metadata, and source-ordered dependency interface inputs;
snapshot-local IDs are never cache keys. A private source change rebuilds only
that module, while a public-interface change propagates through transitive
dependents. A changed optimization identity rebuilds the affected product
without marking its public interface changed. Valid paired interface sidecars can preload
dependency public shape before the current importer check; invalid or missing
sidecars/products fall back to source parsing. Linked programs built from a
single file or direct multi-file input retain their existing metadata; an
import-aware graph additionally records each source's canonical module identity
in its optional debug-source entry.

Module products keep `debug_sources` and `debug_locations` local to the module
before linking. The Rust linker appends each expanded module's source table in
deterministic expansion order and rebases every main/function location through
that table, so runtime diagnostics from an imported function retain the
original module path and call-stack order in the final linked program. The
source table also carries the optional canonical module identity described
below; the linker preserves it while rebasing only artifact-local source
indexes.

### Erased generic struct ordering

A named struct with valid `<`, `<=`, `>`, and `>=` implementations can satisfy a
generic `Ord` bound. Generic comparison continues to use the existing four
comparison instructions. The owning module product also stores each operator
function under an implementation-private global binding named
`__capability_ord_<Struct>_<less|less_equal|greater|greater_equal>`; the Rust VM
uses that binding only when a comparison receives two values of the same
witnessed struct type. This is not a source-visible global or capability
dictionary, and it adds no opcode, section, or `cdbc` version. A malformed or
missing binding is reported as a runtime error after normal artifact
validation.

## Module interface sidecars

The module cache also stores one `cdi 0.1` sidecar under `interfaces/` for each
module. A sidecar contains the canonical module identity, exact source hash,
complete public type shape (including generic constraints, public struct
fields, methods, and ordering-operator records, private-field presence
markers, enum variants, and linkage names), dependency identities and public
interface hashes, entry metadata, and the linkage-name allocator high-water
mark used to reconstruct unchanged snapshots. The high-water mark is cache
reconstruction metadata and is not part of the public interface hash.
Snapshot-local IDs are not serialized.

`--module-interface-cache <directory>` reads these sidecars. A dependency is
preloaded only when its sidecar identity, canonical path, source hash, recursive
dependency sidecars/interface hashes, and paired cached `.cdbc` product all
match. The resulting module node keeps source bytes for diagnostics but has no
parsed dependency body; its preloaded interface supplies semantic import
visibility. An interface-only consumer rejects a missing, malformed, stale, or
unpaired sidecar by default with an `Import` diagnostic. Pass
`--module-cache-fallback` to opt back into the normal source parser for those
cases. `--module-cache` uses the same directory while emitting module products.
That product-building path is strict by default: a cold build bootstraps the
cache, an invalid or inconsistent manifest requires explicit repair
(`--module-cache-fallback` or a cache-directory reset), normal source and
dependency drift rebuilds from source, and schema-4 manifest content digests
detect corrupted cached products before reuse. `--module-cache-strict` remains
an explicit assertion and is mutually exclusive with `--module-cache-fallback`.

The cache manifest is separate from VM artifacts and uses `cdbc-cache 0.2`; its
records include the relative `.cdi` sidecar path and a product content digest.
The `.cdi` sidecar is not a Rust VM input and has no effect on the linked
`cdbc 0.2` wire format.

## Sections

A `.cdbc` file is organized into explicit sections:

```text
cdbc 0.2

constants:
  c0 = number 1
  c1 = string "hello"

names:
  n0 = "x"

globals:
  g0 = n0

native_imports:
  i0 = "print" abi=1

main registers=4:
  r0 = constant c0
  init_global g0, r0
  r1 = load_global g0
  r3 = call_native i0 [r1]

function f0 name="add_one" arity=1 registers=4:
  param 0 = "x"
  upvalue u0 = local l0
  r1 = constant c0
  r2 = add r0, r1
  return r2

debug_sources:
  s0 path="examples/hello.cd" text="print 1;\n"

debug_locations:
  main 0 = s0:1:7
  main 1 = s0:1:1

debug_ranges:
  main 0 = s0:0:7
  main 1 = s0:0:8
```

The section names and reference prefixes are part of the canonical text format.
`cdbc 0.2` bodies are composed of `block bN:` sections; each block ends with one
terminator (`br bN`, `br_if rC, bT, bF`, `return rV`, or `return_nil`), and
implicit fallthrough is forbidden. The linker splices module init bodies at the
block boundary recorded by the `at=N` dependency offset and renumbers block IDs
across the merged main.

The optional `types:` section (between `globals:` and `main`) records runtime
type layouts: struct field names/counts and enum variant names/payload counts.
Names are display/debug metadata only; identity and access use numeric
`TypeId`/`VariantId` and field slots via `make_struct`/`struct_get`/
`struct_set` and `make_variant`/`is_variant`/`variant_get`.

The optional `native_imports:` section (between `types:` and `main`) serializes
the fixed registered native names used by the artifact:

```text
native_imports:
  i0 = "print" abi=1
  i1 = "str" abi=1
```

Each import declares its display name and `abi=1`. Instructions address
imports by numeric `iN` index via `call_native`; the VM dispatches by index
without a string lookup on the hot path. Imports are deduplicated by name
during module linking and remapped to the merged import table.

Pre-execution validation rejects block bodies with undefined registers, unbound
locals, invalid block IDs, missing terminators, out-of-range global upvalue
sources, out-of-range native imports, invalid native arity, or legacy jumps
mixed into block bodies.
The optional `globals:` section (module products and linked programs alike) maps
each numeric global slot to its name index; the linker deduplicates globals by
name across modules. Function `param` lines appear before instructions;
`upvalue uN = local lM` / `upvalue uN = upvalue uM` /
`upvalue uN = global gM` lines follow them and drive closure capture: `local`
takes the parent frame's local cell, `upvalue` takes a parent upvalue, and
`global` takes the global cell current at `make_function` time. Loop bodies
that rebind a captured binding therefore yield one independent cell per
iteration.

## Debug metadata

`debug_sources`, `debug_locations`, and `debug_ranges` are optional additive
sections. The C++ compiler emits them for source-backed instructions, and the
Rust VM uses them to report runtime source locations, source lines, carets, and
call stacks. Each
`debug_sources` entry is ordered by zero-based `sN` index and embeds the
display path plus original source text. Import-aware source entries may add a
stable canonical module identity before `path`:

```text
debug_sources:
  s0 module="/workspace/lib.cd" path="lib.cd" text="fun fail() { return 1 / 0; }\n"

debug_locations:
  main 3 = s0:2:1
  function f0 2 = s0:1:21
```

`module` is optional for compatibility with older metadata and is omitted for
source entries without a module graph identity. When present it must be
non-empty and is the graph's canonical module path; it is not a snapshot-local
numeric ID. `path` remains the display path used by current runtime
diagnostics. The Rust parser and formatter accept both the old
`sN path=... text=...` form and the module-aware form, and preserve the field
through module linking.

`main` identifies the top-level body; `function fN` identifies a function
section. Locations are sparse, but every referenced source, function, and
instruction must exist. Source, function, and instruction references are
zero-based; line and column values are one-based and must be positive. Duplicate
`debug_locations` mappings and out-of-range references are Rust parser errors.
`debug_ranges` uses the same target syntax and maps an instruction to one
source-local half-open byte interval `[start, end)`, encoded as `sN:start:end`.
Each range must have a matching debug location, use the same source index, and
fit within the UTF-8 source text. Duplicate mappings, reversed ranges, and
out-of-range references are Rust parser/validation errors. A metadata-free
`cdbc 0.1` artifact remains valid and executes with
legacy one-line runtime errors.

## Value Encoding

Constants use explicit value tags:

```text
c0 = nil
c1 = number 1.25
c2 = bool true
c3 = string "escaped string"
```

Strings use double quotes and backslash escapes for at least `\\`, `\"`, `\n`, `\r`, and `\t`.

String constants are UTF-8 text. The Rust VM's `len`, `substr`, and `charAt`
operations interpret string offsets as Unicode scalar-value positions and
never split a scalar's UTF-8 encoding. Grapheme segmentation and normalization
are language-level non-goals for this format version.

## References

References use stable prefixes:

- `cN`: constant index.
- `nN`: name index.
- `rN`: register index.
- `fN`: function index.
- `iN`: native import index.

Indexes are zero-based decimal integers.

## Opcode Names

The opcode names are stable snake-case names:

```text
constant
make_function
array
map
make_struct
struct_get
struct_set
make_variant
is_variant
variant_get
move
load_local
bind_local
set_local
load_upvalue
set_upvalue
load_global
init_global
set_global
call
call_native
array_get
array_set
map_get
map_set
range_get
field
assign_field
len_array
len_map
len_range
len_str
iter_init
iter_has
iter_next
assert_number
neg_num
not
add_num
sub_num
mul_num
div_num
concat_str
equal
not_equal
lt_num
le_num
gt_num
ge_num
lt_str
le_str
gt_str
ge_str
br
br_if
return
return_nil
```

The legacy `cdbc 0.1` read path additionally accepts `load_var/store_var/
assign_var`, `struct`, `variant`, `variant_tag`, `variant_field`,
`native_call nName`, `print rV`, the dynamically typed `negate`, `add`,
`subtract`, `multiply`, `divide`, `greater`, `greater_equal`, `less`,
`less_equal`, the dynamically typed `index`, `assign_index`, and `len`, the
`assert_array` for-in adapter, and the linear `jump`, `jump_if_false`, and
`jump_if_true` instructions. These legacy forms are never emitted.

Map construction preserves source order and uses explicit key/value register
pairs:

```text
rD = map [rKey0: rValue0, rKey1: rValue1, ...]
```

The Rust parser rejects malformed entries that do not contain a `: ` pair
separator. Map lookup and assignment reuse the existing `index` and
`assign_index` instructions.

Struct and field instructions use name-table references for field names:

```text
rD = struct {nName: rValue, ...}
rD = struct nType {nName: rValue, ...}
rD = field rObject, nName
rD = assign_field rObject, nName, rValue
```

The optional `nType` name-table reference records a named struct runtime type name for `typeOf`. Anonymous bytecode struct instructions omit it and continue to report `"struct"` when executed by the VM.

`assign_field` mutates an existing struct field and stores the assigned value in `rD`; assigning to a missing field is a runtime error.

Enum variants use two name-table references and an ordered payload register list:

```text
rD = variant nEnum.nVariant [rPayload0, rPayload1, ...]
rD = variant_tag rValue nEnum.nVariant
rD = variant_field rValue payloadIndex
```

`variant_tag` returns a boolean and is false for non-matching values.
`variant_field` reads a positional payload and raises a runtime error for
non-variant values or an out-of-range payload index.

Generic enum type arguments are compile-time metadata and are erased from
these runtime instructions; the emitted enum name and payload layout remain
the same as for non-generic enums.

Native stdlib calls address the serialized import table:

```text
rD = call_native iImport [rArg0, rArg1, ...]
```

`call_native` indexes the `native_imports` section directly; the VM resolves
each import once at construction time and dispatches by numeric ID without a
string lookup on the hot path. The legacy `native_call nName` form remains
read-compatible only and is never emitted. In this version `push`, `pop`,
`remove`, `clear`, `merge`, `keys`, `values`, `floor`, `ceil`, `sqrt`, `str`,
`substr`, `charAt`, `typeOf`, `hash`, `contains`, `slice`, `copy`, `concat`,
`map`, `filter`, `flatMap`, `any`, `all`, `count`, `find`, `findIndex`,
`reduce`, and `print` are supported.

`print` statements are lowered to a `call_native` of the `print` import with
one argument register and a scratch destination. Output budgeting,
cancellation, trace attribution, and side-effect handling stay inside the
native framework, so printing behaves exactly like the former dedicated
opcode.

## Typed arithmetic and comparison

When the compiler knows both operand types, arithmetic and ordered comparison
use single-type opcodes so the interpreter hot path does not branch on runtime
value tags:

```text
rD = add_num rL, rR        rD = concat_str rL, rR
rD = sub_num rL, rR        rD = mul_num rL, rR
rD = div_num rL, rR        rD = neg_num rV

rD = lt_num rL, rR         rD = lt_str rL, rR
rD = le_num rL, rR         rD = le_str rL, rR
rD = gt_num rL, rR         rD = gt_str rL, rR
rD = ge_num rL, rR         rD = ge_str rL, rR
```

`equal` and `not_equal` keep the existing runtime equality semantics for all
value kinds. The dynamically typed `add`, `negate`, `subtract`, `multiply`,
`divide`, and ordered comparison opcodes remain on the legacy read path and
for generic `T: Ord` bodies whose parameter type is not statically known.
Ordered comparisons no longer resolve struct capability witnesses by global
name; struct values are not order-comparable and the compiler rejects such
comparisons before bytecode emission.

## Typed collection access

When the compiler knows the indexed collection's type it emits collection-
specific access instructions instead of the dynamically typed `index` /
`assign_index` / `len`:

```text
rD = array_get rC, rI      rD = array_set rC, rI, rV
rD = map_get rC, rI        rD = map_set rC, rI, rV
rD = range_get rC, rI

rD = len_array rV          rD = len_map rV
rD = len_range rV          rD = len_str rV
```

The typed instructions keep the established runtime diagnostics and budgets:
array/range indexing validates numeric integer bounds, map access validates
keys and charges runtime elements on insertion, `len_str` counts Unicode
scalars, and `range_set` does not exist. The generic `index`, `assign_index`,
and `len` remain on the legacy read path and for collection types that are
not statically known.

## Iterator protocol

`for-in` lowers through an internal iterator protocol instead of the legacy
`assert_array` adapter:

```text
rIter = iter_init rCollection
rHas = iter_has rIter
rValue = iter_next rIter
```

Iterators are VM-internal values and are never exposed to the source
language. `iter_init` snapshots array length at entry while reading live
elements during iteration, snapshots map keys into an insertion-ordered
array, and keeps ranges immutable; `iter_has` is pure and `iter_next`
advances one position. The compiler arranges `iter_has` + `iter_next` into
the loop blocks, so `break`/`continue` and mutation-during-iteration behavior
match the previous lowering exactly. The legacy `assert_array` instruction
remains read-compatible only.

The `range` native is also supported with one to three numeric arguments. Its
result is consumed by the existing `len_range` and `range_get` instructions
and by the iterator protocol.

New opcodes must be added by updating this document, the C++ bytecode artifact emitter, and the Rust VM parser/formatter and executor together.

## Compatibility validation

The Rust parser accepts `cdbc 0.1` and `cdbc 0.2` headers. Before `dump`, `link`, or
`run` receives an artifact, it validates finite number constants, constant/name/
function/register references, jump targets, debug-location table shape, and
the native import metadata plus the supported native-call capability set and
native arity bounds. Module identities, entry metadata,
dependency targets, and insertion offsets are validated by the module envelope
and linker path. Invalid artifacts are rejected before VM execution; valid
linked programs and module products retain the canonical text described above.

The version and compatibility matrix is recorded in
`docs/decisions/m4a-artifact-validation.md`. No successor version is selected
for this validation-only extension.

## Non-Goals for This Phase

This format does not define binary encoding, verifier internals, GC layout, task scheduler, or JIT metadata format. Those belong to later Rust VM phases.
