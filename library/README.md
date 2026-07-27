# Data structures library

This directory contains a small data-structure library written in the public
Compiler Design language. It currently provides generic array-backed `Stack<T>`,
`Queue<T>`, `Deque<T>`, `BinaryHeap<T>`, and `PriorityQueue<T>` types, plus the
generic `Option<T>`, `Result<T, E>`, immutable `List<T>`, and array-backed
`Set<T>` and `MultiSet<T>` types. It also provides an array-backed
`MultiMap<K, V>` for one-to-many mappings and basic generic array algorithms,
including comparator-based insertion sort, window helpers, and interval merge.
It also includes numeric two-pointer helpers for sorted arrays.
Array set operations preserve first-occurrence order and use language equality.

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

let seen = ds.newSet<number>();
seen.add(2);
seen.add(2);
seen.add(1);
print seen.snapshot();

let bag = ds.newMultiSet<string>();
bag.add("tag");
bag.add("tag");
print bag.countOf("tag");

let index = ds.newMultiMap<string, number>();
index.add("even", 2);
index.add("even", 4);
print index.getAll("even");

let components = ds.newDisjointSet(4);
components.union(0, 1);
components.union(2, 3);
print components.connected(0, 3);
print components.componentCount();

let graph = ds.newGraph(3, false);
graph.addEdge(0, 1);
graph.addEdge(1, 2);
print graph.neighbors(1);
print ds.breadthFirstOrder(graph, 0);
print ds.depthFirstOrder(graph, 0);
print ds.shortestPath(graph, 0, 2);

let values = [3, 1, 3, 2];
print ds.reverseArray(values);
print ds.rotateArray(values, 1);
print ds.linearSearch(values, 2);
print ds.countValue(values, 3);
print ds.mostFrequent([3, 1, 3, 2, 3]);

fun ascending(left: number, right: number): bool {
  return left < right;
}

print ds.sortArray(values, ascending);
print ds.mergeSort(values, ascending);
print ds.quickSort(values, ascending);
print ds.heapSort(values, ascending);
print ds.chunkArray(values, 2);
print ds.slidingWindows(values, 2);
print ds.prefixSums(values);

let ranges: [ds.Interval] = [
  ds.Interval { start: 1, end: 3 },
  ds.Interval { start: 2, end: 5 }
];
print ds.mergeIntervals(ranges);
print ds.intersectIntervals(ranges, [
  ds.Interval { start: 2, end: 4 },
  ds.Interval { start: 7, end: 9 }
]);

print ds.mergeSortedNumbers([1, 3], [2, 4]);
print ds.twoSumSorted([1, 2, 4, 7], 6);

print ds.uniqueValues([3, 1, 3, 2]);
print ds.unionValues([1, 2], [2, 3]);
print ds.windowSums([2, -1, 3, 4, -2, 1], 3);
print ds.maxWindowSum([2, -1, 3, 4, -2, 1], 3);
print ds.maxSubarraySum([-2, 1, -3, 4, -1, 2, 1, -5, 4]);

fun ascendingNumber(left: number, right: number): bool {
  return left < right;
}

print ds.lowerBound([1, 2, 2, 4], 2, ascendingNumber);
print ds.upperBound([1, 2, 2, 4], 2, ascendingNumber);
print ds.binarySearch([1, 2, 2, 4], 2, ascendingNumber);
print ds.compareStrings("ant", "apple");
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

`Set<T>` provides an array-backed set with language equality:

- `add(value: T)` — insert a value if it is not already present;
- `has(value: T): bool` — test membership;
- `discard(value: T): bool` — remove a value and report whether it was present;
- `size(): number` and `isEmpty(): bool`;
- `snapshot(): [T]` — return a shallow copy in insertion order.

`newSet<T>()` creates an empty set. `add`, `has`, and `discard` use linear
search, so they are `O(n)`; `discard` preserves the order of the remaining
values and is also `O(n)`. `snapshot` is `O(n)` and allocates one outer array.
Membership follows the language's `==` semantics, including its behavior for
reference values.

`MultiSet<T>` stores one entry per distinct value and its occurrence count:

- `add(value: T)` — add one occurrence;
- `countOf(value: T): number` and `has(value: T): bool` — query occurrences;
- `takeOne(value: T): bool` — remove one occurrence, returning whether it was
  present;
- `entries(): [MultiSetEntry<T>]` — return `{ value, count }` snapshots in first
  insertion order.

`newMultiSet<T>()` creates an empty multiset. Lookup and `add` are `O(n)` over
the number of distinct values; `takeOne` is `O(n)` when the last occurrence is
removed because it preserves entry order. `entries` is `O(n)` and allocates a
new outer array plus one entry value per distinct element.

`MultiMap<K, V>` stores each key once and keeps its values in insertion order:

- `add(key: K, value: V)` — append one value to a key;
- `getAll(key: K): [V]` — return a fresh shallow array, or `[]` for an unknown
  key;
- `discard(key: K, value: V): bool` — remove the first matching pair and report
  whether it was present; removing the last value removes the key entry.

`newMultiMap<K, V>()` creates an empty mapping. Key and value lookup use
language equality. `add`, `getAll`, and `discard` are linear in the number of
keys or values for the selected key; key/value arrays are maintained without a
hash table, and key order is preserved after removals.

`DisjointSet` represents integer vertices `[0, size)` with parent and component
size arrays. `representative` performs path compression, `union` uses union by size and
returns whether two components were joined, and `connected`, `componentCount`,
and `size` provide queries. Construction with a non-positive count creates an
empty set; vertex arguments must be in range. `representative` and `union` are amortized
near-constant time (`O(alpha(n))`), while construction is `O(n)`.

`Graph` is an integer-vertex adjacency-list graph. `newGraph(vertices, directed)`
creates vertices `[0, vertices)`; `addEdge` rejects invalid or duplicate edges,
and undirected edges are stored in both adjacency lists while counting once.
`neighbors` returns a fresh array, and invalid vertex queries return `false` or
`[]`. Adjacency operations are linear in the degree because this version does
not use a hash set.

`breadthFirstOrder(graph, start)` and `depthFirstOrder(graph, start)` return
reachable vertex orders from a start vertex. BFS uses a queue and DFS uses an
explicit stack; both mark vertices when scheduled, preserve adjacency insertion
order, and return `[]` for an invalid start. Their time complexity is `O(V + E)`
over the reachable subgraph, with `O(V)` auxiliary space.

`shortestDistances(graph, start)` uses BFS to return one distance per vertex;
unreachable vertices and invalid starts use `-1`. `shortestPath(graph, start,
goal)` returns a shortest path including both endpoints, or `[]` when either
vertex is invalid or unreachable. Both operate on unweighted edges in
`O(V + E)` time and `O(V)` auxiliary space.

The array algorithms are non-mutating and return shallow copies where they
return arrays:

- `reverseArray<T>(values): [T]` — return values in reverse order;
- `linearSearch<T>(values, target): number` — return the first matching index,
  or `-1` when absent;
- `countValue<T>(values, target): number` — count matching values.

`reverseArray`, `linearSearch`, and `countValue` are all `O(n)`; only
`reverseArray` allocates a new outer array. Empty array arguments need an
explicit type argument, such as `reverseArray<number>([])`, because they carry
no element from which to infer `T`.

`rotateArray<T>(values, shift)` returns a shallow copy rotated right for a
positive shift and left for a negative shift. Shifts larger than the input
length wrap around; empty inputs return `[]`. It runs in `O(n)` time and uses
`O(n)` result space without modifying the input.

`isSorted<T>(values, less)` checks whether every adjacent pair is ordered by
the supplied comparator. Empty and single-element arrays are sorted, and the
check is `O(n)` with `O(1)` extra space.

`frequencyEntries<T>(values)` returns `MultiSetEntry<T>` values in first-seen
order, and `mostFrequent<T>(values)` returns the value with the largest count
or `nil` for an empty input. Ties keep the first-seen value. Both use the
array-backed multiset and therefore scan linearly over the input plus the
number of distinct values.

`sortArray<T>(values, less)` returns a stable insertion-sorted shallow copy;
`sortArrayInPlace<T>(values, less)` applies the same stable insertion sort to
the supplied array. Both take `fun(T, T): bool`, where `less(left, right)` means
that `left` should appear first. Sorting is `O(n^2)`; the copying version uses
`O(n)` additional outer-array space and the in-place version uses `O(1)` extra
space. Neither function requires a total-order check beyond the comparator's
behavior.

`mergeSort<T>(values, less)` returns a stable, sorted shallow copy and leaves
the input unchanged. It uses bottom-up merge sort in `O(n log n)` time and
`O(n)` temporary outer-array space. Equal comparator values keep their input
order.

`quickSort<T>(values, less)` returns a sorted shallow copy, while
`quickSortInPlace<T>(values, less)` sorts the supplied array. Quick sort is not
stable; the middle-element pivot gives average `O(n log n)` time but the worst
case remains `O(n^2)`. The copying version uses `O(n)` outer-array space, and
the in-place version additionally uses recursion stack space proportional to
the partition depth.

`heapSort<T>(values, less)` returns a sorted shallow copy, while
`heapSortInPlace<T>(values, less)` sorts the supplied array in place. Heap sort
is not stable, runs in `O(n log n)` time, and uses `O(1)` extra space for the
in-place version; the copying version additionally allocates the result array.

Window and prefix helpers are also non-mutating:

- `chunkArray<T>(values, size): [[T]]` — split into consecutive chunks;
- `slidingWindows<T>(values, width): [[T]]` — return every consecutive window;
- `prefixSums(values: [number]): [number]` — return same-length cumulative sums.

`chunkArray` and `slidingWindows` return `[]` for non-positive sizes; sliding
windows also return `[]` when the width exceeds the input length. Chunks and
windows are fresh outer arrays with shallowly shared elements. Chunking and
window generation are `O(n)` in the input plus output size; `prefixSums` is
`O(n)` and allocates one array.

`windowSums(values, width)` returns the numeric sum of every fixed-width window
using a rolling sum. `maxWindowSum(values, width)` returns the largest such sum,
or `nil` when the width is non-positive, exceeds the input length, or the input
is empty. Both functions leave the input unchanged; `windowSums` allocates one
result array and runs in `O(n)`, while `maxWindowSum` runs in `O(n)` and uses
`O(n)` temporary space for the current array-backed implementation.

`maxSubarraySum(values)` returns the largest sum of a non-empty contiguous
subarray, or `nil` for an empty input. It preserves the input and handles an
all-negative array by returning its least-negative element. The scan is `O(n)`
time and `O(1)` extra space.

For an array already sorted according to `less`, `lowerBound(values, target,
less)` returns the first insertion position, and `upperBound` returns the
position after all equivalent values. `binarySearch` returns the first matching
index or `-1`. The three functions use `O(log n)` time and `O(1)` extra space;
the comparator must define the same ordering used to sort the input. Empty
arrays return insertion position `0` and `binarySearch` returns `-1`.

`compareStrings(left, right)` provides a library-level comparator for printable
ASCII strings: it returns `-1`, `0`, or `1` for less/equal/greater, and returns
`nil` if either input contains a non-ASCII scalar. `stringLess` adapts that
result to the `fun(string, string): bool` comparator expected by the search
helpers; unsupported-character pairs are treated as unordered (`false`). The
language currently has no string relational operator, so this is an explicit
library contract rather than a replacement for future language-level ordering.

`Interval { start, end }` represents a numeric interval with the documented
precondition `start <= end`. `mergeIntervals(intervals)` returns a new array
sorted by start, merges overlapping or touching intervals, and leaves the input
array unchanged. It uses `O(n log n)` sorting time plus linear merging and
allocates `O(n)` output space.

`intersectIntervals(left, right)` first applies the same normalization to each
input and then returns their pairwise intersection as sorted, non-overlapping
intervals. Endpoints are inclusive, so touching intervals produce a zero-length
intersection such as `[2,2]`. Empty or disjoint inputs return `[]`; both inputs
remain unchanged. The two normalization passes take `O(n log n + m log m)` time,
the sweep is linear afterward, and the result uses `O(n + m)` space.

For non-decreasing numeric arrays, `mergeSortedNumbers(left, right)` returns a
linear-time merged copy and `twoSumSorted(values, target)` uses two pointers to
return the first matching `[leftIndex, rightIndex]`, or `[]` when no pair exists.
Both are `O(n)` time with `O(n)` output space for the returned arrays and do not
modify their inputs. `twoSumSorted` requires its input to already be sorted.

The array set helpers are non-mutating and remove duplicate output values while
preserving the first occurrence order:

- `uniqueValues<T>(values)` — stable de-duplication;
- `intersectionValues<T>(left, right)` — values present in both arrays;
- `unionValues<T>(left, right)` — all values from left, followed by new values
  from right;
- `differenceValues<T>(left, right)` — values from left that are absent in right.

They use linear scans, so the current array-backed versions are `O(n*m)` in the
worst case for the input sizes involved and allocate a new result array.

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
`--case data_structures_result`, `--case data_structures_list`,
`--case data_structures_set`, `--case data_structures_multiset`,
`--case data_structures_multimap`, `--case data_structures_disjoint_set`,
`--case data_structures_graph`, `--case algorithms_graph_traversal`,
`--case algorithms_graph_paths`,
`--case array_algorithms_basic`,
`--case array_algorithms_sort`, `--case array_algorithms_windows`,
`--case array_algorithms_intervals`, `--case array_algorithms_interval_intersection`,
`--case array_algorithms_binary_search`, `--case array_algorithms_merge_sort`,
`--case array_algorithms_rotation`,
`--case array_algorithms_frequency`,
`--case array_algorithms_quick_sort`, `--case array_algorithms_heap_sort`,
`--case array_algorithms_two_pointer`,
`--case array_algorithms_sets`, or `--case array_algorithms_window_stats` for
focused coverage. Use `--update` only when
an intentional compiler-output change requires refreshing library fixtures'
`ast.out` files.
