// 业务层：订单状态机、12 个业务命令、计价公式
// 这一层是"需求"与"内核"的接缝：业务规则写在引擎里，前端只传参数。
#include "db/biz.h"

#include <cctype>

namespace db {
namespace {

struct StateEntry {
  OrderState state;
  const char* name;   // 中文名（协议/文档用）
  const char* alias;  // ASCII 别名（网关与脚本用）
};

const StateEntry kStates[] = {
    {OrderState::kWaiting, "待接单", "waiting"},
    {OrderState::kAccepted, "已接单", "accepted"},
    {OrderState::kArrived, "司机已到达", "arrived"},
    {OrderState::kInTrip, "行程中", "in_trip"},
    {OrderState::kCompleted, "已完成", "completed"},
    {OrderState::kCanceled, "已取消", "canceled"},
};

struct CmdEntry {
  BizCmd cmd;
  const char* name;
};

const CmdEntry kCommands[] = {
    {BizCmd::kLogin, "LOGIN"},
    {BizCmd::kOrderCreate, "ORDER_CREATE"},
    {BizCmd::kDispatch, "DISPATCH"},
    {BizCmd::kOrderAccept, "ORDER_ACCEPT"},
    {BizCmd::kOrderReject, "ORDER_REJECT"},
    {BizCmd::kTripArrive, "TRIP_ARRIVE"},
    {BizCmd::kTripStart, "TRIP_START"},
    {BizCmd::kTripFinish, "TRIP_FINISH"},
    {BizCmd::kPriceCalc, "PRICE_CALC"},
    {BizCmd::kDriverToggle, "DRIVER_TOGGLE"},
    {BizCmd::kAdminQuery, "ADMIN_QUERY"},
    {BizCmd::kStatArea, "STAT_AREA"},
};

std::string ToUpper(const std::string& text) {
  std::string result = text;
  for (auto& ch : result) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return result;
}

}  // namespace

const char* OrderStateName(OrderState s) {
  for (const auto& entry : kStates) {
    if (entry.state == s) return entry.name;
  }
  return "未知状态";
}

bool ParseOrderState(const std::string& name, OrderState* out) {
  if (out == nullptr) return false;
  for (const auto& entry : kStates) {
    if (name == entry.name || name == entry.alias) {
      *out = entry.state;
      return true;
    }
  }
  return false;
}

bool CanTransit(OrderState from, OrderState to) {
  switch (from) {
    case OrderState::kWaiting:
      return to == OrderState::kAccepted || to == OrderState::kCanceled;
    case OrderState::kAccepted:
      return to == OrderState::kArrived || to == OrderState::kCanceled;
    case OrderState::kArrived:
      return to == OrderState::kInTrip || to == OrderState::kCanceled;
    case OrderState::kInTrip:
      // 行程中允许"异常取消"（车坏了/乘客中途下车），在文档里写明这是业务约定
      return to == OrderState::kCompleted || to == OrderState::kCanceled;
    case OrderState::kCompleted:
    case OrderState::kCanceled:
      return false;  // 终态不允许再变
  }
  return false;
}

bool IsTerminal(OrderState s) {
  return s == OrderState::kCompleted || s == OrderState::kCanceled;
}

const char* BizCmdName(BizCmd cmd) {
  for (const auto& entry : kCommands) {
    if (entry.cmd == cmd) return entry.name;
  }
  return "UNKNOWN";
}

Status ParseBizCmd(const std::string& name, BizCmd* out) {
  if (out == nullptr) return Status::kInvalidArgument;
  const std::string upper = ToUpper(name);
  for (const auto& entry : kCommands) {
    if (upper == entry.name) {
      *out = entry.cmd;
      return Status::kOk;
    }
  }
  return Status::kNotFound;
}

int64_t CalcFare(int64_t base_fare_cents, int64_t per_km_cents, int64_t per_min_cents,
                 int32_t distance_m, int32_t duration_s) {
  // 整数运算 + 四舍五入，避免浮点误差；负数入参按 0 处理（防御式）
  const int64_t distance = distance_m > 0 ? distance_m : 0;
  const int64_t duration = duration_s > 0 ? duration_s : 0;
  const int64_t km_part = (per_km_cents * distance + 500) / 1000;
  const int64_t min_part = (per_min_cents * duration + 30) / 60;
  return base_fare_cents + km_part + min_part;
}

}  // namespace db
