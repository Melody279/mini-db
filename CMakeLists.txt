cmake_minimum_required(VERSION 3.16)
project(mini_db LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# ---- 编译选项：对齐大赛环境（GCC 7.1+ / C++17），本机开警告但不 -Werror ----
if(MSVC)
  add_compile_options(/W3 /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---- 核心库：dbcore（六个模块的 .cpp 都进这里）----
file(GLOB_RECURSE DB_SOURCES
  src/storage/*.cpp
  src/catalog/*.cpp
  src/index/*.cpp
  src/query/*.cpp
  src/txn/*.cpp
  src/biz/*.cpp
  src/server/*.cpp
  src/common/*.cpp
)

add_library(dbcore STATIC ${DB_SOURCES})
target_include_directories(dbcore PUBLIC ${CMAKE_SOURCE_DIR}/include)

# ---- 可执行：db_server（常驻服务 + --stdio 调试模式）----
add_executable(db_server src/main.cpp)
target_link_libraries(db_server PRIVATE dbcore)

# ---- M0 自检：不依赖网络与磁盘，验证常量/状态机/catalog 往返 ----
add_executable(m0_selftest tests/m0_selftest.cpp)
target_link_libraries(m0_selftest PRIVATE dbcore)

# ---- 手搓热身三件套（W0，各自独立可跑）----
add_executable(warmup_a_page warmup/a_slotted_page/main.cpp)
add_executable(warmup_b_parser warmup/b_where_parser/main.cpp)
add_executable(warmup_c_socket warmup/c_tcp_echo/main.cpp)
if(WIN32)
  target_link_libraries(warmup_c_socket PRIVATE ws2_32)
endif()

enable_testing()
add_test(NAME m0_selftest COMMAND m0_selftest)
add_test(NAME warmup_a_page COMMAND warmup_a_page)
add_test(NAME warmup_b_parser COMMAND warmup_b_parser)
add_test(NAME warmup_c_socket COMMAND warmup_c_socket)
