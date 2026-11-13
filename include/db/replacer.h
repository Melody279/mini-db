// 缓冲池替换策略（对应大赛 src/replacer/replacer.h + lru_replacer.h）
// 赛题给的接口：Pin / Unpin / Victim / Size；我们原样保留，只把目标帧类型统一为 frame_id_t。
// 本轮只实现 LRU（赛题默认，REPLACER_TYPE=LRU），CLOCK 作为加分项留在 W2。
#pragma once

#include <cstdint>
#include <cstddef>
#include <list>
#include <mutex>
#include <unordered_map>

namespace db {

using frame_id_t = int32_t;  // 帧号 = 缓冲池帧数组下标
constexpr frame_id_t kInvalidFrameId = -1;

// 替换策略基类：只关心"哪些帧现在可以被换出"
class Replacer {
 public:
  virtual ~Replacer() = default;

  // 帧被 pin 住 -> 从可替换集合里摘掉（对应 rmdb Replacer::Pin）
  virtual void Pin(frame_id_t frame_id) = 0;
  // 帧的 pin_count 归零 -> 放回可替换集合（对应 rmdb Replacer::Unpin）
  virtual void Unpin(frame_id_t frame_id) = 0;
  // 挑一个牺牲帧；没有可换出的返回 false（对应 rmdb Replacer::Victim）
  virtual bool Victim(frame_id_t* frame_id) = 0;
  // 当前可替换帧数量（对应 rmdb Replacer::Size）
  virtual size_t Size() const = 0;
};

// LRU：表尾最旧、表头最新。O(1) 摘除靠记录迭代器。
class LRUReplacer : public Replacer {
 public:
  explicit LRUReplacer(size_t capacity);
  ~LRUReplacer() override = default;

  void Pin(frame_id_t frame_id) override;
  void Unpin(frame_id_t frame_id) override;
  bool Victim(frame_id_t* frame_id) override;
  size_t Size() const override;

 private:
  mutable std::mutex latch_;
  size_t capacity_;
  std::list<frame_id_t> lru_;  // 头部最新
  std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> where_;
};

}  // namespace db
