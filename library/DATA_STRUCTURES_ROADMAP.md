# 数据结构与算法库总纲

> 状态：规划稿（2026-07-31）
>
> 本文规划的是使用 Compiler Design 公共语言编写的用户库。大多数数组、枚举和
> 索引结构不要求先修改编译器、字节码格式或 Rust VM；M8 的可变节点结构则依赖
> 已确定的递归 nominal 句柄契约。实现时以项目的
> [`README.md`](../README.md)、[`USER_MANUAL.md`](../USER_MANUAL.md)、
> [`docs/language-grammar.ebnf`](../docs/language-grammar.ebnf) 和本目录现有
> [`library/README.md`](README.md) 为行为依据。

## 1. 目标和边界

这个库的目标是提供一套可直接导入的、泛型优先的数据结构和算法实现，重点
覆盖日常业务代码、刷题、编译器实验和教学示例。第一阶段只使用已经公开且
有文档说明的语言能力：

- 数组 `[T]`、map、range、字符串和数值运算；
- 泛型函数、泛型结构体、泛型枚举、可空类型和模式匹配；
- 闭包、函数值和数组高阶函数；
- 源文件导入、命名空间别名和显式导出。

不在第一阶段自行发明新的语言语义。每个结构都应同时说明：

1. 所有权/别名行为，是原地修改还是返回浅拷贝；
2. 空集合、越界、查找失败等边界行为；
3. 主要操作的时间复杂度和额外空间复杂度；
4. 泛型参数、比较器或谓词的要求；
5. C++ 编译器和 Rust VM 的运行结果是否一致。

当前库已有：

- `Stack<T>`：数组后端，使用 `push`、`pop`、`top` 等方法；
- `Queue<T>`：数组后端，带惰性头指针和周期性压缩；
- `Deque<T>`：双数组栈后端，提供两端插入、删除、查看和快照。
- `RingBuffer<T>`：固定容量，满时拒绝写入，读写使用 `Option<T>`/`bool` 明确表达状态。
- `BinaryHeap<T>`：数组后端，通过 `less` 回调实现最小堆或最大堆。
- `PriorityQueue<T>`：`BinaryHeap<T>` 的队列命名包装。
- `Option<T>`：用于显式区分携带值和缺失值的泛型枚举。
- `Result<T, E>`：用于携带成功值或错误值的泛型枚举。
- `List<T>`：递归泛型枚举表示的不可变持久化链表。
- `LinkedNode<T>`/`LinkedList<T>`：稳定强节点句柄的可变单向链表，支持别名可见的
  节点/链接修改、前后插入、前端删除和浅快照。
- `Tree<T>`：递归泛型枚举表示的不可变二叉树，提供遍历和结构统计。
- 数值树编解码：`serializeNumericTree`/`deserializeNumericTree` 使用固定前序文本格式。
- 路径型 LCA：`treeLowestCommonAncestor` 使用 `0/1` 根路径定位重复值节点。
- BST 辅助：基于 `Tree<T>` 和 `less` 的查找、插入、删除、最值及前驱/后继。
- `AvlTree<T>`：数组节点和整数索引的可变平衡搜索树，节点索引不暴露给调用者。
- `RedBlackTree<T>`：数组节点和颜色维护的可变平衡搜索树，节点索引不暴露给调用者。
- `Set<T: Eq>`：数组线性查找的泛型集合，按插入顺序提供快照。
- `MultiSet<T: Eq>`：数组条目和计数构成的泛型多重集合。
- `MultiMap<K: Eq,V: Eq>`：数组条目表示一个键对应多个值的泛型映射。
- `BiMap<K: Eq,V: Eq>`：数组条目表示双方都唯一的双向映射。
- 数组基础算法：`reverseArray`、`rotateArray`、`linearSearch`、`countValue`、`isSorted`。
- 频率统计：`frequencyEntries`、`mostFrequent`。
- 数组排序：稳定插入排序 `sortArray`、`sortArrayInPlace`、希尔排序 `shellSort`、
  选择排序、冒泡排序、归并排序 `mergeSort`、数值计数排序 `countingSort`、
  快速排序 `quickSort`/`quickSortInPlace` 和堆排序
  `heapSort`/`heapSortInPlace`。
- 选择算法：`topKSmallest`、`topKLargest`、`kthSmallest`、`kthLargest`（三向
  quickselect 定位后只对选中段归并排序；`topK` 平均 `O(n + k log k)`、`kth`
  平均 `O(n)`，当前中点 pivot 的最坏复杂度为 `O(n²)`，不承诺相等元素稳定顺序）。
- 数组窗口算法：`chunkArray`、`slidingWindows`、`prefixSums`。
- 数组前缀/差分与单调栈：`differenceArray`、`prefixMinimums`、`prefixMaximums`、
  `nextGreaterValues`、`nextSmallerValues`。
- 区间算法：`Interval`、`mergeIntervals` 和 `intersectIntervals`。
- 贪心区间调度：`selectNonOverlappingIntervals`。
- 区间资源分配：`minimumIntervalRooms`。
- 跳跃游戏贪心：`canReachEnd`、`minimumJumps`。
- Huffman 贪心核心：`huffmanMergeCost`（最小堆实现，`O(n log n)`）。
- 双指针算法：`mergeSortedNumbers`、`twoSumSorted`、`threeSumZero`、`threeSumClosest`。
- 多路有序数组合并：`mergeSortedArrays`。
- 逆序对统计：`countInversions`。
- 稳定分区：`stablePartition`。
- 数组集合算法：`uniqueValues`、`intersectionValues`、`unionValues`、`differenceValues`。
- 固定窗口统计：`windowSums`、`maxWindowSum`、`maxWindowValues`、`minWindowValues`。
- 子数组统计：`maxSubarraySum`。
- 基础数值算法：`gcd`、`lcm`、`extendedGcd`、`isPrime`、`sievePrimes`、`fastPower`、
  `factorial`、`fibonacci`（整数输入契约）。
- 矩阵快速幂：`matrixPower2x2`。
- 数论辅助：`primeFactors`、`divisors`。
- 组合数辅助：`binomialCoefficient`、`pascalTriangle`。
- 数组数值辅助：`gcdArray`、`prefixProducts`。
- 一维 DP：`climbStairs`、`maxNonAdjacentSum`、`minCoinCount`。
- 网格/字符串 DP：`uniqueGridPaths`、`uniqueGridPathsWithObstacles`、
  `minGridPathSum`、`editDistance`。
- 序列 DP：`longestIncreasingSubsequenceLength`、`longestIncreasingSubsequence`（两者均为
  `O(n log n)` 长度/重建）、
  `longestCommonSubsequenceLength`、`longestCommonSubsequence`、
  `longestCommonSubstringLength`、`longestCommonSubstring`。
- 区间 DP：`matrixChainCost`、`mergeStonesCost`。
- 二分查找：`lowerBound`、`upperBound`、`binarySearch`。
- 字符串比较辅助：`compareStrings`、`stringLess`（当前限定可打印 ASCII）。
- 字符串匹配：`findSubstring`、`prefixFunction`、`zFunction`、`kmpSearch`、`isPalindrome`、
  `minimumWindowSubstring`、`canSegmentString`。
- Trie：`Trie`、`newTrie`、`insert`、`discard`、`has`、`startsWith`、`wordsWithPrefix`。
- Fenwick 树：数值 `FenwickTree`，支持点更新、前缀和、区间和与快照。
- 线段树：数值 `SegmentTree`，支持点更新、区间和、区间最小值与快照。
- 稀疏表：数值 `SparseTable`，静态半开区间最小值/最大值查询。
- 矩阵：不可变数值 `Matrix`，矩形校验、访问、转置与乘法。
- 并查集：整数顶点 `DisjointSet`，支持路径压缩和按大小合并。
- 图基础结构：整数顶点数组邻接表 `Graph`，支持有向/无向边的增加和删除。
- 图遍历：`breadthFirstOrder`、`depthFirstOrder`。
- 无向连通性：`connectedComponents`、`isBipartite`。
- 无向割点与桥：`articulationPoints`、`bridges`。
- 欧拉路径：无向图 `eulerTrail`。
- 无权图路径：`shortestDistances`、`shortestPath`。
- DAG 视图：`inDegrees`、`topologicalOrder`、`hasCycle`。
- 关键路径：`criticalPath`，在非负权 DAG 上返回从起点出发的最长距离、父节点和拓扑序。
- 加权图：非负 `WeightedEdge`/`WeightedGraph`、Dijkstra、Floyd-Warshall、最大流、最小割、Prim 与 Kruskal 最小生成森林。
- 有符号加权图：`SignedWeightedGraph`、`BellmanFordResult` 与 Bellman-Ford 负环检测。
- 强连通分量：有向图的 `stronglyConnectedComponents`（Kosaraju）和
  `stronglyConnectedComponentsTarjan`。

它们的现有 API 和示例继续作为兼容基线。后续新增 API 不应悄悄改变空值、
快照或引用共享语义。

## 2. 当前语言约束

以下约束直接影响实现选择，后续设计不能假设项目文档没有提供的能力。

### 2.1 已有能力

- 数组是可变引用值，`copy`、`slice` 和 `concat` 只复制最外层；
- map 是可变引用值，按插入顺序迭代，键目前只能是 `nil`、`number`、`bool`
  或 `string`；
- range 是不可变的有限整数序列；
- 泛型类型参数按名义类型处理，结构体和枚举泛型参数不变；
- 函数值和闭包可以作为回调，数组已有 `map`、`filter`、`reduce`、`find`
  等高阶操作；
- 递归枚举引用已实现，可用于表达不可变递归结构；
- 结构体方法可以通过 `impl` 定义，泛型结构体可使用 `impl Box<T>` 形式。

### 2.2 设计限制

- 现有 `Eq`、`Ord` 和 `Hash` 能力只在编译期检查，不生成运行时 trait 对象、
  隐式能力字典或动态派发；泛型算法仍可使用显式比较器表达用户定义类型的
  排序行为；
- 命名结构体可以通过 `optional<Node>` 等有限 nominal 类型表达可变递归节点；结构体
  值是稳定强句柄，赋值/传参/字段存储复制句柄，字段修改对别名可见。显式断链不使
  既有句柄失效，结构体相等仍按 identity，活跃格式化路径上的重复引用打印 `<cycle>`。
  强引用环在 VM teardown 前保留；没有 GC、弱引用或自动删除承诺；
- `hash(value)` 提供确定性的基础哈希入口；通用 `HashSet`/`HashMap` 已使用
  `Eq + Hash` 和身份稳定的可变引用键契约实现，但内置 map 仍只接受基础键；
  位集、布隆过滤器和随机洗牌不列入第一批实现；
- 结构体字段支持 `private`，而私有方法仍未提供。已知命名结构体接收者的方法
  优先于同名的内置成员调用，因此 `Stack<T>` 可以使用 `push`、`pop` 等常规
  方法名；数组、map、字符串和 range 接收者仍使用内置形式；
- 命名结构体可以在 `impl` 中声明 `<`、`<=`、`>`、`>=` 运算符；字符串也提供
  内置字典序比较。四个完整且有效的结构体运算符提供静态 `T: Ord` witness
  （并满足 `T: Eq`），不完整实现仍使用显式比较器；
- 模块系统还没有包清单、import map、通配符导出或导出重命名。目录结构应
  先按普通源文件导入设计。

## 3. API 约定

### 3.1 命名

- 泛型结构使用 `Stack<T>`、`Deque<T>`、`PriorityQueue<T>` 这样的名义类型；
- 构造优先提供 `newStack<T>()`、`newDeque<T>()` 等工厂，减少调用者依赖
  底层字段；
- 可能为空的查询返回 `optional<T>`，例如 `peek(): optional<T>`、`take(): optional<T>`；
- 会改变结构的操作使用明确动词，例如 `add`、`enqueue`、`dequeue`、
  `discard`、`update`；
- `snapshot()`、`toArray()` 和 `entries()` 返回新的一层数组；除非文档另有
  说明，元素本身不深拷贝；
- 算法默认不修改输入，原地版本明确使用 `...InPlace` 后缀；
- 需要比较顺序时传入 `less(a, b): bool` 回调，不依赖未定义的泛型 `<`；
- “找不到”优先返回 `nil`、`-1` 或显式 `Option<T>`；预期失败不应依赖
  不透明的运行时错误。

### 3.2 结果类型

库基础模块约定两个公共枚举，供结构和算法复用；当前均已实现：

```cd
enum Option<T> {
  Some(value: T),
  None,
}

enum Result<T, E> {
  Ok(value: T),
  Err(error: E),
}
```

在当前语言中，简单查询可以直接使用 `optional<T>`。`Option<T>` 适用于需要通过
`match` 明确区分结果的 API；`Result<T, E>` 适用于需要携带错误值但没有异常
机制的 API。具体使用哪一种，应按每个模块的 API 文档固定下来，不能同一类
结构随意混用。

### 3.3 复杂度和快照

所有公开结构的 README/API 小节都记录复杂度。数组高阶函数按项目文档对
输入快照执行，库函数也应在会发生回调重入或输入原地修改的地方明确记录
快照语义。对于队列、堆、图等结构，`snapshot()` 只用于观察和测试，不应
承诺返回可作为内部存储的视图。

## 4. 结构清单

状态含义：

- **现有**：仓库已经有实现，先保持行为兼容；
- **第一批**：只依赖当前文档化能力，适合作为下一轮实现；
- **受限实现**：可以实现，但会是数组/枚举/整数索引版本，能力或复杂度
  不等同于传统指针实现；
- **后续**：常用但需要先确定语言契约、性能目标或更强语言能力。

### 4.1 基础序列和缓冲结构

| 结构 | 状态 | 主要 API/用途 | 预期实现 |
| --- | --- | --- | --- |
| 动态数组/向量 `Vector<T>` | 现有能力 | 直接使用 `[T]`，提供少量约定性辅助函数 | 不重复包装语言内置数组 |
| 栈 `Stack<T>` | 现有 | `push`、`pop`、`top`、`size`、`isEmpty`、`snapshot` | 数组后端，`pop/top` 为 `O(1)` |
| 队列 `Queue<T>` | 现有 | `enqueue`、`dequeue`、`front`、`size`、`isEmpty`、`snapshot` | 头指针 + 周期压缩，操作摊销 `O(1)` |
| 双端队列 `Deque<T>` | 现有（S1） | `addFront`、`addBack`、`takeFront`、`takeBack`、`peekFront`、`peekBack` | 双数组栈；两端操作摊销 `O(1)` |
| 环形缓冲区 `RingBuffer<T>` | 现有（S1） | 固定容量、`offer`、`read`、`peek`、`isFull` | `Option<T>` 槽位；满时拒绝并返回 `false` |
| 不可变链表 `List<T>` | 现有（S1） | `emptyList`、`prepend`、`head`、`tail`、`reverse`、`toArray` | 递归泛型枚举，持久化/共享尾部 |
| 单向链表 | 现有（M8） | 稳定节点句柄、前后插入、前端删除、节点查找和浅快照 | `optional<LinkedNode<T>>` 头尾链接；句柄复制共享节点，`pushFront`/`pushBack`/`popFront` 为 `O(1)`，快照为线性遍历 |
| 双向链表 | 后续 | 两端插入删除、迭代器、节点移动 | 需要可表达的双向节点和稳定节点引用 |
| 循环链表 | 后续 | 循环调度、约瑟夫问题 | 需要先确定节点引用和空结构语义 |
| 跳表 | 后续 | 有序集合的平均 `O(log n)` 操作 | 需要随机数和节点/层级表示 |

### 4.2 堆、优先级和集合结构

| 结构 | 状态 | 主要 API/用途 | 预期实现 |
| --- | --- | --- | --- |
| 二叉堆 `BinaryHeap<T>` | 现有（S1） | `add`、`peek`、`take`、`size`、`isEmpty`、`snapshot` | 数组 + `less` 回调；`add/take` 为 `O(log n)` |
| 优先队列 `PriorityQueue<T>` | 现有（S1） | 对外提供队列语义，优先返回最小/最大元素 | `BinaryHeap` 的稳定命名包装 |
| 双堆中位数 | 现有（S6） | `add`、`median`、流式中位数 | 下半部最大堆 + 上半部最小堆；空集合返回 `nil`，偶数个值取算术平均 |
| `Set<T>` | 现有（S2） | `add`、`has`、`discard`、`size`、`isEmpty`、`snapshot` | 数组线性查找，按语言 `==` 去重，保留快照插入顺序 |
| 有序集合 `OrderedSet<T>` | 现有（S6） | 有序插入、范围查询、前驱后继 | 严格比较器 + 有序数组；查找 `O(log n)`，插入/删除 `O(n)` |
| 多重集合 `MultiSet<T>` | 现有（S2） | `add`、`countOf`、`takeOne`、`has`、`entries` | 条目值/计数数组，按语言 `==` 查找，不依赖 map |
| 多重映射 `MultiMap<K,V>` | 现有（S2） | 一个键对应多个值，`add`、`getAll`、`discard(key,value)` | 键数组 + value 数组，按语言 `==` 查找，不依赖 map |
| 哈希集合 `HashSet<T>` | 现有（S6） | `add`、`has`、`discard`、`snapshot` | `Eq + Hash` 数组桶；可变引用键按 identity 稳定匹配，负载超过 75% 扩容 |
| 哈希映射 `HashMap<K,V>` | 现有（S6） | `put`、`get`、`has`、`discard`、`snapshot` | `Eq + Hash` 数组桶；插入顺序快照，缺失返回 `nil` |
| 有序映射 `OrderedMap<K,V>` | 现有（S6） | comparator 排序、更新、删除、遍历条目 | 严格比较器 + 有序数组；等价键合并，查找 `O(log n)`、插入/删除 `O(n)` |
| 双向映射 `BiMap<K,V>` | 现有（S6） | 双向唯一查找、插入、按任一侧删除 | 两侧 `Eq` + 数组；重复 key/value 拒绝，操作 `O(n)` |
| LRU 缓存 `LruCache<K,V>` | 现有（S6，节点版） | 容量限制、`get`、`put`、淘汰最久未用项 | 私有单向节点句柄；`O(n)` 查找/移动，零容量拒绝写入 |
| LFU 缓存 `LfuCache<K,V>` | 现有（S6，节点版） | 按访问频率淘汰、同频按最近使用淘汰 | 私有单向节点与 `lastUsed`；线性查找与淘汰，操作 `O(n)` |
| 计数器/频率表 | 第一批 | 频率统计、最高频项 | 基础键可用 map；泛型版本用条目数组 |

### 4.3 树、索引和文本结构

| 结构 | 状态 | 主要 API/用途 | 预期实现 |
| --- | --- | --- | --- |
| 二叉树 `Tree<T>` | 现有（S3） | 前序/中序/后序/层序、大小、高度、叶子 | 递归枚举，不可变版本 |
| 二叉树 LCA | 现有（S6） | 按根路径查找最近公共祖先 | 路径使用 `0/1` 左右边；非法路径返回 `nil`，不依赖节点值唯一 |
| 二叉搜索树 `Bst<T>` | 现有（S3） | 查找、插入、删除、最小/最大、前驱/后继 | 复用 `Tree<T>` + `less`，操作返回新树 |
| AVL 树 `AvlTree<T>` | 现有（S6，数组节点） | 严格平衡、插入、删除、范围邻居查询 | 并行数组 + 私有整数索引；旋转后插入/删除 `O(log n)` |
| 红黑树 `RedBlackTree<T>` | 现有（S6，数组节点） | 工程化平衡、插入、删除、范围邻居查询 | 并行数组 + 颜色/父索引；更新 `O(log n)` |
| B 树/B+ 树 | 后续 | 磁盘/大规模索引 | 当前没有持久化文件 API，不列入近期目标 |
| Fenwick 树 | 现有（S3） | 单点更新、前缀和、区间和 | 数值数组，零基公共索引；内部预计算低位跨度 |
| 线段树 | 现有（S3） | 区间和、区间最小值、点更新 | 数值数组；固定聚合，非二次幂长度使用活动叶标记 |
| 稀疏表 `SparseTable` | 现有（S6，数值版） | 静态半开区间最小值/最大值查询 | 固定数值聚合；构造 `O(n log n)`，查询 `O(1)` |
| Trie | 受限实现 | 字符串插入、查找、前缀查询、自动补全 | 字符串键 + 数组节点索引，不依赖递归结构体 |
| 后缀数组/后缀自动机 | 后续 | 子串查询、重复子串分析 | 作为文本算法专题，先不混入基础库 |
| 矩阵 `Matrix` | 现有（S6，数值专题） | 矩形校验、行列访问、转置、乘法 | 非空矩形 `[[number]]`；构造/快照深拷贝，非法访问或维度不匹配返回 `nil` |

### 4.4 图结构

第一版图 API 以整数顶点编号为主，避免“泛型顶点如何哈希/编号”的未决
问题。后续再提供泛型顶点适配层。

| 结构 | 状态 | 主要 API/用途 | 预期实现 |
| --- | --- | --- | --- |
| 边 `Edge`/加权边 `WeightedEdge` | 第一批 | `from`、`to`、`weight` | 普通结构体；权重先使用 `number` |
| 邻接表图 `Graph` | 第一批 | 加边、删边、邻居、顶点数、边数 | `[[number]]` 或边数组；支持有向/无向配置 |
| 加权图 `WeightedGraph` | 第一批 | 加权邻居、边遍历 | 邻接边数组；先固定 `number` 权重 |
| 有符号加权图 `SignedWeightedGraph` | 第一批 | 负权边、Bellman-Ford | 与非负图分离，结果使用 `Result` |
| 邻接矩阵图 | 后续 | 稠密图、矩阵算法 | `[[number]]`，需要定义无边哨兵值 |
| 并查集 `DisjointSet` | 第一批 | `representative`、`union`、`connected`、`componentCount` | 整数 parent/size 数组，路径压缩 + 按大小合并 |
| DAG 视图 | 第一批 | 拓扑排序、入度 | 在 `Graph` 上提供算法，不单独复制存储 |
| 流网络 | 后续 | 容量边、残量网络 | 需要明确无穷容量、浮点误差和结果结构 |

### 4.5 其他常见结构的取舍

下列结构会保留在总清单中，但不进入前几轮：位集、布隆过滤器、
并发队列、内存池、对象池、持久化 B 树、跳表和高性能 intrusive list。
原因是位运算、并发、文件随机访问、指针/节点引用或内存管理契约仍未形成库级
稳定 API。Issue #15 已为 `HashSet`/`HashMap` 固定通用 map 键约束和可变键
别名语义；高性能哈希节点版仍不在当前范围。

## 5. 算法清单

算法按输入结构组织，公共函数默认返回新数组或值；会修改数组的版本明确
带 `InPlace`。

### 5.1 数组与序列算法

当前库已提供不修改输入的 `reverseArray`、`linearSearch`、`countValue`、
`chunkArray`、`slidingWindows` 和 `prefixSums`。这些基础版本使用线性扫描、
浅拷贝和新建输出数组；空数组调用需要显式元素类型参数。`Interval` 和
`mergeIntervals` 也已提供，合并时会排序并合并重叠或相接区间。
`mergeSortedNumbers` 和 `twoSumSorted` 已提供非降序数字数组上的双指针版本。
`threeSumZero` 会先排序副本，再用双指针生成去重的非降序零和三元组，保持输入
不变，时间复杂度为 `O(n^2)`。
`threeSumClosest` 在排序副本上用双指针寻找距离目标最近的三数和；少于三个值返回
`nil`，相同距离取较小和，输入保持不变，时间复杂度为 `O(n^2)`，额外空间为 `O(n)`。
`mergeSortedArrays<T>` 接受多个按同一 `less` 比较器排序的数组，使用当前头部
最小堆返回一个新的扁平数组；比较器等价时按输入数组下标优先，因此跨输入数组
保持稳定。空数组集合或全空输入返回 `[]`，算法时间复杂度为 `O(n log k)`，其中
`k` 是输入数组数量，堆额外占用 `O(k)`，输出占用 `O(n)`；输入数组不会被修改。
`uniqueValues`、`intersectionValues`、`unionValues` 和 `differenceValues` 已
提供保序去重、交集、并集和差集的线性扫描版本。
`windowSums` 使用滚动和生成每个固定宽度窗口的和，`maxWindowSum` 在这些
窗口中选择最大值；`maxWindowValues` 和 `minWindowValues` 使用单调索引队列
返回每个窗口的最大值或最小值。
非法窗口宽度返回空数组或 `nil`。
`differenceArray`、`prefixMinimums` 和 `prefixMaximums` 提供数值数组的相邻差分
及前缀极值；`nextGreaterValues` 和 `nextSmallerValues` 使用单调栈返回每个位置
右侧第一个严格更大或更小值，不存在时使用 `-1`。
`intersectIntervals` 先独立规范化两侧区间，再用双指针生成包含端点的相交
区间；因此相接端点会生成零长度区间。
`maxSubarraySum` 使用 Kadane 扫描返回非空连续子数组的最大和，空数组返回
`nil`，全负输入仍保留最大负值。
`lowerBound`、`upperBound` 和 `binarySearch` 接受与输入排序一致的泛型比较器；
前两者返回插入边界，后者返回重复值的首个位置。`rotatedBinarySearch` 在一次
旋转后的非降序数组中查找 comparator 等价值；无重复值时为 `O(log n)`，重复值
导致两侧都可能有序时安全退化为 `O(n)`，未找到或空数组返回 `-1`。
`compareStrings` 和 `stringLess` 保留库层可打印 ASCII 的三路比较与回调契约；
语言层现在额外提供 Unicode scalar-value 字符串排序和比较运算符。
`findSubstring`、`prefixFunction`、`zFunction` 和 `kmpSearch` 使用现有 Unicode scalar-value
字符串位置语义；`isPalindrome` 按 scalar value 比较字符，不依赖 ASCII 排序。
`longestUniqueSubstringLength` 用 string-keyed map 保存最后出现位置和滑动窗口，返回
最长无重复子串长度，平均为 `O(n)`；`longestPalindromicSubstring` 用 Manacher 半径数组
返回最左最长回文子串，空串分别返回 `0` 和空串，后者为 `O(n)` 时间和 `O(n)` 辅助空间。
`longestUniqueSubarrayLength<T: Eq>` 对任意可相等比较的数组元素返回最长无重复
连续区间长度；它保持输入不变，空数组返回 `0`，使用不依赖通用哈希的数组窗口扫描，
最坏时间复杂度为 `O(n^2)`、额外空间为 `O(1)`。
`Trie` 使用数组节点和线性边查找，支持 Unicode scalar-value 字符、重复插入去重、
`discard` 删除及按首次插入的边顺序返回前缀结果，不依赖通用哈希。删除前缀词会
保留子词，删除最后一个分支会移除父节点上的边；节点槽位不复用，避免暴露不稳定
的节点索引。
`peakIndex` 在 `O(log n)` 时间内返回一个弱峰值；`isMountainArray` 用线性扫描
验证严格先升后降，`mountainPeakIndex` 在验证后用二分查找峰顶。空数组或非严格
山脉返回 `nil` 峰值结果。
`minimumLargestPartitionSum` 对非负整数数组的连续分段容量做答案空间二分，返回
至多指定分段数时的最小最大段和；空数组返回 `0`，非法分段数或元素返回 `nil`，
时间复杂度为 `O(n log S)`，其中 `S` 是元素总和。
`mergeSort` 使用稳定的自底向上归并排序，返回新数组，不改变输入。
`countInversions` 在同一归并过程中统计 `i < j` 且 `values[i] > values[j]` 的
严格逆序对，返回 `0` 表示没有逆序对，输入保持不变；时间复杂度为 `O(n log n)`，
额外数组空间为 `O(n)`。
`stablePartition<T>` 按谓词把满足条件的元素放到前面、其余元素放到后面，分别
保持两组元素的输入顺序，返回新数组且不修改输入；它只依赖一元谓词，时间复杂度
为 `O(n)`，额外数组空间为 `O(n)`。
`shellSort` 使用从 `n / 2` 开始不断折半的 gap 序列和分组插入排序，返回浅拷贝，
不保证稳定性；当前序列的最坏时间复杂度为 `O(n^2)`，额外结果空间为 `O(n)`。
`quickSort` 和 `quickSortInPlace` 使用中点 pivot 的三向原地分区版本；它们不保证
稳定性，平均为 `O(n log n)`，最坏为 `O(n^2)`，并通过先递归较小分区、循环处理
较大分区把递归栈限制在 `O(log n)`；全等值区间只需一次线性分区。
`heapSort` 和 `heapSortInPlace` 使用 comparator 定义的相反堆序完成原地堆排序，
不保证稳定性，时间复杂度为 `O(n log n)`。
`rotateArray` 和 `isSorted` 已补充为不改变输入的旋转与有序性检查辅助。
`frequencyEntries` 和 `mostFrequent` 复用 `MultiSet` 的数组后端，保留首次
出现顺序并在并列时选择首次出现值。
`DisjointSet` 已提供整数顶点的路径压缩与按大小合并版本；越界顶点仍遵循
数组索引的运行时边界行为。
`Graph` 已提供整数顶点的去重邻接表；无向边写入两侧邻接数组但只计数一次，
非整数或越界顶点查询返回安全空结果。
`breadthFirstOrder` 和 `depthFirstOrder` 已提供基于邻接插入顺序的 BFS/DFS，
只返回起点所在可达分量。
`connectedComponents` 已提供无向图的升序根节点 BFS 分量划分；有向图查询返回
空数组。
`isBipartite` 已提供无向图的 BFS 二着色检查；有向图、自环和奇环返回 `false`。
`articulationPoints` 和 `bridges` 已提供迭代 Tarjan low-link 版本；有向图查询
返回空数组，割点按编号排序而桥保留 DFS 完成顺序。
`eulerTrail` 已提供无向图 Hierholzer 路径恢复，检查奇度顶点、连通性和起点，
当前图的自环表示也纳入处理。
`shortestDistances` 和 `shortestPath` 已提供基于 BFS 的无权图距离和路径，
不可达顶点使用 `-1` 或空路径。
`inDegrees` 和 `topologicalOrder` 已提供有向无环图的入度与 Kahn 拓扑排序；
无向图或有环图的拓扑查询返回空数组，并保持顶点/邻接插入顺序。
`hasCycle` 已覆盖有向图拓扑检测和无向图带父节点 DFS，包括自环。
`stronglyConnectedComponents` 已提供迭代 Kosaraju 版本；组件顺序按完成序，
组件内部按 DFS 发现序，不对结果额外排序。
`WeightedGraph` 和 Dijkstra 查询已提供非负数权重版本；负权边不加入图，当前
实现使用带 stale-entry 检查的最小堆，等距时按较小顶点 ID 确定顺序，复杂度为
`O((V + E) log V)`。
`SignedWeightedGraph` 单独接受负权边，避免改变 Dijkstra、最小生成森林和最大流
的非负权前置条件。`bellmanFord` 返回带父节点的可空距离数组，并将无效起点和
从起点可达的负环分别报告为 `BellmanFordError`。
`minimumSpanningForest` 已提供无向加权图的最小堆 Prim 版本；非连通输入返回森林，
有向输入返回同顶点数的空森林，复杂度为 `O((V + E) log V)`。
`minimumSpanningForestKruskal` 已提供基于边排序和并查集的等价版本，保留非连通
输入的森林语义，并对有向输入返回空森林。
`allPairsWeightedDistances` 已提供非负权图的 Floyd-Warshall 全源距离矩阵，
不可达项使用 `-1`。
`maxFlow` 已提供有向非负容量图的 Edmonds-Karp 增广路径版本，不修改输入图。
`minCut` 复用同一残量网络，返回源侧可达顶点到非可达顶点的原图正容量边，
无效输入和无向图返回空数组。

- 遍历、复制、拼接、分块、窗口、批处理；
- `reverse`、`rotate`、`partition`、稳定分区、按谓词分组；
- 去重、保序去重、频率统计、交集、并集、差集；
- 前缀和、差分数组、前缀最小/最大值；
- 两指针、滑动窗口、快慢指针、单调栈、单调队列；
- 单调队列窗口最大值（`maxWindowValues`）；
- 稳定分区（`stablePartition`）；
- 合并两个有序数组、合并多个有序数组（`mergeSortedArrays`）、区间合并和区间相交；
- 逆序对统计（`countInversions`）；
- 子数组最大和、最长无重复子数组、固定/可变窗口统计；
- 逆序对、配对和、三数和、最接近目标值等常用题型。

### 5.2 查找算法

- 线性查找：`O(n)`；
- 二分查找：有序数组中的精确查找；
- `lowerBound`、`upperBound`、插入位置和出现次数；
- 旋转有序数组查找：`rotatedBinarySearch`；
- 峰值、山脉数组：`peakIndex`、`isMountainArray`、`mountainPeakIndex`；
- 答案空间二分：非负整数数组的 `minimumLargestPartitionSum`；
- 数值数组的插值查找作为可选专题，不作为通用默认实现。

### 5.3 排序算法

当前库已提供稳定插入排序：`sortArray` 返回浅拷贝，`sortArrayInPlace` 原地
修改；两者都接受 `less` 比较器，时间复杂度为 `O(n^2)`。库还提供不修改输入的
`shellSort`、选择排序、冒泡排序、归并排序、快速排序和堆排序版本。

先提供可读、容易验证的实现，再提供工程上常用的实现：

| 算法 | 复杂度/特征 | 计划 |
| --- | --- | --- |
| 冒泡排序 | `O(n^2)`，教学用途 | 已完成 |
| 选择排序 | `O(n^2)`，低额外空间 | 已完成 |
| 插入排序 | `O(n^2)`，小数组表现好，稳定 | 已完成 |
| 希尔排序 | 折半 gap 序列，通常不稳定 | 已完成 |
| 归并排序 | `O(n log n)`，稳定，需额外数组 | 第一批 |
| 快速排序 | 平均 `O(n log n)`，最坏 `O(n^2)`，栈深 `O(log n)` | 已完成 |
| 堆排序 | `O(n log n)`，原地，不稳定 | 第一批，复用堆 |
| 计数排序 | `O(n+k)`，稀疏值域回退归并排序 | 已完成（数值专题） |
| 基数排序 | `O(d(n+k))`，依赖整数表示 | 等待数值/位运算契约 |
| 桶排序 | 依赖分布和桶策略 | 后续 |

比较器版本使用 `less` 回调；数字升序便利函数可以单独提供。排序的稳定
性、是否原地和是否保留输入必须写进每个函数的 API 文档。

### 5.4 栈、队列和堆算法

- 括号匹配 `isBalancedBrackets` 已完成；表达式后缀化和后缀表达式求值；
- 单调栈求下一个更大/更小元素、直方图最大矩形 `largestHistogramArea` 已完成；
- BFS 使用队列，DFS 使用显式栈；
- 堆化、堆排序、前 `k` 个元素、第 `k` 大/小元素；选择 API 已使用三向
  quickselect 和选中段排序，保持输入不变、非法参数和比较器排序契约；相等
  元素的相对顺序不属于 API 保证；
- 两个优先队列求数据流中位数；
- 多路有序流合并（数组版本 `mergeSortedArrays` 已完成）；
- 任务调度、区间资源分配和会议室数量。

### 5.5 链表算法

针对递归枚举版不可变链表先实现纯函数版本：

- 长度、查找、按位置读取；
- 前插、尾插、拼接、反转；
- 截取、丢弃和两个有序不可变链表的稳定归并；
- 合并两个有序链表；
- 找中点、判断回文：`listMiddle` 使用快慢游标，`listIsPalindrome` 已完成；检测环；
- 删除倒数第 `n` 个元素：`listRemoveFromEnd` 已完成；
- 两链表相交点。

需要原地节点重连的算法暂缓到可变节点表示明确之后。

### 5.6 树算法

当前已完成不可变 `Tree<T>` 的遍历、统计、平衡/宽度检查、根到叶路径和
数值路径和，以及基于它的基础 BST 操作；`AvlTree<T>` 和 `RedBlackTree<T>` 使用
数组节点实现了可变旋转、颜色修复和平衡维护。路径型 LCA 和数值树序列化已完成；
通用 `Tree<T>` 的文本 codec 仍不承诺。

- 前序、中序、后序和层序遍历；
- 高度、大小、叶子数、最大宽度、平衡检查；
- BST 查找、插入、删除、最小/最大、前驱/后继；
- 路径型最近公共祖先、根到叶路径、路径和；
- 数值 `Tree<number>` 的前序序列化/反序列化：`#` 表示空树，节点使用
  `value,left,right`，非法格式、缺失子树或尾随内容返回 `nil`；
- `AvlTree<T>` 的 AVL 旋转和平衡维护；
- `RedBlackTree<T>` 的颜色修复、旋转和平衡维护；
- Fenwick/线段树的建树、更新、前缀/区间查询；
- 数值 `SparseTable` 的静态幂等区间查询；
- 数值 `Matrix` 的矩形校验、转置和乘法；
- Trie 的插入、精确查找、前缀查找、删除和补全。

### 5.7 图算法

按有向/无向、带权/不带权拆分 API，避免一个函数隐藏过多前置条件：

- BFS、DFS、可达性、路径恢复；
- 连通分量、无向图环检测、二分图判定；
- 有向图环检测、拓扑排序、关键路径的基础数据；
- 无权最短路；
- 非负权图 Dijkstra；
- 可处理负边的 Bellman-Ford；
- 全源最短路 Floyd-Warshall；
- Kruskal、Prim 最小生成树；
- 强连通分量（Kosaraju 与 Tarjan）；
- 桥和割点；
- 欧拉路径/回路；
- 最大流/最小割作为高级专题。

图算法的结果应尽量返回 `Option`/`Result` 或带有 `distance`、`parent`、
`order` 等明确字段的结构，而不是通过打印结果暴露状态。

### 5.8 字符串与模式匹配算法

- 朴素子串查找；
- 前缀函数和 KMP；
- Z 函数；
- 回文判断、无重复子串长度、最长回文子串已完成；
- 字符频率、字谜/异位词判断、滑动窗口匹配 `minimumWindowSubstring` 已完成，
  字符串基础键 map 版本平均为 `O(n)`；
- Trie 前缀查找和补全；
- Rabin-Karp 作为哈希契约确定后的可选实现；
- 编辑距离、最长公共子序列、最长公共子串；
- 字符串分割和字典匹配 `canSegmentString` 已完成，使用 Trie 从可达位置做
  前缀遍历，避免逐终点扫描整个字典。

字符串位置必须遵守项目文档的 Unicode scalar value 规则，不能把字节下标
当作公共 API 的默认下标。

### 5.9 数值与基础数学算法

- `gcd`、`lcm`、扩展欧几里得；
- 快速幂、阶乘、斐波那契（fast-doubling）、2×2 矩阵快速幂已完成；
- 素数判断、埃氏筛、线性筛（若性能需要）；
- 质因数分解、约数枚举已完成；
- 最大公约数数组、前缀积已完成；差分已有数组辅助版本；
- 组合数、数值排列数、帕斯卡三角已完成；
- 数值二分 `minimumLargestPartitionSum` 已完成；牛顿法作为可选专题。

这些函数先使用 `number`，并在 API 文档中明确有限整数、负数、溢出和浮点
精度假设；项目文档没有规定的溢出行为不能默默写成库保证。

### 5.10 动态规划、贪心与回溯

当前已完成一维 DP 的爬楼梯、非相邻最大和、最少硬币数，网格路径/最小和、
字符串编辑距离、障碍网格路径、LIS/LCS 以及 0/1 背包；还提供了子集、组合、排列的完整
回溯生成版本。它们只使用数组与数值运算，暂不承诺溢出行为。

- 一维 DP：爬楼梯、打家劫舍、硬币兑换；
- 背包：0/1、完全、多重背包已完成；
- 序列：LIS（长度和 predecessor 重建均为 `O(n log n)`）、LCS、最长公共子串（长度和重建）、编辑距离；
- 网格：路径计数、障碍物路径、最小路径和已完成；
- 区间 DP：矩阵链、二路合并石子已完成；
- 贪心：区间调度、区间资源分配、跳跃游戏、Huffman 合并核心已完成；
- 回溯：子集、组合、排列、括号生成、N 皇后、迷宫路径已完成；
- 记忆化搜索和状态压缩作为后续实现方式。

回溯和生成器 API 先返回完整的 `[T]`/嵌套数组，暂不承诺惰性迭代器；若
输出规模可能指数增长，文档必须说明空间复杂度和上限。

### 5.11 可选专题

图形/几何、概率数据结构、压缩编码和高性能缓存可以另设专题模块：

- 点、向量、方向判断、线段相交、凸包；
- Huffman 编码、前缀码；
- Bloom filter、Count-Min Sketch；
- 跳表、Treap、持久化树；
- 并发队列、无锁结构。

它们不阻塞基础数据结构库的发布。

## 6. 分阶段实现顺序

### S0：契约和测试基线

在扩展代码前，补齐库级约定：命名冲突清单、空值/失败策略、快照/别名
规则、复杂度记录、导入示例和最小 golden fixture。现有 `Stack<T>`、
`Queue<T>` 的输出保持不变。

### S1：线性容器基础

已完成 `Deque<T>`、`BinaryHeap<T>` 和 `PriorityQueue<T>`：前者使用前端反向数组
与后端正向数组，后两者使用数组和 `fun(T, T): bool` 比较器，其中优先队列
只提供队列命名包装。库级 fixture 位于
`library/tests`，由 `library/tests/run_tests.py` 独立运行，复用编译器发射和
Rust VM 执行接口；库测试只比较运行结果，不比较 AST 或 bytecode 文本。

已完成 `Option<T>`、`Result<T,E>` 和递归枚举版 `List<T>`，均有构造工厂、
匹配示例和独立库级 fixture。`List<T>` 使用共享尾部表达持久化值。

S2 已完成 `Set<T>` 的数组后端版本：使用语言 `==` 做线性去重和查找，删除
时保持剩余值的插入顺序。随后完成 `MultiSet<T>`：以条目值/计数数组实现
重复计数，`entries()` 按首次插入顺序返回新条目数组；`MultiMap<K,V>` 使用
键数组和每键 value 数组，`discard(key,value)` 删除一个匹配 pair。随后完成
了基础数组搜索/计数/逆序算法、稳定插入排序、窗口/前缀和算法、区间合并、
双指针、数组集合算法、固定窗口统计、区间交集、最大子数组和、二分查找、归并
排序、快速排序、堆排序、希尔排序以及选择排序和冒泡排序教学版本；基础数值算法已加入
`gcd`、`lcm`、`extendedGcd`、`isPrime`、`sievePrimes`、`fastPower`、`factorial`
与 `fibonacci`。

### S2：集合和序列算法

实现数组搜索/排序、`Set<T>`、`MultiSet<T>`、`MultiMap<K,V>`、频率统计、
区间/窗口/双指针算法。优先完成无随机数、无哈希要求的版本。

### S3：树和区间查询

已实现 Trie、Fenwick 树、不可变二叉树、BST、基础线段树、数组节点版
`AvlTree<T>` 和 `RedBlackTree<T>`；其他可变平衡树再根据实际需求决定。

### S4：图和路径算法

实现整数顶点的 `Graph`/`WeightedGraph`、`DisjointSet`、BFS/DFS、拓扑排序、
最短路、最小生成树和 SCC。每个算法配套路径/父节点结果结构。

### S5：字符串、数学和经典题型

已实现 KMP/Z、Trie 辅助、GCD/筛法、基础 DP、矩阵链、0/1/完全/多重背包、区间调度/资源分配、跳跃游戏、Huffman 合并核心以及子集/组合/排列/括号/N 皇后/迷宫路径
回溯生成。此阶段继续整理
独立专题 README，避免把教学算法和生产容器 API 混在同一个文件里。

### S6：高级结构和性能版本

双堆中位数已实现：`MedianHeap` 使用下半部最大堆和上半部最小堆，保持两侧
大小最多相差一个；空集合的 `median()` 返回 `nil`，偶数个值返回两个中间值的
算术平均。`OrderedSet<T>` 也已实现：使用严格比较器维护有序数组，按比较器等价
去重，支持端点、前驱/后继和闭区间查询；二分查找为 `O(log n)`，插入/删除因数组
移动为 `O(n)`。`LruCache<K: Eq, V>` 使用私有单向节点句柄按最久未使用到最近
使用保存条目，访问和更新刷新顺序，满容量插入淘汰首项；查找、移动和删除均为
`O(n)`，淘汰时清除节点链接且不要求 `Hash` 能力。`OrderedMap<K, V>` 使用同样的有序数组策略，按 comparator
等价键保留一个条目，`put` 更新值但保留首次键和排序位置。`BiMap<K: Eq, V: Eq>`
采用双方唯一且冲突拒绝的数组实现，按 key/value 双向查询，删除时同步移除配对条目。
`LfuCache<K: Eq, V>` 使用私有单向节点保存条目和 `lastUsed` 时钟，按最低频次淘汰，
同频时淘汰最久未使用项；`get` 和更新会增加频次并刷新使用时钟，但保留快照的
存储顺序。`HashSet<T: Eq + Hash>` 与
`HashMap<K: Eq + Hash, V>` 也已实现：两者使用数组桶和确定性 `hash(value)`，
按插入顺序返回浅快照；数组、map、函数和命名结构体键按 identity 稳定匹配，
别名修改不会破坏查找，负载超过 75% 时扩容。`AvlTree<T>` 使用并行数组保存节点和
私有整数索引，旋转、插入和删除保持 `O(log n)`，不向调用者暴露稳定节点引用。
`SparseTable` 也已提供固定数值最小/最大聚合的静态版本，查询为 `O(1)`；其余
`Matrix` 也已提供不可变的非空矩形数值版本，固定深拷贝、越界与维度失败语义；其余
LFU 高性能频率桶/索引版、跳表、哈希/位集、后缀结构和几何算法，
根据真实使用反馈再决定；没有对应语言能力或用户场景时不强行实现。

2026-08-02 的 `library_lfu_cache` scaled workload 使用容量 64、1000 次循环和
固定热键访问，当前节点版与父提交数组版都保持相同输出。7 次 runtime 中位数分别为
`3.334s` 和 `2.689s`；profile 指令数分别为 `6,899,787` 和 `8,538,281`，但
tracked allocation 分别为 `1,379,055` 和 `50,529`。节点版减少了解释器指令，却
没有带来 wall-clock 收益，因此当前不引入频率桶或索引后端；只有更大且真实的缓存
workload 显示稳定收益时再重开该表示决策。

## 7. 建议的目录布局

第一阶段可以保持现有单文件入口；达到 S2 后再按主题拆分，并由入口文件
显式导出稳定 API：

```text
library/
  README.md
  DATA_STRUCTURES_ROADMAP.md
  data_structures.cd          # 兼容入口/公共导出
  tests/                      # 独立库 fixture 和 runner
  sequence.cd                 # Deque、List、数组序列辅助
  heap.cd                     # BinaryHeap、PriorityQueue
  collections.cd              # Set、MultiSet、MultiMap、频率表
  tree.cd                     # Tree、BST、Trie、区间树
  graph.cd                    # Graph、WeightedGraph、DisjointSet
  algorithms.cd               # 搜索、排序、区间和窗口
  string_algorithms.cd
  numeric_algorithms.cd
  dynamic_programming.cd
examples/
  data_structures.cd
  algorithms.cd
```

拆分时保留 `data_structures.cd` 的已有导出，避免现有示例和调用者必须立即
改 import 路径。由于项目目前没有包清单，目录布局只是一组相对路径约定，
不是新的包系统承诺。

## 8. 每个实现切片的验收清单

- 有泛型 happy path、空集合、单元素、边界索引和重复元素用例；
- 有 `number`、`string`、结构体或嵌套数组等至少两类元素用例（不适用时写明
  原因）；
- 有别名观察原地修改、`snapshot` 浅拷贝和回调修改输入的用例；
- 有类型错误、非法索引、空结构和未找到结果的预期行为；
- 有 C++ 编译器运行结果和 `.cdbc` Rust VM 运行结果对照；
- 有 API README、源代码注释和一个可复制的 import 示例；
- 对排序、堆、图和区间树记录复杂度；
- 每个公开函数的参数/返回类型稳定后再加入入口模块的 `export`。

库 fixture 使用现有项目测试接口但不放入 `tests/golden` 或
`tests/bytecode_artifacts`；根项目测试和库测试必须分别执行。

## 9. 已确认的问题与当前决定

以下决定记录当前语言与库之间的边界；未实现的高级结构仍不应被文档视为
已有能力。

1. **泛型比较器和能力约束。** `fun(T, T): bool` 比较器在本地、跨模块、命名
   空间和重导出路径上保持稳定，继续作为库排序、堆和 BST 的通用 API。语言的
   `Eq`、`Ord` 和 `Hash` 是编译期能力约束；`Ord` 隐含 `Eq`，不生成运行时
   trait 对象或隐式能力字典。
2. **哈希。** Issue #11 已形成并实现确定性的 `Hash` 能力和 `hash(value)`；
   Issue #15 进一步固定 `Eq + Hash`、身份稳定的可变引用键和浅别名规则，并
   实现数组桶版 `HashSet<T>`、`HashMap<K,V>`。内置 map 仍限制为基础键，Bloom
   filter 和高性能节点版仍然延期。
3. **比较运算符。** Issue #10 已实现字符串的内置字典序和命名结构体的
   `<`、`<=`、`>`、`>=` 运算符。用户定义结构体的运算符不会自动满足泛型
   `T: Ord`，需要泛型算法时仍使用显式 comparator 包装；`==`、`!=` 及算术、
   逻辑、复合赋值运算符的用户定义版本仍是后续决策。
4. **递归结构体与节点句柄。** M8 已确定命名结构体的稳定强句柄契约，并以
   `LinkedNode<T>`/`LinkedList<T>` 提供可变单向链表。节点别名共享字段和链接；断链
   不删除外部句柄；identity equality 和 `<cycle>` 格式化保持跨 C++/Rust 一致。
   强引用环保留到 VM teardown，暂不提供 GC、弱引用、终结器或自动删除。`LinkedList<T>`
   现在额外保留私有尾节点句柄，使 `pushBack` 不再扫描整个链表；LRU 已采用
   私有单向节点句柄，并在淘汰/删除时清除链接；LFU、双向链表、循环链表和环安全
   遍历仍需各自的 API 决策。

   2026-08-02 的 `linked_structures_workload` 使用 1000 次尾插、500 次前端删除和
   500 次追加，旧版在线性扫描下超过默认 10,000,000 指令预算。统一使用
   `--unlimited` profile 时，旧版/尾句柄版分别为 17,578,780/106,782 条指令和
   3,518,623/23,122 次 tracked allocation；5 次 runtime 中位数约为 8.24s/0.18s，
   输出保持一致。因此尾句柄作为当前公开 API 的内部表示保留。
5. **环形缓冲区。** 当前策略是不覆盖最旧值，满时 `offer` 返回 `false`，读/窥视
   返回 `Option<T>`；后续若出现更合适的错误协议，再单独评估 API 迁移。
6. **失败和数值约定。** `optional<T>` 用于普通缺失，`Option<T>` 用于显式有/无结果，
   `Result<T,E>` 用于携带原因的预期失败。需要整数的 API 只接受数学意义上的
   整数 `number`，不承诺任意精度或溢出异常。
7. **图顶点。** 第一版图 API 使用非负整数顶点 ID，避开泛型顶点的哈希/编号
   契约；泛型顶点适配层留待后续。
8. **测试入口。** 库 fixture 位于 `library/tests`，由独立 runner 执行，不进入
   `tests/golden` 或 `tests/bytecode_artifacts`。每个库 fixture 只比较 Rust VM
   的 `run.out`，不比较 AST 或 bytecode 文本；根项目测试和库测试分别运行。

## 10. 后续语言边界

当前分支已完成可以由现有语言能力稳定表达的库实现。后续工作需要重新确定
语言或运行时契约的项目包括：

- 内置 map 的泛型键放宽、Bloom filter、高性能哈希节点和额外 map/runtime 表示；
- 双向/循环链表的节点所有权、遍历终止和稳定句柄 API；
- LFU 节点版迁移的 key ownership、淘汰和强引用环策略；LRU 当前使用私有单向节点，
  不暴露节点句柄，也不建立缓存内部强环；
- 让用户定义结构体运算符自动参与泛型 `T: Eq`/`T: Ord` 的静态 witness 或
  专门化规则；
- 位集、随机算法、并发结构和高性能缓存所需的额外标准库能力。

实现前述项目之前，库继续采用数组后端、递归枚举、整数顶点和显式比较器等
保守实现；剩余事项同步记录在 [`TODO.md`](TODO.md) 及项目语言决策文档中。
