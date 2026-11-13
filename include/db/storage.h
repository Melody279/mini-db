// 存储层接口（M0 冻结）
// 对应大赛题1（存储管理）+ 缓冲池。分页、槽目录、RID 定位、LRU 与脏页刷盘都在这一层。
// 约定：上层（catalog/query）只通过 HeapFile 与 BufferPool 访问数据，不直接碰文件。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/common.h"

namespace db {

// 一个内存页：8KB 字节缓冲 + 元信息（页号、脏位、pin 计数）
class Page {
 public:
  explicit Page(PageId id = kInvalidPageId);

  PageId Id() const { return id_; }
  void SetId(PageId id) { id_ = id; }

  uint8_t* Data() { return data_.data(); }
  const uint8_t* Data() const { return data_.data(); }
  static constexpr uint32_t Size() { return kPageSize; }

  bool IsDirty() const { return dirty_; }
  void MarkDirty() { dirty_ = true; }
  void ClearDirty() { dirty_ = false; }

  int PinCount() const { return pin_count_; }
  void Pin() { ++pin_count_; }
  void Unpin() { if (pin_count_ > 0) --pin_count_; }

 private:
  PageId id_;
  bool dirty_ = false;
  int pin_count_ = 0;
  std::vector<uint8_t> data_;
};

// 缓冲池：FetchPage 命中直接返回（已 pin），未命中从磁盘载入并做 LRU 替换。
// Hits/Misses 是性能报告 B3（缓冲命中率）的数据来源，必须真实统计。
class BufferPool {
 public:
  BufferPool(const std::string& data_dir, size_t capacity_pages);
  ~BufferPool();

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // 拿到一页（已 pin），调用方用完必须 UnpinPage
  Page* FetchPage(PageId id);

  // 在文件尾追加一页并返回页号
  Page* NewPage(PageId* out_id);

  // 归还 pin；dirty=true 表示内容被修改，淘汰时需先刷盘
  void UnpinPage(Page* page, bool dirty);

  bool FlushPage(PageId id);
  void FlushAll();

  size_t Capacity() const { return capacity_; }
  uint64_t Hits() const { return hits_; }
  uint64_t Misses() const { return misses_; }
  double HitRate() const {
    const uint64_t total = hits_ + misses_;
    return total == 0 ? 0.0 : static_cast<double>(hits_) / static_cast<double>(total);
  }
  PageId NumPages() const { return next_page_id_; }

 private:
  std::string data_dir_;
  size_t capacity_;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
  PageId next_page_id_ = 0;
  // LRU 链表、页表、磁盘文件句柄：W1 实现（M0 只冻结接口）
};

// 堆文件：定长记录 + slotted page（页头 + 槽目录 + 空闲区）
class HeapFile {
 public:
  HeapFile(const std::string& path, BufferPool* buffer_pool, uint16_t record_size);

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

  uint16_t RecordSize() const { return record_size_; }
  PageId NumPages() const { return num_pages_; }
  uint64_t NumRecords() const { return num_records_; }

 private:
  std::string path_;
  BufferPool* buffer_pool_;
  uint16_t record_size_;
  PageId num_pages_ = 0;
  uint64_t num_records_ = 0;
};

}  // namespace db
