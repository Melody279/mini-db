// 存储层接口（M0 冻结；M0.5 于 2026-11-13 对齐大赛 RMDB 框架）
//
// 与大赛 src/storage 的对应关系（同名同职责，只把命名风格改成工程统一的 CamelCase）：
//   rmdb 磁盘管理器  disk_manager.{h,cpp}    -> DiskManager
//   rmdb 页对象      page.h                  -> Page（4KB/8KB 差异见 docs/rmdb对齐说明.md）
//   rmdb 缓冲池      buffer_pool_manager.*   -> BufferPoolManager（帧数组 + 页表 + free_list + replacer + latch）
//   rmdb 替换策略    replacer/lru_replacer.* -> Replacer / LRUReplacer（见 db/replacer.h）
// 大赛没有 HeapFile（记录层是 Record/FileHandle），这一层是我们为"定长记录 + slotted page"自建的，
// 位置相当于 rmdb 的 Record 层，因此它也在本文件里。
//
// 约定：上层（catalog/query/index）只通过 HeapFile 与 BufferPoolManager 访问数据，绝不直接碰文件。
#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/common.h"
#include "db/replacer.h"

namespace db {

// 一页的全局标识：文件句柄 + 页号（等价于 rmdb 的 PageId{fd, page_no}）。
// 为什么不把 fd 塞进 RID：fd 是进程内句柄，重启就变，落盘时不能存它；
// 所以 RID 只存 (page_no, slot)，fd 由 HeapFile 自己持有，缓冲池用 PageKey 区分不同表的同名页号。
struct PageKey {
  int fd = -1;
  PageId page_no = kInvalidPageId;

  bool operator==(const PageKey& o) const { return fd == o.fd && page_no == o.page_no; }
};

struct PageKeyHash {
  size_t operator()(const PageKey& k) const {
    // fd 参与哈希，避免不同表的同号页在页表里打架
    return static_cast<size_t>(k.fd + 1) * 1000003u + static_cast<size_t>(k.page_no);
  }
};

// -------------------- 磁盘管理器（对应大赛 disk_manager）--------------------
// 职责：文件开关、页级读写、页号分配。所有落盘动作只能从这里出去，方便 W2 在中间插 WAL。
class DiskManager {
 public:
  explicit DiskManager(std::string data_dir);
  ~DiskManager();

  const std::string& DataDir() const { return data_dir_; }

  // 目录操作
  bool IsDir(const std::string& path) const;
  void CreateDir(const std::string& path);

  // 文件操作
  bool IsFile(const std::string& path) const;
  void CreateFile(const std::string& path);
  void DestroyFile(const std::string& path);
  int OpenFile(const std::string& path);   // 已打开则复用 fd；不存在则创建
  void CloseFile(int fd);
  std::string FileName(int fd) const;
  int FileFd(const std::string& path);     // 未打开则打开

  // 页级 IO（大赛题1-2 的核心，TODO）
  Status WritePage(int fd, PageId page_no, const char* data, int num_bytes);
  Status ReadPage(int fd, PageId page_no, char* out, int num_bytes);

  // 页号分配：大赛给定的是"简单自增"，已实现；打开文件时按文件长度回填，保证重启不重号
  PageId AllocatePage(int fd);
  void DeallocatePage(PageId page_no);
  PageId NumPages(int fd) const;           // 文件中已分配的页数（下一个可用页号）
  void SetNumPages(int fd, PageId num_pages);

  // 日志（W2 WAL 用，M0 只留接口）
  Status ReadLog(char* out, int size, int offset);
  Status WriteLog(const char* data, int size);
  int LogFd() const { return log_fd_; }
  void SetLogFd(int fd) { log_fd_ = fd; }

 private:
  std::string data_dir_;
  std::unordered_map<std::string, int> path2fd_;  // 路径 -> fd
  std::unordered_map<int, std::string> fd2path_;  // fd -> 路径
  std::unordered_map<int, PageId> fd2pageno_;     // fd -> 已分配页数（对应 rmdb fd2pageno_）
  int log_fd_ = -1;                               // WAL 文件句柄
};

// -------------------- 页（对应大赛 page.h）--------------------
// 一个 Page 就是一个帧的内容：8KB 数据 + 元信息。页里不认识记录，只提供字节缓冲。
// 页内前 4 字节留给 W2 的 page LSN（对齐 rmdb 的 OFFSET_LSN/OFFSET_PAGE_HDR 约定），
// 所以 slotted page 的页头要从第 4 字节开始写，别从 0 开始。
class Page {
  friend class BufferPoolManager;

 public:
  static constexpr size_t kLsnOffset = 0;     // page LSN（4B，W2 写入）
  static constexpr size_t kHeaderOffset = 4;  // slotted page 页头起点（对齐 rmdb）

  Page();

  int Fd() const { return fd_; }
  PageId PageNo() const { return page_no_; }

  char* GetData() { return data_; }
  const char* GetData() const { return data_; }
  static constexpr uint32_t Size() { return kPageSize; }

  bool IsDirty() const { return is_dirty_; }
  void MarkDirty() { is_dirty_ = true; }
  void ClearDirty() { is_dirty_ = false; }

  int PinCount() const { return pin_count_; }
  void Pin() { ++pin_count_; }
  void Unpin() {
    if (pin_count_ > 0) --pin_count_;
  }

  int32_t GetPageLsn() const;
  void SetPageLsn(int32_t lsn);

 private:
  void ResetMemory();  // 清 0 整页并复位元信息

  int fd_ = -1;                            // 所属文件（rmdb 把 fd 放在 PageId 里，我们单独存）
  PageId page_no_ = kInvalidPageId;        // 文件内页号
  char data_[kPageSize] = {};              // 固定数组，和 rmdb 的 char data_[PAGE_SIZE] 一致
  bool is_dirty_ = false;
  int pin_count_ = 0;
};

// -------------------- 缓冲池（对应大赛 buffer_pool_manager）--------------------
// 结构完全照 rmdb：pool_size_ 个帧的数组 + page_table（页 -> 帧）+ free_list（空闲帧）+ replacer + latch。
class BufferPoolManager {
 public:
  BufferPoolManager(size_t pool_size, std::string data_dir);
  ~BufferPoolManager();

  BufferPoolManager(const BufferPoolManager&) = delete;
  BufferPoolManager& operator=(const BufferPoolManager&) = delete;

  static void MarkDirty(Page* page) {
    if (page != nullptr) page->is_dirty_ = true;
  }

  // 拿到一页（返回时已 pin）。失败返回 nullptr。
  Page* FetchPage(int fd, PageId page_no);
  // 归还 pin；pin_count 归零则交给 replacer 变可淘汰
  bool UnpinPage(int fd, PageId page_no, bool is_dirty);
  // 写回一页（不要求在池中已被 pin，只要在页表里）
  bool FlushPage(int fd, PageId page_no);
  // 分配新页（页号由 DiskManager::AllocatePage 给）
  Page* NewPage(int fd, PageId* page_no_out);
  bool DeletePage(int fd, PageId page_no);
  // 关库/检查点用：把所有脏页刷下去；单文件版本供 W2 的 WAL checkpoint 用
  void FlushAll();
  void FlushAll(int fd);

  size_t PoolSize() const { return pool_size_; }
  size_t FreeFrames() const;               // 调试用：free_list_ 长度
  uint64_t Hits() const { return hits_; }
  uint64_t Misses() const { return misses_; }
  double HitRate() const {
    const uint64_t total = hits_ + misses_;
    return total == 0 ? 0.0 : static_cast<double>(hits_) / static_cast<double>(total);
  }
  DiskManager* Disk() { return disk_manager_.get(); }

 private:
  // TODO(题1-1)：先从 free_list_ 取，取不到再问 replacer_ 要一个可淘汰帧
  bool FindVictimPage(frame_id_t* frame_id);
  // TODO(题1-1)：脏页先刷盘 → 更新页表 → 重置新页元信息
  void UpdatePage(Page* page, int fd, PageId new_page_no, frame_id_t frame_id);
  frame_id_t FrameOf(int fd, PageId page_no) const;  // 页表查找，找不到返回 kInvalidFrameId

  size_t pool_size_;
  Page* pages_ = nullptr;                                       // 帧数组（连续内存，同 rmdb）
  std::unordered_map<PageKey, frame_id_t, PageKeyHash> page_table_;
  std::list<frame_id_t> free_list_;
  std::unique_ptr<DiskManager> disk_manager_;
  std::unique_ptr<Replacer> replacer_;                          // 当前固定 LRU（赛题可换 CLOCK）
  mutable std::mutex latch_;                                    // 保护页表/空闲链表/replacer
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
};

// -------------------- 堆文件（slotted page + 定长记录）--------------------
// 对应大赛 Record 层，但只支持定长记录（我们的降级路径），页内布局由 W1 手写：
//   [0,4)      page LSN（Page 预留）
class HeapFile {
 public:
  // path 是表文件名（如 data/orders.dat）；构造时通过 DiskManager 打开并按文件长度回填页数
  HeapFile(const std::string& path, BufferPoolManager* buffer_pool, uint16_t record_size);
  ~HeapFile();

  HeapFile(const HeapFile&) = delete;
  HeapFile& operator=(const HeapFile&) = delete;

  Status Insert(const RecBytes& record, RID* out);
  Status Get(RID rid, RecBytes* out) const;
  Status Update(RID rid, const RecBytes& record);
  Status Remove(RID rid);

  // 顺序扫描迭代器（SeqScan 算子的唯一入口）
  class Iter {
   public:
    explicit Iter(const HeapFile* file);
    bool Next(RID* rid, RecBytes* record);

   private:
    const HeapFile* file_;
    PageId page_ = 0;
    SlotId slot_ = 0;
  };

  Iter Scan() const { return Iter(this); }

  int Fd() const { return fd_; }
  uint16_t RecordSize() const { return record_size_; }
  PageId NumPages() const { return num_pages_; }
  uint64_t NumRecords() const { return num_records_; }

 private:
  std::string path_;
  BufferPoolManager* buffer_pool_;
  int fd_ = -1;
  uint16_t record_size_;
  PageId num_pages_ = 0;
  uint64_t num_records_ = 0;
};

}  // namespace db
