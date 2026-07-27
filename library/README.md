# Data structures library

This directory contains a small data-structure library written in the public
Compiler Design language. It currently provides generic array-backed `Stack<T>`,
`Queue<T>`, `Deque<T>`, `BinaryHeap<T>`, and `PriorityQueue<T>` types, plus the
generic `Option<T>`, `Result<T, E>`, and immutable `List<T>` enums.

The planned structure and algorithm inventory, implementation constraints, and
staged delivery order are documented in
[`DATA_STRUCTURES_ROADMAP.md`](DATA_STRUCTURES_ROADMAP.md).

## Usage

Import the module with a namespace alias:

```cd
import "./library/data_structures.cd" as ds;

let stack = ds.newStack<number>();
stack.add(10);
stack.add(20);
print stack.top();
print stack.take();
print stack.snapshot();

let queue = ds.newQueue<string>();
queue.enqueue("first");
queue.enqueue("second");
print queue.front();
print queue.dequeue();
print queue.snapshot();

let deque = ds.newDeque<number>();
deque.addFront(2);
deque.addFront(1);
deque.addBack(3);
print deque.peekFront();
print deque.peekBack();
print deque.snapshot();

fun ascending(left: number, right: number): bool {
  return left < right;
}

let heap = ds.newBinaryHeap<number>(ascending);
heap.add(5);
heap.add(1);
heap.add(3);
print heap.peek();
print heap.take();

let maybe = ds.some<number>(42);
print match maybe {
  ds.Option.Some(value) => value,
  ds.Option.None => 0,
};

let result: ds.Result<number, string> = ds.ok<number, string>(42);
print match result {
  ds.Result.Ok(value) => value,
  ds.Result.Err(error) => 0,
};

let list = ds.prepend(2, ds.prepend(1, ds.emptyList<number>()));
print ds.toArray(ds.reverse(list));
```

The factory functions make the generic argument explicit while keeping the
backing fields out of normal construction code:

```cd
let numbers = ds.newStack<number>();
let names = ds.newQueue<string>();
let work = ds.newDeque<number>();
let maybeNumber = ds.some<number>(42);
```

## API

`Stack<T>` provides:

- `add(value: T)` — append to the top;
- `take(): T?` — remove and return the top, or `nil` when empty;
- `top(): T?` — inspect the top, or `nil` when empty;
- `size(): number` and `isEmpty(): bool`;
- `snapshot(): [T]` — return a shallow copy from bottom to top.

`Queue<T>` provides:

- `enqueue(value: T)` — add at the back;
- `dequeue(): T?` — remove and return the front, or `nil` when empty;
- `front(): T?` — inspect the front, or `nil` when empty;
- `size(): number` and `isEmpty(): bool`;
- `snapshot(): [T]` — return a shallow copy from front to back.

The queue compacts its backing array as consumed entries accumulate. Both
structures store references to their element values; snapshots copy only the
outer array.

`Deque<T>` provides:

- `addFront(value: T)` and `addBack(value: T)` — insert at either end;
- `takeFront(): T?` and `takeBack(): T?` — remove from either end, or return
  `nil` when empty;
- `peekFront(): T?` and `peekBack(): T?` — inspect either end, or return `nil`
  when empty;
- `size(): number` and `isEmpty(): bool`;
- `snapshot(): [T]` — return a shallow copy from front to back.

The deque uses two array-backed stacks. End operations are amortized `O(1)`;
`snapshot()` is `O(n)` and allocates one outer array. The transfer helpers are
implementation details of the representation, although the language currently
has no private methods.

`BinaryHeap<T>` provides:

- `add(value: T)` — add a value according to the comparator;
- `peek(): T?` and `take(): T?` — inspect or remove the highest-priority value,
  returning `nil` when empty;
- `size(): number` and `isEmpty(): bool`;
- `snapshot(): [T]` — return a shallow copy of the internal heap array.

`newBinaryHeap<T>(less)` accepts a `fun(T, T): bool` comparator. When
`less(a, b)` is true, `a` has higher priority than `b`; passing numeric `<`
creates a min-heap, while numeric `>` creates a max-heap. `add` and `take` are
`O(log n)`, `peek` is `O(1)`, and `snapshot` is `O(n)`. Equal-priority values
are not stable.

`PriorityQueue<T>` provides:

- `enqueue(value: T)` — add a value according to the comparator;
- `front(): T?` and `dequeue(): T?` — inspect or remove the highest-priority
  value, returning `nil` when empty;
- `size(): number` and `isEmpty(): bool`;
- `snapshot(): [T]` — return a shallow copy of the underlying heap array.

`newPriorityQueue<T>(less)` uses the same comparator contract as
`newBinaryHeap<T>`. `enqueue` and `dequeue` are `O(log n)`, `front` is `O(1)`,
and `snapshot` is `O(n)`.

`Option<T>` is an explicit result enum for APIs where callers should distinguish
success from absence with `match`:

- `Some(value: T)` — carries a value;
- `None` — carries no value;
- `some<T>(value: T): Option<T>` and `none<T>(): Option<T>` — construct the two
  variants.

`Option<T>` is useful when a nullable return would make the absence case
ambiguous or when a caller wants an exhaustive branch. The enum is a value; it
does not copy or mutate a payload supplied to `Some`.

`Result<T, E>` is an explicit success-or-error enum for APIs that need to carry
an error value:

- `Ok(value: T)` — carries the successful result;
- `Err(error: E)` — carries the error value;
- `ok<T, E>(value: T): Result<T, E>` and `err<T, E>(error: E): Result<T, E>` —
  construct the two variants.

Because each factory payload supplies only one of the two type parameters,
callers should usually provide both type arguments explicitly. `Result<T, E>`
does not throw or log errors; callers must inspect it with an exhaustive
`match`.

`List<T>` is an immutable recursive list:

- `emptyList<T>(): List<T>` — create an empty list;
- `prepend(value: T, list: List<T>): List<T>` — add a value at the front without
  changing the existing list;
- `head(list): T?` and `tail(list): List<T>?` — inspect the first value or the
  remaining list, returning `nil` for an empty list;
- `reverse(list): List<T>` — return a persistent reversed list;
- `toArray(list): [T]` — copy the values into a new array in list order.

`prepend`, `head`, and `tail` are `O(1)`. `reverse` and `toArray` are `O(n)`;
`reverse` allocates a new list while `toArray` allocates one new array. Tails
are shared between list values, so the structure supports persistent snapshots.
The type argument for `emptyList` must be explicit because the empty constructor
has no payload from which to infer `T`.

## Current language limitation

The language's builtin member-call sugar reserves names such as `push`, `pop`,
`remove`, and `clear`. A user-defined struct method with one of those names is
currently rejected, so the stack uses `add` and `take` instead of the usual
`push` and `pop` API. The public language manual explains that builtin member
forms are not shadowed, but it does not currently document this method-name
collision explicitly.

The language also has no private struct fields or private methods yet. The
backing fields are therefore technically accessible to callers; use the
factory functions and public methods as the supported API.

## Tests

Library fixtures live under `library/tests/` and are intentionally not
discovered by the compiler project's root test suites. The library runner
reuses the existing compiler golden and Rust VM test interfaces while keeping
its fixture root independent:

```sh
python3 library/tests/run_tests.py ./build/compiler_design vm-rs
```

Use `--case data_structures_binary_heap`, `--case data_structures_option`,
`--case data_structures_result`, or `--case data_structures_list` for a focused
run, or `--update` only when an intentional compiler-output change requires
refreshing library fixtures' `ast.out` files.
