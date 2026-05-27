# v3 Benchmark Notes

## 新 benchmark 设计

新增 `v3/tests/BenchmarkTest.cpp`，并在 `v3/CMakeLists.txt` 中新增 `v3_benchmark` 目标。原有 `unit_test` 和 `perf_test` 保留。

benchmark 设计原则：

- 使用 `std::chrono::steady_clock`。
- 每组测试先 warm up，再多轮运行取平均。
- 核心循环中不打印。
- 使用 `vector<void*>` 保存指针，避免编译器优化掉分配。
- 对分配到的内存写入少量字节，并通过 `sink` 防止优化。
- 多线程测试中每个线程使用局部累加，结束后写入 atomic sink，避免统计数据竞争。
- Release 结果用于性能分析，Debug 只用于正确性和基本运行验证。
- 如实输出 memory pool、malloc/free、operator new/delete 和 speedup ratio。

## 测试场景目的

1. `fixed immediate`：8B、16B、32B、64B、128B、256B、512B、1024B、4096B 固定大小立即分配释放，观察 ThreadCache 命中路径。
2. `batch alloc/free`：先批量分配保存到 vector，再统一释放，模拟 request context 或 scheduler node 的批量生命周期。
3. `repeated reuse`：同一 size class 多轮分配释放，观察本地 free list 复用。
4. `multi same class`：2/4/8 线程同一 64B size class，观察 ThreadCache 是否减少锁竞争。
5. `mixed small sizes`：单线程混合 8B 到 4096B，模拟不同临时对象大小。
6. `multi mixed sizes`：多线程 mixed size，模拟推理服务中的 request metadata、token buffer、临时通信 buffer。
7. `refill pressure`：4096B 批量分配释放，观察动态 batch 下较大 size class 的 refill 成本。
8. `large bypass`：`MAX_BYTES * 2`，验证大对象直接走 malloc/free，不把它当作内存池优势场景。
9. `long stress mixed`：大量 mixed size 分配释放，观察稳定性和平均性能。

## 三次 Release benchmark 结果摘要

测试环境：WSL/Linux，`CMAKE_BUILD_TYPE=Release`。原始输出保存于：

- `docs/benchmark_results/v3_benchmark_run_1.txt`
- `docs/benchmark_results/v3_benchmark_run_2.txt`
- `docs/benchmark_results/v3_benchmark_run_3.txt`

下表为三次运行平均值，单位为 ms。`malloc/pool > 1.0` 表示 memory pool 快于 malloc/free。

| test | size | iterations | threads | pool_ms | malloc_ms | new_ms | malloc/pool | new/pool |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed immediate | 8 | 200000 | 1 | 0.537 | 1.187 | 2.146 | 2.209 | 3.997 |
| fixed immediate | 16 | 200000 | 1 | 0.640 | 1.179 | 2.094 | 1.842 | 3.272 |
| fixed immediate | 32 | 200000 | 1 | 0.630 | 1.191 | 2.077 | 1.890 | 3.295 |
| fixed immediate | 64 | 200000 | 1 | 0.659 | 1.202 | 2.035 | 1.827 | 3.095 |
| fixed immediate | 128 | 200000 | 1 | 0.652 | 1.204 | 2.139 | 1.851 | 3.280 |
| fixed immediate | 256 | 200000 | 1 | 0.674 | 1.204 | 2.080 | 1.795 | 3.104 |
| fixed immediate | 512 | 200000 | 1 | 0.630 | 1.180 | 2.006 | 1.871 | 3.181 |
| fixed immediate | 1024 | 200000 | 1 | 0.647 | 1.158 | 2.065 | 1.789 | 3.191 |
| fixed immediate | 4096 | 200000 | 1 | 0.677 | 2.737 | 3.402 | 4.052 | 5.042 |
| batch alloc/free | 64 | 80000 | 1 | 0.590 | 0.960 | 1.082 | 1.627 | 1.833 |
| batch alloc/free | 256 | 80000 | 1 | 0.524 | 1.478 | 1.614 | 2.830 | 3.084 |
| batch alloc/free | 1024 | 80000 | 1 | 0.790 | 11.835 | 11.190 | 15.117 | 14.245 |
| batch alloc/free | 4096 | 80000 | 1 | 1.340 | 63.280 | 61.692 | 47.409 | 46.222 |
| repeated reuse | 64 | 400000 | 1 | 2.534 | 3.553 | 4.906 | 1.404 | 1.937 |
| multi same class | 64 | 50000 | 2 | 0.508 | 0.561 | 0.723 | 1.103 | 1.421 |
| multi same class | 64 | 50000 | 4 | 0.659 | 0.686 | 0.917 | 1.038 | 1.402 |
| multi same class | 64 | 50000 | 8 | 1.337 | 1.332 | 1.695 | 0.996 | 1.274 |
| mixed small sizes | mixed | 200000 | 1 | 1.151 | 2.765 | 3.511 | 2.566 | 3.247 |
| multi mixed sizes | mixed | 100000 | 2 | 0.813 | 1.621 | 2.128 | 2.001 | 2.625 |
| multi mixed sizes | mixed | 50000 | 4 | 0.564 | 1.006 | 1.290 | 1.784 | 2.286 |
| multi mixed sizes | mixed | 25000 | 8 | 1.251 | 1.327 | 1.548 | 1.064 | 1.239 |
| refill pressure | 4096 | 60000 | 1 | 1.108 | 47.115 | 47.275 | 42.527 | 42.679 |
| large bypass | 524288 | 20000 | 1 | 0.615 | 0.631 | 0.633 | 1.026 | 1.028 |
| long stress mixed | mixed | 500000 | 1 | 2.251 | 6.658 | 8.723 | 2.956 | 3.874 |

## 哪些场景 v3 更快

v3 在大多数小对象场景中快于 malloc/free：

- fixed immediate 8B 到 1024B：约 1.8x 到 2.2x。
- fixed immediate 4096B：约 4.0x。
- batch alloc/free：64B/256B 有稳定优势，1024B/4096B 优势很大。
- repeated reuse：64B 约 1.4x。
- mixed small sizes：约 2.6x。
- multi mixed sizes：2/4/8 线程均快于 malloc/free，但 8 线程优势明显收窄。
- refill pressure 4096B：memory pool 仍显著快于 malloc/free。
- long stress mixed：约 3.0x。

## 哪些场景 v3 更慢或优势不明显

- `multi same class` 8 线程平均 `malloc/pool` 为 0.996，基本打平且略慢于 malloc/free。
- `large bypass` 平均 ratio 约 1.026，接近噪声范围。因为大对象直接走 malloc/free，memory pool 不应在这个场景声称明显优势。
- `multi same class` 2/4 线程虽然平均略快，但优势只有 1.1x 和 1.0x 左右，不算强。

## 原因分析

v3 的小对象固定大小和 mixed size 表现较好，说明 ThreadCache 本地命中路径有效，动态 batch refill 可以减少部分 CentralCache 访问。

但多线程同 size class 的优势不稳定，原因可能是：

- 多线程同时打到同一 size class 时，CentralCache 的 per-class 自旋锁仍会被集中竞争。
- ThreadCache 小对象一次多拿会提升命中率，但释放阶段超过阈值后仍可能触发 return path。
- 现代 malloc/free 自带线程缓存和 fast path，在单一小对象高频场景下并不弱。

4096B batch/refill 场景虽然绝对优势明显，但 v3 相比 v2 的原始结果并没有更快。原因是 v3 修正后对 1024B 以上对象一次只拿 1 个，更符合减少内存浪费的设计目标，但会增加较大对象 size class 的 CentralCache 访问频率。因此 v3 的动态 batch 收益更多体现在策略合理性和内存占用控制，而不是所有场景的吞吐都提升。

## v3 和 v2 思路上的差异

v2 更像“有三层缓存但 refill 策略较粗”的版本；v3 明确把 refill 数量和对象大小绑定：

- 小对象：多拿，减少中心缓存访问。
- 中等对象：逐步降低 batch。
- 大对象：少拿，减少线程本地囤积和内存浪费。

本次 benchmark 显示 v3 在常见小对象场景中保持优势，但并没有证明 v3 在所有场景都优于 v2。特别是较大 size class 的吞吐，动态 batch 可能用更少缓存换取更低内存浪费。

## 是否建议进一步优化源码

建议继续优化，但要基于数据小步推进：

1. 让归还阈值也按 size class 动态调整，而不是固定 64。
2. 给 benchmark 增加内存占用峰值，验证“大对象少拿”是否真的降低缓存占用。
3. 增加 CentralCache miss/refill/return 计数，用数据判断瓶颈。
4. 对多线程同 size class 做更细的锁竞争分析。
5. 后续再考虑 PageCache span 管理优化，不建议现在大改。
