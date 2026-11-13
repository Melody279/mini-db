// 公共类型与常量（M0 冻结）
// 设计要点：页大小/记录定位/状态码/日志/计时，全工程共用，避免各模块自造轮子。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace db {

// ---------- 全局常量 ----------
constexpr uint32_t kPageSize = 8192;        // 页大小 8KB（对应大赛题1 存储管理）
constexpr uint32_t kBufferPoolPages = 256;  // 缓冲池页数，性能基线 B3 会调到 1024
constexpr uint32_t kInvalidPageId = 0xFFFFFFFFu;
constexpr uint16_t kInvalidSlot = 0xFFFFu;

using PageId = uint32_t;   // 页号
using SlotId = uint16_t;   // 页内槽号
using TableId = uint16_t;  // 表号
using TxnId = uint64_t;    // 事务号，0 视为无效
using Timestamp = int64_t; // 秒级 Unix 时间戳，便于范围比较与索引

// ---------- 记录定位 ----------
// 定长记录方案（降级路径已约定）：一条记录 = record_size 字节，RID 即可唯一定位。
struct RID {
  PageId page = kInvalidPageId;
  SlotId slot = kInvalidSlot;

  bool IsValid() const { return page != kInvalidPageId && slot != kInvalidSlot; }
  bool operator==(const RID& o) const { return page == o.page && slot == o.slot; }
  bool operator!=(const RID& o) const { return !(*this == o); }
};

// ---------- 状态码（错误码同时用于协议 ERR 行）----------
enum class Status : int16_t {
  kOk = 0,
  kNotFound = 1,
  kDuplicate = 2,
  kFull = 3,
  kInvalidArgument = 4,
  kNotImplemented = 5,  // M0 骨架占位：接口已冻结、实现待填
  kAborted = 6,
  kIoError = 7,
  kDeadlock = 8,
  kOutOfMemory = 9
};

const char* StatusName(Status s);
inline bool IsOk(Status s) { return s == Status::kOk; }

// ---------- 列类型 ----------
enum class ColumnType : uint8_t {
  kInt32 = 1,
  kInt64 = 2,
  kFixedChar = 3,  // char(n)，定长
  kMoney = 4,      // 以"分"为单位的 int64，避免浮点误差
  kTimestamp = 5   // int64 秒级
};
const char* ColumnTypeName(ColumnType t);

// 返回该类型占用的字节数；kFixedChar 需要给定 char_len
uint16_t TypeSize(ColumnType t, uint16_t char_len);

// ---------- 记录字节流 ----------
using RecBytes = std::vector<uint8_t>;

// ---------- 日志 ----------
enum class LogLevel { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };
void Log(LogLevel level, const std::string& msg);
void SetLogLevel(LogLevel level);

#define LOG_DEBUG_MSG(m) ::db::Log(::db::LogLevel::kDebug, (m))
#define LOG_INFO_MSG(m) ::db::Log(::db::LogLevel::kInfo, (m))
#define LOG_WARN_MSG(m) ::db::Log(::db::LogLevel::kWarn, (m))
#define LOG_ERROR_MSG(m) ::db::Log(::db::LogLevel::kError, (m))

// ---------- 计时（性能报告用）----------
class Stopwatch {
 public:
  Stopwatch();
  void Reset();
  int64_t ElapsedMs() const;

 private:
  std::chrono::steady_clock::time_point start_;
};

// ---------- 时间与金额格式化 ----------
Timestamp NowTimestamp();
std::string FormatTimestamp(Timestamp ts);            // -> "YYYY-MM-DD HH:MM:SS"
bool ParseTimestamp(const std::string& text, Timestamp* out);
std::string FormatMoney(int64_t cents);               // -> "12.34"

}  // namespace db
