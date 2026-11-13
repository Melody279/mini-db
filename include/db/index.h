// 索引层接口（M0 冻结）
// 对应大赛题3（唯一索引）：键为定长字节串（由行内容序列化而来），比较用 memcmp，天然支持复合索引。
// 树的每个节点都是一页（复用存储层），叶子存 (key, RID)，内节点存 (key, child_page)。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "db/common.h"

namespace db {

class BufferPoolManager;

// 键视图：不自持内存，调用方保证 key 在本次调用期间有效
struct Key {
  const uint8_t* data = nullptr;
  uint16_t len = 0;
};

class BPlusTree {
 public:
  BPlusTree(BufferPoolManager* buffer_pool, uint16_t key_len, bool unique, PageId root);

  // 新建一棵空树，返回根页号
  static Status Create(BufferPoolManager* buffer_pool, uint16_t key_len, bool unique, PageId* root_out);

  // unique=true 时重复键返回 Status::kDuplicate
  Status Insert(Key key, RID rid);
  Status Search(Key key, RID* out) const;
  Status Remove(Key key);

  // 范围扫描：[lo, hi]，lo/hi 传 nullptr 表示该端无界
  class RangeIter {
   public:
    RangeIter();
    bool Valid() const;
    bool Next();
    Key CurrentKey() const;
    RID CurrentRid() const;

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
  };
  RangeIter Scan(const Key* lo, const Key* hi) const;

  // 性能报告要用的统计：树高、叶页数、总页数、分裂次数
  struct Stats {
    uint16_t height = 0;
    uint32_t leaf_pages = 0;
    uint32_t total_pages = 0;
    uint64_t splits = 0;
  };
  Stats GetStats() const;

  uint16_t KeyLen() const { return key_len_; }
  bool IsUnique() const { return unique_; }
  PageId Root() const { return root_; }

 private:
  BufferPoolManager* buffer_pool_;
  uint16_t key_len_;
  bool unique_;
  PageId root_;
};

}  // namespace db
