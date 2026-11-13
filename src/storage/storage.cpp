#include "db/storage.h"

#include <cstring>

namespace db {

Page::Page(PageId id) : id_(id), data_(kPageSize, 0) {}

// -------------------- BufferPool（M0 只冻结接口，W1 实现 LRU + 刷盘）--------------------
BufferPool::BufferPool(const std::string& data_dir, size_t capacity_pages)
    : data_dir_(data_dir), capacity_(capacity_pages) {
  LOG_INFO_MSG("BufferPool 初始化：data_dir=" + data_dir_ +
               " capacity=" + std::to_string(capacity_) + " pages");
}

BufferPool::~BufferPool() = default;

Page* BufferPool::FetchPage(PageId id) {
  (void)id;
  LOG_WARN_MSG("BufferPool::FetchPage 尚未实现（W1 交付）");
  return nullptr;
}

Page* BufferPool::NewPage(PageId* out_id) {
  if (out_id != nullptr) *out_id = kInvalidPageId;
  LOG_WARN_MSG("BufferPool::NewPage 尚未实现（W1 交付）");
  return nullptr;
}

void BufferPool::UnpinPage(Page* page, bool dirty) {
  if (page == nullptr) return;
  page->Unpin();
  if (dirty) page->MarkDirty();
}

bool BufferPool::FlushPage(PageId id) {
  (void)id;
  return false;
}

void BufferPool::FlushAll() {}

// -------------------- HeapFile（W1 实现 slotted page）--------------------
HeapFile::HeapFile(const std::string& path, BufferPool* buffer_pool, uint16_t record_size)
    : path_(path), buffer_pool_(buffer_pool), record_size_(record_size) {
  LOG_INFO_MSG("HeapFile 挂载：" + path_ + " record_size=" + std::to_string(record_size_));
}

Status HeapFile::Insert(const RecBytes& record, RID* out) {
  (void)record;
  if (out != nullptr) *out = RID{};
  return Status::kNotImplemented;
}

Status HeapFile::Get(RID rid, RecBytes* out) const {
  (void)rid;
  (void)out;
  return Status::kNotImplemented;
}

Status HeapFile::Update(RID rid, const RecBytes& record) {
  (void)rid;
  (void)record;
  return Status::kNotImplemented;
}

Status HeapFile::Remove(RID rid) {
  (void)rid;
  return Status::kNotImplemented;
}

HeapFile::Iter::Iter(const HeapFile* file) : file_(file) {}

bool HeapFile::Iter::Next(RID* rid, RecBytes* record) {
  (void)rid;
  (void)record;
  (void)file_;
  return false;
}

}  // namespace db
