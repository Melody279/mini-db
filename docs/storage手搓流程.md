# storage.cpp 手搓流程（以 HeapFile 为例）

> 适用对象：`src/storage/{storage.cpp, replacer.cpp}` 里所有标了 `TODO(题1-x)` 的函数。
> 存储层已按大赛 RMDB 的结构摆好（`DiskManager / Page / BufferPoolManager / LRUReplacer`，另加我们自己的 `HeapFile`），
> 类与类的对应关系、和赛题的差异，见 `docs/rmdb对齐说明.md`。
> 一句话原则：**先图后码、先正确后性能、一次只加一个能力，加完立刻自测。**

---

## 0. 你手上现在是什么

打开 `src/storage/storage.cpp`，会看到两种东西：

**已经写好的（不用你动）**：文件开关、目录创建、页号自增分配（`DiskManager::AllocatePage`，赛题也是给好的）、
`Page` 的元信息与 page LSN、帧数组与页表的构造、`OpenFile` 时按文件长度回填页数。

**留给你手搓的（都带编号 TODO + 显式失败返回）**：

```cpp
Page* BufferPoolManager::FetchPage(int fd, PageId page_no) {
  // TODO(题1-1 缓冲池)：
  // 1. 命中：FrameOf(fd, page_no) 找到帧，++hits_，Pin()，replacer_->Pin(frame)，返回
  // ...
  LOG_WARN_MSG("BufferPoolManager::FetchPage 尚未实现（W1 题1-1）");
  return nullptr;   // 注意：桩是"报错"不是"假成功"
}
```

三个已经定死、**不能改**的东西：

| 已定死的 | 在哪 | 含义 |
|---|---|---|
| 页大小 8KB | `common.h: kPageSize` | 一页 8192 字节，不能按需变 |
| 页内前 4 字节是 page LSN | `storage.h: Page::kLsnOffset` | 页头从第 4 字节开始，对齐大赛 `OFFSET_PAGE_HDR = 4` |
| 记录定长 + RID 定位 | `数据模型v0.md`、`common.h: RID{page, slot}` | `orders` 216B；RID 只存 (页号, 槽号)，fd 由 HeapFile 自己持有 |

**手搓第一步不是写代码，是回答四个问题**：页长什么样？一页放几行？哪些操作要标脏？谁负责 pin/unpin？
答不上来就先别打开编辑器——storage 层的返工 90% 来自布局和所有权没想清。

---

## 1. 流程总览（7 步，每步都有"完成定义"）

| 步 | 动作 | 完成定义（达不到就别进下一步） | 预计 |
|---|---|---|---|
| S0 | 补掉 `DiskManager::ReadPage/WritePage` | 写一页 → 读到同样的字节；读越界页 → 全 0 而不是脏内存 | 45 min |
| S1 | 纸面设计页布局 + 算容量 | 能画出字节图，说出"一页 37 行"怎么算的 | 30–45 min |
| S2 | `HeapFile::Insert` + `Get`，只走单页 | 插 100 条（都在 page 0）、逐条读回、字节一致 | 1.5–2 h |
| S3 | 加顺序扫描迭代器 | 全表扫出 100 条，删掉的不出现 | 45 min |
| S4 | `BufferPoolManager` 全套 + `LRUReplacer` | 页数超过池容量仍正确，Hits/Misses 有数，pin 计数收敛回 0 | 2–3 h |
| S5 | `Update` + `Remove` | 原地改、删后不可见、槽可复用、RID 不指向别人 | 45 min |
| S6 | 持久化与关闭语义 | 关库再开库，数据还在，页数一致 | 30 min |
| S7 | 扩 `m0_selftest` + 小性能测 | 新增用例全绿 + 插入/全扫/命中率数字 | 1 h |

合计 ≈ 8 小时。建议**拆成两个半天**：S0–S3 一个半天（先看到"能读回来"），S4–S7 一个半天（再碰缓冲池——它最容易超时）。

> 顺序是刻意这样排的：S0 先把磁盘打通，S2 先假设"所有页都在池里"把页内逻辑写对，
> S4 才引入淘汰与脏页。反过来先写缓冲池，会导致一页都插不进去就开始调 LRU，很难定位。

---

## 2. S0 先把磁盘层打通（题1-2 的一半）

`DiskManager::ReadPage/WritePage` 是唯一直接碰文件的地方，所有上层都从这里出去，所以先写它。

```cpp
Status DiskManager::WritePage(int fd, PageId page_no, const char* data, int num_bytes) {
  const off_t offset = static_cast<off_t>(page_no) * kPageSize;   // 页号 * 页大小 = 文件偏移
  if (lseek(fd, offset, SEEK_SET) < 0) return Status::kIoError;
  const ssize_t written = write(fd, data, static_cast<size_t>(num_bytes));
  if (written != num_bytes) return Status::kIoError;              // 短写就是失败，别当成功
  return Status::kOk;
}

Status DiskManager::ReadPage(int fd, PageId page_no, char* out, int num_bytes) {
  // 1) 页号 >= 已分配页数 → 这是"新页"，全 0 返回，绝不能让上层读到上一次的残留
  if (page_no >= NumPages(fd)) {
    std::memset(out, 0, static_cast<size_t>(num_bytes));
    return Status::kOk;
  }
  if (lseek(fd, static_cast<off_t>(page_no) * kPageSize, SEEK_SET) < 0) return Status::kIoError;
  const ssize_t got = read(fd, out, static_cast<size_t>(num_bytes));
  if (got < 0) return Status::kIoError;
  if (got < num_bytes) std::memset(out + got, 0, static_cast<size_t>(num_bytes - got));  // 短读补 0
  return Status::kOk;
}
```

两个验收动作（写进自测，别只用眼睛看）：

1. 写满一页 0xAB → `ReadPage` 读回，`memcmp` 全等。
2. `ReadPage` 读一个从没写过的页号 → 8192 字节全 0（这条最容易错，漏了就会读到上一条记录的残留）。

---

## 3. S1 纸面设计：把布局画出来，别在脑子里想

### 3.1 页内字节图（注意第 4 字节才开始是你的页头）

```
 offset 0    ┌─────────────────────────────┐
             │ page LSN（4B，W2 写）        │ ← Page::kLsnOffset，别动
 offset 4    ├─────────────────────────────┤
             │ PageHeader                  │ 4B：slot_count(uint16) + free_low(uint16)
 offset 8    ├─────────────────────────────┤
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

### 3.2 结构体和取值函数（放在 storage.cpp 的匿名 namespace 里）

```cpp
namespace {
constexpr uint16_t kSlotSize = 4;

struct PageHeader {       // 4B，位于 Page::kHeaderOffset
  uint16_t slot_count;    // 已分配的槽数（含 used=0 的）
  uint16_t free_low;      // 记录区下界，新记录写在这里
};
struct SlotEntry {        // 4B
  uint16_t offset;
  uint16_t used;
};
static_assert(sizeof(PageHeader) == 4, "页头必须 4 字节，布局依赖它");
static_assert(sizeof(SlotEntry) == 4, "槽必须 4 字节");

inline PageHeader* Header(char* data) {
  return reinterpret_cast<PageHeader*>(data + Page::kHeaderOffset);
}
inline SlotEntry* Slots(char* data) {
  return reinterpret_cast<SlotEntry*>(data + Page::kHeaderOffset + sizeof(PageHeader));
}
}  // namespace
```

### 3.3 亲手算一遍容量（答辩必问）

```
可用 = 8192 − 4（page LSN）− 4（页头）   = 8184
每行 = 216（orders）+ 4（槽）             = 220
行数 = 8184 / 220                        = 37.2 → 37 行
200000 行 → 200000/37 ≈ 5406 页 → 5406×8192 ≈ 44.3 MB
```

**拿笔算，别用计算器糊过去**——被问到"为什么不是 38 行"时，你要能说出"再加一行要 220B，只剩 8184−37×220 = 44B，不够"。这 44B 就是页内碎片的直觉上限。

---

## 4. S2 只做 Insert + Get：先证明"能读回来"

### 4.1 【原代码】

```cpp
Status HeapFile::Insert(const RecBytes& record, RID* out) {
  (void)record;
  if (out != nullptr) *out = RID{};
  LOG_WARN_MSG("HeapFile::Insert 尚未实现（W1）");
  return Status::kNotImplemented;
}
```

### 4.2 【修改】

```cpp
Status HeapFile::Insert(const RecBytes& record, RID* out) {
  if (record.size() != record_size_) return Status::kInvalidArgument;
  if (fd_ < 0 || buffer_pool_ == nullptr) return Status::kIoError;

  // 1) 已分配过的页里找空位（正确性优先；W2 再加 last_insert_page_ 提示）
  PageId pid = 0;
  for (; pid < num_pages_; ++pid) {
    Page* page = buffer_pool_->FetchPage(fd_, pid);
    if (page == nullptr) return Status::kIoError;

    SlotId slot = kInvalidSlot;
    if (IsOk(InsertIntoPage(page->GetData(), record, record_size_, &slot))) {
      buffer_pool_->UnpinPage(fd_, pid, /*is_dirty=*/true);   // 写进去了才脏
      ++num_records_;
      if (out != nullptr) *out = RID{pid, slot};
      return Status::kOk;
    }
    buffer_pool_->UnpinPage(fd_, pid, /*is_dirty=*/false);    // 没写进去就一定不脏
  }

  // 2) 所有页都满 → 追加一页（页号由 DiskManager::AllocatePage 给）
  PageId new_pid = kInvalidPageId;
  Page* page = buffer_pool_->NewPage(fd_, &new_pid);
  if (page == nullptr) return Status::kIoError;

  SlotId slot = kInvalidSlot;
  const Status st = InsertIntoPage(page->GetData(), record, record_size_, &slot);
  buffer_pool_->UnpinPage(fd_, new_pid, /*is_dirty=*/IsOk(st));
  if (!IsOk(st)) return st;          // 空页都放不下 → record_size 设错了
  ++num_pages_;                      // 页数只在这里维护；DiskManager 那边由 AllocatePage 自增
  ++num_records_;
  if (out != nullptr) *out = RID{new_pid, slot};
  return Status::kOk;
}
```

页内插入的辅助函数（**这段是核心，写错了后面全崩**）：

```cpp
namespace {
// 返回 kOk 时 *slot_out 有效；kFull 表示这页放不下
Status InsertIntoPage(char* data, const RecBytes& rec, uint16_t record_size, SlotId* slot_out) {
  PageHeader* hdr = Header(data);

  // 1) 优先复用已删除的槽（槽下标不能变，所以只能复用 used=0 的）
  SlotId slot = kInvalidSlot;
  for (uint16_t i = 0; i < hdr->slot_count; ++i) {
    if (Slots(data)[i].used == 0) { slot = i; break; }
  }
  const uint16_t extra_slots = (slot == kInvalidSlot) ? 1 : 0;

  // 2) 算剩余空间：记录区下界 −（page LSN + 页头 + 槽目录 + 可能新增的槽）
  const uint32_t slot_area_end = static_cast<uint32_t>(Page::kHeaderOffset) +
                                 sizeof(PageHeader) + (hdr->slot_count + extra_slots) * kSlotSize;
  if (hdr->free_low < slot_area_end + record_size) return Status::kFull;  // 相撞 = 页满

  // 3) 记录从 free_low 往上长
  const uint16_t offset = static_cast<uint16_t>(hdr->free_low - record_size);
  std::memcpy(data + offset, rec.data(), record_size);
  hdr->free_low = offset;

  if (slot == kInvalidSlot) slot = hdr->slot_count++;   // 新槽追加在目录尾
  Slots(data)[slot].offset = offset;
  Slots(data)[slot].used = 1;
  *slot_out = slot;
  return Status::kOk;
}
}  // namespace
```

初始化：`NewPage` 返回的页是全 0，所以第一次插入前要先把页头写成 `{slot_count=0, free_low=kPageSize}`。
这一步放在 `Insert` 里判断（`hdr->free_low == 0` 说明是全新页），别放在缓冲池里——缓冲池不认识 slotted page。

`Get` 就简单了——**只读，绝不标脏**：

```cpp
Status HeapFile::Get(RID rid, RecBytes* out) const {
  if (!rid.IsValid() || rid.page >= num_pages_ || out == nullptr) return Status::kNotFound;
  Page* page = buffer_pool_->FetchPage(fd_, rid.page);
  if (page == nullptr) return Status::kIoError;

  Status st = Status::kNotFound;
  const char* data = page->GetData();
  const PageHeader* hdr = reinterpret_cast<const PageHeader*>(data + Page::kHeaderOffset);
  if (rid.slot < hdr->slot_count) {
    const SlotEntry* s = Slots(const_cast<char*>(data)) + rid.slot;
    if (s->used != 0) {
      out->assign(data + s->offset, data + s->offset + record_size_);
      st = Status::kOk;
    }
  }
  buffer_pool_->UnpinPage(fd_, rid.page, /*is_dirty=*/false);
  return st;
}
```

### 4.3 【解释：三个容易写错的点】

1. **`UnpinPage` 的 is_dirty 必须和"这页到底改了没有"严格对应**。插入成功才 true。写反的后果是脏页没刷盘 → 关库丢数据（进程内还读得到，很难查）。
2. **`record.size() != record_size_` 要早退**。定长方案下，多一字节少一字节都会让整页错位。
3. **`Get` 里 `FetchPage` 之后一定要 `UnpinPage`**，哪怕返回 NotFound 也要 unpin——pin 泄漏最常见的来源。

### 4.4 这一段的"完成验证"

```bash
cmake --build build -j && ./build/m0_selftest     # 现有 34 项先不能红
# 再加 storage 用例：
#   HeapFile(data/orders.dat, pool, 216) → 插 100 条 → 逐条 Get 比对 → NumRecords()==100
```

---

## 5. S3 加顺序扫描：执行器只认这一个入口

`Iter::Next` 的桩现在 `return false;`。语义必须是：**跨页推进 + 跳过 used=0 的槽**。

```cpp
bool HeapFile::Iter::Next(RID* rid, RecBytes* record) {
  while (page_ < file_->NumPages()) {
    Page* page = file_->buffer_pool_->FetchPage(file_->fd_, page_);
    if (page == nullptr) return false;

    const char* data = page->GetData();
    const PageHeader* hdr = reinterpret_cast<const PageHeader*>(data + Page::kHeaderOffset);
    while (slot_ < hdr->slot_count) {
      const SlotId current = slot_++;
      const SlotEntry* s = Slots(const_cast<char*>(data)) + current;
      if (s->used == 0) continue;                 // 删除的槽要跳过
      if (rid != nullptr) *rid = RID{page_, current};
      if (record != nullptr) record->assign(data + s->offset, data + s->offset + file_->record_size_);
      file_->buffer_pool_->UnpinPage(file_->fd_, page_, false);
      return true;                                // 一次只吐一条
    }
    file_->buffer_pool_->UnpinPage(file_->fd_, page_, false);
    ++page_;
    slot_ = 0;                                    // 跨页必须重置槽号，漏了会跳过整页
  }
  return false;
}
```

**为什么强调"一次只吐一条"**：执行器按 `while (iter.Next(&rid,&rec))` 驱动，一次塞一批就没法做短路（`LIMIT 5` 会退化成全表扫）。

---

## 6. S4 缓冲池：坑最密的一步，先立 5 条不变量

`BufferPoolManager` 的结构照大赛：`pages_`（帧数组）+ `page_table_`（`{fd,page}` → 帧）+ `free_list_` + `replacer_` + `latch_`。
你要写的是 `FindVictimPage / UpdatePage / FetchPage / UnpinPage / FlushPage / NewPage / DeletePage / FlushAll` 和 `LRUReplacer` 的三个函数。

实现要点不是 LRU 链表怎么写，而是**守住这 5 条**：

| # | 不变量 | 违反后的症状 |
|---|---|---|
| 1 | `FetchPage` 命中/未命中都**必定 +1 pin** | 计数漂移 → 页被提前淘汰 → 读到脏数据 |
| 2 | `UnpinPage` 与 `FetchPage` **严格 1:1 配对** | pin 泄漏 → 池满后无可淘汰帧 → 卡死或无限新建页 |
| 3 | **pin_count > 0 的帧绝不能被淘汰**（`Victim` 只从 replacer 的可替换集合里取） | 正在用的页被换出 → 数据错乱（最难查） |
| 4 | 淘汰脏页前**必须先刷盘**，成功后才 `ClearDirty` | 丢数据，且进程内看不出来 |
| 5 | Hits/Misses 只在 `FetchPage` 里各加一次 | 性能报告数字虚高，答辩被戳穿 |

关键片段（`UpdatePage` 是所有换页动作的唯一出口）：

```cpp
void BufferPoolManager::UpdatePage(Page* page, int fd, PageId new_page_no, frame_id_t frame_id) {
  if (page->IsDirty()) {                                  // 1) 脏页先落盘
    const Status st = disk_manager_->WritePage(page->Fd(), page->PageNo(),
                                               page->GetData(), kPageSize);
    if (!IsOk(st)) { LOG_ERROR_MSG("换页刷盘失败"); return; }   // 失败保持脏位，别丢数据
    page->ClearDirty();
  }
  page_table_.erase(PageKey{page->Fd(), page->PageNo()});  // 2) 摘旧键
  page->ResetMemory();                                   // 3) 复位（ResetMemory 是 Page 的私有函数，
  page->fd_ = fd;                                        //    BufferPoolManager 是它的 friend）
  page->page_no_ = new_page_no;
  page_table_[PageKey{fd, new_page_no}] = frame_id;       // 4) 挂新键
}
```

`LRUReplacer` 三个函数就三件事：`Pin` 从集合里摘掉、`Unpin` 追加到表头、`Victim` 取表尾。`where_` 存迭代器是为了 O(1) 摘除。

**调试手段**：在 `FetchPage/UnpinPage` 打 debug 日志，跑 100 条插入后确认"所有帧 pin_count 归零、`replacer_->Size() == pool_size_`"。不收敛就是泄漏，别往下写。

---

## 7. S5 Update / Remove：定长带来的两个"便宜"，和一处必须写清的取舍

- **Update 原地覆盖**：偏移不变，直接 `memcpy` 回原 offset，`UnpinPage(dirty=true)` 即可；RID 不变，索引不用动。
- **Remove 只置 `used=0`**：不动 `free_low`，不搬记录。槽下标保留，老 RID 依然"指向同一槽但已失效"（返回 NotFound，而不是指向别人）。

**必须写进文档的取舍**：不实现页内 compact。代价是碎片——最坏情况一页 37 个槽全删，却因为 `free_low` 卡在最上面而放不进新记录。缓解靠槽复用（复用 used=0 的槽不占新槽空间），日常删插混合负载下碎片不会无限增长。

> 答辩若被问"碎片上限多少"：单页最多浪费 `37 × 216 ≈ 8KB`（整页不可用），但只有"删满一整页且不再插入该页"才会发生。换成 compact 要重写所有 RID 与索引项，代价远大于收益——这句话要能一口气说完。

---

## 8. S6 持久化与关闭：把"现在的一致性等级"说清楚

- `HeapFile` 构造时 `num_pages_ = Disk()->NumPages(fd_)`，**以文件长度为准**（这一步已在 `DiskManager::OpenFile` 里按 `文件大小 / kPageSize` 回填）。
- `Engine::Close()` 的顺序是 `FlushAll()` → 析构 `HeapFile`（关 fd）。顺序反了会出现"页还没刷、文件已关"。
- **明确边界**：这一层只保证"正常关闭后数据完整"。`kill -9` 后最后一次 `FlushAll` 之后的修改可能丢——那是 W2 的 WAL 要解决的，**不要在这里偷偷做半套日志**。文档里写一句"当前一致性等级：关库一致；崩溃恢复见 W2"，比含糊其辞强。

---

## 9. S7 验收：把"我觉得对"变成"数字说对"

在 `tests/m0_selftest.cpp` 里加一组 storage 用例（照现有 `Check` 宏的写法）：

| 用例 | 断言 |
|---|---|
| 插 100 条 216B | `NumRecords()==100`，逐条 Get 字节一致 |
| 全扫 | 迭代器正好吐 100 条，顺序稳定 |
| 删 10 条再扫 | 吐 90 条，被删的 oid 不出现 |
| 删 10 条再插 5 条 | 槽复用，`NumRecords()==95` |
| 页边界 | 插到第 38 条必落 page 1，Get 仍正确 |
| 页池压力 | 缓冲池设 4 帧、插 200 条（约 6 页），数据仍全对，`HitRate>0` |
| 关库重开 | 重新构造 HeapFile，`NumPages()` 与记录数一致 |

小性能测（**性能报告 S1 的原始素材，别扔**）：

```
1000 条插入耗时 = ? ms
全表扫描 1000 条 = ? ms，平均 X μs/行
Hits/Misses = ? / ?，命中率 ?%
```

---

## 10. 每次提交前的 4 问（贴墙）

1. 编译过了吗？（`cmake --build build -j`）
2. 自检红了吗？（`./build/m0_selftest`，含新加用例）
3. **接口动了吗？**（动了 `include/db/storage.h` 就在周五走查会说，并同步 README 模块表）
4. 日志记了吗？（三行：做了什么、怎么验证的、踩了什么坑）

提交粒度参考（一个能力一个 commit，方便从 git log 生成"开发过程"章节）：

```
[storage] DiskManager 页读写 + 越界页清零
[storage] slotted page 布局与 HeapFile 插入/读取
[storage] 顺序扫描迭代器（跨页 + 跳过删除槽）
[storage] BufferPoolManager 与 LRUReplacer（帧/页表/free_list/脏页）
[storage] 更新/删除：原地覆盖与槽复用（不做 compact 的取舍）
```

---

## 11. 常见 bug 对照表

| 症状 | 根因 | 修法 |
|---|---|---|
| 插第二条就覆盖第一条 | `free_low` 没初始化成 `kPageSize` | 新页首次插入前写页头 `{0, kPageSize}` |
| 读到上一条记录的残留 | `ReadPage` 越界页没清 0 | 页号 ≥ `NumPages` 时 `memset` 后返回 kOk |
| 读到旧数据 | 改了页但 `UnpinPage(fd, pg, false)` | 判据只有一条：写没写进去 |
| 页满后新建页，重启数据只剩最后一页 | 页数只在内存自增 | 打开时按文件长度回填（已在 `OpenFile` 实现） |
| 页数翻倍 / 插着插着跳到空页 | `BufferPool::NewPage` 和 `HeapFile` 各加了一次页号 | 页号只在 `AllocatePage` 自增；`HeapFile` 只维护 `num_pages_` |
| 扫描跳过整页 | `Iter` 跨页时忘 `slot_ = 0` | 每次 `++page_` 后重置槽号 |
| 删除后扫描还返回该行 | `used=0` 写了但 `Next` 没检查 | 迭代器必须 `if (s->used == 0) continue;` |
| 跑一会儿卡死 / 内存暴涨 | pin 泄漏，池里全是 pin>0 的帧 | 打印 pin 计数，严格配对；断言 `pin_count>=0` |
| 命中率一直是 0 或 >100% | 统计加在了子函数里，或被重复计 | 只在 `FetchPage` 主路径各加一次 |
| 插 20 万行越来越慢 | 每次插入都从 page 0 线性找空位（O(n²)） | 加 `last_insert_page_` 提示；W2 再上 free-space map——**这是性能报告第一个优化点** |

---

## 12. 顺序原则（术语化收口）

- **S1 不定布局，S2 不动代码**：页布局是所有 RID、迭代器、索引的共同前提，改一次等于三处返工。
- **先"能读回来"，再"读得快"**：S2/S3 只谈正确性，S4 才引入命中率；顺序反了会拿性能掩盖 bug。
- **脏页判据单一化**：只有"这次真的写进页了"才 dirty=true，不做"可能改了"的模糊判断。
- **pin/unpin 是所有权，不是提示**：谁 Fetch 谁 Unpin，跨函数传递要写清。
- **边界写在文档里，不写在脑子里**：崩溃一致性、碎片上限、容量算术，三样都要落成文字（答辩就考这三样）。

---

## 13. storage 相关的答辩三问（自测）

1. 一页能放几行？怎么算的？页内碎片最多能有多少？
2. 删除一行做了什么？为什么不 compact？老 RID 怎么保证不指向别人？
3. 缓冲池满了淘汰谁？为什么 pin>0 的帧不能淘汰？命中率是怎么统计出来的？

答不上任何一条，说明那一段还不是"你的"代码。
