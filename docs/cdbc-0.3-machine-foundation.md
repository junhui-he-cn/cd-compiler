# cdbc 0.3 Machine Foundation

Status: VM03-00 contract for the cdbc 0.3 implementation line.

This document defines the machine layer that will be added to the Compiler
Design VM. It is a VM and artifact contract, not a description of the current
0.2 implementation. Later milestones must implement these rules before the
ABI can be declared frozen.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT, and MAY are normative.

## 1. Scope and boundaries

cdbc 0.3 adds a low-level machine layer alongside the existing dynamic CD
layer. The machine layer is intended to carry the semantics needed by a later
C or LLVM backend without making that backend part of the VM contract.

The machine layer provides:

- exact-width integer values and operations;
- IEEE machine floating-point values and operations;
- deterministic VM addresses;
- byte-addressable linear memory;
- typed memory access;
- per-task machine stack frames;
- RODATA, DATA, and BSS segments;
- checked memory bulk operations; and
- scalar function ABI metadata.

The following remain outside this contract:

- LLVM, Clang, SelectionDAG, target-specific machine types, and target
  calling-convention identifiers;
- libc, POSIX, native object files, dynamic libraries, and host pointers;
- a general heap allocator;
- function pointers and indirect calls;
- varargs and stack arguments;
- atomics, threads, TLS, SIMD, exceptions, setjmp/longjmp, and VLA support.

The existing dynamic VM remains the owner of language values, dynamic
collections, module initialization, native calls, and current scheduler
behavior unless a later milestone explicitly extends one of those boundaries.

## 2. Two value layers

The VM has two distinct value domains:

~~~text
dynamic layer                         machine layer
-------------                         -------------
Nil, Number(f64), Bool, String,       MachineInt(u64),
Function, Array, Map, Range,           MachineFloat(f64),
Struct, Variant, Iterator              Address(u64)
~~~

Machine values MUST be added alongside the current dynamic Value variants.
They MUST NOT replace or reinterpret Number(f64). In particular, an exact C
integer MUST NOT be transported through a dynamic number, even when its value
happens to be representable as an f64.

There is no implicit conversion between dynamic and machine values. A future
adapter or explicitly specified instruction MAY cross the boundary; ordinary
machine arithmetic, memory access, and calls MUST reject a value from the
wrong domain through verifier validation or a VM trap.

Dynamic equality, hashing, truthiness, formatting, collection identity, and
existing dynamic call behavior are unchanged by this contract. ICMP produces
the existing exact boolean value used by conditional branches; it does not
produce a dynamic Number(0) or Number(1).

## 3. Machine value representation

### 3.1 MachineInt

MachineInt stores exactly 64 raw bits:

~~~rust
MachineInt(u64)
~~~

The value does not carry a width or signedness tag. Every instruction that
uses a machine integer supplies an explicit width. The only valid integer
widths are 8, 16, 32, and 64 bits.

For width w, define:

~~~text
mask(w) = 2^w - 1
low_w(x) = x & mask(w)
signed_w(x) = sign-extend low_w(x) from w bits to a mathematical signed integer
~~~

Integer-producing instructions return a canonical raw value:

~~~text
MachineInt(low_w(result))
~~~

Bits above the instruction width are therefore zero in a newly produced value.
An input with non-zero high bits is accepted, but an operation reads only the
low w bits. Width 64 reads and writes all 64 bits.

Signed values use two's-complement interpretation. Signedness affects only
operations whose name or predicate is signed; it never changes stored bits.

### 3.2 MachineFloat

MachineFloat is represented as:

~~~rust
MachineFloat(f64)
~~~

The instruction format selects either F32 or F64 for each floating-point
operation. An F64 value uses IEEE 754 binary64 bits. An F32 value is
canonicalized by rounding to IEEE 754 binary32 and converting that binary32
value back to f64 for storage in the VM value. An F32 operation cannot
silently retain binary64 precision between operations.

Machine floating-point operations follow IEEE 754 behavior for finite values,
infinities, NaNs, and signed zero. Floating division by zero does not produce
an integer-style VM division trap; it produces the IEEE result. NaN results
are canonical quiet NaNs when materialized or serialized. The sign of zero is
preserved where the underlying IEEE operation defines it.

The VM MUST perform an explicit F32 round step after every F32 arithmetic,
conversion, and result-producing operand preparation. FPEXT from F32 to F64
is exact. FPTRUNC from F64 to F32 rounds using IEEE round-to-nearest,
ties-to-even.

### 3.3 Address

Address is a VM virtual byte address:

~~~rust
Address(u64)
~~~

Address 0 is NULL and is never a valid byte in a mapped object. An address is
an offset in the VM's linear address space, not a Rust pointer, C pointer,
file offset, or host virtual address. A .cdbc artifact MUST NOT contain a host
pointer.

An address value may be copied and stored as raw bits. It becomes usable for a
memory operation only when the target byte range is mapped and has the
required permission. The VM does not infer object types from an address.

## 4. Machine type descriptors

The following descriptors are part of the machine ABI:

~~~text
I8, I16, I32, I64
F32, F64
ADDR
~~~

I8 through I64 describe raw integer widths and do not specify signedness.
ADDR always describes an eight-byte VM address. A scalar function parameter or
return descriptor is one of these types.

Memory access sizes and encodings are fixed:

| Type | Size | Encoding |
| --- | ---: | --- |
| I8 | 1 | low 8 integer bits |
| I16 | 2 | little-endian low 16 integer bits |
| I32 | 4 | little-endian low 32 integer bits |
| I64 | 8 | little-endian 64 integer bits |
| F32 | 4 | IEEE 754 binary32 bits, little-endian |
| F64 | 8 | IEEE 754 binary64 bits, little-endian |
| ADDR | 8 | little-endian u64 VM address bits |

Natural alignment is 1, 2, 4, 8, 4, 8, and 8 bytes respectively. Segment
starts and frame bases obey declared alignment. Individual accesses MAY be
unaligned in cdbc 0.3; an unaligned access is not itself a trap.

## 5. Integer semantics

All integer operations use an explicit width. Arithmetic results are reduced
to that width with wrapping semantics and then canonicalized to u64.

### 5.1 Constants and arithmetic

The abstract instruction forms are:

~~~text
ICONST dst, width, raw
IADD   dst, lhs, rhs, width
ISUB   dst, lhs, rhs, width
IMUL   dst, lhs, rhs, width
~~~

ICONST writes low_w(raw). IADD, ISUB, and IMUL compute the mathematical
operation on the low-width bit patterns and retain the low w bits. No Rust
release/debug overflow behavior is observable.

### 5.2 Division and remainder

The forms are:

~~~text
SDIV dst, lhs, rhs, width
UDIV dst, lhs, rhs, width
SREM dst, lhs, rhs, width
UREM dst, lhs, rhs, width
~~~

UDIV and UREM use unsigned values in [0, 2^w - 1]. SDIV and SREM use
two's-complement signed values. Signed division rounds towards zero. Signed
remainder has the sign of the dividend and satisfies:

~~~text
lhs = (lhs / rhs) * rhs + (lhs % rhs)
~~~

with truncation towards zero.

The following behavior is fixed:

- a zero divisor traps with IntegerDivisionByZero;
- signed MIN / -1 and signed MIN % -1 trap with IntegerDivisionOverflow;
- unsigned operations never trap for overflow because their results are within
  the selected width;
- division and remainder results are canonicalized to the selected width.

### 5.3 Bitwise operations and shifts

The forms are:

~~~text
AND  dst, lhs, rhs, width
OR   dst, lhs, rhs, width
XOR  dst, lhs, rhs, width
NOT  dst, value, width
SHL  dst, value, amount, width
LSHR dst, value, amount, width
ASHR dst, value, amount, width
~~~

AND, OR, XOR, and NOT operate on selected low-width bit patterns. SHL and
LSHR shift in zero bits. ASHR sign-extends the selected-width input and shifts
in its sign bit. All results are reduced to the selected width.

The shift amount is interpreted as an unsigned 64-bit value. An amount greater
than or equal to w traps with InvalidShiftAmount; the VM MUST NOT mask the
amount modulo the width. This keeps LSHR distinct from ASHR.

### 5.4 Integer comparison

The form is:

~~~text
ICMP dst, lhs, rhs, width, predicate
~~~

The allowed predicates are:

~~~text
EQ NE
SLT SLE SGT SGE
ULT ULE UGT UGE
~~~

EQ and NE compare selected-width raw bits. S* predicates compare signed_w
values. U* predicates compare unsigned selected-width values. The result is
Bool(true) or Bool(false).

## 6. Floating-point semantics

The abstract forms are:

~~~text
FCONST dst, format, bits
FADD   dst, lhs, rhs, format
FSUB   dst, lhs, rhs, format
FMUL   dst, lhs, rhs, format
FDIV   dst, lhs, rhs, format
FNEG   dst, value, format
FCMP   dst, lhs, rhs, format, predicate
~~~

format is F32 or F64. FCONST carries the IEEE bit pattern for the selected
format rather than a host pointer or a dynamic number literal. A serializer
MAY choose a textual representation, but it MUST preserve the selected format
and all non-NaN bits exactly.

FCMP predicates are:

~~~text
OEQ ONE OLT OLE OGT OGE
UEQ UNE ULT ULE UGT UGE
ORD UNO
~~~

The O predicates are ordered and return false when either operand is NaN. The
U predicates are unordered and return true when either operand is NaN, then
apply the named relation to non-NaN operands. ORD is true only when neither
operand is NaN, and UNO is true when at least one operand is NaN. All
comparison results are Bool values. A NaN is never equal to another NaN under
OEQ, and UNE is true for two NaN operands.

## 7. Conversion semantics

### 7.1 Integer width conversions

The forms are:

~~~text
TRUNC dst, value, from_width, to_width
ZEXT  dst, value, from_width, to_width
SEXT  dst, value, from_width, to_width
~~~

TRUNC requires to_width < from_width and retains the low destination bits.
ZEXT and SEXT require to_width > from_width; they zero-extend or sign-extend
the source bits respectively. Equal widths are rejected by the verifier
rather than being encoded as a hidden move. The result is a canonical
MachineInt at to_width.

### 7.2 Integer to float

The forms are:

~~~text
SITOFP dst, value, int_width, float_format
UITOFP dst, value, int_width, float_format
~~~

SITOFP interprets the source with signed_w; UITOFP interprets it as an
unsigned selected-width value. Conversion follows IEEE integer-to-floating
conversion. A result may lose precision when the destination format cannot
represent every source integer; this is not a VM trap. F32 results are rounded
to binary32 before storage.

### 7.3 Float to integer

The forms are:

~~~text
FPTOSI dst, value, float_format, int_width
FPTOUI dst, value, float_format, int_width
~~~

The source is truncated towards zero before range checking. NaN, positive or
negative infinity, and a finite value outside the destination integer range
trap with InvalidConversion. The valid truncated ranges are:

~~~text
FPTOSI: [-2^(w-1), 2^(w-1) - 1]
FPTOUI: [0, 2^w - 1]
~~~

Negative zero converts to integer zero. The result is a canonical MachineInt.

### 7.4 Float width conversions

The forms are:

~~~text
FPEXT   dst, value, from_format, to_format
FPTRUNC dst, value, from_format, to_format
~~~

FPEXT is only valid for F32 -> F64 and is exact. FPTRUNC is only valid for
F64 -> F32 and performs the defined IEEE rounding. Other direction or
same-format pairs are verifier errors.

## 8. Linear memory and address policy

### 8.1 Memory model

Machine objects live in a byte-addressable LinearMemory owned by the VM. The
conceptual representation is:

~~~rust
struct LinearMemory {
    bytes: Vec<u8>,
    // segment and live-frame metadata is kept separately
}
~~~

An implementation MAY use an equivalent checked backing store, but observable
semantics MUST be identical to a contiguous byte array. Dynamic arrays, maps,
strings, structs, variants, and iterators remain managed by the existing
dynamic object system; they MUST NOT be used as storage for machine C objects.

The address space is little-endian and has this logical order:

~~~text
0 .. 0x1000       reserved NULL/invalid guard
RODATA            initialized, read-only bytes
DATA              initialized, writable bytes
BSS               zero-initialized, writable bytes
HEAP              reserved for a future allocator
STACK             per-task machine stack regions
~~~

The null guard is not mapped. Every non-empty access to address zero traps.
Every mapped region has a half-open byte interval [base, end). Regions MUST
not overlap.

### 8.2 Deterministic allocation

For a given artifact set and VM configuration, segment and stack addresses MUST
be deterministic. The loader allocates static segments in the order RODATA,
DATA, BSS, beginning with cursor 0x1000:

~~~text
base = align_up(cursor, max(requested_alignment, 1))
end  = checked_add(base, size)
cursor = end
~~~

Static segment descriptors are ordered by their artifact table order. A
requested alignment MUST be a positive power of two and checked arithmetic
failure is an artifact validation error. The heap start is the next 8-byte
aligned cursor; cdbc 0.3 reserves the region but does not define allocation.

Each task receives a separate machine stack region. Stack regions are assigned
in task creation order after the shared static allocation and are aligned to
8 bytes. A stack grows upward in address order for this ABI. A frame base is
the first byte of its allocation, and the next frame starts after the current
frame with the required alignment. This direction is a VM address policy, not
a host ABI requirement.

The exact stack capacity is a VM execution limit. Exhausting it traps with
StackOverflow before the new frame becomes visible. A machine address is valid
only while its mapped segment or live stack frame is valid. An address into a
released frame is invalid even if the backing byte vector retains old bytes.

### 8.3 Effective addresses and bounds

An operation that computes base + offset MUST use checked arithmetic. An
overflow, an unmapped result, or a range whose end exceeds its mapped region
traps with InvalidAddress or MemoryOutOfBounds as follows:

- NullPointerAccess is used for a direct non-empty access whose base is zero.
  InvalidAddress is used for address arithmetic overflow and a non-null
  address that does not fall within any mapped region;
- MemoryOutOfBounds is used when the start is mapped but the requested
  non-empty range extends beyond that region.

The implementation MUST perform the range check before indexing its backing
storage. A Rust slice panic is never the specified behavior.

## 9. Typed memory operations

The uniform forms are:

~~~text
LOAD  dst, address, type
STORE address, source, type
~~~

LOAD I8, LOAD I16, LOAD I32, and LOAD I64 read raw little-endian bits and
return a MachineInt containing those bits. LOAD does not sign-extend; the
producer MUST use SEXT or ZEXT when a wider value is needed.

LOAD F32 and LOAD F64 decode the corresponding IEEE bit pattern into a
MachineFloat. LOAD ADDR reads eight little-endian bytes into an Address,
including Address(0); dereferencing that result is what traps.

STORE requires a source from the matching machine domain. Integer stores write
only the low bits of the selected width. Floating stores write the selected
IEEE format. Address stores write the raw u64 address. A store to RODATA
traps with WriteToReadOnlyMemory after address validity is checked.

Every load and store requires a non-empty valid range. An unaligned access is
allowed. The access type determines size, byte order, and domain; it does not
perform sign extension or implicit numeric conversion.

## 10. Machine frames and calls

### 10.1 Frame ownership

The machine frame is metadata attached to the existing VM call frame. The VM
MUST reuse its current call stack, register array, return transfer, and
scheduler frame model. It MUST NOT create a separate dynamic call mechanism
just for machine functions.

Each function MAY carry machine ABI metadata:

~~~text
machine_frame_size: unsigned byte count
machine_params:     zero to eight scalar type descriptors
machine_return:     none or one scalar type descriptor
~~~

Dynamic functions without machine metadata retain their existing arity and
return behavior. Machine functions use the metadata for direct scalar calls;
there is no implicit machine-to-dynamic or dynamic-to-machine coercion.

### 10.2 Frame address

The form is:

~~~text
FRAME_ADDR dst, offset
~~~

It returns:

~~~text
Address(current_machine_frame_base + offset)
~~~

offset is an unsigned byte offset encoded in the instruction. The verifier
requires offset <= machine_frame_size; the frame end is allowed as a one-past
address but cannot be dereferenced. The addition is checked. The current frame
base is aligned to 8 bytes, and machine frame size includes any padding needed
by its producer.

On a machine call, the VM allocates the callee frame before executing its first
instruction, sets its frame base, and transfers scalar arguments through the
existing register call boundary. On return, the frame allocation is released
before control resumes in the caller. Recursive calls receive distinct frame
regions. A trap unwinds active machine frames through the normal VM error path
and MUST NOT leave a frame allocation available to a later task.

### 10.3 Scalar call ABI

The 0.3 scalar ABI permits at most eight machine scalar arguments and one
machine scalar return. Arguments and the result are passed using the existing
virtual-register call representation; cdbc 0.3 does not add stack arguments.
The verifier rejects a call whose argument count or declared scalar types do
not match the callee metadata. Function values and indirect calls are not
part of this ABI.

Machine ABI metadata contains only VM-level scalar types, frame size, and
register-facing call shape. It MUST NOT contain LLVM calling-convention IDs,
SelectionDAG metadata, target register names, or native ABI details.

## 11. Data segments

Machine globals are represented by segment-backed bytes, independently of the
existing dynamic global table. A segment descriptor contains:

~~~text
kind:       RODATA | DATA | BSS
alignment:  positive power of two
size:       byte count
initial:    bytes for RODATA/DATA, absent for BSS
permissions: read-only or read-write, implied by kind
~~~

The loader allocates and validates segments before resolving relocations.

- RODATA is readable and not writable.
- DATA is initialized from initial and is readable and writable.
- BSS has no serialized payload, is zero-filled for size bytes, and is
  readable and writable.

An initialized segment's payload length MUST equal its declared size. A BSS
segment MUST NOT carry an initialization payload. Segment overlap, invalid
alignment, integer overflow during placement, and permission mismatches are
artifact verification errors.

Existing dynamic globals continue to use GlobalId -> Value; a machine global is
addressed through a VM Address and its symbol metadata. The two tables MUST
NOT be merged.

## 12. Symbols and relocations

The initial symbol table supports:

~~~text
function symbol: function table target
data symbol:    segment plus byte offset
~~~

Symbol names are unique in a linked artifact. Duplicate or undefined symbols
are load-time verification errors.

The initial relocation kinds are:

~~~text
ABS64       write the resolved VM address as little-endian u64 data
FUNC_INDEX  resolve a function symbol to a function-table index in a call
            target operand
~~~

ABS64 is valid only at an eight-byte location in a DATA or BSS payload, or in
RODATA initialization bytes that are part of the artifact relocation contract.
The loader may patch RODATA during relocation before enforcing its runtime
read-only permission. ABS64 writes a VM virtual address, never a host pointer.
The target symbol plus addend MUST be checked for address overflow. FUNC_INDEX
does not create a function pointer and does not expose host executable
addresses.

Relocations are applied only after all segment allocation and symbol resolution
succeeds. A relocation with an unknown kind, invalid target range, invalid
symbol, or incompatible target section is rejected before execution.

## 13. Memory bulk operations

The forms are:

~~~text
MEMCPY  dst_address, src_address, size
MEMMOVE dst_address, src_address, size
MEMSET  dst_address, byte_value, size
~~~

The address operands are Address values. `size` is an unsigned `MachineInt`
interpreted as a byte count, and `byte_value` is an unsigned `MachineInt` whose
low eight bits are used. The three operations validate all non-zero source and
destination ranges before modifying memory.

MEMCPY copies exactly size bytes and requires source and destination ranges not
to overlap. Overlap is a deterministic InvalidMemoryOperation trap; it is not
delegated to libc and is not left as undefined behavior. MEMMOVE is
overlap-safe and has the same final bytes as if the source range had been
copied to a temporary buffer. MEMSET writes the low eight bits of byte_value
to every destination byte.

For all three operations, a non-zero size requires valid mapped ranges and
appropriate write permissions. A zero-size operation is a no-op and does not
dereference its addresses, so a null address is accepted only in that case.
Range arithmetic is checked before any byte is changed. A failed operation is
atomic from the VM's observable point of view; it MUST NOT partially modify
memory.

## 14. Control flow and traps

Machine instructions use the existing verified basic-block and register
execution model. A machine comparison produces Bool, and conditional branches
consume that boolean. Existing dynamic branch and return semantics remain
unchanged.

The minimum machine trap set is:

~~~text
MemoryOutOfBounds
NullPointerAccess
WriteToReadOnlyMemory
StackOverflow
IntegerDivisionByZero
IntegerDivisionOverflow
InvalidShiftAmount
InvalidAddress
InvalidConversion
InvalidMemoryOperation
InvalidInstruction
InvalidOperand
~~~

NullPointerAccess is the user-visible trap for a direct non-empty access whose
base address is exactly zero. InvalidAddress covers arithmetic overflow and
non-null unmapped addresses. MemoryOutOfBounds covers a mapped start with an
overlong range. An implementation MAY retain a more detailed internal cause,
but the public trap kind and deterministic message category MUST remain stable.

The verifier MUST reject malformed static operands before execution, including
unknown opcodes, unsupported widths or formats, invalid register references,
invalid type/domain combinations, invalid segment descriptors, bad frame
metadata, invalid symbols, and invalid relocations. Runtime-dependent failures
such as a dynamic divisor of zero or a dynamic out-of-range address are VM
traps, not verifier errors.

Rust panics, integer overflow panics, slice-index panics, allocation panics, and
assertion failures MUST NOT be used as cdbc machine trap semantics. The public
Rust library and CLI MUST report a checked VM error instead.

## 15. cdbc 0.3 artifact contract

### 15.1 Versioning

The machine artifact header is exactly:

~~~text
cdbc 0.3
~~~

The artifact version is separate from the repository's semantic release
version. The 0.3 writer emits only cdbc 0.3 once implementation begins. A 0.3
reader MUST accept valid 0.2 artifacts and preserve their dynamic semantics.
A 0.2 reader MUST reject a 0.3 header cleanly as an unsupported version before
interpreting the body.

No field, opcode spelling, constant encoding, or section meaning in a file
whose header is cdbc 0.2 may be changed under this plan. A dynamic-only program
MAY be emitted as a 0.3 artifact, but the header then still declares 0.3 and
all 0.3 validation rules apply.

### 15.2 Sections and encodings

A 0.3 artifact retains the existing dynamic sections and adds machine sections
as needed:

~~~text
constants:
machine_constants:
data_segments:
symbols:
relocations:
main:
function ... machine_frame_size=...
debug_sources:
debug_locations:
debug_ranges:
~~~

The exact canonical text grammar is implemented in VM03-11, but its semantic
schema is fixed here:

- machine integer constants carry raw u64 bits plus an explicit integer width
  where the instruction consumes them;
- machine float constants carry F32 or F64 plus the corresponding IEEE bits;
- address constants and relocation results carry VM address bits only;
- data segment payloads are bytes, never dynamic arrays or strings;
- function metadata carries VM scalar descriptors and frame size;
- machine instruction operands use checked artifact-local indexes and
  immediates;
- debug metadata remains source-level metadata and does not contain host
  addresses or persistent snapshot IDs.

The serializer MUST use a canonical field and table order so an artifact that
is parsed and formatted without semantic changes has stable text. Machine
constant bits MUST round-trip exactly, including signed integer bit patterns,
negative zero, infinities, and non-NaN floating bits. NaN values use the
canonical representation defined in Section 3.2.

### 15.3 Loading order

The 0.3 loader performs these stages in order:

~~~text
parse header and sections
-> validate artifact structure and machine operands
-> allocate RODATA, DATA, BSS, and task-independent metadata
-> resolve symbols
-> apply relocations
-> create executable VM state
-> execute through the interpreter or an admitted JIT function
~~~

An artifact that fails parsing, verification, allocation arithmetic, symbol
resolution, or relocation validation MUST not reach machine instruction
execution.

## 16. Debugging, disassembly, and JIT policy

The debugger and disassembler MUST distinguish machine values from dynamic
values:

~~~text
MachineInt    decimal value and hexadecimal raw bits
MachineFloat  decimal value using its declared format
Address       hexadecimal VM address
~~~

An optional memory-examine command MAY be added after core machine value
display. It MUST use checked ranges and report VM errors rather than panic.
Existing source locations, call stacks, trace records, and profile counters
remain valid for dynamic programs.

New machine instructions are interpreter-first. JIT admission MUST reject a
function containing an unsupported 0.3 machine instruction and fall back to
the existing interpreter at a materialized VM frame. It MUST NOT emit an
approximate or incorrectly typed host instruction. Machine instructions are
not considered part of the JIT ABI merely because they are present in a
cdbc 0.3 artifact.

## 17. Verification requirements

The VM03 implementation must add verifier coverage for:

- all legal and illegal integer widths and float formats;
- operand domain and register validity;
- shift, conversion, and call metadata constraints;
- typed load/store sizes and domains;
- segment permissions, alignment, payload sizes, and overlap;
- frame size, frame offsets, and scalar ABI limits;
- symbol uniqueness, undefined references, and relocation targets;
- unsupported machine instructions and version mismatch.

Runtime coverage must include wrapping arithmetic, signed and unsigned
comparisons, both shift flavors, conversion traps, unaligned access, null and
out-of-bounds access, read-only writes, frame allocation/release, recursive
frames, segment initialization, and all three bulk memory operations.

Every 0.3 test run must also retain the current dynamic regression suite,
0.2 artifact compatibility tests, cdbc format tests, malformed-artifact tests,
debug/trace tests, and JIT fallback tests.

## 18. Milestone boundary

VM03-00 is complete when this document defines, without relying on LLVM or a
host ABI:

- the machine value model;
- integer representation, widths, signedness, wrapping, shifts, division,
  remainder, and comparison;
- float formats, arithmetic, comparisons, NaN, infinity, signed zero, and
  conversions;
- the address-zero policy and deterministic linear-memory layout;
- endian, alignment, typed access, bounds, and permissions;
- machine frame ownership and scalar call metadata;
- DATA, BSS, and RODATA semantics;
- machine bulk memory operations;
- symbols, relocations, and load ordering;
- trap categories and the verifier/runtime boundary;
- debug/disassembly and interpreter fallback policy; and
- the cdbc 0.2/0.3 compatibility contract.

VM03-01 begins implementation of MachineInt, MachineFloat, and Address. No
machine opcode implementation, cdbc 0.3 parser, or JIT lowering is implied by
completing VM03-00.
