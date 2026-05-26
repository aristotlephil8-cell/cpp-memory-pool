# 潜在 bug 分析

## P0：`ThreadCache::allocate()` 先减计数导致 `size_t` 下溢

### 位置

`src/ThreadCache.cpp:22-36`

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

### 触发路径

以 `MemoryPool::allocate(1024)` 为例：

| 步骤 | 变量变化 |
| --- | --- |
| 初始状态 | `freeList_[127] = nullptr`，`freeListSize_[127] = 0` |
| 进入 `allocate(1024)` | `index = 127` |
| 执行 `freeListSize_[127]--` | `0 -> SIZE_MAX`，即 `18446744073709551615` |
| 检查 `freeList_[127]` | 为空，进入 `fetchFromCentralCache(127)` |
| `fetchFromCentralCache` 当前通常统计 `batchNum = 1` | `SIZE_MAX + 1 -> 0`，可能绕回 |

这就是你手动调试看到 `freeListSize_[127]` 非常大的直接原因。

### 影响范围

| 影响点 | 说明 |
| --- | --- |
| 本地缓存计数 | `freeListSize_` 不再表示真实链表长度 |
| 归还策略 | `shouldReturnToCentralCache(index)` 基于错误计数判断，可能过早触发 |
| `returnToCentralCache()` | 把错误计数当作 `batchNum`，导致 `keepNum/returnNum` 失真 |
| 性能 | 可能频繁进入 CentralCache，增加锁竞争 |
| 稳定性 | 在极端计数下，链表切分循环可能执行异常多次或依赖提前遇到 `nullptr` 才退出 |

### 建议修复方式

把 `freeListSize_[index]--` 移动到本地 freeList 命中分支内部，只在真正从 ThreadCache 本地链表弹出一个块时减少计数：

```cpp
if (void* ptr = freeList_[index])
{
    freeList_[index] = *reinterpret_cast<void**>(ptr);
    freeListSize_[index]--;
    return ptr;
}

return fetchFromCentralCache(index);
```

如果要更稳健，可以加断言或保护：

```cpp
assert(freeListSize_[index] > 0);
```

不建议在 miss 路径减少 ThreadCache 的本地计数，因为 miss 时本地没有块被消耗。

## P0：`fetchFromCentralCache()` 把返回给用户的块也计入了 `freeListSize_`

### 位置

`src/ThreadCache.cpp:71-95`

```cpp
void* start = CentralCache::getInstance().fetchRange(index);
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
return result;
```

### 问题

如果 CentralCache 真正返回一段链表，`result = start` 是要交给用户的块，不应该计入 ThreadCache 本地空闲链表数量。ThreadCache 本地只留下 `start->next` 之后的块。

正确计数应该是：

```text
CentralCache 返回链表长度 = batchNum
返回给用户 = 1
挂到 ThreadCache = batchNum - 1
freeListSize_[index] += batchNum - 1
```

当前代码会多计 1。

### 当前实现下的特殊情况

`CentralCache::fetchRange()` 当前会把 `result->next` 置空，只返回单个块，剩余块留在 CentralCache。因此 `ThreadCache::fetchFromCentralCache()` 里 `batchNum` 通常为 1，`freeList_[index] = nullptr`。

这使得当前 bug 表现为：

| 层面 | 表现 |
| --- | --- |
| 设计意图 | ThreadCache 以为 CentralCache 批量返回 |
| 实际行为 | CentralCache 只返回 1 个块 |
| 计数影响 | ThreadCache 会给本地 freeList 增加 1，但本地实际没有空闲块 |
| 与下溢交互 | 首次 miss 的 `SIZE_MAX + 1` 可能绕回 0，掩盖下溢 |

### 建议修复方式

二选一，优先选方案 A。

| 方案 | 内容 | 优点 |
| --- | --- | --- |
| A | 修改 `CentralCache::fetchRange`，让它按批量返回链表给 ThreadCache；ThreadCache 计数 `batchNum - 1` | 符合 ThreadCache 批量缓存设计 |
| B | 保持 CentralCache 单块返回；ThreadCache 不设置本地 freeList，也不增加 `freeListSize_` | 改动小，但 ThreadCache miss 后没有批量收益 |

## P1：`returnToCentralCache()` 依赖错误计数，归还数量可能不准确

### 位置

`src/ThreadCache.cpp:98-145`

### 问题

`batchNum = freeListSize_[index]`，而不是遍历真实链表长度。只要 `freeListSize_` 曾经下溢或多计，`batchNum/keepNum/returnNum` 就会失真。

| 风险 | 说明 |
| --- | --- |
| 归还数量错误 | `returnNum * alignedSize` 传给 CentralCache，CentralCache 再推导 blockCount |
| 循环过长 | `keepNum` 极大时，循环依赖链表提前 `nullptr` 才停 |
| 本地计数继续错误 | 最后 `freeListSize_[index] = keepNum`，可能把错误扩大 |

### 建议修复方式

在 `returnToCentralCache()` 开始时遍历真实链表长度，并以真实长度为准：

```text
actualCount = countList(start)
batchNum = min(freeListSize_[index], actualCount)
```

更彻底的修法是保证所有 push/pop/fetch/return 都维护真实计数，然后用 debug assert 验证链长和计数一致。

## P1：CentralCache 的 `fetchRange()` 名称/语义不一致

### 位置

`src/CentralCache.cpp:37-138`

函数名叫 `fetchRange`，ThreadCache 注释也写“批量获取”，但实现只返回一个块。

| 代码行为 | 影响 |
| --- | --- |
| 新 span 切成多个 block | 是 |
| 返回给 ThreadCache 的链表 | 只有第一个块 |
| 剩余块位置 | 留在 `centralFreeList_[index]` |
| ThreadCache 批量缓存收益 | 基本没有 |

建议真正实现批量接口，例如返回 `[start, end, count]` 或通过输出参数返回实际数量，避免 ThreadCache 自己遍历未知链表。

## P1：`CentralCache::updateSpanFreeCount()` 可能重复累计空闲块

### 位置

`src/CentralCache.cpp:228-269`

`performDelayedReturn()` 遍历当前 `centralFreeList_` 统计每个 span 的空闲块数，然后调用：

```cpp
size_t oldFreeCount = tracker->freeCount.load(...);
size_t newFreeCount = oldFreeCount + newFreeBlocks;
tracker->freeCount.store(newFreeCount, ...);
```

如果 `freeCount` 已经代表当前中心缓存里的空闲数，再把扫描结果累加一次，就可能重复计数。更合理的语义是“用扫描值重算”或“只对新增归还块增量更新”，不要混用。

## P2：`PageCache::deallocateSpan()` 没有从 `spanMap_` 移除当前 span

### 位置

`src/PageCache.cpp:66-124`

释放 span 后把它插入 `freeSpans_`，但当前 span 地址仍保留在 `spanMap_` 中。这个 map 同时承担“已分配 span 查询”和“相邻 span 合并查询”，语义会变复杂。后续合并或重复释放时可能难以判断 span 状态。

## 优先修复顺序

| 优先级 | 问题 | 原因 |
| --- | --- | --- |
| 1 | `allocate()` 中 `freeListSize_[index]--` 位置错误 | 直接导致你观察到的 size_t 下溢，是计数系统失真的源头 |
| 2 | 明确 `fetchRange()` 是单块还是批量 | 决定 ThreadCache 是否真的具备 v2 的性能提升 |
| 3 | 修正 `fetchFromCentralCache()` 的计数 | 避免把用户块计入本地 freeList |
| 4 | `returnToCentralCache()` 使用真实链长校验 | 增强回收路径鲁棒性 |
| 5 | Central/Page 的 span 计数和 map 语义 | 属于后续稳定性和内存归还质量优化 |

## 和 AI Infra / 大模型推理服务的关系

这类计数 bug 在推理服务里很危险，因为 allocator 往往影响的是系统的“隐性性能路径”。一次请求可能没问题，但高并发、多线程、长时间运行后，错误计数会造成内存缓存策略失真：要么本地缓存过度膨胀，要么频繁回收到全局缓存。两者都会影响吞吐、P99 延迟和内存水位，是线上服务最难定位的一类问题。
