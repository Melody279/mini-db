// 元数据管理接口（M0 冻结）
// 表定义、列偏移、索引元数据持久化到 data/catalog.dat（纯文本，便于 diff 与人工排查）。
// 列偏移在 CreateTable 时按 (type, len) 顺序计算，因此"记录 = 定长字节串"这一约定在此固化。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/common.h"

namespace db {

struct Column {
  std::string name;
  ColumnType type = ColumnType::kInt32;
  uint16_t len = 0;       // 仅 kFixedChar 有意义
  uint16_t offset = 0;    // 由 Catalog 计算
  bool nullable = false;
  bool primary_key = false;
};

struct IndexMeta {
  std::string name;
  std::vector<uint16_t> column_idx;  // 复合索引：列的序号
  bool unique = false;
  PageId root = kInvalidPageId;      // B+ 树根页号，建索引时写入
};

struct TableMeta {
  TableId id = 0;
  std::string name;
  std::string heap_path;      // 例：data/orders.heap
  uint16_t record_size = 0;   // 定长记录总字节数
  std::vector<Column> columns;
  std::vector<IndexMeta> indexes;

  const Column* FindColumn(const std::string& col_name) const;
  int FindColumnIndex(const std::string& col_name) const;  // 未找到返回 -1
};

class Catalog {
 public:
  explicit Catalog(const std::string& data_dir);

  Status Load();
  Status Save();

  // 传入表定义（record_size 与列 offset 由本方法计算并回填）
  Status CreateTable(TableMeta table, TableId* out_id);

  const TableMeta* FindTable(const std::string& table_name) const;
  const TableMeta* FindTable(TableId id) const;
  Status AddIndex(TableId id, const IndexMeta& index);

  const std::vector<TableMeta>& Tables() const { return tables_; }
  const std::string& CatalogPath() const { return catalog_path_; }

 private:
  std::string data_dir_;
  std::string catalog_path_;
  std::vector<TableMeta> tables_;
};

}  // namespace db
