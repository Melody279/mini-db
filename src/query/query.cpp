// 查询层：parser 与执行器（M0 只冻结接口，W1/W2 实现）
#include "db/query.h"

#include "db/storage.h"
#include "db/txn.h"

namespace db {

const char* CmpOpName(CmpOp op) {
  switch (op) {
    case CmpOp::kEq: return "=";
    case CmpOp::kNe: return "<>";
    case CmpOp::kLt: return "<";
    case CmpOp::kLe: return "<=";
    case CmpOp::kGt: return ">";
    case CmpOp::kGe: return ">=";
  }
  return "?";
}

Status Parser::Parse(const std::string& sql, Statement* out, std::string* err) const {
  (void)sql;
  if (out != nullptr) *out = Statement{};
  if (err != nullptr) *err = "parser 尚未实现（W1 交付，对应大赛题2）";
  return Status::kNotImplemented;
}

Executor::Executor(Catalog* catalog, BufferPool* buffer_pool, LockManager* lock_mgr,
                   TransactionManager* txn_mgr)
    : catalog_(catalog), buffer_pool_(buffer_pool), lock_mgr_(lock_mgr), txn_mgr_(txn_mgr) {
  LOG_INFO_MSG("Executor 组装完成（算子实现在 W1/W2）");
}

Status Executor::Execute(const Statement& stmt, TxnId txn, ResultSet* out) {
  (void)stmt;
  (void)txn;
  if (out != nullptr) *out = ResultSet{};
  return Status::kNotImplemented;
}

Status Executor::Explain(const Statement& stmt, std::string* plan_text) {
  (void)stmt;
  if (plan_text != nullptr) plan_text->clear();
  return Status::kNotImplemented;
}

}  // namespace db
