# Data structures library

This directory contains a small data-structure library written in the public
Compiler Design language. It currently provides generic array-backed `Stack<T>`,
`Queue<T>`, `Deque<T>`, `RingBuffer<T>`, `BinaryHeap<T>`, and `PriorityQueue<T>` types, plus the
generic `Option<T>`, `Result<T, E>`, immutable `List<T>`, and array-backed
`Tree<T>`, `Set<T>`, and `MultiSet<T>` types. It also provides an array-backed
`MultiMap<K, V>` for one-to-many mappings, immutable BST helpers, and basic generic array algorithms,
including comparator-based insertion sort, window helpers, and interval merge.
It also includes array-backed numeric `FenwickTree` and `SegmentTree` types,
numeric two-pointer helpers for sorted arrays, and string/tree/graph algorithms.
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

let ring = ds.newRingBuffer<number>(2);
print ring.offer(10);
print ring.offer(20);
print ring.offer(30);
print ring.snapshot();
print ring.read();

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
print ds.connectedComponents(graph);
print ds.isBipartite(graph);
print ds.articulationPoints(graph);
print ds.bridges(graph);
print ds.eulerTrail(graph, 0);
print ds.shortestPath(graph, 0, 2);

let dag = ds.newGraph(3, true);
dag.addEdge(0, 1);
dag.addEdge(1, 2);
print ds.inDegrees(dag);
print ds.topologicalOrder(dag);
print ds.hasCycle(dag);

let weighted = ds.newWeightedGraph(3, false);
weighted.addEdge(0, 1, 2);
weighted.addEdge(1, 2, 3);
print ds.shortestWeightedPath(weighted, 0, 2);
print ds.allPairsWeightedDistances(weighted);
print ds.maxFlow(weighted, 0, 2);
print ds.minCut(weighted, 0, 2);

let forest = ds.minimumSpanningForest(weighted);
print forest.edgeCount();

let sums = ds.newFenwickTree([1, 2, 3, 4, 5]);
print sums.prefixSum(3);
print sums.rangeSum(1, 4);
sums.add(2, 7);
print sums.snapshot();

let aggregates = ds.newSegmentTree([5, 1, 4, 2, 8]);
print aggregates.rangeSum(1, 4);
print aggregates.rangeMinimum(0, 5);
aggregates.add(1, 5);
print aggregates.snapshot();

let tree = ds.treeNode(2, ds.treeLeaf(1), ds.treeLeaf(3));
print ds.treeInorder(tree);

let values = [3, 1, 3, 2];
print ds.reverseArray(values);
print ds.rotateArray(values, 1);
print ds.linearSearch(values, 2);
print ds.countValue(values, 3);
print ds.mostFrequent([3, 1, 3, 2, 3]);

fun ascending(left: number, right: number): bool {
  return left < right;
}

let ordered = ds.bstInsert(ds.bstInsert(ds.emptyTree<number>(), 2, ascending), 1, ascending);
print ds.treeInorder(ordered);
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
print ds.selectNonOverlappingIntervals(ranges);
print ds.minimumIntervalRooms(ranges);
print ds.canReachEnd([2, 3, 1, 1, 4]);
print ds.minimumJumps([2, 3, 1, 1, 4]);
print ds.huffmanMergeCost([5, 9, 12, 13, 16, 45]);

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

print ds.knapsack01([2, 3, 4], [3, 4, 5], 5);
print ds.completeKnapsack([2, 3], [3, 4], 7);
print ds.boundedKnapsack([2, 3], [3, 4], [2, 1], 7);
print ds.subsets([1, 2]);
print ds.combinations([1, 2, 3], 2);
print ds.permutations([1, 2, 3]);
print ds.generateParentheses(3);
print ds.nQueens(4);
print ds.mazePaths([
  [true, true],
  [true, true]
]);
print ds.uniqueGridPathsWithObstacles([
  [false, false, false],
  [false, true, false],
  [false, false, false]
]);
print ds.matrixChainCost([10, 30, 5, 60]);
print ds.matrixPower2x2([[1, 1], [1, 0]], 5);
print ds.primeFactors(12);
print ds.divisors(12);
print ds.binomialCoefficient(5, 2);
print ds.permutationCount(5, 2);
print ds.pascalTriangle(5);
print ds.gcdArray([12, 18, 24]);
print ds.prefixProducts([2, 3, 4]);
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

`RingBuffer<T>` is a fixed-capacity circular buffer:

- `newRingBuffer<T>(capacity): RingBuffer<T>` — floor the capacity at zero;
- `offer(value: T): bool` — append when space exists, returning `false` when
  full without overwriting the oldest value;
- `read(): Option<T>` and `peek(): Option<T>` — remove or inspect the oldest
  value, returning `None` when empty;
- `capacity`, `size`, `isEmpty`, `isFull`, and `snapshot` — inspect state and
  return logical values in FIFO order.

The buffer uses `Option<T>` slots and keeps FIFO operations at `O(1)`;
`snapshot()` is `O(n)` and allocates a fresh outer array. This library chooses
reject-on-full semantics for now, so callers can explicitly handle a failed
`offer` and no value is silently discarded.

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

Additional immutable list algorithms provide:

- `listLength(list): number` and `listGet(list, index): T?` — length and
  zero-based lookup, with `nil` for invalid/non-integral indexes;
- `listAppend(list, value): List<T>` and `listConcat(left, right): List<T>` —
  persistent tail append and concatenation;
- `listTake(list, count): List<T>` and `listDrop(list, count): List<T>` —
  prefix/suffix selection, with non-positive counts producing the natural empty
  or unchanged result;
- `mergeSortedLists(left, right, less): List<T>` — stable merge of two lists
  already ordered by the supplied comparator.

These operations preserve the input lists. `listLength`/`listGet` and
`listAppend` are `O(n)` in the traversed list, `listConcat` is `O(n)` in its
left input and shares the right input, `listTake`/`listDrop` are `O(k)`, and
`mergeSortedLists` is `O(n + m)` while sharing only its final untouched suffix.

`Tree<T>` is an immutable recursive binary tree:

- `emptyTree<T>(): Tree<T>` — create an empty tree;
- `treeNode(value, left, right): Tree<T>` and `treeLeaf(value): Tree<T>` —
  construct nodes and leaves;
- `treeSize`, `treeHeight`, and `treeLeafCount` — return structural statistics;
- `treePreorder`, `treeInorder`, `treePostorder`, and `treeLevelOrder` — return
  fresh arrays of values in the selected traversal order.
- `treeIsBalanced` and `treeMaxWidth` — check height balance and return the
  largest number of nodes on one level;
- `treeRootToLeafPaths` — return fresh path arrays in left-to-right order;
- `treeRootToLeafSums(tree: Tree<number>)` — return numeric root-to-leaf sums.

Tree constructors reuse their child values without mutating them. Traversals
and statistics are `O(n)`; recursive traversals use `O(h)` call-stack space,
while level order, width, balance checking, and path collection use `O(n)`
auxiliary space in the current implementation. Empty trees have size, height,
leaf count, and maximum width zero; all traversals and path helpers return `[]`.

The BST helpers use the same `Tree<T>` representation and a caller-supplied
`fun(T, T): bool` comparator:

- `bstContains(tree, target, less): bool` — search for a value;
- `bstInsert(tree, value, less): Tree<T>` — return a tree with the value added;
- `bstDelete(tree, value, less): Tree<T>` — return a tree with one matching
  value removed;
- `bstMinimum` and `bstMaximum` — return the extrema or `nil` for an empty tree;
- `bstPredecessor` and `bstSuccessor` — return the strict neighboring value or
  `nil` when none exists.

BST insertion ignores comparator-equivalent duplicates. Insert and delete
return new roots and preserve the original tree's shared subtrees. Operations
are `O(h)` time and `O(h)` recursive/temporary space, where `h` is the tree
height; without balancing, the worst case is `O(n)`.

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

`connectedComponents(graph)` returns BFS-ordered components for an undirected
graph, starting roots in ascending vertex order; it returns `[]` for directed
graphs and empty graphs. It runs in `O(V + E)` time with `O(V)` auxiliary space.

`isBipartite(graph)` colors each undirected component with BFS and returns
`false` for directed graphs, self-loops, or odd cycles. Empty undirected graphs
are bipartite; the check runs in `O(V + E)` time with `O(V)` auxiliary space.

`articulationPoints(graph)` returns undirected cut vertices in ascending vertex
order. `bridges(graph)` returns `[parent, child]` edge pairs in DFS completion
order. Both return `[]` for directed graphs and use iterative Tarjan low-link
traversal in `O(V + E)` time and `O(V)` auxiliary space.

`eulerTrail(graph, start)` uses Hierholzer traversal for undirected graphs and
returns a vertex sequence covering every edge once. It requires zero or two odd
vertices, uses an odd vertex as the start when two exist, and returns `[]` for
invalid, disconnected, directed, or empty-edge cases. Self-loops are supported.

`shortestDistances(graph, start)` uses BFS to return one distance per vertex;
unreachable vertices and invalid starts use `-1`. `shortestPath(graph, start,
goal)` returns a shortest path including both endpoints, or `[]` when either
vertex is invalid or unreachable. Both operate on unweighted edges in
`O(V + E)` time and `O(V)` auxiliary space.

`inDegrees(graph)` returns one incoming-edge count per vertex for a directed
graph, or `[]` for an undirected graph. `topologicalOrder(graph)` uses Kahn's
algorithm and preserves vertex/adjacency insertion order when multiple vertices
are ready. It returns `[]` for an undirected graph or a directed graph with a
cycle, and otherwise runs in `O(V + E)` time with `O(V)` auxiliary space.

`hasCycle(graph)` detects directed cycles with the topological-order check and
undirected cycles with an iterative parent-aware DFS. It handles self-loops,
returns `false` for empty or acyclic graphs, and runs in `O(V + E)` time with
`O(V)` auxiliary space.

`stronglyConnectedComponents(graph)` applies iterative Kosaraju traversal to a
directed graph and returns components in finish-order discovery order, with
each component listing vertices in its DFS discovery order. It returns `[]`
for undirected graphs and uses `O(V + E)` time and `O(V + E)` auxiliary space.

`WeightedGraph` stores non-negative numeric edge weights. Its Dijkstra helpers
`shortestWeightedDistances` and `shortestWeightedPath` return `-1`/`[]` for
unreachable results and reject negative weights at insertion. The current array
scan implementation runs in `O(V^2 + E)` time and `O(V)` auxiliary space.

`SignedWeightedGraph` is the separate signed-weight graph type. Its
`addEdge` accepts negative weights while retaining duplicate/vertex validation;
the existing non-negative `WeightedGraph` contract is unchanged.
`bellmanFord(graph, start)` returns
`Result<BellmanFordResult, BellmanFordError>`. A successful result contains
nullable `distances` (`nil` means unreachable) and `parents`; invalid starts
return `InvalidStart`, and a negative cycle reachable from `start` returns
`NegativeCycle`. The algorithm runs in `O(V * E)` time and `O(V)` auxiliary
space.

`minimumSpanningForest(graph)` applies Prim's algorithm to an undirected
`WeightedGraph`, preserves the vertex count, and returns a new undirected graph.
Disconnected inputs produce one tree per component; directed inputs produce an
empty forest with the same vertex count. The array-scan implementation runs in
`O(V^2 + E)` time and `O(V)` auxiliary space.

`minimumSpanningForestKruskal(graph)` returns the same forest contract using
weight-sorted edges and the array-backed `DisjointSet`. Ties are ordered by
`from` and then `to` vertex, disconnected inputs remain forests, and directed
inputs return an empty forest. The implementation runs in `O(E log E)` time and
uses `O(V + E)` auxiliary space.

`allPairsWeightedDistances(graph)` returns a Floyd–Warshall distance matrix for
the non-negative weighted graph, with diagonal zeroes and `-1` for unreachable
pairs. It supports directed and undirected graphs, runs in `O(V^3)` time, and
uses `O(V^2)` auxiliary space.

`maxFlow(graph, source, sink)` applies Edmonds–Karp to directed non-negative
capacities without modifying the input. It returns `0` for undirected or
invalid source/sink inputs and uses `O(V * E^2)` time and `O(V^2)` auxiliary
space.

`minCut(graph, source, sink)` runs the same residual search and returns original
positive-capacity edges crossing from the source-reachable side to the
non-reachable side after maximum flow. It preserves input order and returns
`[]` for undirected or invalid source/sink inputs. It uses `O(V * E^2)` time and
`O(V^2)` auxiliary space.

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

`selectionSort<T>` and `bubbleSort<T>` return sorted shallow copies, while
their `...InPlace` variants modify the supplied array. Selection sort is not
stable, uses `O(n^2)` time and `O(1)` extra space in place; bubble sort preserves
the input order of comparator-equal values, can stop early for an already
ordered input, and uses `O(n^2)` worst-case time and `O(1)` extra space. All
four use the same `less` callback contract as `sortArray`.

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
- `differenceArray(values: [number]): [number]` — return the first value and
  adjacent differences;
- `prefixMinimums` and `prefixMaximums` — return the running numeric extrema;
- `nextGreaterValues(values: [number]): [number]` — return the first strictly
  greater value to the right, or `-1` when none exists.

`chunkArray` and `slidingWindows` return `[]` for non-positive sizes; sliding
windows also return `[]` when the width exceeds the input length. Chunks and
windows are fresh outer arrays with shallowly shared elements. Chunking and
window generation are `O(n)` in the input plus output size; all prefix and
difference helpers are `O(n)` and allocate one array. `nextGreaterValues` is
`O(n)` time and `O(n)` stack/output space; equal values do not count as greater.

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

The one-dimensional DP helpers are:

- `climbStairs(steps): number?` — count one-step/two-step climbs; `0` has one
  empty climb and invalid/non-integral inputs return `nil`;
- `maxNonAdjacentSum(values): number` — maximize a sum without adjacent picks;
  an empty or all-negative input returns `0` by allowing the empty selection;
- `minCoinCount(amount, coins): number?` — return the minimum number of
  positive-integer coins or `nil` when the target is invalid/unreachable;
  non-positive or non-integral coin entries are ignored.

`climbStairs` is `O(steps)` time and `O(1)` space. `maxNonAdjacentSum` is
`O(n)` time and `O(1)` space. `minCoinCount` is `O(amount * coinCount)` time
and `O(amount)` space; these numeric helpers do not define overflow behavior.

The two-dimensional/string DP helpers are:

- `uniqueGridPaths(rows, columns): number?` — count right/down paths in a
  positive rectangular grid; zero dimensions return `0` and invalid dimensions
  return `nil`;
- `minGridPathSum(grid): number?` — find the minimum top-left to bottom-right
  sum using right/down moves; empty or non-rectangular grids return `nil`;
- `uniqueGridPathsWithObstacles(grid: [[bool]]): number?` — count right/down
  paths where `true` cells are blocked. Empty grids and zero-width grids return
  `0`, non-rectangular grids return `nil`, and a blocked start or end returns
  `0`;
- `editDistance(left, right): number` — compute insertion/deletion/substitution
  distance over Unicode scalar-value characters.

Grid DP uses `O(rows * columns)` time and `O(columns)` space for the two numeric
helpers and the obstacle-path helper. `editDistance` uses `O(leftLength * rightLength)` time and
`O(rightLength)` space; numeric overflow remains outside the library contract.

`longestIncreasingSubsequenceLength(values)` computes the strict numeric LIS
length with `O(n^2)` time and `O(n)` space; equal values do not extend a
subsequence. `longestCommonSubsequenceLength(left, right)` computes the LCS
length over Unicode scalar values with `O(leftLength * rightLength)` time and
`O(rightLength)` space.

`matrixChainCost(dimensions)` computes the minimum scalar multiplication cost
for matrices whose adjacent dimensions are given by the array. Positive integer
dimensions are required; an empty or one-element dimensions array returns `0`,
and invalid dimensions return `nil`. It uses interval DP in `O(n^3)` time and
`O(n^2)` space, where `n` is the number of matrices.

`matrixPower2x2(matrix, exponent)` raises a 2×2 numeric matrix to a
non-negative integer power, returning `nil` for another shape or invalid
exponent. Exponent zero returns the 2×2 identity matrix; exponentiation by
squaring takes `O(log exponent)` matrix multiplications.

The current backtracking and knapsack helpers are:

- `knapsack01(weights, values, capacity): number?` — maximize the value of
  selecting each item at most once. It returns `nil` when the arrays differ in
  length, the capacity is negative or non-integral, or a weight is negative or
  non-integral; zero-weight items are supported. The result is `0` for a valid
  zero capacity and uses `O(capacity)` space.
- `completeKnapsack(weights, values, capacity): number?` — maximize the value
  when each item may be selected repeatedly. It returns `nil` for mismatched
  arrays, invalid capacity, or a non-positive/non-integral weight; a valid
  zero capacity returns `0`. The ascending-capacity DP uses `O(itemCount *
  capacity)` time and `O(capacity)` space.
- `boundedKnapsack(weights, values, counts, capacity): number?` — maximize the
  value with a finite non-negative integer `counts` limit per item. It returns
  `nil` for mismatched arrays, invalid capacity, non-positive/non-integral
  weights, or invalid counts; a valid zero capacity returns `0`. Its repeated
  0/1 passes use `O(capacity * sum(counts))` time and `O(capacity)` space.
- `subsets<T>(values): [[T]]` — enumerate every subset, including the empty
  subset, in depth-first include/exclude order. The output has `2^n` entries
  and requires `O(n * 2^n)` output space.
- `combinations<T>(values, count): [[T]]` — enumerate index-preserving
  selections of the requested size. Invalid or out-of-range counts return an
  empty array; `count == 0` returns `[[]]`. Output order follows increasing
  source indexes.
- `permutations<T>(values): [[T]]` — enumerate index-based permutations in
  depth-first source-index order. An empty input returns `[[]]`; repeated
  values are treated as distinct positions, so duplicate values can produce
  duplicate-looking permutations.
- `generateParentheses(pairs): [string]` — enumerate balanced parenthesis
  strings in depth-first order, trying `(` before `)`. Negative or
  non-integral counts return `[]`; `0` returns the one empty string. The
  result count is the Catalan number for `pairs`, so the complete output has
  exponential time and space cost.
- `nQueens(size): [[number]]` — enumerate every N-Queens solution as one
  zero-based column index per row, with rows and columns searched in ascending
  order. Negative or non-integral sizes return `[]`; `0` returns `[[]]`.
  Complete enumeration has exponential search cost and stores all solutions.
- `mazePaths(maze: [[bool]]): [string]` — enumerate paths from the top-left
  to bottom-right of a rectangular maze where `true` is walkable. The current
  version permits only right (`R`) and down (`D`) moves, tries `R` before `D`,
  and returns `[]` for empty, non-rectangular, or blocked-endpoint mazes.
  It stores all generated path strings.

The three generators allocate complete nested arrays rather than exposing a
lazy iterator. Their running time and output space are proportional to the
number and total size of generated results; callers should keep exponential
inputs small. All generated rows are fresh outer arrays, while their elements
are shallowly copied.

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

`findSubstring(text, pattern)` returns the first Unicode scalar-value offset,
or `-1` when absent; an empty pattern matches at `0`. `prefixFunction(pattern)`
returns the KMP failure table, `zFunction(text)` returns the Z table, and
`kmpSearch` uses the prefix table for `O(n + m)` matching. Both tables use
Unicode scalar-value offsets and return `[]` for an empty string.
`isPalindrome` compares scalar values from both ends and treats an empty string
as a palindrome. These functions do not normalize combining marks.

`Trie` is an array-node prefix tree. `insert(word)` returns `true` only when a
new word is added; `has`, `startsWith`, and `wordsWithPrefix` handle empty and
Unicode scalar-value strings. Prefix results use child insertion order rather
than lexical sorting, and the current linear edge scans take `O(k)` per node
lookup where `k` is that node's outgoing edge count.

`FenwickTree` stores numeric values in an array-backed binary indexed tree:

- `newFenwickTree(values: [number]): FenwickTree` — build from a copied initial
  value array;
- `size(): number` and `snapshot(): [number]` — inspect the logical values;
- `add(index: number, delta: number): bool` — apply a point update using a
  zero-based index;
- `valueAt(index: number): number?` — read one value, or `nil` for an invalid
  index;
- `prefixSum(endExclusive: number): number?` — sum `[0, endExclusive)`;
- `rangeSum(start: number, endExclusive: number): number?` — sum the half-open
  range `[start, endExclusive)`.

Invalid or non-integral indexes and ranges return `false` or `nil` without
changing the tree. The constructor copies the input array, and `snapshot`
returns another outer-array copy. Construction is `O(n log n)` with the
language's arithmetic-only low-bit precomputation; point updates and both
query methods are `O(log n)`, while `snapshot` is `O(n)`.

`SegmentTree` stores numeric values with fixed sum and minimum aggregates:

- `newSegmentTree(values: [number]): SegmentTree` — build from a copied input;
- `setValue(index: number, value: number): bool` and `add(index, delta): bool` —
  replace or increment one zero-based element;
- `rangeSum(start, endExclusive): number?` — query the half-open range sum;
- `rangeMinimum(start, endExclusive): number?` — query its minimum, or `nil`
  for an empty/invalid range;
- `size`, `valueAt`, and `snapshot` — inspect the logical values.

The tree uses inactive padded leaves, so non-power-of-two input lengths and
negative values need no sentinel convention. Construction is `O(n)`, point
updates and aggregate queries are `O(log n)`, and `snapshot` is `O(n)`. Invalid
indexes/ranges return `false` or `nil` without changing the structure; an
empty valid sum range returns `0`.

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

`selectNonOverlappingIntervals(intervals)` returns a fresh maximum-cardinality
schedule chosen by earliest end time. It preserves the input, rejects an
interval with `start > end` by returning `[]`, and treats touching endpoints as
overlapping, so a selected interval must have `start > lastEnd`. The result is
ordered by selection and the algorithm takes `O(n log n)` time and `O(n)` space.

`minimumIntervalRooms(intervals)` returns the maximum number of simultaneously
active inclusive intervals. Empty input needs `0` rooms; an invalid interval
returns `nil`; touching endpoints count as simultaneous activity. The event
sweep takes `O(n log n)` time and `O(n)` space and leaves the input unchanged.

`canReachEnd(values)` treats each non-negative integer as the maximum number of
positions that can be advanced from that index and returns whether the final
position is reachable. `minimumJumps(values)` returns the corresponding minimum
number of jumps or `nil` when unreachable. Both reject negative or non-integral
entries (`false`/`nil`), treat an empty or one-element input as already reached,
and run in `O(n)` time with `O(1)` extra space.

`huffmanMergeCost(weights)` returns the minimum total cost of repeatedly merging
the two smallest non-negative weights. Empty and single-weight inputs cost `0`,
negative weights return `nil`, and the input is unchanged. This is the numeric
Huffman construction core; it does not assign symbol codes. The current
array-only implementation takes `O(n^2)` time and `O(n)` temporary space.

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

`gcd(left, right)` uses the Euclidean algorithm and returns a non-negative
greatest common divisor; `gcd(0, 0)` is `0`, and negative operands are treated
by magnitude. `lcm(left, right)` returns a non-negative least common multiple
with `0` whenever either operand is zero. Both APIs require finite integer
`number` inputs, run in `O(log(min(|left|, |right|)))` arithmetic steps in the
usual case, and do not define overflow behavior.

`ExtendedGcdResult` stores the non-negative `gcd` and coefficients satisfying
`left * leftCoefficient + right * rightCoefficient == gcd`. `extendedGcd`
returns zero coefficients for `(0, 0)` and otherwise uses the same integer
input contract as `gcd`; its iterative Euclidean pass uses logarithmic steps
for the usual inputs.

`isPrime(value)` returns `false` for values below `2`, non-integers, and
composites. `sievePrimes(limit)` uses the Sieve of Eratosthenes and returns
primes in ascending order up to `floor(limit)`, or `[]` below `2`. The primality
check uses trial division with `O(sqrt(n))` arithmetic steps; the sieve uses
`O(n log log n)` time and `O(n)` auxiliary space for an integer bound `n`.

`primeFactors(value)` returns the prime factorization with repeated factors in
ascending order, while `divisors(value)` returns all positive divisors in
ascending order. Both return `[]` for non-positive or non-integral inputs;
`primeFactors(1)` is also empty and `divisors(1)` is `[1]`. Factorization and
divisor enumeration use `O(sqrt(n))` trial steps plus output space.

`binomialCoefficient(n, k)` computes `n` choose `k` with non-negative integer
arguments, returning `nil` when the arguments are invalid or `k > n`.
`permutationCount(n, k)` computes `nPk` under the same argument contract.
`pascalTriangle(rows)` returns the first `rows` rows, with invalid row counts
producing `[]`; both use numeric DP and do not define overflow behavior.

`gcdArray(values)` folds the non-negative `gcd` over a non-empty integer array,
returning `nil` for an empty or non-integral input. `prefixProducts(values)`
returns cumulative products in a fresh array and returns `[]` for empty input;
both are linear scans and numeric overflow is outside the contract.

`fastPower(base, exponent)` uses exponentiation by squaring for a non-negative
integer exponent and returns `nil` for negative or non-integer exponents;
`base^0`, including `0^0`, returns `1`. `factorial(value)` and
`fibonacci(index)` likewise return `nil` for negative or non-integer inputs,
with `factorial(0) == 1`, `fibonacci(0) == 0`, and `fibonacci(1) == 1`.
`fastPower` takes `O(log exponent)` steps, while the current factorial and
Fibonacci implementations take `O(value)`/`O(index)` steps; none define numeric
overflow behavior.

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
`--case data_structures_tree`, `--case data_structures_bst`,
`--case data_structures_ring_buffer`,
`--case data_structures_set`, `--case data_structures_multiset`,
`--case data_structures_multimap`, `--case data_structures_disjoint_set`,
`--case data_structures_fenwick`,
`--case data_structures_segment_tree`,
`--case data_structures_graph`, `--case data_structures_weighted_graph`,
`--case algorithms_graph_topological`,
`--case algorithms_graph_traversal`,
`--case algorithms_graph_paths`,
`--case algorithms_graph_bellman_ford`,
`--case algorithms_graph_kruskal`,
`--case algorithms_string_matching`,
`--case algorithms_string_trie`,
`--case algorithms_graph_max_flow`, `--case algorithms_graph_min_cut`,
`--case array_algorithms_basic`,
`--case array_algorithms_teaching_sort`,
`--case numeric_algorithms_gcd`,
`--case numeric_algorithms_extended_gcd`,
`--case numeric_algorithms_primes`,
`--case numeric_algorithms_sequences`,
`--case algorithms_dp_1d`,
`--case algorithms_dp_grid`,
`--case algorithms_dp_sequences`,
`--case array_algorithms_sort`, `--case array_algorithms_windows`,
`--case array_algorithms_prefix_monotonic`,
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
