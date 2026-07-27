# Data structures library

This directory contains a small data-structure library written in the public
Compiler Design language. It currently provides generic array-backed `Stack<T>`
and `Queue<T>` types.

The planned structure and algorithm inventory, implementation constraints, and
staged delivery order are documented in
[`DATA_STRUCTURES_ROADMAP.md`](DATA_STRUCTURES_ROADMAP.md).

## Usage

Import the module with a namespace alias:

```cd
import "./library/data_structures.cd" as ds;

let stack = ds.newStack<number>();
stack.push(10);
stack.push(20);
print stack.top();
print stack.pop();
print stack.snapshot();

let queue = ds.newQueue<string>();
queue.enqueue("first");
queue.enqueue("second");
print queue.front();
print queue.dequeue();
print queue.snapshot();
```

The factory functions make the generic argument explicit while keeping the
backing fields out of normal construction code:

```cd
let numbers = ds.newStack<number>();
let names = ds.newQueue<string>();
```

## API

`Stack<T>` provides:

- `push(value: T)` — append to the top;
- `pop(): T?` — remove and return the top, or `nil` when empty;
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

## Member-call precedence

Methods declared for a known struct receiver take precedence over builtin
member-call sugar with the same name. Array, map, string, and range receivers
continue to use the builtin forms, so the stack can expose `push` and `pop`
without changing array behavior.

The backing fields are private to this module. Construct stacks and queues
through `newStack` and `newQueue`; callers use the public methods and cannot
replace the internal storage or queue cursor directly.
