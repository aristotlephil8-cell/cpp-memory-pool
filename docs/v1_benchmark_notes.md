# v1 Benchmark Notes

## 新 benchmark 设计

新增 `v1/tests/BenchmarkTest.cpp`，并在 `v1/CMakeLists.txt` 中新增 `v1_benchmark` 可执行文件。原 `MemoryPoolProject` 仍然只构建并运行原来的 `UnitTest.cpp`。

benchmark 使用 `std::chrono::steady_clock`，每组测试先 warm up，再运行多轮取平均值。核心循环中不打印，分配出的内存会写入数据，并把指针低位累加到 sink，避免编译器把分配结果优化掉。多线程测试中每个线程使用本地累加值，线程结束后再写一次全局 atomic sink，避免把全局 atomic 竞争混进核心分配循环。

输出列包括测试名称、对象大小、迭代次数、线程数、内存池耗时、malloc/free 耗时、operator new/delete 耗时，以及 baseline 与内存池的耗时比值。

比值含义：

- `malloc/pool > 1.0`：内存池比 malloc/free 快。
- `new/pool > 1.0`：内存池比 operator new/delete 快。
- 小于 1.0：内存池在该场景不占优势。

## 测试场景目的

1. **fixed immediate**：单线程固定大小小对象立即分配释放，覆盖 32B、64B、128B，测试 size class 命中和 free list 快速复用。
2. **batch alloc/free**：先批量分配 N 个对象保存到 vector，再统一释放，模拟请求对象生命周期不是立刻结束的场景。
3. **repeated reuse**：多轮批量分配和释放同一个 size class，观察 free list 复用是否能稳定发挥作用。
4. **multi-thread small**：2/4/8 个线程并发做小对象分配释放，观察 v1 的 atomic/CAS 和 block mutex 成本。该场景使用多个小对象 size class，避免 benchmark 因单一 free list 高竞争而崩溃。
5. **mixed small sizes**：在 8B、16B、32B、64B、128B、256B、512B 之间随机选择，模拟真实系统中多个小对象尺寸混合。
6. **large bypass**：使用 1024B 大对象，超过 `MAX_SLOT_SIZE`，验证大对象直接走系统分配，因此不应作为内存池优势场景。

## 三次 Release 运行结果

测试环境：WSL/Linux，`CMAKE_BUILD_TYPE=Release`。原始输出保存在：

- `docs/benchmark_results/v1_benchmark_run_1.txt`
- `docs/benchmark_results/v1_benchmark_run_2.txt`
- `docs/benchmark_results/v1_benchmark_run_3.txt`

下表为三次运行的平均值，单位为 ms。

| test | size | iterations | threads | pool_ms | malloc_ms | new_ms | malloc/pool | new/pool |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fixed immediate | 32 | 500000 | 1 | 9.100 | 2.911 | 4.747 | 0.320 | 0.522 |
| fixed immediate | 64 | 500000 | 1 | 8.732 | 2.971 | 4.765 | 0.340 | 0.546 |
| fixed immediate | 128 | 500000 | 1 | 8.595 | 2.981 | 5.022 | 0.347 | 0.584 |
| batch alloc/free | 32 | 200000 | 1 | 4.102 | 6.613 | 6.918 | 1.613 | 1.686 |
| batch alloc/free | 64 | 200000 | 1 | 4.558 | 9.342 | 9.191 | 2.054 | 2.017 |
| batch alloc/free | 128 | 200000 | 1 | 6.603 | 13.605 | 12.919 | 2.069 | 1.965 |
| repeated reuse | 64 | 1000000 | 1 | 18.854 | 12.051 | 16.082 | 0.640 | 0.855 |
| multi-thread small | mixed | 20000 | 2 | 0.664 | 0.284 | 0.346 | 0.428 | 0.521 |
| multi-thread small | mixed | 20000 | 4 | 0.591 | 0.297 | 0.477 | 0.501 | 0.831 |
| multi-thread small | mixed | 20000 | 8 | 0.984 | 0.595 | 0.841 | 0.602 | 0.867 |
| mixed small sizes | mixed | 300000 | 1 | 5.237 | 2.352 | 5.706 | 0.449 | 1.090 |
| large bypass | 1024 | 200000 | 1 | 2.624 | 1.198 | 2.330 | 0.456 | 0.888 |

## 结果分析

这组结果没有把内存池包装成“所有场景都更快”，而是更接近 v1 的真实特征：

- `batch alloc/free` 是 v1 表现最好的场景。32B、64B、128B 下，内存池相对 malloc/free 约为 1.6x、2.05x、2.07x，相对 operator new/delete 也约为 1.69x、2.02x、1.97x。
- `fixed immediate` 中，内存池明显慢于 malloc/free 和 operator new/delete。原因是每次分配释放都要走 HashBucket、atomic/CAS free list 路径，而现代 allocator 对这种小对象即时释放场景也有很强的小对象缓存优化。
- `repeated reuse` 没有表现出优势，说明 v1 的 atomic/CAS 成本和 benchmark 中的写内存成本仍然比较显著。它也提示“复用”不能只看次数，还要看路径开销是否足够低。
- `multi-thread small` 中，v1 没有领先。多线程测试使用 mixed size class，避免所有线程猛烈竞争同一个 free list；在早期同 size class 高竞争实验中，v1 benchmark 曾在更高并发下触发崩溃，提示该 lock-free stack 设计可能存在 ABA 或并发安全风险，后续应单独分析。
- `mixed small sizes` 中，内存池慢于 malloc/free，但略快于 operator new/delete。混合 size class 会让局部性和 free list 命中模式变复杂，优势不如固定大小批量场景明显。
- `large bypass` 慢于 malloc/free，接近但仍慢于 operator new/delete。因为超过 `MAX_SLOT_SIZE` 后 v1 本来就直接走 `operator new/delete`，这类场景不应该作为内存池优势宣传。

## 哪些场景体现内存池优势

本次最能体现 v1 优势的是单线程批量分配再批量释放，尤其是 64B 和 128B 小对象。这个场景更接近“请求对象或临时上下文批量创建，生命周期结束后集中释放”的服务端模式，内存池可以减少通用分配器路径成本，并从固定 size class 的连续 slot 分配中获益。

## 哪些场景内存池不占优势以及原因

内存池不占优势的场景包括即时分配释放、多线程 mixed size、混合小对象大小和大对象绕过。主要原因包括：

- v1 的 free list 使用 atomic/CAS，即使单线程也会付出原子操作成本。
- `HashBucket::useMemory/freeMemory` 每次都要做 size class 映射。
- 高并发下多个线程访问同一 free list 可能产生激烈 CAS 重试，甚至暴露 lock-free stack 的 ABA 风险。
- 现代 malloc/free 和 operator new/delete 对小对象已经有很强优化。
- 大对象本来就绕过内存池，因此不应期待内存池在该场景领先。

结论：内存池不是任何场景都比 malloc/free 快。v1 的优势依赖“高频小对象 + 固定 size class + 复用充分 + 测试场景匹配”。
