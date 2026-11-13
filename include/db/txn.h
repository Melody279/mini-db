// 事务层接口（M0 冻结）
// 对应大赛题8（事务控制语句）/ 题9（隔离级别，降级为锁等待演示）/ 题10（WAL + 重启恢复）。
// 约定：锁粒度到 RID（行级）；等锁超时即回滚该事务（不做等待图，理由见开发日志）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/common.h"

namespace db {

enum class LockMode : uint8_t { kShared = 1, kExclusive = 2 };
const char* LockModeName(LockMode mode);

class LockManager {
 public:
  LockManager();

  // 拿不到锁时最多等待 timeout_ms（W3 压测用来暴露死锁），超时返回 Status::kDeadlock
  Status Acquire(TxnId txn, RID rid, LockMode mode, int timeout_ms = 50);
  Status Release(TxnId txn, RID rid);
  void ReleaseAll(TxnId txn);

  uint64_t WaitCount() const { return wait_count_; }
  uint64_t DeadlockAborts() const { return deadlock_aborts_; }

 private:
  uint64_t wait_count_ = 0;
  uint64_t deadlock_aborts_ = 0;
};

// WAL：redo-only（记录新值），崩溃恢复 = 从 checkpoint 之后重放
class Wal {
 public:
  explicit Wal(const std::string& path);

  Status Append(const std::string& record);
  Status Sync();
  Status Checkpoint();
  Status Recover();

  const std::string& Path() const { return path_; }
  uint64_t RecordCount() const { return record_count_; }

 private:
  std::string path_;
  uint64_t record_count_ = 0;
};

enum class TxState : uint8_t { kActive = 1, kCommitted = 2, kAborted = 3 };
const char* TxStateName(TxState s);

class Transaction {
 public:
  TxnId id = 0;
  TxState state = TxState::kActive;
};

class TransactionManager {
 public:
  TransactionManager(Wal* wal, LockManager* lock_mgr);

  TxnId Begin();
  const Transaction* Get(TxnId id) const;
  Status Commit(TxnId id);
  Status Abort(TxnId id);

  uint64_t CommittedCount() const { return committed_; }
  uint64_t AbortedCount() const { return aborted_; }

 private:
  Wal* wal_;
  LockManager* lock_mgr_;
  TxnId next_id_ = 1;
  uint64_t committed_ = 0;
  uint64_t aborted_ = 0;
};

}  // namespace db
