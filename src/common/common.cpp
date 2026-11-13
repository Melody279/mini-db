#include "db/common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

namespace db {
namespace {

LogLevel g_level = LogLevel::kInfo;
std::mutex g_log_mutex;

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo: return "INFO";
    case LogLevel::kWarn: return "WARN";
    case LogLevel::kError: return "ERROR";
  }
  return "LOG";
}

}  // namespace

const char* StatusName(Status s) {
  switch (s) {
    case Status::kOk: return "OK";
    case Status::kNotFound: return "NotFound";
    case Status::kDuplicate: return "Duplicate";
    case Status::kFull: return "Full";
    case Status::kInvalidArgument: return "InvalidArgument";
    case Status::kNotImplemented: return "NotImplemented";
    case Status::kAborted: return "Aborted";
    case Status::kIoError: return "IoError";
    case Status::kDeadlock: return "Deadlock";
    case Status::kOutOfMemory: return "OutOfMemory";
  }
  return "Unknown";
}

const char* ColumnTypeName(ColumnType t) {
  switch (t) {
    case ColumnType::kInt32: return "int32";
    case ColumnType::kInt64: return "int64";
    case ColumnType::kFixedChar: return "char";
    case ColumnType::kMoney: return "money";
    case ColumnType::kTimestamp: return "timestamp";
  }
  return "unknown";
}

uint16_t TypeSize(ColumnType t, uint16_t char_len) {
  switch (t) {
    case ColumnType::kInt32: return 4;
    case ColumnType::kInt64: return 8;
    case ColumnType::kMoney: return 8;
    case ColumnType::kTimestamp: return 8;
    case ColumnType::kFixedChar: return char_len;
  }
  return 0;
}

void SetLogLevel(LogLevel level) { g_level = level; }

void Log(LogLevel level, const std::string& msg) {
  if (level < g_level) return;
  std::lock_guard<std::mutex> guard(g_log_mutex);
  std::fprintf(stderr, "[%s] %s\n", LevelName(level), msg.c_str());
}

Stopwatch::Stopwatch() : start_(std::chrono::steady_clock::now()) {}

void Stopwatch::Reset() { start_ = std::chrono::steady_clock::now(); }

int64_t Stopwatch::ElapsedMs() const {
  const auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count();
}

Timestamp NowTimestamp() { return static_cast<Timestamp>(std::time(nullptr)); }

std::string FormatTimestamp(Timestamp ts) {
  const time_t t = static_cast<time_t>(ts);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  char buf[32] = {0};
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
  return std::string(buf);
}

bool ParseTimestamp(const std::string& text, Timestamp* out) {
  if (out == nullptr) return false;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  const int matched = std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour,
                                  &minute, &second);
  if (matched != 6) return false;
  std::tm tm_buf{};
  tm_buf.tm_year = year - 1900;
  tm_buf.tm_mon = month - 1;
  tm_buf.tm_mday = day;
  tm_buf.tm_hour = hour;
  tm_buf.tm_min = minute;
  tm_buf.tm_sec = second;
  tm_buf.tm_isdst = -1;
  const time_t t = std::mktime(&tm_buf);
  if (t == static_cast<time_t>(-1)) return false;
  *out = static_cast<Timestamp>(t);
  return true;
}

std::string FormatMoney(int64_t cents) {
  const bool negative = cents < 0;
  const int64_t abs_value = negative ? -cents : cents;
  char buf[32] = {0};
  std::snprintf(buf, sizeof(buf), "%lld.%02lld", static_cast<long long>(abs_value / 100),
                static_cast<long long>(abs_value % 100));
  return std::string(negative ? "-" : "") + buf;
}

}  // namespace db
