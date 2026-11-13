// 索引层：B+ 树（M0 只冻结接口，W2 实现分裂/合并/范围扫描）
#include "db/index.h"

#include "db/storage.h"

namespace db {

struct BPlusTree::RangeIter::Impl {
  // W2 填充：当前叶页号、槽号、上界键
};

BPlusTree::BPlusTree(BufferPool* buffer_pool, uint16_t key_len, bool unique, PageId root)
    : buffer_pool_(buffer_pool), key_len_(key_len), unique_(unique), root_(root) {}

Status BPlusTree::Create(BufferPool* buffer_pool, uint16_t key_len, bool unique, PageId* root_out) {
  (void)buffer_pool;
  (void)key_len;
  (void)unique;
  if (root_out != nullptr) *root_out = kInvalidPageId;
  LOG_WARN_MSG("BPlusTree::Create 尚未实现（W2 交付，对应大赛题3）");
  return Status::kNotImplemented;
}

Status BPlusTree::Insert(Key key, RID rid) {
  (void)key;
  (void)rid;
  return Status::kNotImplemented;
}

Status BPlusTree::Search(Key key, RID* out) const {
  (void)key;
  if (out != nullptr) *out = RID{};
  return Status::kNotImplemented;
}

Status BPlusTree::Remove(Key key) {
  (void)key;
  return Status::kNotImplemented;
}

BPlusTree::RangeIter::RangeIter() = default;

bool BPlusTree::RangeIter::Valid() const { return impl_ != nullptr; }

bool BPlusTree::RangeIter::Next() { return false; }

Key BPlusTree::RangeIter::CurrentKey() const { return Key{}; }

RID BPlusTree::RangeIter::CurrentRid() const { return RID{}; }

BPlusTree::RangeIter BPlusTree::Scan(const Key* lo, const Key* hi) const {
  (void)lo;
  (void)hi;
  return RangeIter{};
}

BPlusTree::Stats BPlusTree::GetStats() const { return Stats{}; }

}  // namespace db
