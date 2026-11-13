# mini-db · 打车系统大作业（C++ 迷你数据库引擎 + 手机 App）

大作业交付物分两块：**自己手搓的迷你数据库引擎**（存储 / 索引 / 查询 / 事务 / 元数据 / 服务）与**打车业务 App**（手机端，经 HTTP 网关调用引擎）。业务只是负载，内核才是要被评审看的东西。

当前阶段：**M0 骨架完成**（接口冻结 + 可编译 + 可交互 + 自检通过），实现留待 W1–W3 填充。

## 目录

```
mini-db/
├─ include/db/           # 六个模块的接口（M0 已冻结）
│   ├─ common.h          #   常量、RID、Status、日志、计时
│   ├─ storage.h         #   页 / 缓冲池 / 堆文件（大赛题1）
│   ├─ catalog.h         #   元数据（表定义、列偏移、索引）
│   ├─ index.h           #   B+ 树与范围扫描（大赛题3）
│   ├─ query.h           #   parser + 算子 + EXPLAIN（题2/4/5/7）
│   ├─ txn.h             #   锁 / 事务 / WAL（题8/9/10）
│   ├─ biz.h             #   订单状态机 + 12 个业务命令 + 计价
│   └─ server.h          #   协议与引擎组装
├─ src/{common,storage,catalog,index,query,txn,biz,server}/
├─ tests/m0_selftest.cpp # 34 项断言：常量/类型/状态机/计价/catalog 往返
├─ warmup/{a_slotted_page,b_where_parser,c_tcp_echo}/   # 手搓热身三件套
├─ docs/                 # 数据模型 v0、协议 v0、开发日志
└─ CMakeLists.txt
```

## 快速开始（Ubuntu 18.04 / 20.04，比赛同款环境）

```bash
sudo apt install -y build-essential cmake        # gcc 7.1+ / cmake 3.16+
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
cd build && ctest --output-on-failure            # 4 个测试全绿才算环境没问题
```

单独跑：

```bash
./build/m0_selftest                       # 骨架自检
./build/warmup_a_page                     # 8KB 页 + 槽目录
./build/warmup_b_parser                   # WHERE 谓词递归下降
./build/warmup_c_socket                   # TCP 多连接 + 互斥序号
printf 'PING\nHELLO\nQUIT\n' | ./build/db_server --stdio
```

### Windows 上临时验证（没有装 MSVC 时）

项目不以 Windows 为交付环境，但本机验证过可以编译：

```powershell
python -m pip install ziglang
python -m ziglang c++ -std=c++17 -I include `
  src/common/common.cpp src/catalog/catalog.cpp src/storage/storage.cpp `
  src/index/bplus.cpp src/query/query.cpp src/txn/txn.cpp src/biz/biz.cpp `
  src/server/server.cpp tests/m0_selftest.cpp -o build/m0_selftest.exe
```

注意 `warmup_c_socket` 在 Windows 需要 `-lws2_32`（CMake 里已按 `WIN32` 加）。

## M0 验收清单

| 项 | 判据 | 状态 |
|---|---|---|
| 六个模块头文件冻结 | `include/db/` 全部可编译，接口签名不再随意改 | ✅ 11-13 |
| 统一构建 | CMake 一条命令出全部 target | ✅（Ubuntu 待队友复验） |
| 骨架自检 | `m0_selftest` 34 项全 PASS | ✅ 11-13 |
| 数据模型 v0 | 9 张表 + 11 个索引 + 需求映射 | ✅ `docs/数据模型v0.md` |
| 协议 v0 | 帧格式 / 命令 / 错误码 / 12 业务命令 | ✅ `docs/协议v0.md` |
| 手搓热身 | A/B/C 三个 demo 各自 PASS | ✅ 11-13 |
| 引擎可交互 | `--stdio` 下 PING/HELLO/EXEC/QUIT 响应正确 | ✅ 11-13 |
| TCP 监听 | `Server::Start` | ⬜ W1 |
| 页与堆文件 | `HeapFile` 真能增删查 | ⬜ W1 |
| B+ 树 | 插入/查找/范围扫描 | ⬜ W2 |

## 模块与分工

| 模块 | 负责人 | 状态 |
|---|---|---|
| `storage`（页/堆文件/缓冲池） | A | 桩实现，W1 填 |
| `catalog`（元数据） | A | **已实现**（含落盘与往返） |
| `index`（B+ 树） | A | 桩实现，W2 填 |
| `query`（parser/算子/EXPLAIN） | B | 桩实现，W1–W2 填 |
| `biz`（状态机/计价/12 命令） | B | **状态机与计价已实现**，命令待 W3 |
| `txn`（锁/事务/WAL） | C | 桩实现，W2 填 |
| `server`（协议/监听） | C | 协议已实现，监听待 W1 |
| `gateway` + `app` | C 主 + A/B 分摊页面 | W3 |

## 协作约定

1. **接口先改文档再改代码**：动 `include/db/*.h` 必须在周五走查会上说，改完同步更新本 README 的模块表。
2. **分支**：`main` 只收可编译的提交；各人 `dev-a / dev-b / dev-c`，合并前必须 `ctest` 全绿。
3. **提交信息**：`[storage] 实现 slotted page 插入与删除`，前缀即模块名，方便后面从 git log 生成"开发过程"章节。
4. **不许绕过评测**：本地跑 `m0_selftest` 与用例脚本是硬门槛，红了不许合。
5. **AI 使用**：核心算法（页、B+ 树、parser、算子、锁、WAL）手搓；AI 只用于外围（网关、App 页面、查报错），逐条记入 `AI使用声明.md`。

## 相关文档

- 总体规划：`../DB大作业-打车系统准备方案.md`
- 数据模型：`docs/数据模型v0.md`
- 协议：`docs/协议v0.md`
- 开发日志：`docs/开发日志.md`
