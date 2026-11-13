// LRU 替换策略实现（对应大赛 src/replacer/lru_replacer.cpp）
// 赛题在 lru_replacer 里同样留了 TODO：Pin / Unpin / Victim 三个函数要自己写。
// 这里保持一样的待填状态，只把构造函数和 Size 写掉，方便你对照着补。
#include "db/replacer.h"

#include "db/common.h"

namespace db {

LRUReplacer::LRUReplacer(size_t capacity) : capacity_(capacity) {}

void LRUReplacer::Pin(frame_id_t frame_id) {
  // TODO：帧被 pin 住后不再是淘汰候选
  // 1. where_ 里查不到该帧 -> 什么都不用做（它本来就不在可替换集合里）
  // 2. 查到 -> lru_.erase(where_[frame_id])，再 where_.erase(frame_id)
  (void)frame_id;
}

void LRUReplacer::Unpin(frame_id_t frame_id) {
  // TODO：帧的 pin_count 归零，重新变成候选，并放到"最新"一端
  // 1. where_ 已有该帧 -> 先挪到表头（或直接跳过，取决于你想要的语义，写进注释）
  // 2. 没有 -> 若 lru_.size() < capacity_ 则 lru_.push_front(frame_id)，并记录迭代器
  (void)frame_id;
}

bool LRUReplacer::Victim(frame_id_t* frame_id) {
  // TODO：挑最旧的一帧（lru_ 表尾）
  // 1. 集合为空 -> 返回 false
  // 2. 取 lru_.back()，从 lru_ 与 where_ 里同时删除，写回 *frame_id，返回 true
  if (frame_id != nullptr) *frame_id = kInvalidFrameId;
  return false;
}

size_t LRUReplacer::Size() const {
  std::lock_guard<std::mutex> guard(latch_);
  return lru_.size();
}

}  // namespace db
