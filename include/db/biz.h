// 业务层接口（M0 冻结）
// 需求侧的 7 条核心需求压成 12 个固定命令，App 只传参数、不拼 SQL（防注入 + 状态机校验在内核内）。
// 订单状态机在这里集中定义，是"不出现非法跳转"的唯一权威。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/common.h"

namespace db {

// 待接单 → 已接单 → 司机已到达 → 行程中 → 已完成 / 已取消
enum class OrderState : uint8_t {
  kWaiting = 1,
  kAccepted = 2,
  kArrived = 3,
  kInTrip = 4,
  kCompleted = 5,
  kCanceled = 6
};

const char* OrderStateName(OrderState s);
bool ParseOrderState(const std::string& name, OrderState* out);

// 唯一权威的转移表：任何写入 orders.state 的路径都必须先过这里
bool CanTransit(OrderState from, OrderState to);
bool IsTerminal(OrderState s);

// 12 个业务命令
enum class BizCmd : uint8_t {
  kLogin = 1,
  kOrderCreate = 2,
  kDispatch = 3,
  kOrderAccept = 4,
  kOrderReject = 5,
  kTripArrive = 6,
  kTripStart = 7,
  kTripFinish = 8,
  kPriceCalc = 9,
  kDriverToggle = 10,
  kAdminQuery = 11,
  kStatArea = 12
};

const char* BizCmdName(BizCmd cmd);
Status ParseBizCmd(const std::string& name, BizCmd* out);

// 计价：车费 = 起步价 + 里程单价 × 公里 + 时长单价 × 分钟（夜间系数为选做，暂不启用）
// 全部用"分"做整数运算，避免浮点误差；返回值单位同样是分。
int64_t CalcFare(int64_t base_fare_cents, int64_t per_km_cents, int64_t per_min_cents,
                 int32_t distance_m, int32_t duration_s);

}  // namespace db
