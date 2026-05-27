# v1 Performance Analysis

## v1 架构简述

v1 是一个基础小对象内存池，核心组件包括：

- `Slot`：释放后的内存块会被解释成 `Slot`，其中内嵌 `next` 指针，用于串成 free list。
- `MemoryPool`：每个 `MemoryPool` 管理一种固定 slot size。它维护当前 block、未使用 slot 指针、free list 和 block 分配锁。
- `HashBucket`：按 8 字节对齐把请求大小映射到 64 个 bucket，每个 bucket 对应一个 `MemoryPool`。超过 `MAX_SLOT_SIZE` 的大对象直接走 `operator new/delete`。
- `allocate()`：优先从 free list 弹出复用块，free list 为空时从当前 block 切一个 slot，当前 block 用完后再 `allocateNewBlock()`。
- `deallocate()`：把释放块插回 free list。
- `newElement/deleteElement`：在内存池原始内存上做 placement new 和显式析构，提供对象级接口。

## 原测试为什么不充分

原 `v1/tests/UnitTest.cpp` 更像一个很小规模演示程序，而不是稳定 benchmark：

- 默认只运行 `BenchmarkMemoryPool(100, 1, 10)` 和 `BenchmarkNew(100, 1, 10)`，分配次数太少。
- 使用 `clock()` 计时，粒度和语义都不如 `std::chrono::steady_clock` 适合 wall-clock benchmark。
- 对象刚申请就释放，生命周期过短，不能覆盖批量分配后统一释放的真实服务场景。
- 测试规模太小，首次 block 分配、线程创建、计时误差都会显著影响结果。
- 只混合测试几个 C++ 对象，没有分清固定大小、批量释放、多线程、混合 size class、大对象绕过等场景。
- 输出文案里把 new/delete 和 malloc/free 混在一起，容易造成对比对象不清晰。

## 为什么内存池可能不如 malloc/free

内存池并不是所有场景都必然更快，v1 尤其如此：

1. **测试规模太小**：几百或几千次分配时，计时误差、首次 block 分配、线程创建成本都可能盖过真实分配差异。
2. **场景不匹配**：内存池适合高频、小对象、尺寸稳定、重复复用。如果测试没有充分复用，优势不明显。
3. **v1 自身有额外开销**：`HashBucket` index 计算、atomic free list、CAS 循环、block 分配 mutex、placement new 和显式析构都会带来成本。
4. **现代 malloc/free 已经很强**：glibc malloc、Windows allocator、tcmalloc/jemalloc 等通用分配器本身也会对小对象做线程缓存和快速路径优化。
5. **Debug 模式影响性能**：Debug 下优化不足，函数调用、断言、调试符号和未优化代码路径会让 benchmark 不可靠。
6. **计时代码和 I/O 干扰**：核心循环内不能打印，计时范围也应该只覆盖分配、写入和释放逻辑。

## 哪些场景更适合内存池

v1 更适合观察这些场景：

- 固定大小小对象，例如 32B、64B、128B。
- 大量重复 allocate/deallocate，且 size class 稳定。
- 先批量分配一批请求对象，再批量释放。
- 业务对象生命周期短但不是立即销毁，释放后很快被同 size class 复用。
- 小对象总量大，系统分配器元数据和通用路径成本占比较高。

## v1 不适合哪些场景

v1 不适合把所有分配场景都包进来比较：

- 极小规模 demo 循环，计时误差会比 allocator 差异更大。
- 单次申请后马上释放，且系统 allocator 已经命中自己的小对象缓存。
- 多线程高度竞争同一个 size class 的 free list，CAS 重试和 ABA 风险会削弱稳定性。
- 大对象分配，超过 `MAX_SLOT_SIZE` 后 v1 会直接绕过内存池。
- 对象尺寸高度随机且复用不足的负载，free list 的局部性和命中率都会下降。

## Debug 和 Release 测试差异

Debug 构建适合断点调试和验证逻辑，不适合做性能结论。Release 构建会启用编译器优化，更接近真实运行性能。本项目的 benchmark 应优先看 Release 结果，Debug 结果只用于确认程序能构建和运行。

## 后续优化方向

以下优化点可以考虑，但本次不直接修改 v1 内存池实现：

- 增加单线程专用 free list，与当前 atomic/CAS 版本对比。
- 调整 block size，减少频繁 `allocateNewBlock()` 的影响。
- 细化 `HashBucket` index 计算路径，降低每次分配的常数开销。
- 拆分对象构造析构成本和 raw memory allocation 成本。
- 增加 P50/P95/P99 延迟、内存占用和碎片率统计。
- 单独比较 Debug 与 Release、单线程与多线程、固定 size 与混合 size。
