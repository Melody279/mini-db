// 手搓热身 C：TCP 多连接 + 互斥锁分配的全局序号
// 目标：这是 db_server（W1）的雏形——thread-per-conn、全局会话计数、请求/响应成帧。
// 协议（与正式协议同构）：请求一行文本，响应 "OK <seq>\t<回显>\nEND\n"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
using socket_t = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#endif

namespace {

constexpr uint16_t kPort = 19010;

std::atomic<bool> g_stop(false);
std::mutex g_mutex;
uint64_t g_sequence = 0;
uint64_t g_handled = 0;

void CloseSock(socket_t sock) {
#ifdef _WIN32
  closesocket(sock);
#else
  ::close(sock);
#endif
}

bool InitSockets(std::string* err) {
#ifdef _WIN32
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    if (err != nullptr) *err = "WSAStartup 失败";
    return false;
  }
#endif
  (void)err;
  return true;
}

void CleanupSockets() {
#ifdef _WIN32
  WSACleanup();
#endif
}

bool SendAll(socket_t sock, const std::string& payload) {
  size_t sent = 0;
  while (sent < payload.size()) {
    const int n = static_cast<int>(send(sock, payload.c_str() + sent,
                                        static_cast<int>(payload.size() - sent), 0));
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

void HandleClient(socket_t client) {
  std::string buffer;
  char chunk[256];
  bool keep_running = true;
  while (keep_running) {
    const int n = static_cast<int>(recv(client, chunk, sizeof(chunk), 0));
    if (n <= 0) break;
    buffer.append(chunk, static_cast<size_t>(n));

    size_t newline = 0;
    while ((newline = buffer.find('\n')) != std::string::npos) {
      const std::string line = buffer.substr(0, newline);
      buffer.erase(0, newline + 1);
      if (line == "QUIT") {
        keep_running = false;
        break;
      }
      uint64_t seq = 0;
      {
        std::lock_guard<std::mutex> guard(g_mutex);
        seq = ++g_sequence;
        ++g_handled;
      }
      const std::string reply = "OK " + std::to_string(seq) + "\t" + line + "\nEND\n";
      if (!SendAll(client, reply)) {
        keep_running = false;
        break;
      }
    }
  }
  CloseSock(client);
}

void AcceptLoop(socket_t listen_sock, std::vector<std::thread>* workers) {
  while (!g_stop.load()) {
    socket_t client = accept(listen_sock, nullptr, nullptr);
    if (client == static_cast<socket_t>(-1)) break;  // 监听套接字被关闭
    workers->emplace_back(HandleClient, client);
  }
}

bool StartServer(socket_t* listen_out, std::string* err) {
  socket_t listen_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_sock == static_cast<socket_t>(-1)) {
    if (err != nullptr) *err = "socket() 失败";
    return false;
  }
  int reuse = 1;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (err != nullptr) *err = "bind() 失败（端口可能被占用）";
    CloseSock(listen_sock);
    return false;
  }
  if (listen(listen_sock, 8) != 0) {
    if (err != nullptr) *err = "listen() 失败";
    CloseSock(listen_sock);
    return false;
  }
  *listen_out = listen_sock;
  return true;
}

bool ReadOneReply(socket_t sock, std::string* out, std::string* err) {
  std::string buffer;
  char chunk[256];
  while (true) {
    if (buffer.find("\nEND\n") != std::string::npos) break;
    const int n = static_cast<int>(recv(sock, chunk, sizeof(chunk), 0));
    if (n <= 0) {
      if (err != nullptr) *err = "对端关闭连接";
      return false;
    }
    buffer.append(chunk, static_cast<size_t>(n));
  }
  *out = buffer;
  return true;
}

bool RunClient(int expected_first_seq, std::string* err) {
  socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (err != nullptr) *err = "connect() 失败";
    CloseSock(sock);
    return false;
  }

  for (int i = 0; i < 3; ++i) {
    const std::string request = "HELLO " + std::to_string(i + 1) + "\n";
    if (!SendAll(sock, request)) {
      if (err != nullptr) *err = "发送失败";
      CloseSock(sock);
      return false;
    }
    std::string reply;
    if (!ReadOneReply(sock, &reply, err)) {
      CloseSock(sock);
      return false;
    }
    const std::string expected_prefix =
        "OK " + std::to_string(expected_first_seq + i) + "\tHELLO " + std::to_string(i + 1);
    if (reply.compare(0, expected_prefix.size(), expected_prefix) != 0) {
      if (err != nullptr) *err = "序号或回显不匹配：" + reply;
      CloseSock(sock);
      return false;
    }
  }
  SendAll(sock, "QUIT\n");
  CloseSock(sock);
  return true;
}

}  // namespace

int main() {
  std::string err;
  if (!InitSockets(&err)) {
    std::printf("FAIL: %s\n", err.c_str());
    return 1;
  }

  socket_t listen_sock = static_cast<socket_t>(-1);
  if (!StartServer(&listen_sock, &err)) {
    std::printf("FAIL: %s\n", err.c_str());
    CleanupSockets();
    return 1;
  }
  std::printf("server listening on 127.0.0.1:%u\n", kPort);

  std::vector<std::thread> workers;
  std::thread acceptor(AcceptLoop, listen_sock, &workers);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  const bool client_ok = RunClient(1, &err);
  if (client_ok) {
    std::printf("ok   client 3 次请求的序号连续\n");
  } else {
    std::printf("FAIL: %s\n", err.c_str());
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  g_stop.store(true);
  CloseSock(listen_sock);
  acceptor.join();
  for (auto& worker : workers) {
    if (worker.joinable()) worker.join();
  }

  // 全部 worker 已 join，此处读取 g_handled 无并发风险
  std::printf("handled=%llu\n", static_cast<unsigned long long>(g_handled));
  CleanupSockets();

  const bool all_ok = client_ok && g_handled == 3;
  std::printf("RESULT: %s (warmup_c_tcp_echo)\n", all_ok ? "PASS" : "FAIL");
  return all_ok ? 0 : 1;
}
