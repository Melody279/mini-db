// 事务层：锁、事务管理、WAL（M0 只冻结接口，W2/W3 实现）
#include "db/txn.h"

namespace db {

const char* LockModeName(LockMode mode) {
  return mode == LockMode::kShared ? "S" : "X";
}

LockManager::LockManager() { LOG_INFO_MSG("LockManager 初始化（行级锁，W2 实现 2PL）"); }

Status LockManager::Acquire(TxnId txn, RID rid, LockMode mode, int timeout_ms) {
  (void)txn;
  (void)rid;
  (void)mode;
  (void)timeout_ms;
  return Status::kNotImplemented;
}

Status LockManager::Release(TxnId txn, RID rid) {
  (void)txn;
  (void)rid;
  return Status::kNotImplemented;
}

void LockManager::ReleaseAll(TxnId txn) { (void)txn; }

const char* TxStateName(TxState s) {
  switch (s) {
    case TxState::kActive: return "active";
    case TxState::kCommitted: return "committed";
    case TxState::kAborted: return "aborted";
  }
  return "unknown";
}

Wal::Wal(const std::string& path) : path_(path) {
  LOG_INFO_MSG("Wal 打开：" + path_ + "（redo-only，W2 实现）");
}

Status Wal::Append(const std::string& record) {
  (void)record;
  return Status::kNotImplemented;
}

Status Wal::Sync() { return Status::kNotImplemented; }

Status Wal::Checkpoint() { return Status::kNotImplemented; }

Status Wal::Recover() { return Status::kNotImplemented; }

TransactionManager::TransactionManager(Wal* wal, LockManager* lock_mgr)
    : wal_(wal), lock_mgr_(lock_mgr) {}

TxnId TransactionManager::Begin() { return next_id_++; }

const Transaction* TransactionManager::Get(TxnId id) const {
  (void)id;
  return nullptr;
}

Status TransactionManager::Commit(TxnId id) {
  (void)id;
  return Status::kNotImplemented;
}

Status TransactionManager::Abort(TxnId id) {
  (void)id;
  return Status::kNotImplemented;
}

}  // namespace db
