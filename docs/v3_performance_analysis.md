# v3 Performance Analysis

## v3 架构简述

v3 仍然是 `ThreadCache` / `CentralCache` / `PageCache` 三层结构：

- `MemoryPool::allocate(size)` 只是入口，转发到当前线程的 `ThreadCache`。
- `ThreadCache` 使用 `thread_local` 本地 free list，命中时只做链表 pop，不访问全局锁。
- `CentralCache` 按 size class 维护中心 free list，并用每个 size class 独立的 `atomic_flag` 自旋锁保护。
- `PageCache` 以 page/span 为粒度向系统申请内存，底层使用 `mmap`。

完整分配链路：

`MemoryPool::allocate` -> `ThreadCache::allocate` -> `ThreadCache::fetchFromCentralCache` -> `CentralCache::fetchRange` -> `PageCache::allocateSpan` -> `mmap`

完整释放链路：

`MemoryPool::deallocate` -> `ThreadCache::deallocate` -> `ThreadCache::returnToCentralCache` -> `CentralCache::returnRange` -> `PageCache::deallocateSpan`

## v3 相比 v2 的变化

v3 的主要变化是引入了按对象大小动态调整的 batch refill：

- `ThreadCache::getBatchNum(size)` 根据 size class 计算一次从 CentralCache 获取多少块。
- `CentralCache::fetchRange(index, batchNum)` 接收 batch 数量，并可能根据中心链表实际可用数量修改 `batchNum`。
- 小对象会一次获取更多块，例如 32B 以内最多 64 个，64B 最多 32 个。
- 大对象会一次获取更少块，1024B 以内逐步降低，超过 1024B 的对象设计上每次只取 1 个。
- `returnToCentralCache` 仍然使用本地 free list 超过阈值后归还一部分的策略，阈值当前是固定 64。

这比 v2 更强调“按对象大小动态调整批量获取数量”。v2 的核心问题是 ThreadCache refill 的批量程度不足，v3 尝试通过小对象多拿、大对象少拿来减少中心锁访问和内存浪费之间的矛盾。

## 本次最小源码修正

阅读源码时发现两个会扭曲 v3 benchmark 的局部问题，因此做了最小修复：

- `ThreadCache::allocate` 原本在确认本地 free list 是否为空之前就执行 `freeListSize_[index]--`。当本地链表为空时会发生 `size_t` 下溢，导致本地缓存计数错误。
- `ThreadCache::getBatchNum` 原本使用 `std::max(sizeof(1), ...)`，`sizeof(1)` 通常是 4，不是 1。这会让设计上应该一次只取 1 个的大对象实际至少取 4 个。

修复后，`freeListSize_` 只在本地 pop 成功后递减，`getBatchNum` 的最小值改为 `size_t(1)`。

## 当前 benchmark 原本的问题

原 `v3/tests/PerformanceTest.cpp` 可以作为 smoke/perf demo，但不足以支撑 allocator 性能结论：

- 使用 `high_resolution_clock`，不是 benchmark 更常用的 `steady_clock`。
- 只对比 `new/delete`，没有单独对比 `malloc/free`。
- 没有多轮平均，单次波动会影响判断。
- 没有独立覆盖 fixed small object、batch alloc/free、repeated reuse、large bypass、refill pressure。
- 多线程场景使用 `rand()`，不够可复现。
- 输出没有 ratio，不方便判断 memory pool 相比 baseline 的输赢。
- 没有明确区分 Debug/Release 结论。

新增 `v3_benchmark` 后，Release benchmark 使用 `steady_clock`、warm up、多轮平均、保存指针、防优化写入，并分别输出 pool、malloc/free、new/delete 结果。

## 为什么 v3 理论上应优于 v2

v3 的理论收益来自动态 batch：

- 小对象更高频，单块内存成本低，一次多拿能显著减少 ThreadCache miss 后访问 CentralCache 的次数。
- 大对象单块成本高，一次少拿可以减少线程本地囤积和中心缓存中的内存浪费。
- `CentralCache::fetchRange(index, batchNum)` 可以按请求数量切分链表，而不是每次只返回一个块。
- 对 mixed size workload 来说，不同 size class 的 refill 压力不同，动态 batch 比固定策略更接近真实需求。

## 为什么 v3 仍可能不如 malloc/free

现代 malloc/free 已经非常强，通常也有线程缓存、size class、arena、fast bin/tcache 等机制。v3 是学习版实现，仍有明显成本：

- `CentralCache` 使用自旋锁，竞争时会消耗 CPU。
- `PageCache` 使用 `std::map` 和全局 mutex，span 管理不是低成本路径。
- `returnToCentralCache` 的阈值固定为 64，没有按 size class 动态调整。
- 小对象多拿能提升命中率，但也可能导致单线程持有过多空闲块。
- 大对象少拿减少浪费，但在 4096B 这类 refill pressure 场景中会增加访问 CentralCache 的频率，牺牲吞吐。
- benchmark 中的 `large bypass` 本质直接走 malloc/free，memory pool wrapper 不应被期待有明显优势。

## v3 适合的场景

v3 更适合：

- 高频小对象分配释放。
- 固定 size class 的本地复用。
- request context、scheduler node、token metadata、临时通信 buffer 等短生命周期对象。
- 多线程 mixed small size，对象大小仍然在 `MAX_BYTES` 以下。
- 需要用 batch refill 减少中心缓存访问的服务端路径。

## v3 不适合的场景

v3 不适合：

- 大对象分配，超过 `MAX_BYTES` 会直接走 malloc/free。
- 希望在所有场景下无条件击败系统 allocator 的泛化 benchmark。
- 线程数量很高且集中争抢同一 size class 的场景。
- 对内存占用峰值极敏感，但又允许 ThreadCache 囤积小对象的场景。
- 需要工业级碎片控制、span 回收和延迟分位数稳定性的生产 allocator。

## 可优化方向

建议后续按数据逐步优化，不要一次性大改：

1. 将 `shouldReturnToCentralCache` 的固定阈值改为按 size class 自适应，小对象阈值可以更高，大对象阈值应更低。
2. 给 `getBatchNum` 增加可观测统计，确认不同 size class 的 miss/refill 频率。
3. 优化 `returnToCentralCache`，避免归还太频繁或线程本地囤积过多。
4. 优化 `PageCache` 的 span 合并逻辑，目前主要尝试向后合并，仍可完善相邻 span 管理。
5. 减少 `CentralCache` 链表遍历和自旋等待成本。
6. 增加 P50/P95/P99 延迟和内存占用峰值统计，只看平均耗时还不够。
