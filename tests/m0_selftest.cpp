// M0 自检：不依赖网络与磁盘（catalog 往返用临时目录，跑完清理）
// 覆盖：常量契约 / RID / 类型尺寸 / 时间与金额格式 / 订单状态机 / 计价 / catalog 往返
#include <cstdio>
#include <iostream>
#include <string>

#include "db/biz.h"
#include "db/catalog.h"
#include "db/common.h"

namespace {

int g_failed = 0;

void Check(bool condition, const std::string& message) {
  if (condition) {
    std::cout << "ok   " << message << "\n";
  } else {
    std::cout << "FAIL " << message << "\n";
    ++g_failed;
  }
}

db::TableMeta MakeOrdersTable() {
  db::TableMeta table;
  table.name = "orders";
  table.heap_path = "data/selftest/orders.heap";

  auto add = [&table](const std::string& name, db::ColumnType type, uint16_t len, bool pk,
                      bool nullable) {
    db::Column col;
    col.name = name;
    col.type = type;
    col.len = len;
    col.primary_key = pk;
    col.nullable = nullable;
    table.columns.push_back(col);
  };

  add("oid", db::ColumnType::kInt64, 0, true, false);
  add("pid", db::ColumnType::kInt32, 0, false, false);
  add("state", db::ColumnType::kInt32, 0, false, false);
  add("fare", db::ColumnType::kMoney, 0, false, true);
  add("created_at", db::ColumnType::kTimestamp, 0, false, false);
  return table;
}

void TestConstants() {
  Check(db::kPageSize == 8192, "页大小 8KB");
  Check(db::kBufferPoolPages == 256, "缓冲池默认 256 页");

  db::RID rid;
  Check(!rid.IsValid(), "默认 RID 无效");
  rid.page = 7;
  rid.slot = 3;
  Check(rid.IsValid() && rid == db::RID{7, 3}, "RID 有效且可比较");

  Check(db::TypeSize(db::ColumnType::kInt32, 0) == 4, "int32 占 4 字节");
  Check(db::TypeSize(db::ColumnType::kInt64, 0) == 8, "int64 占 8 字节");
  Check(db::TypeSize(db::ColumnType::kFixedChar, 32) == 32, "char(32) 占 32 字节");
}

void TestFormat() {
  db::Timestamp ts = 0;
  Check(db::ParseTimestamp("2026-11-13 10:00:00", &ts) && ts > 0, "时间解析成功");
  Check(db::FormatTimestamp(ts) == "2026-11-13 10:00:00", "时间格式化往返一致");
  Check(db::FormatMoney(12345) == "123.45", "金额 12345 分 -> 123.45");
  Check(db::FormatMoney(-5) == "-0.05", "金额 -5 分 -> -0.05");
}

void TestStateMachine() {
  using db::OrderState;
  Check(db::CanTransit(OrderState::kWaiting, OrderState::kAccepted), "待接单 -> 已接单");
  Check(db::CanTransit(OrderState::kWaiting, OrderState::kCanceled), "待接单 -> 已取消");
  Check(!db::CanTransit(OrderState::kWaiting, OrderState::kCompleted), "待接单 不能直接 -> 已完成");
  Check(db::CanTransit(OrderState::kAccepted, OrderState::kArrived), "已接单 -> 司机已到达");
  Check(db::CanTransit(OrderState::kArrived, OrderState::kInTrip), "司机已到达 -> 行程中");
  Check(db::CanTransit(OrderState::kInTrip, OrderState::kCompleted), "行程中 -> 已完成");
  Check(!db::CanTransit(OrderState::kCompleted, OrderState::kInTrip), "已完成是终态");
  Check(!db::CanTransit(OrderState::kCanceled, OrderState::kAccepted), "已取消是终态");
  Check(db::IsTerminal(OrderState::kCompleted) && db::IsTerminal(OrderState::kCanceled),
        "终态判定");

  db::OrderState parsed = OrderState::kWaiting;
  Check(db::ParseOrderState("in_trip", &parsed) && parsed == OrderState::kInTrip,
        "别名解析 in_trip");
  Check(db::ParseOrderState("行程中", &parsed), "中文名解析");
}

void TestFare() {
  // 起步 8 元 + 2.5 元/公里 × 3.2km + 0.5 元/分钟 × 10min = 8 + 8 + 5 = 21 元
  const int64_t fare = db::CalcFare(800, 250, 50, 3200, 600);
  Check(fare == 2100, "计价：8+8+5=21 元（2100 分）");
  Check(db::CalcFare(800, 250, 50, 0, 0) == 800, "零里程零时长只收起步价");
}

void TestCatalog() {
  const std::string dir = "selftest_data";
  db::Catalog catalog(dir);
  db::TableId orders_id = 0;
  Check(db::IsOk(catalog.CreateTable(MakeOrdersTable(), &orders_id)), "建表 orders");

  const db::TableMeta* table = catalog.FindTable("orders");
  Check(table != nullptr, "按名字找到 orders");
  if (table != nullptr) {
    Check(table->record_size == 32, "定长记录 = 8+4+4+8+8 = 32 字节");
    Check(table->columns.size() == 5, "orders 有 5 列");
    Check(table->columns[1].offset == 8, "pid 偏移为 8");
  }

  db::IndexMeta index;
  index.name = "idx_orders_created_at";
  index.column_idx.push_back(4);
  index.unique = false;
  Check(db::IsOk(catalog.AddIndex(orders_id, index)), "新增索引元数据");
  Check(db::IsOk(catalog.Save()), "catalog 落盘");

  db::Catalog reloaded(dir);
  Check(db::IsOk(reloaded.Load()), "重新载入 catalog");
  const db::TableMeta* again = reloaded.FindTable("orders");
  Check(again != nullptr && again->columns.size() == 5 && again->indexes.size() == 1,
        "catalog 往返：列与索引都还在");
  Check(again != nullptr && again->record_size == 32, "catalog 往返：record_size 一致");

  std::remove((dir + "/catalog.dat").c_str());
  std::remove(dir.c_str());
}

}  // namespace

int main() {
  std::cout << "=== M0 selftest ===\n";
  TestConstants();
  TestFormat();
  TestStateMachine();
  TestFare();
  TestCatalog();

  std::cout << "-------------------\n";
  if (g_failed == 0) {
    std::cout << "RESULT: PASS\n";
    return 0;
  }
  std::cout << "RESULT: FAIL (" << g_failed << " checks failed)\n";
  return 1;
}
