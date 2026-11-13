// 服务层接口（M0 冻结）
// 承载协议：一行一条请求 → 引擎处理 → 多行响应（见 docs/协议v0.md）。
// 手机 App 不直连这里，中间隔一层 HTTP 网关（gateway/），网关负责 JSON↔TSV。
#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>

#include "db/catalog.h"
#include "db/common.h"

namespace db {

class BufferPool;
class BPlusTree;
class Executor;
class LockManager;
class TransactionManager;
class Wal;

struct Config {
  std::string data_dir = "data";
  uint16_t port = 8888;
  uint32_t buffer_pages = kBufferPoolPages;
  bool durable = true;  // 关掉可做"不落盘"对比实验
};

// 引擎：把六个模块组装起来，对外只暴露一个 HandleLine
class Engine {
 public:
  Engine();
  ~Engine();

  Status Open(const Config& config);
  void Close();

  // 处理一行协议请求，把响应写进 out（含结尾 END）
  Status HandleLine(const std::string& line, std::ostream& out);

  const Config& GetConfig() const { return config_; }
  Catalog* GetCatalog() { return catalog_.get(); }
  bool ShouldQuit() const { return quit_; }

 private:
  Config config_;
  std::unique_ptr<Catalog> catalog_;
  std::unique_ptr<BufferPool> buffer_pool_;
  std::unique_ptr<LockManager> lock_mgr_;
  std::unique_ptr<Wal> wal_;
  std::unique_ptr<TransactionManager> txn_mgr_;
  std::unique_ptr<Executor> executor_;
  TxnId session_txn_ = 0;
  bool quit_ = false;
};

class Server {
 public:
  Server(const Config& config, Engine* engine);
  ~Server();

  Status Start();   // 监听端口（W1 实现）
  void Run();       // 事件循环，accept 后 thread-per-conn
  void Stop();

  uint16_t Port() const { return config_.port; }

 private:
  Config config_;
  Engine* engine_;
  bool running_ = false;
};

}  // namespace db
