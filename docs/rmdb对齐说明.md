# 存储层对齐大赛 RMDB 框架（2026-11-13）

> 目的：把 M0 里"一个大 `BufferPool` 自己管文件"的结构，改成大赛 `src/storage` 的四件套
> `DiskManager / Page / BufferPoolManager / Replacer`，让我们写代码时的分层、命名、TODO 粒度都和赛题一致，
> 答辩时能对着 PPT 上的模块图直接讲。
> 差异不是随便省的：每一处"故意不一样"都写了理由，评委追问时照这个答。

---

## 1. 类与文件对照

| 大赛 RMDB | 我们的 mini-db | 说明 |
|---|---|---|
| `src/storage/disk_manager.h/.cpp` | `include/db/storage.h` + `src/storage/storage.cpp` 的 `DiskManager` | 文件开关、页读写、页号分配、日志文件 |
| `src/storage/page.h`（`PageId` / `Page`） | `PageKey` + `Page` | `PageKey{fd, page_no}` 相当于 `PageId{fd, page_no}`；`Page` 保留 `data_/is_dirty_/pin_count_` |
| `src/storage/buffer_pool_manager.h/.cpp` | `BufferPoolManager` | 字段一一对应：`pool_size_ / pages_ / page_table_ / free_list_ / disk_manager_ / replacer_ / latch_` |
| `src/replacer/replacer.h` + `lru_replacer.*` | `include/db/replacer.h` + `src/storage/replacer.cpp` | `Replacer` 抽象类 + `LRUReplacer`，接口原样：`Pin / Unpin / Victim / Size` |
| `src/record/`（Record 层，赛题里是表/记录组织） | `HeapFile` | 我们只做定长记录 + slotted page，属于降级路径，故自建一层 |

方法名对照（我们统一 CamelCase，语义与参数顺序照抄）：

| rmdb | mini-db |
|---|---|
| `write_page(fd, page_no, data, n)` / `read_page` | `WritePage` / `ReadPage`（返回 `Status`，见 §3.3） |
| `allocate_page(fd)` / `deallocate_page(page_no)` | `AllocatePage` / `DeallocatePage`（自增分配、空实现，与赛题给定代码一致） |
| `open_file / close_file / get_file_size / get_file_fd` | `OpenFile / CloseFile / FileFd`（文件大小走内部 `port::FileSize`） |
| `fetch_page(page_id)` | `FetchPage(fd, page_no)` |
| `unpin_page(page_id, is_dirty)` | `UnpinPage(fd, page_no, is_dirty)` |
| `flush_page / flush_all_pages / new_page / delete_page` | `FlushPage / FlushAll / NewPage / DeletePage` |
| `find_victim_page / update_page`（私有） | `FindVictimPage / UpdatePage`（私有） |
| `mark_dirty(page)`（静态） | `MarkDirty(page)`（静态） |

---

## 2. 现在的分层与调用链

```
         HeapFile（我们的 Record 层）
              │  Insert/Get/Update/Remove/Iter
              ▼
      BufferPoolManager  ──►  LRUReplacer（谁可以被换出）
        │        │
        │        └─ page_table_: {fd, page} → 帧号；free_list_：空闲帧；latch_：并发保护
        ▼
     DiskManager ──► 文件（data/orders.dat、data/drivers.dat …）
        │
        ▼
      Page（帧里的 8KB：page LSN + slotted page）
```

三条纪律（和赛题给的教学代码一致，别自作聪明改）：
1. **只有 DiskManager 碰文件**。上层任何地方直接 `fopen` 都是越权，W2 想插 WAL 会插不进去。
2. **只有 BufferPoolManager 管帧**。`Page` 不负责淘汰、不负责刷盘。
3. **只有 Replacer 决定淘汰谁**。缓冲池不许自己写 LRU 逻辑，否则换 CLOCK 时得改两处。

---

## 3. 五处"故意不一样"，以及理由

### 3.1 没把 `fd` 塞进 `RID`（改用 `PageKey` 只在缓冲池内部用）

rmdb 的 `PageId{fd, page_no}` 直接把文件句柄写进页标识，因为它的 Record 层拿着 `PageId` 到处走。
我们的 `RID{page, slot}` 要**落盘**（索引叶子存 RID、`order_events` 记 `(pid, oid)` 关联），而 **fd 是进程内句柄，重启就变**——把 fd 写进磁盘等于埋雷。
所以：`RID` 只存 (页号, 槽号)，fd 由 `HeapFile` 自己持有；缓冲池用 `PageKey{fd, page_no}` 做键，保证 9 张表各自的 page 0 不会互相顶掉。

> 顺带修掉 M0 的一个隐患：原来一个 `BufferPool` 全局自增页号，`orders` 和 `drivers` 的 page 0 会在页表里打架。现在每个文件独立分配页号（`fd2pageno_[fd]`），和赛题一样。

### 3.2 页大小 8KB（大赛是 4KB）

- 我们的记录大：`orders` 216B。4KB 一页只能放 18 行、8KB 放 37 行，槽目录（4B/条）的固定开销摊得更薄。
- 接口层不依赖具体值：`DiskManager` 用 `kPageSize` 算偏移，`Page` 用 `kPageSize` 开数组。
- **如果赛题评测要求 4KB**：改 `common.h` 一个常量即可，但 `数据模型v0.md` 的"37 行/页"和 `warmup_a_page` 的容量数字要重算（18 行/页），`docs` 里所有容量结论也要同步。
- 页内前 4 字节和大赛一样留给 `page LSN`（`Page::kLsnOffset = 0`、`kHeaderOffset = 4`）。这一点不能省：W2 加 WAL 时每页都要写 LSN，布局晚改就是全量返工。

### 3.3 用 `Status` 返回错误，而不是抛异常

赛题代码用 `throw InternalError / UnixError`。我们用 `Status`（`kIoError / kNotImplemented / kFull …`），原因：
- 与协议错误码、算子层返回值是同一条路径，`ERR 500 disk-error` 能一路传上去；
- 答辩时"每个失败分支都显式返回，没有异常穿透"比"我们 throw 了很多异常"更好讲。
- 代价：调用方可能忘检查返回值。所以 mock 纪律是——**桩函数一律返回失败并打 WARN，禁止静默返回成功**（本项目从头就守这条）。

### 3.4 一个根 `CMakeLists.txt` + `dbcore` 静态库，而不是每模块一个 `CMakeLists`

rmdb 用 `src/storage/CMakeLists.txt` 生成 `libstorage.a`，九个模块各一个。我们只有一个可执行 `db_server` 和几个测试，模块化成多个静态库收益低（还多一遍链接配置）。
但**目录结构照样按模块切**（`src/storage`、`src/index`、`src/query`…），等同于保留赛题的模块划分；将来要拆库，把根 CMake 里的 `file(GLOB …)` 换成 `add_library(storage STATIC src/storage/*.cpp)` 即可。

### 3.5 顺手记下上游代码的一个小 bug（答辩可以提一句）

`BufferPoolManager` 构造函数里用 `REPLACER_TYPE.compare("LRU")` 做判断，而 `std::string::compare` **返回 0 表示相等**，写在 `if` 里语义正好反了：任何配置都会建 `LRUReplacer`，CLOCK 分支永远走不到。
我们这边不复制这个坑，直接 `replacer_.reset(new LRUReplacer(pool_size_))` 并在注释里标"CLOCK 属加分项，W2 换实现"。

---

## 4. 待手搓清单（按赛题模块编号）

| 题号 | 函数 | 位置 | 预计 | 备注 |
|---|---|---|---|---|
| 题1-2 | `DiskManager::ReadPage / WritePage` | storage.cpp | 45 min | 越界页要清 0 |
| 题1-2 | `HeapFile::Insert / Get / Update / Remove` + `Iter::Next` | storage.cpp | 3–4 h | slotted page 布局 |
| 题1-1 | `FindVictimPage / UpdatePage / FetchPage / UnpinPage` | storage.cpp | 1.5 h | pin/脏页不变量 |
| 题1-1 | `FlushPage / NewPage / DeletePage / FlushAll` | storage.cpp | 1 h | 与 UpdatePage 复用 |
| 题1-1 | `LRUReplacer::Pin / Unpin / Victim` | replacer.cpp | 40 min | `where_` 存迭代器做 O(1) |
| 题4 | `DiskManager::ReadLog / WriteLog` | storage.cpp | — | W2 与 WAL 一起做 |

已实现、不需要动的：目录/文件管理、`OpenFile`（含按文件长度回填页数）、`AllocatePage`、`Page` 元信息与 page LSN、帧数组与页表构造、`Engine::Close` 的刷盘顺序。

---

## 5. 怎么用这套骨架贴近赛题评测

赛题的 judge 脚本我们看不到（不能凭空编造它的用例），能做的是**等价自测**，把评分维度拆成可判定的断言：

| 赛题关心的 | 我们的等价自测 |
|---|---|
| 页读写正确（题1-2） | 写 0xAB 满页 → 读回 `memcmp` 全等；读未分配页 → 全 0 |
| 缓冲池 pin 语义（题1-1） | 同一页 Fetch 两次 → pin_count==2；两次 Unpin → 归零；此时 `Victim` 才拿得到它 |
| 池满后的替换 | 池 4 帧、扫 6 页 → 数据仍全对，`Misses` 增长，`Size()==pool_size_` |
| 脏页不丢 | 改页后不 Flush 直接析构 → 重开文件读回，数据在 |
| 命中率（性能） | `Hits/(Hits+Misses)`，同一查询跑两次第二次应明显更高 |
| 并发（加分项） | 两个线程各 Fetch 不同页 → 无数据竞争（`latch_` + TSAN/压力跑） |

这些断言写进 `tests/` 后，等于我们自己有一份"赛题风格"的验收脚本；分数不完全可比，但**覆盖的失败模式是同一批**——这才是"用大作业备赛"的实质。

---

## 6. 本次改动记录

| 文件 | 改动 |
|---|---|
| `include/db/storage.h` | 拆成 `DiskManager / Page / BufferPoolManager / HeapFile`；`Page` 固定数组 + page LSN；缓冲池改为帧数组 + 页表 + `free_list_` + `replacer_` + `latch_` |
| `include/db/replacer.h`（新增） | `Replacer` 抽象类 + `LRUReplacer`，接口照赛题 |
| `src/storage/storage.cpp` | 重写：实现磁盘/文件/页号基础设施，其余按 `TODO(题1-x)` 编号留空并显式失败 |
| `src/storage/replacer.cpp`（新增） | `LRUReplacer` 三个函数留 TODO（与赛题给的状态一致） |
| `include/db/{index,query,server}.h`、`src/{index/query/server}/*.cpp` | `BufferPool` → `BufferPoolManager`（构造参数改为 `(pool_size, data_dir)`） |
| `docs/storage手搓流程.md` | 按新类与新页布局（LSN 占前 4B）重写 |

验证：5 个 target 全部编译通过（仅剩"桩成员未使用"的预期告警），`m0_selftest` 34/34 PASS，三个热身与 `db_server --stdio` 行为不变。

---

## 7. 下一步（W1 首日，和 storage 手搓流程 S0–S2 对齐）

1. A 写 `ReadPage/WritePage` → 单页读写自测（半天内必须绿）。
2. A 写 `HeapFile::Insert/Get` + 页头初始化 → 100 条单页插入读回。
3. B 同时把 `warmup/b` 的谓词解析扩成完整受限 SQL，不碰 storage。
4. C 同时写 `Server::Start`（thread-per-conn），不碰 storage。
