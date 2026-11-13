// 元数据管理：catalog.dat 的读写（纯文本，便于 diff 与人工排查）
// 文件格式：
//   TABLE|<id>|<name>|<heap_path>|<record_size>|<ncols>
//   COL|<name>|<type>|<len>|<offset>|<nullable>|<pk>
//   IDX|<name>|<col_csv>|<unique>|<root_page>
#include "db/catalog.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace db {
namespace {

std::vector<std::string> Split(const std::string& line, char delim) {
  std::vector<std::string> parts;
  std::string current;
  for (const char ch : line) {
    if (ch == delim) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  parts.push_back(current);
  return parts;
}

int ToInt(const std::string& text) { return std::atoi(text.c_str()); }

void EnsureDir(const std::string& dir) {
  if (dir.empty()) return;
#if defined(_WIN32)
  _mkdir(dir.c_str());
#else
  ::mkdir(dir.c_str(), 0755);
#endif
}

}  // namespace

const Column* TableMeta::FindColumn(const std::string& col_name) const {
  for (const auto& col : columns) {
    if (col.name == col_name) return &col;
  }
  return nullptr;
}

int TableMeta::FindColumnIndex(const std::string& col_name) const {
  for (size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].name == col_name) return static_cast<int>(i);
  }
  return -1;
}

Catalog::Catalog(const std::string& data_dir) : data_dir_(data_dir) {
  catalog_path_ = data_dir_ + "/catalog.dat";
}

Status Catalog::Load() {
  std::ifstream in(catalog_path_);
  if (!in.is_open()) {
    LOG_INFO_MSG("Catalog 未找到 " + catalog_path_ + "，按空库启动");
    return Status::kNotFound;
  }

  tables_.clear();
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const auto parts = Split(line, '|');
    if (parts.empty()) continue;

    if (parts[0] == "TABLE" && parts.size() >= 6) {
      TableMeta table;
      table.id = static_cast<TableId>(ToInt(parts[1]));
      table.name = parts[2];
      table.heap_path = parts[3];
      table.record_size = static_cast<uint16_t>(ToInt(parts[4]));
      tables_.push_back(table);
    } else if (parts[0] == "COL" && parts.size() >= 7 && !tables_.empty()) {
      Column col;
      col.name = parts[1];
      col.type = static_cast<ColumnType>(ToInt(parts[2]));
      col.len = static_cast<uint16_t>(ToInt(parts[3]));
      col.offset = static_cast<uint16_t>(ToInt(parts[4]));
      col.nullable = ToInt(parts[5]) != 0;
      col.primary_key = ToInt(parts[6]) != 0;
      tables_.back().columns.push_back(col);
    } else if (parts[0] == "IDX" && parts.size() >= 5 && !tables_.empty()) {
      IndexMeta index;
      index.name = parts[1];
      index.unique = ToInt(parts[3]) != 0;
      index.root = static_cast<PageId>(ToInt(parts[4]));
      for (const auto& item : Split(parts[2], ',')) {
        if (!item.empty()) index.column_idx.push_back(static_cast<uint16_t>(ToInt(item)));
      }
      tables_.back().indexes.push_back(index);
    }
  }

  LOG_INFO_MSG("Catalog 载入 " + std::to_string(tables_.size()) + " 张表：" + catalog_path_);
  return Status::kOk;
}

Status Catalog::Save() {
  EnsureDir(data_dir_);

  std::ofstream out(catalog_path_, std::ios::trunc);
  if (!out.is_open()) {
    LOG_ERROR_MSG("Catalog 写入失败：" + catalog_path_);
    return Status::kIoError;
  }

  for (const auto& table : tables_) {
    out << "TABLE|" << table.id << "|" << table.name << "|" << table.heap_path << "|"
        << table.record_size << "|" << table.columns.size() << "\n";
    for (const auto& col : table.columns) {
      out << "COL|" << col.name << "|" << static_cast<int>(col.type) << "|" << col.len << "|"
          << col.offset << "|" << (col.nullable ? 1 : 0) << "|" << (col.primary_key ? 1 : 0)
          << "\n";
    }
    for (const auto& index : table.indexes) {
      out << "IDX|" << index.name;
      out << "|";
      for (size_t i = 0; i < index.column_idx.size(); ++i) {
        if (i > 0) out << ",";
        out << index.column_idx[i];
      }
      out << "|" << (index.unique ? 1 : 0) << "|" << index.root << "\n";
    }
  }
  return Status::kOk;
}

Status Catalog::CreateTable(TableMeta table, TableId* out_id) {
  if (table.name.empty()) return Status::kInvalidArgument;
  if (FindTable(table.name) != nullptr) return Status::kDuplicate;
  if (table.columns.empty()) return Status::kInvalidArgument;

  // 定长布局：按列顺序累加偏移，record_size = 总字节数
  uint16_t offset = 0;
  for (auto& col : table.columns) {
    col.offset = offset;
    offset = static_cast<uint16_t>(offset + TypeSize(col.type, col.len));
  }
  table.record_size = offset;
  if (table.heap_path.empty()) table.heap_path = "data/" + table.name + ".heap";

  TableId next_id = 1;
  for (const auto& existing : tables_) {
    if (existing.id >= next_id) next_id = static_cast<TableId>(existing.id + 1);
  }
  table.id = next_id;
  tables_.push_back(table);
  if (out_id != nullptr) *out_id = table.id;
  return Status::kOk;
}

const TableMeta* Catalog::FindTable(const std::string& table_name) const {
  for (const auto& table : tables_) {
    if (table.name == table_name) return &table;
  }
  return nullptr;
}

const TableMeta* Catalog::FindTable(TableId id) const {
  for (const auto& table : tables_) {
    if (table.id == id) return &table;
  }
  return nullptr;
}

Status Catalog::AddIndex(TableId id, const IndexMeta& index) {
  for (auto& table : tables_) {
    if (table.id != id) continue;
    for (const auto& existing : table.indexes) {
      if (existing.name == index.name) return Status::kDuplicate;
    }
    table.indexes.push_back(index);
    return Status::kOk;
  }
  return Status::kNotFound;
}

}  // namespace db
