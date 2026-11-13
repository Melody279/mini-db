// 服务层：协议解析 + 引擎组装
// M0 只实现 PING / HELLO / QUIT（可交互），网络监听由 W1 交付。
// 协议格式见 docs/协议v0.md：命令与参数用 \t 分隔，响应 = OK|ERR 行 + 数据行 + END。
#include "db/server.h"

#include <algorithm>
#include <cctype>
#include <ostream>

#include "db/query.h"
#include "db/storage.h"
#include "db/txn.h"

namespace db {
namespace {

std::string Trim(const std::string& text) {
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

std::string ToUpper(const std::string& text) {
  std::string result = text;
  for (auto& ch : result) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return result;
}

void WriteOk(std::ostream& out, const std::string& header, const std::string& row) {
  out << "OK 1\n";
  if (!header.empty()) out << header << "\n";
  if (!row.empty()) out << row << "\n";
  out << "END\n";
}

void WriteOkEmpty(std::ostream& out) { out << "OK 0\nEND\n"; }

void WriteError(std::ostream& out, int code, const std::string& message) {
  out << "ERR " << code << " " << message << "\nEND\n";
}

}  // namespace

Engine::Engine() = default;

Engine::~Engine() { Close(); }

Status Engine::Open(const Config& config) {
  config_ = config;

  catalog_.reset(new Catalog(config_.data_dir));
  const Status load_status = catalog_->Load();
  if (load_status != Status::kOk && load_status != Status::kNotFound) {
    LOG_ERROR_MSG("Catalog 载入失败：" + std::string(StatusName(load_status)));
    return load_status;
  }

  buffer_pool_.reset(new BufferPool(config_.data_dir, config_.buffer_pages));
  lock_mgr_.reset(new LockManager());
  wal_.reset(new Wal(config_.data_dir + "/wal.log"));
  txn_mgr_.reset(new TransactionManager(wal_.get(), lock_mgr_.get()));
  executor_.reset(
      new Executor(catalog_.get(), buffer_pool_.get(), lock_mgr_.get(), txn_mgr_.get()));

  LOG_INFO_MSG("引擎启动：data_dir=" + config_.data_dir +
               " port=" + std::to_string(config_.port) +
               " buffer=" + std::to_string(config_.buffer_pages) + " pages");
  LOG_INFO_MSG("M0 骨架：存储/索引/查询/事务的具体实现待 W1–W3 填充");
  return Status::kOk;
}

void Engine::Close() {
  if (buffer_pool_) buffer_pool_->FlushAll();
  executor_.reset();
  txn_mgr_.reset();
  wal_.reset();
  lock_mgr_.reset();
  buffer_pool_.reset();
  catalog_.reset();
}

Status Engine::HandleLine(const std::string& line, std::ostream& out) {
  const std::string text = Trim(line);
  if (text.empty()) {
    WriteOkEmpty(out);  // 空行当心跳，便于网关保活
    return Status::kOk;
  }

  const size_t sep = text.find_first_of(" \t");
  const std::string command = ToUpper(sep == std::string::npos ? text : text.substr(0, sep));
  const std::string args = sep == std::string::npos ? std::string() : text.substr(sep + 1);

  if (command == "PING") {
    WriteOkEmpty(out);
    return Status::kOk;
  }
  if (command == "QUIT") {
    WriteOkEmpty(out);
    quit_ = true;
    return Status::kOk;
  }
  if (command == "HELLO") {
    WriteOk(out, "version", "M0-skeleton");
    return Status::kOk;
  }
  if (command == "EXEC" || command == "EXPLAIN") {
    WriteError(out, 501, "engine-not-implemented");
    return Status::kOk;
  }
  if (command == "BIZ") {
    WriteError(out, 501, "biz-not-implemented");
    return Status::kOk;
  }

  (void)args;
  WriteError(out, 400, "unknown-command:" + command);
  return Status::kInvalidArgument;
}

Server::Server(const Config& config, Engine* engine) : config_(config), engine_(engine) {}

Server::~Server() = default;

Status Server::Start() {
  LOG_WARN_MSG("Server::Start 尚未实现（W1 交付 TCP 监听端口 " + std::to_string(config_.port) + "）");
  return Status::kNotImplemented;
}

void Server::Run() { (void)engine_; }

void Server::Stop() { running_ = false; }

}  // namespace db
