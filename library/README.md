# Data structures library

This directory contains a small data-structure library written in the public
Compiler Design language. It currently provides generic array-backed `Stack<T>`,
`Queue<T>`, `Deque<T>`, `RingBuffer<T>`, `BinaryHeap<T>`, `PriorityQueue<T>`, and
numeric `MedianHeap` types, plus the
generic `Option<T>`, `Result<T, E>`, immutable `List<T>`, and array-backed
`Tree<T>`, `AvlTree<T>`, `RedBlackTree<T>`, `Set<T: Eq>`, `HashSet<T: Eq + Hash>`, `OrderedSet<T>`, `OrderedMap<K, V>`, `HashMap<K: Eq + Hash, V>`, `BiMap<K: Eq, V: Eq>`, `LruCache<K: Eq, V>`, `LfuCache<K: Eq, V>`, and `MultiSet<T: Eq>` types. It also provides an array-backed
`MultiMap<K: Eq, V: Eq>` for one-to-many mappings, immutable BST helpers, and basic generic array algorithms,
including comparator-based sorting, window helpers, and interval merge.
It also provides mutable singly linked `LinkedNode<T>`/`LinkedList<T>` structures
with stable node handles and cycle-safe debug output.
It also includes array-backed numeric `FenwickTree`, `SegmentTree`, and `SparseTable`,
and immutable numeric `Matrix` types,
numeric two-pointer helpers for sorted arrays, and string/tree/graph algorithms.
Array set operations preserve first-occurrence order and use language equality.

The planned structure and algorithm inventory, implementation constraints, and
staged delivery order are documented in
[`DATA_STRUCTURES_ROADMAP.md`](DATA_STRUCTURES_ROADMAP.md).

The implementation is split into topic modules: `collections.cd`, `trees.cd`,
`sets.cd`, `hash_collections.cd`, `graphs.cd`, `strings.cd`, `range_trees.cd`,
`sorting.cd`, `array_algorithms.cd`, `dynamic_programming.cd`, `backtracking.cd`,
`numeric.cd`, and `linked_structures.cd`. `data_structures.cd` remains the
compatibility facade and
re-exports the stable public API, so existing `as ds` imports do not change.

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
print graph.removeEdge(0, 1);
print graph.edgeCount();
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

let schedule = ds.newWeightedGraph(3, true);
schedule.addEdge(0, 1, 3);
schedule.addEdge(1, 2, 4);
print ds.criticalPath(schedule, 0);
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
print ds.shellSort(values, ascending);
print ds.mergeSort(values, ascending);
print ds.countingSort(values);
print ds.topKSmallest(values, 2, ascending);
print ds.topKLargest(values, 2, ascending);
print ds.kthSmallest(values, 1, ascending);
print ds.kthLargest(values, 1, ascending);
print ds.countInversions(values);
fun atMostThree(value: number): bool {
  return value <= 3;
}
print ds.stablePartition(values, atMostThree);
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
print ds.mergeSortedArrays([[1, 3], [2, 4]], ascending);
print ds.twoSumSorted([1, 2, 4, 7], 6);
print ds.threeSumZero([-1, 0, 1, 2, -1, -4]);
print ds.threeSumClosest([-1, 2, 1, -4], 1);

print ds.uniqueValues([3, 1, 3, 2]);
print ds.unionValues([1, 2], [2, 3]);
print ds.windowSums([2, -1, 3, 4, -2, 1], 3);
print ds.maxWindowSum([2, -1, 3, 4, -2, 1], 3);
print ds.maxWindowValues([2, -1, 3, 4, -2, 1], 3);
print ds.minWindowValues([2, -1, 3, 4, -2, 1], 3);
print ds.maxSubarraySum([-2, 1, -3, 4, -1, 2, 1, -5, 4]);

fun ascendingNumber(left: number, right: number): bool {
  return left < right;
}

print ds.lowerBound([1, 2, 2, 4], 2, ascendingNumber);
print ds.upperBound([1, 2, 2, 4], 2, ascendingNumber);
print ds.binarySearch([1, 2, 2, 4], 2, ascendingNumber);
print ds.rotatedBinarySearch([4, 5, 6, 7, 0, 1, 2], 0, ascendingNumber);
print ds.peakIndex([1, 3, 5, 4, 2]);
print ds.mountainPeakIndex([1, 3, 5, 4, 2]);
print ds.minimumLargestPartitionSum([7, 2, 5, 10, 8], 2);
print ds.compareStrings("ant", "apple");
print ds.longestUniqueSubstringLength("abcabcbb");
print ds.longestPalindromicSubstring("babad");
print ds.longestCommonSubstringLength("ababc", "babca");
print ds.longestCommonSubsequence("abcde", "ace");

print ds.knapsack01([2, 3, 4], [3, 4, 5], 5);
print ds.completeKnapsack([2, 3], [3, 4], 7);
print ds.boundedKnapsack([2, 3], [3, 4], [2, 1], 7);
print ds.subsets([1, 2]);
print ds.combinations([1, 2, 3], 2);
print ds.permutations([1, 2, 3]);
print ds.generateParentheses(3);
print ds.isBalancedBrackets("([{}])");
print ds.largestHistogramArea([2, 1, 5, 6, 2, 3]);
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
print ds.mergeStonesCost([4, 1, 1, 4]);
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

- `push(value: T)` — append to the top;
- `pop(): optional<T>` — remove and return the top, or `nil` when empty;
- `top(): optional<T>` — inspect the top, or `nil` when empty;
- `size(): number` and `isEmpty(): bool`;
- `snapshot(): [T]` — return a shallow copy from bottom to top.

`Queue<T>` provides:

- `enqueue(value: T)` — add at the back;
- `dequeue(): optional<T>` — remove and return the front, or `nil` when empty;
- `front(): optional<T>` — inspect the front, or `nil` when empty;
- `size(): number` and `isEmpty(): bool`;
- `snapshot(): [T]` — return a shallow copy from front to back.

The queue compacts its backing array as consumed entries accumulate. Both
structures store references to their element values; snapshots copy only the
outer array.

`LinkedNode<T>` and `LinkedList<T>` provide mutable singly linked storage:

- `newLinkedNode<T>(value)` — create a node handle with an empty `next` link;
- `readValue(): T`, `setValue(value: T)`, `nextNode(): optional<LinkedNode<T>>`,
  and `setNext(next)` — inspect or mutate a node through any alias;
- `newLinkedList<T>()` — create an empty list;
- `pushFront(value)` and `pushBack(value)` — insert at either end;
- `popFront(): optional<T>` — remove and return the front value, or `nil` when empty;
- `frontNode(): optional<LinkedNode<T>>` and `nodeAt(index): optional<LinkedNode<T>>`
  — obtain stable node handles;
- `size(): number`, `isEmpty(): bool`, and `snapshot(): [T]` — inspect the list or
  copy its values into a fresh outer array.

Node assignment and parameter passing copy the handle, not the node. A value or
link mutation through one alias is visible through every other alias. Removing a
node from a list only removes the list's link; a previously returned handle stays
valid and keeps the node alive. `pushFront`, `popFront`, and `nodeAt` are `O(1)`
or `O(index)` as applicable; `pushBack` and an acyclic `snapshot` are `O(n)`.
Snapshots are shallow and assume an acyclic list. Strong cycles are allowed and
remain retained until VM teardown; there is no cycle collector or weak handle.
Printing a cyclic node graph emits `<cycle>` on the active recursive path, while
struct equality remains identity-based.

`Deque<T>` provides:

- `addFront(value: T)` and `addBack(value: T)` — insert at either end;
- `takeFront(): optional<T>` and `takeBack(): optional<T>` — remove from either end, or return
  `nil` when empty;
- `peekFront(): optional<T>` and `peekBack(): optional<T>` — inspect either end, or return `nil`
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
- `peek(): optional<T>` and `take(): optional<T>` — inspect or remove the highest-priority value,
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
- `front(): optional<T>` and `dequeue(): optional<T>` — inspect or remove the highest-priority
  value, returning `nil` when empty;
- `size(): number` and `isEmpty(): bool`;
- `snapshot(): [T]` — return a shallow copy of the underlying heap array.

`newPriorityQueue<T>(less)` uses the same comparator contract as
`newBinaryHeap<T>`. `enqueue` and `dequeue` are `O(log n)`, `front` is `O(1)`,
and `snapshot` is `O(n)`.

`MedianHeap` maintains the running median of numeric values with two heaps:

- `newMedianHeap(): MedianHeap` — create an empty tracker;
- `add(value: number)` — insert a value;
- `median(): optional<number>` — return `nil` when empty, the middle value for
  odd sizes, or the arithmetic mean of the two middle values for even sizes;
- `size(): number` and `isEmpty(): bool` — inspect the number of values.

The lower max-heap and upper min-heap differ in size by at most one. Insertion
is `O(log n)`, median lookup is `O(1)`, and storage is `O(n)`; values are kept
with their normal numeric semantics and no overflow behavior is promised.

`Option<T>` is an explicit result enum for APIs where callers should distinguish
success from absence with `match`:

- `Some(value: T)` — carries a value;
- `None` — carries no value;
- `some<T>(value: T): Option<T>` and `none<T>(): Option<T>` — construct the two
  variants.

Simple missing-value APIs use the language's `optional<T>` return type and
return `nil`. `Option<T>` is useful when a nullable return would make the
absence case ambiguous or when a caller wants an exhaustive branch. The enum
is a value; it does not copy or mutate a payload supplied to `Some`.

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
- `head(list): optional<T>` and `tail(list): optional<List<T>>` — inspect the first value or the
  remaining list, returning `nil` for an empty list;
- `reverse(list): List<T>` — return a persistent reversed list;
- `toArray(list): [T]` — copy the values into a new array in list order.

`prepend`, `head`, and `tail` are `O(1)`. `reverse` and `toArray` are `O(n)`;
`reverse` allocates a new list while `toArray` allocates one new array. Tails
are shared between list values, so the structure supports persistent snapshots.
The type argument for `emptyList` must be explicit because the empty constructor
has no payload from which to infer `T`.

Additional immutable list algorithms provide:

- `listLength(list): number` and `listGet(list, index): optional<T>` — length and
  zero-based lookup, with `nil` for invalid/non-integral indexes;
- `listAppend(list, value): List<T>` and `listConcat(left, right): List<T>` —
  persistent tail append and concatenation;
- `listTake(list, count): List<T>` and `listDrop(list, count): List<T>` —
  prefix/suffix selection, with non-positive counts producing the natural empty
  or unchanged result;
- `mergeSortedLists(left, right, less): List<T>` — stable merge of two lists
  already ordered by the supplied comparator.
- `listMiddle(list): optional<T>` — return the upper middle value, or `nil` for empty;
- `listIsPalindrome<T: Eq>(list): bool` — compare values from both ends;
- `listRemoveFromEnd(list, count): List<T>` — return a new list without the
  `count`th value from the end, or the original list for an invalid/out-of-range
  count.

These operations preserve the input lists. `listLength`/`listGet` and
`listAppend` are `O(n)` in the traversed list, `listConcat` is `O(n)` in its
left input and shares the right input, `listTake`/`listDrop` are `O(k)`, and
`mergeSortedLists` is `O(n + m)` while sharing only its final untouched suffix.
`listMiddle` uses fast/slow list cursors in `O(n)` time and `O(1)` extra space.
`listIsPalindrome` uses an `O(n)` temporary array, while `listRemoveFromEnd`
uses an `O(n)` temporary array before returning a persistent rebuilt list.

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
- `treeRootToLeafSums(tree: Tree<number>)` — return numeric root-to-leaf sums;
- `serializeNumericTree(tree: Tree<number>): string` — encode a numeric tree in
  pre-order as `#` for an empty tree or `value,left,right` for a node;
- `deserializeNumericTree(text: string): optional<Tree<number>>` — parse that
  format, returning `nil` for malformed input or trailing content;
- `treeLowestCommonAncestor(tree, firstPath, secondPath): optional<T>` — return
  the value at the lowest common ancestor of two node paths.

LCA paths start at the root and use `0` for a left edge and `1` for a right
edge. Both paths must contain integral `0`/`1` entries and identify existing
nodes; invalid paths or an empty tree return `nil`. A path may be empty to name
the root, and if one path is a prefix of the other the prefix node is returned.
Paths identify occurrences rather than values, so duplicate node values remain
unambiguous. Path validation and the common-prefix walk take `O(p + q)` time
and `O(1)` extra space for path lengths `p` and `q`.

Tree constructors reuse their child values without mutating them. Traversals
and statistics are `O(n)`; recursive traversals use `O(h)` call-stack space,
while level order, width, balance checking, and path collection use `O(n)`
auxiliary space in the current implementation. Empty trees have size, height,
leaf count, and maximum width zero; all traversals and path helpers return `[]`.

Numeric serialization is deterministic and uses the language `str` rendering
for node values. The wire format is pre-order: `#` means empty and each node is
`value,left,right`, with no whitespace. Deserialization accepts signed decimal
and scientific numeric text, but only for `Tree<number>`; malformed input,
missing children, or trailing text returns `nil`. The recursive parser and
serializer take `O(n)` time and `O(h)` call-stack space for a tree of height `h`.

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

`AvlTree<T>` is a mutable balanced search tree backed by parallel arrays of
values, child indexes, and heights. Node indexes are private implementation
details and are reused after deletion:

- `newAvlTree<T>(less): AvlTree<T>` — create an empty tree with a strict-order
  comparator;
- `insert(value: T): bool` and `discard(value: T): bool` — add or remove one
  comparator-equivalent value, returning whether the tree changed;
- `has`, `minimum`, `maximum`, `predecessor`, and `successor` — query membership,
  endpoints, or strict neighboring values; missing results return `false` or
  `nil`;
- `size`, `height`, `isEmpty`, and `snapshot` — inspect state or return a fresh
  in-order sorted array.

Comparator-equivalent duplicates are ignored. AVL rotations keep insert and
delete at `O(log n)` time, while queries are `O(log n)` and `snapshot` is `O(n)`.
The arrays are mutable through aliases of the tree value, but `snapshot` copies
the outer result array and does not expose node indexes.

`RedBlackTree<T>` uses the same public search-tree contract with a red-black
color array and private parent/child indexes:

- `newRedBlackTree<T>(less): RedBlackTree<T>` — create an empty tree with a
  strict-order comparator;
- `insert`, `discard`, `has`, `minimum`, `maximum`, `predecessor`, and `successor`
  — update or query comparator-ordered values; duplicate values are ignored;
- `size`, `height`, `isEmpty`, and `snapshot` — inspect state or return a fresh
  in-order sorted array.

Red-black insertion and deletion repair preserve `O(log n)` updates and
queries. `snapshot` is `O(n)`; `height` currently computes the height in `O(n)`.
Node indexes remain private and are reused after deletion, so callers should
use values and snapshots rather than retaining structural references.

`Set<T: Eq>` provides an array-backed set with language equality:

- `add(value: T)` — insert a value if it is not already present;
- `has(value: T): bool` — test membership;
- `discard(value: T): bool` — remove a value and report whether it was present;
- `size(): number` and `isEmpty(): bool`;
- `snapshot(): [T]` — return a shallow copy in insertion order.

`newSet<T: Eq>()` creates an empty set. `add`, `has`, and `discard` use linear
search, so they are `O(n)`; `discard` preserves the order of the remaining
values and is also `O(n)`. `snapshot` is `O(n)` and allocates one outer array.
Membership follows the language's `==` semantics, including its behavior for
reference values.

`OrderedSet<T>` stores distinct values in comparator order:

- `add(value: T): bool` — insert a comparator-distinct value and report whether
  the set changed;
- `has(value: T): bool` and `discard(value: T): bool` — query or remove a value;
- `minimum(): optional<T>` and `maximum(): optional<T>` — return an endpoint or
  `nil` when empty;
- `predecessor(value: T): optional<T>` and `successor(value: T): optional<T>` —
  return strict neighboring values or `nil` when none exists;
- `rangeInclusive(lower: T, upper: T): [T]` — return values in comparator order
  between the inclusive bounds, or `[]` when the bounds are reversed;
- `size`, `isEmpty`, and `snapshot` — inspect state or return a shallow ordered
  copy.

`newOrderedSet<T>(less)` accepts a `fun(T, T): bool` strict-order comparator.
Comparator equivalence means neither value is less than the other, so equivalent
values are stored only once even when the language `==` relation would differ.
`add`, `has`, `discard`, `minimum`, `maximum`, `predecessor`, and `successor`
use a binary search plus array movement where needed: lookup is `O(log n)`,
insertion and deletion are `O(n)`, and `rangeInclusive` is `O(log n + k)` for
`k` returned values. `snapshot` is `O(n)` and allocates a new outer array.

`OrderedMap<K, V>` stores key/value entries in comparator order:

- `newOrderedMap<K, V>(less): OrderedMap<K, V>` — create an empty map with a
  `fun(K, K): bool` strict-order comparator;
- `put(key: K, value: V): bool` — insert a key or update its comparator-equivalent
  entry; return `true` only when a new key is inserted;
- `get(key: K): optional<V>` and `has(key: K): bool` — query a value or key;
- `discard(key: K): bool` — remove a key and report whether it was present;
- `size`, `isEmpty`, and `snapshot` — inspect state or return
  `[OrderedMapEntry<K, V>]` in comparator order.

Comparator equivalence means neither key is less than the other. Equivalent keys
share one entry; updating one changes the value while preserving the first
stored key and its sorted position. `get` uses `optional<V>`, so callers storing
a `nil` value should use `has` to distinguish it from a missing key. Lookup is
`O(log n)`, insertion and deletion are `O(n)` because of array movement, and
`snapshot` is `O(n)` with a fresh outer array and entry values.

`BiMap<K: Eq, V: Eq>` maintains a one-to-one mapping with lookup from either
side:

- `newBiMap<K: Eq, V: Eq>(): BiMap<K, V>` — create an empty bidirectional map;
- `put(key: K, value: V): bool` — insert a new pair, returning `false` when
  either side is already present; rejected pairs leave the map unchanged;
- `get(key: K): optional<V>` and `getKey(value: V): optional<K>` — query either
  direction;
- `hasKey`, `hasValue`, `discardKey`, and `discardValue` — inspect or remove
  either side while preserving the one-to-one invariant;
- `size`, `isEmpty`, and `snapshot` — inspect state or return
  `[BiMapEntry<K, V>]` in insertion order.

This implementation deliberately rejects duplicate keys and duplicate values;
it does not replace an existing association implicitly. Both lookup directions
are linear `O(n)`, removal is `O(n)` because paired arrays are compacted, and
`snapshot` is `O(n)` with a fresh outer array and entry values.

`LruCache<K: Eq, V>` is an array-backed least-recently-used cache:

- `newLruCache<K: Eq, V>(capacity): LruCache<K, V>` — create a cache with the
  floored non-negative capacity;
- `get(key: K): optional<V>` — return a value and mark its entry as most recent,
  or return `nil` when absent;
- `put(key: K, value: V): bool` — insert or update a value and mark it most
  recent; return `false` when the capacity is zero;
- `has(key: K): bool` and `discard(key: K): bool` — query or remove a key;
- `capacity`, `size`, `isEmpty`, and `snapshot` — inspect state or return
  `[LruCacheEntry<K, V>]` entries ordered from least recent to most recent.

When an insertion would exceed capacity, the least-recent entry is discarded.
Updating an existing key also refreshes its recency. `get` uses `optional<V>`, so
callers storing a `nil` value should use `has` to distinguish it from a missing
key. Key lookup and all recency moves are `O(n)` in this simple implementation;
`snapshot` is `O(n)` and allocates a new outer array and entry values.

`LfuCache<K: Eq, V>` is an array-backed least-frequently-used cache:

- `newLfuCache<K: Eq, V>(capacity): LfuCache<K, V>` — create a cache with the
  floored non-negative capacity;
- `get(key: K): optional<V>` — return a value, increment its frequency, and
  refresh its recency, or return `nil` when absent;
- `put(key: K, value: V): bool` — insert or update a value, counting the
  operation as a use; return `false` when the capacity is zero;
- `frequencyOf(key: K): optional<number>` and `has(key: K): bool` — inspect frequency
  or presence without changing it;
- `discard`, `capacity`, `size`, `isEmpty`, and `snapshot` — remove or inspect
  entries; snapshots contain `[LfuCacheEntry<K, V>]` with each frequency.

When full, the entry with the smallest frequency is evicted; ties evict the
least recently used entry. New entries start at frequency `1`. Lookup, eviction,
and removal are `O(n)` in this array implementation; `snapshot` is `O(n)` and
returns entries in current storage order.

`MultiSet<T: Eq>` stores one entry per distinct value and its occurrence count:

- `add(value: T)` — add one occurrence;
- `countOf(value: T): number` and `has(value: T): bool` — query occurrences;
- `takeOne(value: T): bool` — remove one occurrence, returning whether it was
  present;
- `entries(): [MultiSetEntry<T>]` — return `{ value, count }` snapshots in first
  insertion order.

`newMultiSet<T: Eq>()` creates an empty multiset. Lookup and `add` are `O(n)` over
the number of distinct values; `takeOne` is `O(n)` when the last occurrence is
removed because it preserves entry order. `entries` is `O(n)` and allocates a
new outer array plus one entry value per distinct element.

`MultiMap<K: Eq, V: Eq>` stores each key once and keeps its values in insertion order:

- `add(key: K, value: V)` — append one value to a key;
- `getAll(key: K): [V]` — return a fresh shallow array, or `[]` for an unknown
  key;
- `discard(key: K, value: V): bool` — remove the first matching pair and report
  whether it was present; removing the last value removes the key entry.

`newMultiMap<K: Eq, V: Eq>()` creates an empty mapping. Key and value lookup use
language equality. `add`, `getAll`, and `discard` are linear in the number of
keys or values for the selected key; key/value arrays are maintained without a
hash table, and key order is preserved after removals.

`HashSet<T: Eq + Hash>` and `HashMap<K: Eq + Hash, V>` use array-backed bucket
tables and the deterministic `hash(value)` operation:

- `newHashSet<T: Eq + Hash>()` and `newHashMap<K: Eq + Hash, V>()` create empty
  containers;
- `add`/`put` return `true` only when a new entry is inserted; `put` updates an
  existing value without moving its key;
- `has`, `discard`, `size`, `isEmpty`, `clear`, and `snapshot` provide lookup,
  removal, inspection, clearing, and insertion-order shallow snapshots;
- `HashMapEntry<K, V>` exposes `key` and `value` fields for snapshot results;
- `get` returns `optional<V>` and therefore returns `nil` for a missing key;
  use `has` when a stored value may itself be `nil`.

Arrays, maps, functions, and named structs are identity keys. Mutating one
through an alias does not invalidate its membership, but the mutation is
visible through the stored key and snapshots. A separately constructed value
with equal-looking contents is a different identity key. Non-reflexive keys
such as NaN are rejected by these APIs. The tables resize above a 75% load
factor, resolve collisions with `Eq`, preserve insertion order, and do not
shrink after removal. This generic API does not widen the built-in map's
primitive-only key restriction.

`DisjointSet` represents integer vertices `[0, size)` with parent and component
size arrays. `representative` performs path compression, `union` uses union by size and
returns whether two components were joined, and `connected`, `componentCount`,
and `size` provide queries. Construction with a non-positive count creates an
empty set; vertex arguments must be in range. `representative` and `union` are amortized
near-constant time (`O(alpha(n))`), while construction is `O(n)`.

`Graph` is an integer-vertex adjacency-list graph. `newGraph(vertices, directed)`
creates vertices `[0, vertices)`; `addEdge` rejects invalid or duplicate edges,
and undirected edges are stored in both adjacency lists while counting once.
`removeEdge` removes one existing edge, updates both adjacency lists for an
undirected graph, and returns whether the edge was present. Self-loops are
stored and removed once; directed removal does not affect the reverse edge.
`neighbors` returns a fresh array, and invalid or non-integral vertex queries
return `false` or `[]`. Adjacency operations are linear in the degree because
this version does not use a hash set.

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

`stronglyConnectedComponentsTarjan(graph)` provides the recursive low-link
variant for directed graphs. It returns components when each root completes,
with members in stack-pop order, returns `[]` for undirected graphs, and uses
`O(V + E)` time and `O(V)` auxiliary state plus recursion depth.

`criticalPath(graph, start)` computes longest non-negative-weight paths from a
start vertex in a directed acyclic `WeightedGraph`. A successful
`CriticalPathResult` contains nullable `distances` (`nil` means unreachable),
`parents`, and the stable Kahn topological `order`; equal-length alternatives
keep the first predecessor encountered. It returns `UndirectedGraph` for an
undirected input, `InvalidStart` for an invalid or non-integral start, and
`Cycle` for a directed cycle. The algorithm runs in `O(V + E)` time and uses
`O(V)` auxiliary space.

`WeightedGraph` stores non-negative numeric edge weights. Its Dijkstra helpers
`shortestWeightedDistances` and `shortestWeightedPath` return `-1`/`[]` for
unreachable results and reject negative weights at insertion. A min-heap
implementation with stale-entry checks runs in `O((V + E) log V)` time and
`O(V + E)` auxiliary space; equal distances are settled by smaller vertex ID.
`removeEdge` has the same symmetry and missing-edge behavior as `Graph`; removing
an edge discards its stored weight and updates the edge count.

`SignedWeightedGraph` is the separate signed-weight graph type. Its
`addEdge` accepts negative weights while retaining duplicate/vertex validation;
`removeEdge` removes directed or undirected edges without changing their signed
weight validation contract. The existing non-negative `WeightedGraph` contract
is unchanged.
`bellmanFord(graph, start)` returns
`Result<BellmanFordResult, BellmanFordError>`. A successful result contains
nullable `distances` (`nil` means unreachable) and `parents`; invalid starts
return `InvalidStart`, and a negative cycle reachable from `start` returns
`NegativeCycle`. The algorithm runs in `O(V * E)` time and `O(V)` auxiliary
space.

`minimumSpanningForest(graph)` applies Prim's algorithm to an undirected
`WeightedGraph`, preserves the vertex count, and returns a new undirected graph.
Disconnected inputs produce one tree per component; directed inputs produce an
empty forest with the same vertex count. The min-heap implementation runs in
`O((V + E) log V)` time and `O(V + E)` auxiliary space.

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
- `linearSearch<T: Eq>(values, target): number` — return the first matching index,
  or `-1` when absent;
- `countValue<T: Eq>(values, target): number` — count matching values.

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

`frequencyEntries<T: Eq>(values)` returns `MultiSetEntry<T>` values in first-seen
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

`shellSort<T>(values, less)` returns a sorted shallow copy and leaves the input
unchanged. It uses a halving gap sequence and gapped insertion sort; it is not
stable, has `O(n^2)` worst-case time with the current gap sequence, and uses
`O(n)` result space. Empty arrays need an explicit type argument when `T` cannot
be inferred.

`mergeSort<T>(values, less)` returns a stable, sorted shallow copy and leaves
the input unchanged. It uses bottom-up merge sort in `O(n log n)` time and
`O(n)` temporary outer-array space. Equal comparator values keep their input
order.

`countingSort(values: [number])` returns a sorted copy of an integral numeric
array, including negative values, without modifying the input. Empty input and
non-integral input return `[]`. Dense bounded ranges use offset counting in
`O(n + R)` time and `O(R)` auxiliary space, where `R` is `max - min + 1`; a
sparse range falls back to numeric merge sort instead of allocating a count
array proportional to a large gap. The fallback uses `O(n log n)` time and
`O(n)` temporary space.

`topKSmallest<T>(values, count, less)` and `topKLargest<T>(values, count, less)`
return a new array containing at most `count` values in comparator order.
The former returns the first values and the latter the last values of the
ordered input; both clamp an oversized count to the input length and return
`[]` for empty, non-positive, or non-integral counts. `kthSmallest<T>` and
`kthLargest<T>` use a zero-based rank and return `nil` for an empty, negative,
non-integral, or out-of-range rank. The selection functions copy the input,
use three-way quickselect to isolate the requested prefix/suffix, and merge-sort
only that selected segment before returning it. They take average `O(n + k log k)`
time for `k = min(count, n)`, `O(n^2)` worst-case time with the current middle
pivot, and `O(n + k)` outer-array space. The `kth` functions take average `O(n)`
and `O(n^2)` worst-case time with `O(n)` work space. Inputs remain unchanged;
comparator-equal values are ordered correctly but their relative order is not a
stability guarantee.

`countInversions(values)` counts pairs `i < j` for which
`values[i] > values[j]`. It returns `0` for empty, one-element, or already
non-decreasing inputs, leaves the input unchanged, and uses the same bottom-up
merge structure in `O(n log n)` time with `O(n)` temporary outer-array space.

`stablePartition<T>(values, predicate)` returns a new array with every value
accepted by `predicate: fun(T): bool` before every rejected value. It preserves
the original order within both groups, invokes the predicate once per input
element, leaves the input unchanged, and uses `O(n)` time and `O(n)` additional
outer-array space. Empty input and an all-rejected input retain their natural
empty/order behavior.

`quickSort<T>(values, less)` returns a sorted shallow copy, while
`quickSortInPlace<T>(values, less)` sorts the supplied array. Quick sort is not
stable; it uses a middle-element pivot and three-way partitioning, then
recurses into the smaller side before iterating over the larger side. This gives
average `O(n log n)` time, `O(n^2)` worst-case time for a bad pivot sequence,
and `O(log n)` recursion stack depth even when the partitions are badly
unbalanced. All-equal ranges are handled by one linear partition. The copying
version uses `O(n)` outer-array space in addition to the bounded stack.

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
- `nextSmallerValues(values: [number]): [number]` — return the first strictly
  smaller value to the right, or `-1` when none exists.

`chunkArray` and `slidingWindows` return `[]` for non-positive sizes; sliding
windows also return `[]` when the width exceeds the input length. Chunks and
windows are fresh outer arrays with shallowly shared elements. Chunking and
window generation are `O(n)` in the input plus output size; all prefix and
difference helpers are `O(n)` and allocate one array. `nextGreaterValues` and
`nextSmallerValues` are `O(n)` time and `O(n)` stack/output space; equal values
do not count as greater or smaller.

`isBalancedBrackets(text)` checks `()[]{}` nesting with an array-backed stack;
non-bracket characters are ignored, and an empty string is balanced. It runs in
`O(n)` time and `O(n)` auxiliary space. `largestHistogramArea(heights)` uses a
monotonic index stack to return the largest rectangle area, with `0` for an
empty array and `nil` for negative or non-integral heights. It runs in `O(n)`
time and `O(n)` stack space.

`windowSums(values, width)` returns the numeric sum of every fixed-width window
using a rolling sum. `maxWindowSum(values, width)` returns the largest such sum,
or `nil` when the width is non-positive, exceeds the input length, or the input
is empty. `maxWindowValues(values, width)` returns the maximum value from every
window in left-to-right order; non-positive, non-integral, oversized, or empty
inputs return `[]`. `minWindowValues(values, width)` returns the corresponding
minimum from every window with the same invalid-width behavior. All four
functions leave the input unchanged. The sum helpers run in `O(n)`, while both
extrema functions use a monotonic index queue in `O(n)` time and
`O(n)` auxiliary/output space.

`maxSubarraySum(values)` returns the largest sum of a non-empty contiguous
subarray, or `nil` for an empty input. It preserves the input and handles an
all-negative array by returning its least-negative element. The scan is `O(n)`
time and `O(1)` extra space.

`longestUniqueSubarrayLength<T: Eq>(values)` returns the maximum length of a
contiguous range without repeated values. It accepts any equality-capable
element type, leaves the input unchanged, and returns `0` for an empty array.
The array-backed window scan takes `O(n^2)` worst-case time and `O(1)` extra
space because the library does not require a generic hash table.

The one-dimensional DP helpers are:

- `climbStairs(steps): optional<number>` — count one-step/two-step climbs; `0` has one
  empty climb and invalid/non-integral inputs return `nil`;
- `maxNonAdjacentSum(values): number` — maximize a sum without adjacent picks;
  an empty or all-negative input returns `0` by allowing the empty selection;
- `minCoinCount(amount, coins): optional<number>` — return the minimum number of
  positive-integer coins or `nil` when the target is invalid/unreachable;
  non-positive or non-integral coin entries are ignored.

`climbStairs` is `O(steps)` time and `O(1)` space. `maxNonAdjacentSum` is
`O(n)` time and `O(1)` space. `minCoinCount` is `O(amount * coinCount)` time
and `O(amount)` space; these numeric helpers do not define overflow behavior.

The two-dimensional/string DP helpers are:

- `uniqueGridPaths(rows, columns): optional<number>` — count right/down paths in a
  positive rectangular grid; zero dimensions return `0` and invalid dimensions
  return `nil`;
- `minGridPathSum(grid): optional<number>` — find the minimum top-left to bottom-right
  sum using right/down moves; empty or non-rectangular grids return `nil`;
- `uniqueGridPathsWithObstacles(grid: [[bool]]): optional<number>` — count right/down
  paths where `true` cells are blocked. Empty grids and zero-width grids return
  `0`, non-rectangular grids return `nil`, and a blocked start or end returns
  `0`;
- `editDistance(left, right): number` — compute insertion/deletion/substitution
  distance over Unicode scalar-value characters.

Grid DP uses `O(rows * columns)` time and `O(columns)` space for the two numeric
helpers and the obstacle-path helper. `editDistance` uses `O(leftLength * rightLength)` time and
`O(rightLength)` space; numeric overflow remains outside the library contract.

`longestIncreasingSubsequenceLength(values)` computes the strict numeric LIS
length with `O(n log n)` time and `O(n)` space using a tails array and lower-bound
replacement; equal values do not extend a subsequence. `longestIncreasingSubsequence(values)` returns one strict numeric
LIS as a new array, returns `[]` for empty input, and leaves the input unchanged.
It uses tails, tail indices, and predecessor pointers with `O(n log n)` time and
`O(n)` auxiliary space. Lower-bound replacement makes the selected LIS
deterministic for a given input, but no particular alternate LIS ordering is
promised. `longestCommonSubsequenceLength(left, right)` computes the LCS
length over Unicode scalar values with `O(leftLength * rightLength)` time and
`O(rightLength)` space.
`longestCommonSubsequence(left, right)` reconstructs one longest common
subsequence as a new string. It uses Unicode scalar-value positions and leaves
both inputs unchanged. Empty inputs return `""`; when several choices have the
same length, backtracking prefers the upper DP cell so the result is stable.
The reconstruction table uses `O(leftLength * rightLength)` time and space.
`longestCommonSubstringLength(left, right)` computes the longest contiguous
common Unicode scalar-value run, returning its length. Empty inputs return `0`;
the rolling-row DP uses `O(leftLength * rightLength)` time and
`O(rightLength)` space.
`longestCommonSubstring(left, right)` reconstructs one such run as a new string,
returns `""` when no common run exists, leaves both inputs unchanged, and uses
the same `O(leftLength * rightLength)` time and `O(rightLength)` space. Equal
length candidates keep the first match found by the left-to-right DP scan.

`matrixChainCost(dimensions)` computes the minimum scalar multiplication cost
for matrices whose adjacent dimensions are given by the array. Positive integer
dimensions are required; an empty or one-element dimensions array returns `0`,
and invalid dimensions return `nil`. It uses interval DP in `O(n^3)` time and
`O(n^2)` space, where `n` is the number of matrices.

`matrixPower2x2(matrix, exponent)` raises a 2×2 numeric matrix to a
non-negative integer power, returning `nil` for another shape or invalid
exponent. Exponent zero returns the 2×2 identity matrix; exponentiation by
squaring takes `O(log exponent)` matrix multiplications.

`mergeStonesCost(values)` computes the minimum cost to repeatedly merge
adjacent non-negative integer piles, where each merge costs the sum of the two
resulting adjacent ranges. Empty or single-pile inputs cost `0`; invalid values
return `nil`. The interval DP uses `O(n^3)` time and `O(n^2)` space.

The current backtracking and knapsack helpers are:

- `knapsack01(weights, values, capacity): optional<number>` — maximize the value of
  selecting each item at most once. It returns `nil` when the arrays differ in
  length, the capacity is negative or non-integral, or a weight is negative or
  non-integral; zero-weight items are supported. The result is `0` for a valid
  zero capacity and uses `O(capacity)` space.
- `completeKnapsack(weights, values, capacity): optional<number>` — maximize the value
  when each item may be selected repeatedly. It returns `nil` for mismatched
  arrays, invalid capacity, or a non-positive/non-integral weight; a valid
  zero capacity returns `0`. The ascending-capacity DP uses `O(itemCount *
  capacity)` time and `O(capacity)` space.
- `boundedKnapsack(weights, values, counts, capacity): optional<number>` — maximize the
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

`rotatedBinarySearch<T>(values, target, less)` searches a non-decreasing array
that was rotated at one pivot and returns an index whose value is comparator-
equivalent to `target`, or `-1` when no such value exists. It does not modify
the input and returns `-1` for an empty array. With distinct values it runs in
`O(log n)` time; duplicate values may make the sorted side ambiguous, so the
duplicate-safe implementation can degrade to `O(n)`. The comparator defines
equivalence through `!less(left, right) && !less(right, left)`.

`peakIndex(values)` returns the index of a weak local peak using binary search;
the empty input returns `nil`, and equal neighbors may make either edge of a
plateau a valid result. `isMountainArray(values)` recognizes a strict mountain
with at least one increasing and one decreasing step. `mountainPeakIndex(values)`
returns its peak index or `nil` for an invalid mountain. Peak search is
`O(log n)`; mountain validation is `O(n)` followed by an `O(log n)` peak search.
Neither function modifies the input.

`minimumLargestPartitionSum(values, parts)` uses answer-space binary search to
split a non-negative integer array into at most `parts` non-empty contiguous
partitions while minimizing the largest partition sum. It returns `0` for a
valid empty input and `nil` for a non-positive/non-integral partition count or
invalid values. The result is computed in `O(n log S)` time, where `S` is the
sum of the values, and `O(1)` auxiliary space; `parts` may exceed the number of
values.

`compareStrings(left, right)` provides a library-level comparator for printable
ASCII strings: it returns `-1`, `0`, or `1` for less/equal/greater, and returns
`nil` if either input contains a non-ASCII scalar. `stringLess` adapts that
result to the `fun(string, string): bool` comparator expected by the search
helpers; unsupported-character pairs are treated as unordered (`false`). The
language now also provides scalar-value lexicographic ordering for strings, so
these helpers remain the explicit three-way/comparator API and preserve the
library's existing behavior.

`findSubstring(text, pattern)` returns the first Unicode scalar-value offset,
or `-1` when absent; an empty pattern matches at `0`. `prefixFunction(pattern)`
returns the KMP failure table, `zFunction(text)` returns the Z table, and
`kmpSearch` uses the prefix table for `O(n + m)` matching. Both tables use
Unicode scalar-value offsets and return `[]` for an empty string.
`isPalindrome` compares scalar values from both ends and treats an empty string
as a palindrome. These functions do not normalize combining marks.

`characterFrequency(text)` returns `MultiSetEntry<string>` values in first-seen
character order, and `areAnagrams(left, right)` compares scalar-value
multiplicities without normalizing text. Both use primitive string-keyed maps
and take average `O(n)` time with `O(k)` auxiliary space for `k` distinct
characters.

`minimumWindowSubstring(text, pattern)` returns the shortest contiguous Unicode
scalar-value span that contains every pattern character with the required
multiplicity. It returns `""` for an empty pattern, an empty text, or no
solution; equal-length candidates keep the leftmost window. Its primitive
string-keyed sliding-window maps give average `O(n)` time and `O(k)` auxiliary
space, where `k` is the number of distinct pattern characters.

`canSegmentString(text, dictionary)` returns whether the text can be formed by
concatenating whole dictionary words. Empty dictionary entries are ignored,
empty text is segmentable, and a non-empty text with no valid segmentation
returns `false`. Matching uses a Unicode scalar-value Trie and dynamic
programming from reachable text positions. With `D` total dictionary
characters, `w` maximum word length, and `e` maximum outgoing-edge scan, it
takes `O((D + n * w) * e)` time and `O(D + n)` space; empty dictionary words
are not inserted.

`longestUniqueSubstringLength(text)` returns the length of the longest substring
with no repeated Unicode scalar values. It uses a string-keyed last-seen map and
a moving window for average `O(n)` time and `O(n)` space; an empty string
returns `0`. This string-specific map use does not change the library's
generic hash-container boundary.
`longestPalindromicSubstring(text)` uses Manacher radii over Unicode scalar
values and returns the leftmost longest palindrome, or `""` for empty input.
It takes `O(n)` time and `O(n)` auxiliary space in addition to the returned
substring.

`Trie` is an array-node prefix tree. `insert(word)` returns `true` only when a
new word is added; `discard(word)` unmarks and removes a word, returning
`false` when it was absent. Deleting a prefix word preserves its longer words,
while deleting the last word on a branch prunes that branch. `has`,
`startsWith`, and `wordsWithPrefix` handle empty and Unicode scalar-value
strings. Prefix results use child insertion order rather than lexical sorting,
and the current linear edge scans take `O(k)` per node lookup where `k` is that
node's outgoing edge count. `discard` uses `O(n * k)` time in the word length
and `O(n)` temporary path space.

`FenwickTree` stores numeric values in an array-backed binary indexed tree:

- `newFenwickTree(values: [number]): FenwickTree` — build from a copied initial
  value array;
- `size(): number` and `snapshot(): [number]` — inspect the logical values;
- `add(index: number, delta: number): bool` — apply a point update using a
  zero-based index;
- `valueAt(index: number): optional<number>` — read one value, or `nil` for an invalid
  index;
- `prefixSum(endExclusive: number): optional<number>` — sum `[0, endExclusive)`;
- `rangeSum(start: number, endExclusive: number): optional<number>` — sum the half-open
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
- `rangeSum(start, endExclusive): optional<number>` — query the half-open range sum;
- `rangeMinimum(start, endExclusive): optional<number>` — query its minimum, or `nil`
  for an empty/invalid range;
- `size`, `valueAt`, and `snapshot` — inspect the logical values.

The tree uses inactive padded leaves, so non-power-of-two input lengths and
negative values need no sentinel convention. Construction is `O(n)`, point
updates and aggregate queries are `O(log n)`, and `snapshot` is `O(n)`. Invalid
indexes/ranges return `false` or `nil` without changing the structure; an
empty valid sum range returns `0`.

`SparseTable` is an immutable numeric range structure with fixed minimum and
maximum aggregates:

- `newSparseTable(values: [number]): SparseTable` — copy values and build the
  static tables;
- `rangeMinimum(start, endExclusive): optional<number>` and
  `rangeMaximum(start, endExclusive): optional<number>` — query half-open
  ranges in `O(1)` time;
- `size`, `valueAt`, and `snapshot` — inspect the copied input values.

Ranges must be integral and satisfy `0 <= start < endExclusive <= size`; invalid
or empty ranges return `nil`. The structure has no update operation. Construction
uses `O(n log n)` time and space, queries use `O(1)` time, and `snapshot` is
`O(n)` with a fresh outer array.

`Matrix` is an immutable rectangular numeric matrix:

- `newMatrix(rows: [[number]]): optional<Matrix>` — validate a non-empty
  rectangular input and make a deep copy; empty or ragged inputs return `nil`;
- `rowCount`, `columnCount`, and `valueAt(row, column): optional<number>` —
  inspect dimensions and return `nil` for non-integral or out-of-range indexes;
- `snapshot` — return a fresh deep copy of all rows;
- `transpose(): Matrix` — create the transposed matrix;
- `multiply(other: Matrix): optional<Matrix>` — multiply compatible dimensions,
  or return `nil` when the left column count differs from the right row count.

The constructor and snapshot are `O(rows * columns)`, transpose is
`O(rows * columns)`, and multiplication is `O(rows * shared * columns)`. Matrix
values use the language's numeric arithmetic and do not define overflow behavior.

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
Huffman construction core; it does not assign symbol codes. The min-heap
implementation takes `O(n log n)` time and `O(n)` temporary space.

For non-decreasing numeric arrays, `mergeSortedNumbers(left, right)` returns a
linear-time merged copy and `twoSumSorted(values, target)` uses two pointers to
return the first matching `[leftIndex, rightIndex]`, or `[]` when no pair exists.
Both are `O(n)` time with `O(n)` output space for the returned arrays and do not
modify their inputs. `twoSumSorted` requires its input to already be sorted.

`mergeSortedArrays<T>(sequences, less)` merges any number of already sorted
arrays into one new array. It accepts a strict `fun(T, T): bool` comparator,
does not modify the outer or inner input arrays, and returns `[]` for an empty
collection of arrays or when all arrays are empty. Comparator-equivalent values
are selected by increasing source-array index, so the merge is stable across
the input arrays. A min-heap of the `k` current heads takes `O(n log k)` time and
`O(k)` heap space, in addition to the `O(n)` output.

`threeSumZero(values)` returns unique nondecreasing triples whose values sum to
zero. It sorts a shallow copy, uses two pointers, and keeps triples in the
sorted outer-index order; the input is unchanged. The algorithm takes
`O(n^2)` time and `O(n)` auxiliary/output space. Empty and shorter-than-three
inputs return `[]`.

`threeSumClosest(values, target)` returns the sum of three distinct input
positions whose value is closest to `target`, or `nil` when fewer than three
values are supplied. It sorts a shallow copy and uses two pointers in
`O(n^2)` time with `O(n)` auxiliary space, leaving the input unchanged. An
exact target returns immediately; equal absolute distances choose the smaller
sum for deterministic results.

The array set helpers are non-mutating and remove duplicate output values while
preserving the first occurrence order:

- `uniqueValues<T: Eq>(values)` — stable de-duplication;
- `intersectionValues<T: Eq>(left, right)` — values present in both arrays;
- `unionValues<T: Eq>(left, right)` — all values from left, followed by new values
  from right;
- `differenceValues<T: Eq>(left, right)` — values from left that are absent in right.

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
`factorial` takes `O(value)` steps and `fibonacci` uses fast doubling in
`O(log index)` steps; none define numeric overflow behavior.

## Current language support

Methods declared for a known struct receiver take precedence over builtin
member-call sugar. Array, map, string, and range receivers continue to use the
builtin forms, so the stack can expose `push` and `pop` without changing array
behavior. The stack and queue backing fields are private to this module; callers
should construct them through `newStack` and `newQueue` and use their public
methods.

The language provides compile-time `Eq`, `Ord`, and `Hash` capability bounds,
deterministic `hash(value)`, builtin string ordering, and statically dispatched
ordering operators for named structs. A named struct with all four valid
ordering operators (`<`, `<=`, `>`, `>=`) satisfies generic `T: Ord` (and thus
`T: Eq`); partial or comparator-specific implementations remain explicit
`less` callback inputs for library algorithms. Hash containers require
`Eq + Hash`; mutable reference keys use stable identity semantics and are
retained as shallow aliases. User-defined `Eq`/`Hash` witnesses remain outside
the current library contract.
Nullable annotations in this library use the canonical `optional<T>` spelling;
the compiler retains the postfix nullable spelling only for migration
compatibility.

## Tests

Library fixtures live under `library/tests/` and are intentionally not
discovered by the compiler project's root test suites. Each fixture keeps only
`input.cd` and `run.out`; the runner uses a temporary bytecode artifact to
execute the Rust VM, but does not compare AST or bytecode text:

```sh
python3 library/tests/run_tests.py ./build/compiler_design vm-rs
```

Use `--case data_structures_binary_heap`, `--case data_structures_option`,
`--case data_structures_result`, `--case data_structures_list`,
`--case data_structures_list_algorithms`,
`--case data_structures_hash_collections`,
`--case data_structures_tree`, `--case data_structures_bst`,
`--case data_structures_ring_buffer`,
`--case data_structures_set`, `--case data_structures_multiset`,
`--case data_structures_multimap`, `--case data_structures_disjoint_set`,
`--case data_structures_fenwick`,
`--case data_structures_segment_tree`,
`--case data_structures_graph`, `--case data_structures_weighted_graph`,
`--case data_structures_graph_remove_edge`,
`--case data_structures_tree_serialization`,
`--case algorithms_graph_topological`,
`--case algorithms_graph_traversal`,
`--case algorithms_graph_paths`,
`--case algorithms_graph_bellman_ford`,
`--case algorithms_graph_kruskal`,
`--case algorithms_graph_tarjan`,
`--case algorithms_string_matching`,
`--case algorithms_string_trie`, `--case data_structures_trie_remove`,
`--case algorithms_string_sequences`,
`--case algorithms_string_window`,
`--case algorithms_string_dictionary`,
`--case algorithms_stack`,
`--case algorithms_graph_max_flow`, `--case algorithms_graph_min_cut`,
`--case array_algorithms_basic`,
`--case array_algorithms_teaching_sort`,
`--case numeric_algorithms_gcd`,
`--case numeric_algorithms_extended_gcd`,
`--case numeric_algorithms_primes`,
`--case numeric_algorithms_sequences`,
`--case numeric_algorithms_answer_space`,
`--case algorithms_dp_1d`,
`--case algorithms_dp_grid`,
`--case algorithms_dp_sequences`, `--case algorithms_dp_common_substring`,
`--case array_algorithms_sort`, `--case array_algorithms_windows`,
`--case array_algorithms_prefix_monotonic`,
`--case array_algorithms_intervals`, `--case array_algorithms_interval_intersection`,
`--case array_algorithms_binary_search`, `--case array_algorithms_rotated_search`,
`--case array_algorithms_merge_sort`, `--case array_algorithms_inversions`,
`--case array_algorithms_merge_sorted`, `--case array_algorithms_partition`,
`--case array_algorithms_window_max`, `--case array_algorithms_window_min`,
`--case array_algorithms_next_smaller`, `--case array_algorithms_rotation`,
`--case array_algorithms_frequency`,
`--case array_algorithms_quick_sort`, `--case array_algorithms_heap_sort`,
`--case array_algorithms_shell_sort`,
`--case array_algorithms_search_peaks`,
`--case array_algorithms_three_sum`, `--case array_algorithms_three_sum_closest`,
`--case array_algorithms_two_pointer`,
`--case array_algorithms_sets`, `--case array_algorithms_unique_subarray`, or
`--case array_algorithms_window_stats` for
focused coverage. Every selected fixture contributes one result-validation
check against its `run.out` file.
