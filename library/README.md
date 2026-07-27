# Data structures library

This directory contains a small data-structure library written in the public
Compiler Design language. It currently provides generic array-backed `Stack<T>`
and `Queue<T>` types.

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
```

The factory functions make the generic argument explicit while keeping the
backing fields out of normal construction code:

```cd
let numbers = ds.newStack<number>();
let names = ds.newQueue<string>();
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
