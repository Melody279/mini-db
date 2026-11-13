// 查询层接口（M0 冻结）
// 对应大赛题2/4/5/7：lexer + 递归下降 parser（受限 SQL 子集）→ Statement → 执行算子 → ResultSet。
// 受限语法（W1 只需实现这些，别贪）：
//   SELECT  col[, col ...] FROM t [WHERE p] [ORDER BY col [DESC]] [LIMIT n]
//   SELECT  COUNT(*)|SUM(col) FROM t [GROUP BY col] [WHERE p]
//   SELECT  cols FROM t JOIN t2 ON t.c = t2.c [WHERE p]
//   INSERT INTO t VALUES (v, ...)
//   UPDATE t SET col = v [WHERE p]
//   DELETE FROM t [WHERE p]
//   EXPLAIN <上述任意语句>
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/catalog.h"
#include "db/common.h"

namespace db {

class BufferPoolManager;
class LockManager;
class TransactionManager;

// 字面值
struct Value {
  ColumnType type = ColumnType::kInt32;
  int64_t int_val = 0;
  std::string str_val;

  static Value Int(int64_t v) {
    Value r;
    r.type = ColumnType::kInt32;
    r.int_val = v;
    return r;
  }
  static Value Str(const std::string& s) {
    Value r;
    r.type = ColumnType::kFixedChar;
    r.str_val = s;
    return r;
  }
};

enum class CmpOp : uint8_t { kEq = 1, kNe = 2, kLt = 3, kLe = 4, kGt = 5, kGe = 6 };
const char* CmpOpName(CmpOp op);

struct Predicate {
  uint16_t col = kInvalidSlot;
  CmpOp op = CmpOp::kEq;
  Value val;
};

enum class AggKind : uint8_t { kNone = 0, kCount = 1, kSum = 2 };

enum class StmtKind : uint8_t {
  kSelect = 1,
  kInsert = 2,
  kUpdate = 3,
  kDelete = 4
};

struct Statement {
  StmtKind kind = StmtKind::kSelect;
  std::string table;

  // SELECT
  std::vector<uint16_t> select_cols;              // 空表示 SELECT *
  std::vector<Predicate> where;
  uint16_t order_by_col = kInvalidSlot;
  bool order_desc = false;
  int32_t limit = -1;

  // 聚合（题5）
  AggKind agg = AggKind::kNone;
  uint16_t agg_col = kInvalidSlot;
  std::vector<uint16_t> group_by;

  // 两表连接（题7）
  std::string join_table;
  std::vector<Predicate> join_on;
  uint16_t join_left_col = kInvalidSlot;   // join_on 只支持单等值条件
  uint16_t join_right_col = kInvalidSlot;

  // INSERT / UPDATE
  std::vector<Value> values;
  std::vector<uint16_t> set_cols;
  std::vector<Value> set_values;
};

class Parser {
 public:
  // 失败时把人类可读原因写入 *err
  Status Parse(const std::string& sql, Statement* out, std::string* err) const;
};

// 结果集：网关直接把 headers/rows 转 JSON，引擎侧只负责产 TSV
struct ResultSet {
  std::vector<std::string> headers;
  std::vector<std::vector<std::string>> rows;
  std::string plan_text;   // EXPLAIN 的输出（题4）
  int64_t elapsed_ms = 0;
};

class Executor {
 public:
  Executor(Catalog* catalog, BufferPoolManager* buffer_pool, LockManager* lock_mgr,
           TransactionManager* txn_mgr);

  Status Execute(const Statement& stmt, TxnId txn, ResultSet* out);
  Status Explain(const Statement& stmt, std::string* plan_text);

 private:
  Catalog* catalog_;
  BufferPoolManager* buffer_pool_;
  LockManager* lock_mgr_;
  TransactionManager* txn_mgr_;
};

}  // namespace db
