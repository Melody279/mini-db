// db_server 入口
// 用法：
//   db_server --stdio                 # 从 stdin 读协议行（M0 可交互）
//   db_server --port 8888             # 常驻服务（网络层 W1 交付）
//   db_server --data data --buffer 256
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "db/common.h"
#include "db/server.h"

namespace {

void PrintUsage() {
  std::cout << "usage: db_server [--stdio] [--port N] [--data DIR] [--buffer PAGES] [--verbose]\n"
            << "  --stdio    从标准输入读取协议行（M0 调试模式）\n"
            << "  --port     监听端口，默认 8888（网络层 W1 交付）\n"
            << "  --data     数据目录，默认 data\n"
            << "  --buffer   缓冲池页数，默认 256\n";
}

}  // namespace

int main(int argc, char** argv) {
  db::Config config;
  bool stdio_mode = false;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--stdio") {
      stdio_mode = true;
    } else if (arg == "--verbose") {
      verbose = true;
    } else if (arg == "--port" && i + 1 < argc) {
      config.port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--data" && i + 1 < argc) {
      config.data_dir = argv[++i];
    } else if (arg == "--buffer" && i + 1 < argc) {
      config.buffer_pages = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      PrintUsage();
      return 2;
    }
  }

  if (verbose) db::SetLogLevel(db::LogLevel::kDebug);

  db::Engine engine;
  const db::Status status = engine.Open(config);
  if (!db::IsOk(status)) {
    std::cerr << "engine open failed: " << db::StatusName(status) << "\n";
    return 1;
  }

  if (stdio_mode) {
    std::string line;
    std::cout << "M0 stdio ready (type PING / HELLO / QUIT)\n";
    while (std::getline(std::cin, line)) {
      engine.HandleLine(line, std::cout);
      std::cout.flush();
      if (engine.ShouldQuit()) break;
    }
    return 0;
  }

  std::cout << "db_server M0 skeleton\n"
            << "  data_dir = " << config.data_dir << "\n"
            << "  port     = " << config.port << "\n"
            << "  buffer   = " << config.buffer_pages << " pages (" << db::kPageSize << "B/page)\n"
            << "TCP 监听在 W1 交付；现在请用 --stdio 交互。\n";
  return 0;
}
