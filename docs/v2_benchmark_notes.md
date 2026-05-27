# v2 Benchmark Notes

## 新 benchmark 设计

新增 `v2/tests/BenchmarkTest.cpp`，并在 `v2/CMakeLists.txt` 中新增 `v2_benchmark` 目标。原有 `unit_test` 和 `perf_test` 保留。

benchmark 使用 `std::chrono::steady_clock`，每组测试先 warm up，再多轮平均。核心循环中不打印，分配到的内存会写入少量字节，并通过 sink 防止优化。多线程场景使用线程局部累加，结束后再写全局 atomic，避免统计变量数据竞争。

输出字段包括：

- test name
- object size
- iterations
- threads
- memory pool time
- malloc/free time
- operator new/delete time
- malloc/pool 和 new/pool speedup ratio

## 测试场景目的

1. **fixed immediate**：8B 到 4096B 固定小对象即时分配释放，观察 ThreadCache 命中路径。
2. **batch alloc/free**：分块批量分配再批量释放，模拟请求对象批量生命周期。
3. **repeated reuse**：同一 size class 多轮复用，观察 ThreadCache 本地缓存稳定性。
4. **multi same class**：2/4/8 线程分配释放同一 size class，观察 ThreadCache 是否减少全局锁竞争。
5. **mixed small sizes**：单线程 8B 到 4096B mixed size，模拟多类请求上下文。
6. **multi mixed sizes**：多线程 mixed size，模拟高并发服务中不同临时对象大小。
7. **refill pressure**：4096B 批量分配，观察 ThreadCache miss 和 CentralCache/PageCache refill 成本。
8. **large bypass**：`MAX_BYTES * 2` 大对象，验证大对象绕过内存池。
9. **long stress mixed**：长时间 mixed size 压力测试，观察稳定性。

说明：初版 benchmark 在一次性释放大量同 size class 对象时触发段错误，说明当前 v2 的 ThreadCache 归还 CentralCache 路径有稳定性风险。为了完成常规性能对比，最终 batch/warmup 场景采用 200 块分块释放，避免常规 benchmark 被该风险中断。该问题应作为后续源码分析重点。

## 三次 Release benchmark 结果摘要

测试环境：WSL/Linux，`CMAKE_BUILD_TYPE=Release`。原始输出保存在：

- `docs/benchmark_results/v2_benchmark_run_1.txt`
- `docs/benchmark_results/v2_benchmark_run_2.txt`
- `docs/benchmark_results/v2_benchmark_run_3.txt`

下表为三次运行平均值，单位为 ms。

| test | size | iterations | threads | pool_ms | malloc_ms | new_ms | malloc/pool | new/pool |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed immediate | 8 | 200000 | 1 | 0.592 | 1.221 | 2.117 | 2.062 | 3.575 |
| fixed immediate | 16 | 200000 | 1 | 0.639 | 1.238 | 1.985 | 1.937 | 3.104 |
| fixed immediate | 32 | 200000 | 1 | 0.641 | 1.199 | 2.095 | 1.871 | 3.270 |
| fixed immediate | 64 | 200000 | 1 | 0.656 | 1.190 | 2.022 | 1.817 | 3.085 |
| fixed immediate | 128 | 200000 | 1 | 0.640 | 1.187 | 2.045 | 1.856 | 3.196 |
| fixed immediate | 256 | 200000 | 1 | 0.641 | 1.193 | 2.072 | 1.860 | 3.234 |
| fixed immediate | 512 | 200000 | 1 | 0.652 | 1.238 | 2.013 | 1.898 | 3.090 |
| fixed immediate | 1024 | 200000 | 1 | 0.635 | 1.170 | 1.995 | 1.842 | 3.140 |
| fixed immediate | 4096 | 200000 | 1 | 0.650 | 2.671 | 3.445 | 4.112 | 5.303 |
| batch alloc/free | 64 | 80000 | 1 | 0.342 | 0.996 | 1.088 | 2.911 | 3.184 |
| batch alloc/free | 256 | 80000 | 1 | 0.501 | 1.530 | 1.584 | 3.068 | 3.177 |
| batch alloc/free | 1024 | 80000 | 1 | 0.514 | 11.360 | 11.338 | 22.167 | 22.099 |
| batch alloc/free | 4096 | 80000 | 1 | 1.034 | 63.567 | 62.592 | 61.491 | 60.581 |
| repeated reuse | 64 | 400000 | 1 | 1.137 | 3.356 | 4.729 | 2.955 | 4.157 |
| multi same class | 64 | 50000 | 2 | 0.609 | 0.510 | 0.689 | 0.854 | 1.162 |
| multi same class | 64 | 50000 | 4 | 0.625 | 0.704 | 0.804 | 1.131 | 1.298 |
| multi same class | 64 | 50000 | 8 | 1.287 | 1.395 | 1.603 | 1.082 | 1.250 |
| mixed small sizes | mixed | 200000 | 1 | 0.969 | 2.646 | 3.392 | 2.731 | 3.501 |
| multi mixed sizes | mixed | 100000 | 2 | 1.147 | 1.583 | 2.161 | 1.389 | 1.897 |
| multi mixed sizes | mixed | 50000 | 4 | 0.852 | 1.126 | 1.364 | 1.317 | 1.601 |
| multi mixed sizes | mixed | 25000 | 8 | 1.322 | 1.497 | 1.510 | 1.127 | 1.139 |
| refill pressure | 4096 | 60000 | 1 | 0.738 | 47.719 | 47.749 | 64.666 | 64.709 |
| large bypass | 524288 | 20000 | 1 | 0.634 | 0.647 | 0.674 | 1.021 | 1.064 |
| long stress mixed | mixed | 500000 | 1 | 2.462 | 6.699 | 8.867 | 2.725 | 3.608 |

## 哪些场景内存池更快

v2 在多数小对象场景中优于 malloc/free 和 operator new/delete：

- 固定小对象即时分配释放：8B 到 1024B 对 malloc/free 约 1.8x 到 2.1x，对 new/delete 约 3x。
- 4096B fixed immediate：对 malloc/free 约 4.1x。
- batch alloc/free：64B/256B 约 3x，1024B 和 4096B 因为系统分配成本更高，ratio 非常大。
- repeated reuse：64B 场景约 3x 快于 malloc/free。
- mixed small sizes 和 long stress mixed：约 2.7x 快于 malloc/free。
- multi mixed sizes：2/4/8 线程均领先，但 8 线程优势收窄。

## 哪些场景内存池更慢或优势不稳定

- `multi same class` 2 线程时内存池略慢于 malloc/free，4/8 线程才略领先。
- `large bypass` 基本和 malloc/free 接近，因为大对象直接走系统分配，wrapper 开销决定结果不会有显著优势。
- 一次性释放大量同 size class 对象曾触发崩溃，说明归还 CentralCache 路径需要单独修复和压力验证。

## 原因分析

v2 在小对象即时分配释放中表现好，主要是 ThreadCache 本地 free list 命中路径非常短，避免了系统 allocator 的通用路径。mixed small size 也表现不错，说明 size class 映射和 ThreadCache 复用在常规小对象场景中有效。

但 v2 当前仍有实现层面的短板：

- CentralCache 当前没有真正批量返回多个块给 ThreadCache，理论上的 refill 摊销优势没有完全发挥。
- ThreadCache 归还 CentralCache 的阈值固定，且大批量同 size class 释放存在稳定性风险。
- CentralCache 使用自旋锁，多线程同 size class 下优势不稳定。
- PageCache/SpanTracker 的线性扫描和固定容量会影响长时间运行的可扩展性。

## 是否建议优化源码

建议优化，但不要一次性大改。优先级：

1. 修复并验证 `ThreadCache::returnToCentralCache` / `CentralCache::returnRange` 在大批量同 size class 释放下的稳定性。
2. 让 `CentralCache::fetchRange` 真正返回一批块给 ThreadCache。
3. 让 ThreadCache 的批量获取和归还阈值按 size class 自适应。
4. 为 SpanTracker 建立更高效的地址索引，减少线性扫描。
5. 增加 P50/P95/P99 延迟统计，而不仅仅看平均耗时。
