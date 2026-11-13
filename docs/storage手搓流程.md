# storage.cpp 手搓流程（以 HeapFile 为例）

> 适用对象：`src/storage/storage.cpp` 里的 6 个桩函数（`BufferPool::FetchPage/NewPage/FlushPage/FlushAll`、`HeapFile::Insert/Get/Update/Remove`、`Iter::Next`）。
> 一句话原则：**先图后码、先正确后性能、一次只加一个能力，加完立刻自测。**

---

## 0. 先认清你手上有什么

打开 `src/storage/storage.cpp`，现在的状态是：

```cpp
Status HeapFile::Insert(const RecBytes& record, RID* out) {
  (void)record;
  if (out != nullptr) *out = RID{};
  return Status::kNotImplemented;   // ← 桩：接口已定，实现要你填
}
```

三个已经定死、**不能改**的东西：

| 已定死的 | 在哪 | 含义 |
|---|---|---|
| 页大小 8KB | `common.h: kPageSize` | 一页 8192 字节，不能按需变 |
| 记录定长 | `数据模型v0.md` | `orders` 216B，插入时必须恰好 `record_size_` 字节 |
| 定位靠 RID | `common.h: RID{page, slot}` | 拿到 (页号, 槽号) 就能算出偏移，不需要额外索引 |

**手搓第一步不是写代码，是回答三个问题**：页长什么样？一页放几行？哪些操作要标脏？答不上来就先别打开编辑器——storage 层的返工 90% 来自布局没想清。

---

## 1. 流程总览（7 步，每步都有"完成定义"）

| 步 | 动作 | 完成定义（达不到就别进下一步） | 预计 |
|---|---|---|---|
| S1 | 纸面设计页布局 + 算容量 | 能在纸上画出字节图，说出"一页 37 行"怎么算的 | 30–45 min |
| S2 | 只做 Insert + Get | 插 100 条、逐条读回、内容 100% 一致 | 1.5–2 h |
| S3 | 加顺序扫描迭代器 | 全表扫出 100 条，删掉的不出现 | 45 min |
| S4 | 接缓冲池（LRU + 脏页） | 页数超过池容量仍正确，Hits/Misses 有数 | 2–3 h |
| S5 | Update + Remove | 原地改、删后不可见、空间不回收但可复用槽 | 45 min |
| S6 | 持久化与关闭语义 | 关库再开库，数据还在 | 30 min |
| S7 | 扩 `m0_selftest` + 小性能测 | 新增用例全绿 + 插入/全扫耗时数字 | 1 h |

合计 ≈ 7 小时。建议**拆成两个半天**：S1–S3 一个半天（先看到"能读回来"），S4–S7 一个半天（再碰缓冲池——它最容易超时）。

---

## 2. S1 纸面设计：把布局画出来，别在脑子里想

### 2.1 页内字节图

```
 offset 0    ┌─────────────────────────────┐
             │ PageHeader                  │ 4B：slot_count(uint16) + free_low(uint16)
 offset 4    ├─────────────────────────────┤
             │ Slot[0]  offset,used        │ 4B
             │ Slot[1]  offset,used        │ 4B
             │ ...                         │ 槽目录向下生长
             ├─────────────────────────────┤
             │          空闲区              │
             ├─────────────────────────────┤
 free_low    │ 记录 n（216B，向上生长）     │
             │ ...                         │
 offset 8192 └─────────────────────────────┘
```

两条铁律：
- **槽目录从上往下长，记录从页尾往上长**，两者相撞 = 页满。
- **RID.slot 是槽目录下标，不是记录序号**；槽被删了 `used=0`，但下标永远不变（否则 RID 会失效）。

### 2.2 结构体和取值函数（照这个写，别自己改名）

```cpp
namespace {
constexpr uint16_t kSlotSize = 4;

struct PageHeader {       // 4B
  uint16_t slot_count;    // 已分配的槽数（含 used=0 的）
  uint16_t free_low;      // 记录区下界，新记录写在这里
};
struct SlotEntry {        // 4B
  uint16_t offset;
  uint16_t used;
};
static_assert(sizeof(PageHeader) == 4, "页头必须 4 字节，布局依赖它");
static_assert(sizeof(SlotEntry) == 4, "槽必须 4 字节");

inline PageHeader* Header(uint8_t* data) { return reinterpret_cast<PageHeader*>(data); }
inline SlotEntry* Slots(uint8_t* data) {
  return reinterpret_cast<SlotEntry*>(data + sizeof(PageHeader));
}
}  // namespace
```

### 2.3 亲手算一遍容量（答辩必问）

```
可用 = 8192 - 4（页头）         = 8188
每行 = 216（orders）+ 4（槽）    = 220
行数 = 8188 / 220              = 37.2 → 37 行
200000 行 → 200000/37 ≈ 5406 页 → 5406×8192 ≈ 44.3 MB
```

**拿笔算，别用计算器糊过去**——被问到"为什么不是 38 行"时，你要能说出"再加一行需要 220B，只剩 8188−37×220=48B，不够"。这个 48B 就是页内碎片的上限直觉。

---

## 3. S2 只做 Insert + Get：先证明"能读回来"

### 3.1 【原代码】

```cpp
Status HeapFile::Insert(const RecBytes& record, RID* out) {
  (void)record;
  if (out != nullptr) *out = RID{};
  return Status::kNotImplemented;
}
```

### 3.2 【修改】

```cpp
Status HeapFile::Insert(const RecBytes& record, RID* out) {
  if (record.size() != record_size_) return Status::kInvalidArgument;

  // 1) 已分配过的页里找空位（正确性优先；W2 再用 free-space map 提速）
  PageId pid = 0;
  for (; pid < num_pages_; ++pid) {
    Page* page = buffer_pool_->FetchPage(pid);
    if (page == nullptr) return Status::kIoError;

    SlotId slot = kInvalidSlot;
    if (IsOk(InsertIntoPage(page->Data(), record, record_size_, &slot))) {
      buffer_pool_->UnpinPage(page, /*dirty=*/true);
      ++num_records_;
      if (out != nullptr) *out = RID{pid, slot};
      return Status::kOk;
    }
    buffer_pool_->UnpinPage(page, /*dirty=*/false);  // 没写进去就一定不脏
  }

  // 2) 所有页都满 → 追加一页（NewPage 负责分配页号并返回指针）
  Page* page = buffer_pool_->NewPage(&pid);
  if (page == nullptr) return Status::kIoError;

  SlotId slot = kInvalidSlot;
  const Status st = InsertIntoPage(page->Data(), record, record_size_, &slot);
  buffer_pool_->UnpinPage(page, /*dirty=*/IsOk(st));
  if (!IsOk(st)) return st;                     // 空页都放不下 → record_size 设错了
  ++num_pages_;                                 // 页号只在一处自增，别和 BufferPool 各加一次
  ++num_records_;
  if (out != nullptr) *out = RID{pid, slot};
  return Status::kOk;
}
```

页内插入的辅助函数（**这段是核心，写错了后面全崩**）：

```cpp
namespace {
// 返回 kOk 时 *slot_out 有效；kFull 表示这页放不下
Status InsertIntoPage(uint8_t* data, const RecBytes& rec, uint16_t record_size,
                      SlotId* slot_out) {
  PageHeader* hdr = Header(data);

  // 1) 优先复用已删除的槽（槽下标不能变，所以只能复用 used=0 的）
  SlotId slot = kInvalidSlot;
  for (uint16_t i = 0; i < hdr->slot_count; ++i) {
    if (Slots(data)[i].used == 0) { slot = i; break; }
  }
  const uint16_t extra_slots = (slot == kInvalidSlot) ? 1 : 0;

  // 2) 算剩余空间：记录区下界 - (页头 + 槽目录 + 可能新增的槽)
  const uint16_t slot_area_end =
      static_cast<uint16_t>(sizeof(PageHeader) + (hdr->slot_count + extra_slots) * kSlotSize);
  if (hdr->free_low < static_cast<uint16_t>(slot_area_end + record_size)) {
    return Status::kFull;                       // 相撞 = 页满
  }

  // 3) 记录从 free_low 往上长
  const uint16_t offset = static_cast<uint16_t>(hdr->free_low - record_size);
  std::memcpy(data + offset, rec.data(), record_size);
  hdr->free_low = offset;

  if (slot == kInvalidSlot) slot = hdr->slot_count++;  // 新槽追加在目录尾
  Slots(data)[slot].offset = offset;
  Slots(data)[slot].used = 1;
  *slot_out = slot;
  return Status::kOk;
}
}  // namespace
```

`Get` 就简单了——**只读，绝不标脏**：

```cpp
Status HeapFile::Get(RID rid, RecBytes* out) const {
  if (!rid.IsValid() || rid.page >= num_pages_ || out == nullptr) return Status::kNotFound;
  Page* page = buffer_pool_->FetchPage(rid.page);
  if (page == nullptr) return Status::kIoError;

  Status st = Status::kNotFound;
  const uint8_t* data = page->Data();
  const PageHeader* hdr = reinterpret_cast<const PageHeader*>(data);
  if (rid.slot < hdr->slot_count) {
    const SlotEntry& s = Slots(const_cast<uint8_t*>(data))[rid.slot];
    if (s.used != 0) {
      out->assign(data + s.offset, data + s.offset + record_size_);
      st = Status::kOk;
    }
  }
  buffer_pool_->UnpinPage(page, false);
  return st;
}
```

### 3.3 【解释：三个容易写错的点】

1. **`UnpinPage` 的 dirty 参数必须和"这页到底改了没有"严格对应**。插入成功才 true，找不到空位就 false。写反了的后果是：脏页没刷盘 → 关库数据丢失（很难查，因为进程内读得到）。
2. **`record.size() != record_size_` 要早退**。定长方案下，多一字节少一字节都会让整页错位。
3. **`Get` 里 `FetchPage` 之后一定要 `UnpinPage`**。哪怕返回 `NotFound` 也要 unpin——这是 pin 泄漏的最常见来源。

### 3.4 这一段的"完成验证"（照抄就能跑）

```bash
cmake --build build -j && ./build/m0_selftest        # 现有 34 项先不能红
# 再在 m0_selftest 里加一段：
#   建 HeapFile(record_size=216) → 插 100 条 → 逐条 Get 比对 → 打印 num_records()
```

期望输出：`inserted=100 records=100`，且 100 条内容逐字节一致。

---

## 4. S3 加顺序扫描：执行器只认这一个入口

`HeapFile::Iter::Next` 的桩现在是 `return false;`。它的语义必须是：**跨页推进 + 跳过 used=0 的槽**。

```cpp
bool HeapFile::Iter::Next(RID* rid, RecBytes* record) {
  while (page_ < file_->num_pages_) {
    Page* page = file_->buffer_pool_->FetchPage(page_);
    if (page == nullptr) return false;

    const uint8_t* data = page->Data();
    const PageHeader* hdr = reinterpret_cast<const PageHeader*>(data);
    while (slot_ < hdr->slot_count) {
      const SlotId current = slot_++;
      const SlotEntry& s = Slots(const_cast<uint8_t*>(data))[current];
      if (s.used == 0) continue;                 // 删除的槽要跳过
      if (rid != nullptr) *rid = RID{page_, current};
      if (record != nullptr) record->assign(data + s.offset, data + s.offset + file_->record_size_);
      file_->buffer_pool_->UnpinPage(page, false);
      return true;                               // 一次只吐一条，迭代器语义
    }
    file_->buffer_pool_->UnpinPage(page, false);
    ++page_;
    slot_ = 0;
  }
  return false;                                  // 扫完
}
```

**为什么强调"一次只吐一条"**：执行器（W2）会按 `while (iter.Next(&rid,&rec))` 驱动，如果这里图省事一次塞一批，算子层就没法做阻塞/短路优化（`LIMIT 5` 会变成全表扫）。

---

## 5. S4 接缓冲池：坑最密的一步，先立 5 条不变量

`BufferPool` 的桩现在返回 `nullptr`。实现要点不是 LRU 链表怎么写，而是**守住这 5 条**：

| # | 不变量 | 违反后的症状 |
|---|---|---|
| 1 | `FetchPage` 命中/未命中都**必定 +1 pin** | 计数漂移 → 页被提前淘汰 → 读到脏数据 |
| 2 | `UnpinPage` 与 `FetchPage` **严格 1:1 配对** | pin 泄漏 → 池满后无可淘汰页 → 卡死或无限新建页 |
| 3 | **pin_count > 0 的页绝不能被淘汰** | 正在用的页被换出 → 数据错乱（最难查） |
| 4 | 淘汰脏页前**必须先刷盘**，再复用它 | 丢数据，且进程内看不出来 |
| 5 | Hits/Misses 只在 `FetchPage` 里统计，**一次请求记一次** | 性能报告数字虚高，答辩被戳穿 |

最小可用实现的结构（不追求花活）：

```cpp
// 页表 + LRU 链 + 磁盘文件，三者同增同减
std::unordered_map<PageId, std::unique_ptr<Page>> page_table_;
std::list<PageId> lru_;                       // 头部最新、尾部最旧
std::FILE* file_ = nullptr;                   // 或 fd；seek 到 id*kPageSize 读写整页

Page* BufferPool::FetchPage(PageId id) {
  auto it = page_table_.find(id);
  if (it != page_table_.end()) {              // 命中
    ++hits_;
    it->second->Pin();
    Touch(id);                                // 挪到 LRU 头
    return it->second.get();
  }
  ++misses_;
  Page* page = LoadOrCreate(id);              // 未命中：读盘 或 新页
  if (page == nullptr) return nullptr;
  page->Pin();
  return page;
}
```

`UnpinPage` 保持桩里已有的两行就够，但**不要**在这里做淘汰——淘汰只发生在需要新页且池满时，否则会出现"刚 unpin 就被换出、下一句又读回来"的抖动。

**调试手段**：在 `FetchPage/UnpinPage` 里打 debug 日志（`-DLOG_LEVEL=debug` 或临时打开），每次跑 100 条插入看 pin 计数是否收敛回 0。收敛不了就是泄漏，别往下写。

---

## 6. S5 Update / Remove：定长带来的两个"便宜"，和一处必须写清的取舍

- **Update 就地覆盖**：定长记录，偏移不变，直接 `memcpy` 回原 offset，标脏即可。RID 不变，索引不用动。
- **Remove 只置 `used=0`**：不改 `free_low`，不搬记录。槽下标保留，保证老 RID 依然"指向同一槽但已失效"（返回 NotFound，而不是指向别人）。

**必须写进文档的取舍**：我们不实现页内 compact。代价是碎片——最坏情况一页 37 个槽全删，却因为 `free_low` 卡在最上面而放不进一条新记录。缓解办法是 S2 里已经有的**槽复用**（复用 used=0 的槽不占新槽空间），日常删除-插入混合负载下碎片不会无限增长。

> 答辩若被问"碎片上限多少"，答案：单页最多浪费 `37 × 216 ≈ 8KB`（即整页不可用），但只有"删满一整页且不再插入该页"才会发生；我们接受这个上限，因为换成 compact 要重写所有 RID 与索引项，代价远大于收益。

---

## 7. S6 持久化与关闭：把"现在的一致性等级"说清楚

- `BufferPool` 析构前 `FlushAll()`（`Engine::Close` 已经调了，别删）。
- **`num_pages_` 要在打开文件时由文件大小推导**，不要只在内存里 ++：

```cpp
std::fseek(file_, 0, SEEK_END);
num_pages_ = static_cast<PageId>(std::ftell(file_) / kPageSize);   // 以文件为准
```

- **明确边界**：这一层只保证"正常关闭后数据完整"。进程被 `kill -9` 时，最后一次 `FlushAll` 之后的修改可能丢——那是 W2 的 WAL 要解决的问题，**不要在这里偷偷做半套日志**。文档里写一句"当前一致性等级：关库一致；崩溃恢复见 W2"，比含糊其辞强。

---

## 8. S7 验收：把"我觉得对"变成"数字说对"

在 `tests/m0_selftest.cpp` 里加一组 storage 用例（照现有 `Check` 宏的写法）：

| 用例 | 断言 |
|---|---|
| 插 100 条 216B | `NumRecords() == 100`，逐条 Get 字节一致 |
| 全扫 | 迭代器吐出正好 100 条，顺序稳定 |
| 删 10 条再扫 | 吐出 90 条，且被删的 oid 不出现 |
| 删 10 条再插 5 条 | 槽复用，`NumRecords() == 95` |
| 页边界 | 插到 38 条必然跨页（第 38 条落在 page 1），Get 仍正确 |
| 关库重开 | 重新构造 HeapFile，`NumRecords()` 与数据一致 |

小性能测（**这是性能报告 S1 的原始素材，别扔**）：

```
1000 条插入耗时 = ? ms
全表扫描 1000 条 = ? ms，平均 X μs/行
Hits/Misses = ? / ?，命中率 ?%
```

---

## 9. 每次提交前的 4 问（贴墙）

1. 编译过了吗？（`cmake --build build -j`）
2. 自检红了吗？（`./build/m0_selftest`，含新加用例）
3. **接口动了吗？**（动了 `include/db/storage.h` 就在周五走查会说，并同步 README 模块表）
4. 日志记了吗？（三行：做了什么、怎么验证的、踩了什么坑）

提交粒度参考（一个能力一个 commit，方便后面从 git log 生成"开发过程"章节）：

```
[storage] Page 头 + 槽目录 + HeapFile 插入/读取
[storage] 顺序扫描迭代器（跨页 + 跳过删除槽）
[storage] 缓冲池 LRU、脏页刷盘、命中率统计
[storage] 更新/删除：原地覆盖与槽复用（不做 compact 的取舍）
```

---

## 10. 常见 bug 对照表

| 症状 | 根因 | 修法 |
|---|---|---|
| 插第二条就覆盖第一条 | `free_low` 初始化成 0 或没减记录长度 | 初始化为 `kPageSize`，每次 `free_low -= record_size` |
| 读到旧数据 | 修改了页但 `UnpinPage(page,false)` | 判据只有一条：写没写进去 |
| 页满后新建页，重启数据只剩最后一页 | `num_pages_` 只在内存自增 | 打开时按文件长度推导 |
| 页数翻倍 / 越插越快跳到空页 | `BufferPool::NewPage` 和 `HeapFile` 各加了一次页号 | 页号只在一处自增，另一处读 `BufferPool::NumPages()` |
| 扫描跳过整页 | `Iter` 跨页时忘 `slot_ = 0` | 每次 `++page_` 后重置槽号 |
| 删除后扫描还返回该行 | `used=0` 写了但 `Next` 没检查 | 迭代器必须 `if (s.used == 0) continue;` |
| 跑一会儿卡死 / 内存暴涨 | pin 泄漏，池里全是 pin>0 的页 | 打印 pin 计数，配对检查；断言 `pin_count>=0` |
| 插 20 万行越来越慢 | 每次插入都从 page 0 线性找空位（O(n²)） | 加 `last_insert_page_` 提示；W2 再上 free-space map——**这正好是性能报告的第一个优化点** |

---

## 11. 顺序原则（术语化收口）

- **S1 不定布局，S2 不动代码**：页布局是所有 RID、迭代器、索引的共同前提，改一次等于三处返工。
- **先"能读回来"，再"读得快"**：S2/S3 只谈正确性，S4 才引入命中率；顺序反了会拿性能掩盖 bug。
- **脏页判据单一化**：只有"这次真的写进页了"才 dirty=true，不做"可能改了"的模糊判断。
- **边界写在文档里，不写在脑子里**：崩溃一致性、碎片上限、容量算术，三样都要落成文字（答辩就考这三样）。

---

## 12. storage 相关的答辩三问（自测）

1. 一页能放几行？怎么算的？页内碎片最多能有多少？
2. 删除一行做了什么？为什么不 compact？老 RID 怎么保证不指向别人？
3. 缓冲池满了淘汰谁？为什么 pin>0 的页不能淘汰？命中率是怎么统计出来的？

答不上任何一条，说明那一段还不是"你的"代码。
