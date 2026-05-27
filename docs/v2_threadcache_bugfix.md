# v2 ThreadCache freeListSize_ Bugfix

## 问题背景

v2 引入了 `ThreadCache` / `CentralCache` / `PageCache` 三层缓存。`ThreadCache` 使用 `freeList_[index]` 保存当前线程本地可复用的小块内存，并使用 `freeListSize_[index]` 统计本地 free list 中的空闲块数量。

`freeListSize_[index]` 的准确性会影响 `shouldReturnToCentralCache(index)`。如果计数偏大，ThreadCache 会过早或错误地归还缓存；如果计数偏小，则可能长期持有过多本地块。

## 手动调试现象

调试 v2 时发现 `freeListSize_[index]` 在某些路径下会出现不符合真实链表长度的值。特别是在本地 free list 为空时进入 `allocate`，计数可能先被递减；在从 CentralCache 批量获取时，返回给用户的第一个块也可能被计入本地缓存数量。

## bug 触发路径

触发路径一：

1. `ThreadCache::allocate(size)` 计算 size class index。
2. 本地 `freeList_[index]` 为空。
3. 旧代码仍先执行 `freeListSize_[index]--`。
4. 如果原值是 0，`size_t` 下溢成极大的整数。
5. 后续 `shouldReturnToCentralCache(index)` 判断失真。

触发路径二：

1. `ThreadCache::fetchFromCentralCache(index)` 从 CentralCache 获取链表 `start`。
2. `start` 会作为 `result` 返回给用户。
3. 旧代码从 `start` 开始遍历统计 batch 数量。
4. 已经返回给用户的块被错误计入 ThreadCache 本地 free list。

## 修复前代码逻辑

```cpp
size_t index = SizeClass::getIndex(size);
freeListSize_[index]--;

if (void* ptr = freeList_[index])
{
    freeList_[index] = *reinterpret_cast<void**>(ptr);
    return ptr;
}
return fetchFromCentralCache(index);
```

```cpp
void* result = start;
freeList_[index] = *reinterpret_cast<void**>(start);

size_t batchNum = 0;
void* current = start;
while (current != nullptr)
{
    batchNum++;
    current = *reinterpret_cast<void**>(current);
}
freeListSize_[index] += batchNum;
```

## 修复后代码逻辑

```cpp
if (void* ptr = freeList_[index])
{
    freeList_[index] = *reinterpret_cast<void**>(ptr);
    freeListSize_[index]--;
    return ptr;
}
return fetchFromCentralCache(index);
```

```cpp
void* result = start;
freeList_[index] = *reinterpret_cast<void**>(start);

size_t batchNum = 0;
void* current = freeList_[index];
while (current != nullptr)
{
    batchNum++;
    current = *reinterpret_cast<void**>(current);
}
freeListSize_[index] += batchNum;
```

修复后的语义是：`freeListSize_[index]` 只表示当前线程本地 free list 中真实存在的空闲块数量。

## 为什么 size_t 会下溢

`size_t` 是无符号整数。当它的值为 0 时继续执行 `--`，结果不会变成 -1，而是根据无符号整数规则回绕到该类型能表示的最大值，例如 64 位平台上通常是 `18446744073709551615`。这会让后续阈值判断完全失真。

## 对缓存归还策略的影响

`shouldReturnToCentralCache(index)` 根据 `freeListSize_[index]` 判断是否需要把部分本地缓存归还给 CentralCache。计数下溢或虚高后，ThreadCache 会误以为本地缓存极多，导致归还策略被错误触发；而计数包含已返回给用户的块，也会让本地缓存状态和真实链表不一致。

## 回归测试结果

本次修复后已执行：

- `v1` Debug 构建并运行 `MemoryPoolProject`：通过。
- `v2/unit_test`：通过。
- `v2/perf_test`：可运行并完成输出。
- `v3/unit_test`：通过。
- `v3/perf_test`：可运行并完成输出。

性能结果会受机器状态和 WSL 环境影响，后续 benchmark 文档会再记录稳定数据。

## 面试表达版本

我在调试 v2 ThreadCache 时发现一个计数语义 bug：`freeListSize_` 应该表示线程本地 free list 中真实空闲块数量，但旧代码在本地链表为空时也会先递减，可能触发 `size_t` 下溢；同时从 CentralCache 批量获取时，会把已经返回给用户的第一个块也算入本地缓存。这个问题会影响是否归还 CentralCache 的阈值判断。我把递减移动到真实 pop 成功之后，并让 batch 统计从 `freeList_[index]` 开始，保证计数和真实本地链表一致。
