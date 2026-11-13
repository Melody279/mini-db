// 存储层实现（结构对齐大赛 RMDB：DiskManager / Page / BufferPoolManager / Replacer / HeapFile）
//
// 已完成：文件与目录管理、页号分配（赛题给定实现）、Page 元信息与 page LSN 预留、
//        帧数组与页表的构造、缓冲池统计字段。
// 未完成：一律写成 rmdb 风格的编号 TODO + 显式失败返回，禁止静默返回"成功"。
// 手搓顺序与验收口径见 docs/storage手搓流程.md，与赛题模块的对应见 docs/rmdb对齐说明.md。
#include "db/storage.h"

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "db/replacer.h"

namespace db {

// -------------------- 平台差异集中在这里（Linux 走 POSIX，Windows 走 _xxx）--------------------
namespace port {

inline int OpenRdwr(const std::string& path) {
#if defined(_WIN32)
  return _open(path.c_str(), _O_RDWR | _O_CREAT, _S_IREAD | _S_IWRITE);
#else
  return ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
#endif
}

inline int OpenExcl(const std::string& path) {
#if defined(_WIN32)
  return _open(path.c_str(), _O_CREAT | _O_EXCL | _O_RDWR, _S_IREAD | _S_IWRITE);
#else
  return ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
#endif
}

inline void Close(int fd) {
#if defined(_WIN32)
  _close(fd);
#else
  ::close(fd);
#endif
}

inline bool IsDir(const std::string& path) {
#if defined(_WIN32)
  struct _stat st;
  if (_stat(path.c_str(), &st) != 0) return false;
  return (st.st_mode & _S_IFDIR) != 0;
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  return S_ISDIR(st.st_mode);
#endif
}

inline bool IsFile(const std::string& path) {
#if defined(_WIN32)
  struct _stat st;
  if (_stat(path.c_str(), &st) != 0) return false;
  return (st.st_mode & _S_IFREG) != 0;
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  return S_ISREG(st.st_mode);
#endif
}

inline int64_t FileSize(const std::string& path) {
#if defined(_WIN32)
  struct _stat st;
  if (_stat(path.c_str(), &st) != 0) return -1;
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return -1;
#endif
  return static_cast<int64_t>(st.st_size);
}

inline void MakeDir(const std::string& path) {
#if defined(_WIN32)
  _mkdir(path.c_str());
#else
  ::mkdir(path.c_str(), 0755);
#endif
}

}  // namespace port

// ==================== DiskManager ====================

DiskManager::DiskManager(std::string data_dir) : data_dir_(std::move(data_dir)) {
  if (data_dir_.empty()) data_dir_ = "data";
  if (!port::IsDir(data_dir_)) {
    port::MakeDir(data_dir_);
  }
  if (!port::IsDir(data_dir_)) {
    LOG_ERROR_MSG("DiskManager：数据目录无法创建 " + data_dir_);
    return;
  }
  LOG_INFO_MSG("DiskManager 就绪：data_dir=" + data_dir_);
}

DiskManager::~DiskManager() {
  for (const auto& item : fd2path_) {
    port::Close(item.first);
  }
  fd2path_.clear();
  path2fd_.clear();
}

bool DiskManager::IsDir(const std::string& path) const { return port::IsDir(path); }

void DiskManager::CreateDir(const std::string& path) {
  if (port::IsDir(path)) return;
  port::MakeDir(path);
  if (!port::IsDir(path)) {
    LOG_ERROR_MSG("DiskManager::CreateDir 失败：" + path);
  }
}

bool DiskManager::IsFile(const std::string& path) const { return port::IsFile(path); }

void DiskManager::CreateFile(const std::string& path) {
  // 对齐赛题要求：不能重复创建相同文件
  if (port::IsFile(path)) {
    LOG_WARN_MSG("DiskManager::CreateFile 文件已存在，跳过：" + path);
    return;
  }
  const int fd = port::OpenExcl(path);
  if (fd < 0) {
    LOG_ERROR_MSG("DiskManager::CreateFile 失败：" + path);
    return;
  }
  port::Close(fd);
}

void DiskManager::DestroyFile(const std::string& path) {
  // 对齐赛题要求：不能删除未关闭的文件
  if (path2fd_.find(path) != path2fd_.end()) {
    LOG_WARN_MSG("DiskManager::DestroyFile 文件仍打开，拒绝删除：" + path);
    return;
  }
  if (std::remove(path.c_str()) != 0) {
    LOG_WARN_MSG("DiskManager::DestroyFile 失败：" + path);
  }
}

int DiskManager::OpenFile(const std::string& path) {
  auto it = path2fd_.find(path);
  if (it != path2fd_.end()) return it->second;  // 已打开则复用，避免同一文件两个偏移游标打架

  if (!port::IsDir(data_dir_)) port::MakeDir(data_dir_);
  const int fd = port::OpenRdwr(path);
  if (fd < 0) {
    LOG_ERROR_MSG("DiskManager::OpenFile 失败：" + path);
    return -1;
  }
  path2fd_[path] = fd;
  fd2path_[fd] = path;

  // 关键：页号按文件实际长度回填，否则重启后从 0 重新分配会把老页覆盖掉
  const int64_t size = port::FileSize(path);
  const PageId pages = size <= 0 ? 0 : static_cast<PageId>(size / kPageSize);
  fd2pageno_[fd] = pages;
  LOG_INFO_MSG("DiskManager::OpenFile " + path + " fd=" + std::to_string(fd) +
               " pages=" + std::to_string(pages));
  return fd;
}

void DiskManager::CloseFile(int fd) {
  auto it = fd2path_.find(fd);
  if (it == fd2path_.end()) {
    LOG_WARN_MSG("DiskManager::CloseFile 未打开的文件 fd=" + std::to_string(fd));
    return;
  }
  path2fd_.erase(it->second);
  fd2path_.erase(it);
  fd2pageno_.erase(fd);
  if (fd == log_fd_) log_fd_ = -1;
  port::Close(fd);
}

std::string DiskManager::FileName(int fd) const {
  auto it = fd2path_.find(fd);
  return it == fd2path_.end() ? std::string() : it->second;
}

int DiskManager::FileFd(const std::string& path) { return OpenFile(path); }

Status DiskManager::WritePage(int fd, PageId page_no, const char* data, int num_bytes) {
  // TODO(题1-2 页式存储)：
  // 1. lseek(fd, static_cast<off_t>(page_no) * kPageSize, SEEK_SET) 定位页起点
  // 2. write(fd, data, num_bytes)，返回值必须等于 num_bytes，短写按 kIoError 处理
  // 3. 写完不要在这里 fsync（刷盘时机由缓冲池/WAL 决定，混在一起会拖慢性能测试）
  (void)fd;
  (void)page_no;
  (void)data;
  (void)num_bytes;
  LOG_WARN_MSG("DiskManager::WritePage 尚未实现（W1 题1-2）");
  return Status::kNotImplemented;
}

Status DiskManager::ReadPage(int fd, PageId page_no, char* out, int num_bytes) {
  // TODO(题1-2 页式存储)：
  // 1. 先看 fd2pageno_：页号超出已分配范围说明是"新页"，把 out 清 0 后返回 kOk
  // 2. 否则 lseek 到 page_no * kPageSize，read 满 num_bytes
  // 3. 短读也要把剩余字节清 0，绝不能让上层读到上一次残留的内存
  (void)fd;
  (void)page_no;
  (void)out;
  (void)num_bytes;
  LOG_WARN_MSG("DiskManager::ReadPage 尚未实现（W1 题1-2）");
  return Status::kNotImplemented;
}

PageId DiskManager::AllocatePage(int fd) {
  // 赛题给定的实现：简单自增。fd 未打开属于调用方错误，直接报错返回
  auto it = fd2pageno_.find(fd);
  if (it == fd2pageno_.end()) {
    LOG_ERROR_MSG("DiskManager::AllocatePage 文件未打开 fd=" + std::to_string(fd));
    return kInvalidPageId;
  }
  return it->second++;
}

void DiskManager::DeallocatePage(PageId page_no) {
  // 赛题里是空实现：不维护空闲页表，删页只是"逻辑作废"，页号不回收
  (void)page_no;
}

PageId DiskManager::NumPages(int fd) const {
  auto it = fd2pageno_.find(fd);
  return it == fd2pageno_.end() ? 0 : it->second;
}

void DiskManager::SetNumPages(int fd, PageId num_pages) { fd2pageno_[fd] = num_pages; }

Status DiskManager::ReadLog(char* out, int size, int offset) {
  // TODO(题4 事务与恢复)：W2 实现，读 WAL 时按 offset 顺序取记录
  (void)out;
  (void)size;
  (void)offset;
  LOG_WARN_MSG("DiskManager::ReadLog 尚未实现（W2）");
  return Status::kNotImplemented;
}

Status DiskManager::WriteLog(const char* data, int size) {
  // TODO(题4 事务与恢复)：W2 实现，追加写 + 每条记录带 LSN
  (void)data;
  (void)size;
  LOG_WARN_MSG("DiskManager::WriteLog 尚未实现（W2）");
  return Status::kNotImplemented;
}

// ==================== Page ====================

Page::Page() { ResetMemory(); }

void Page::ResetMemory() {
  std::memset(data_, 0, sizeof(data_));
  fd_ = -1;
  page_no_ = kInvalidPageId;
  is_dirty_ = false;
  pin_count_ = 0;
}

int32_t Page::GetPageLsn() const {
  int32_t lsn = 0;
  std::memcpy(&lsn, data_ + kLsnOffset, sizeof(lsn));
  return lsn;
}

void Page::SetPageLsn(int32_t lsn) { std::memcpy(data_ + kLsnOffset, &lsn, sizeof(lsn)); }

// ==================== BufferPoolManager ====================

BufferPoolManager::BufferPoolManager(size_t pool_size, std::string data_dir)
    : pool_size_(pool_size),
      pages_(new Page[pool_size]),
      disk_manager_(new DiskManager(std::move(data_dir))) {
  // 初始化时所有帧都在 free_list_ 里（对齐 rmdb 构造函数的做法）
  for (size_t i = 0; i < pool_size_; ++i) {
    free_list_.push_back(static_cast<frame_id_t>(i));
  }
  // 赛题默认 LRU；CLOCK 作为加分项，W2 用同一套 Replacer 接口替换即可
  replacer_.reset(new LRUReplacer(pool_size_));
  LOG_INFO_MSG("BufferPoolManager 初始化：frames=" + std::to_string(pool_size_) +
               " page_size=" + std::to_string(kPageSize));
}

BufferPoolManager::~BufferPoolManager() {
  delete[] pages_;
  pages_ = nullptr;
}

size_t BufferPoolManager::FreeFrames() const { return free_list_.size(); }

frame_id_t BufferPoolManager::FrameOf(int fd, PageId page_no) const {
  PageKey key{fd, page_no};
  auto it = page_table_.find(key);
  return it == page_table_.end() ? kInvalidFrameId : it->second;
}

bool BufferPoolManager::FindVictimPage(frame_id_t* frame_id) {
  // TODO(题1-1 缓冲池)：
  // 1. free_list_ 非空：从表头取一个帧，直接返回 true（说明池还没满）
  // 1.1 池已满：调用 replacer_->Victim(frame_id)，失败说明所有帧都被 pin 住，返回 false
  // 2. 取到的帧若 pin_count_ != 0 属于内部状态错误，打 ERROR 日志并返回 false
  (void)frame_id;
  LOG_WARN_MSG("BufferPoolManager::FindVictimPage 尚未实现（W1 题1-1）");
  return false;
}

void BufferPoolManager::UpdatePage(Page* page, int fd, PageId new_page_no, frame_id_t frame_id) {
  // TODO(题1-1 缓冲池)：
  // 1. 若 page->is_dirty_：调 disk_manager_->WritePage(旧 fd/旧页号, page->GetData(), kPageSize)，
  //    写成功后才 ClearDirty；写失败必须保持脏位，别把数据弄丢
  // 2. 从 page_table_ 删除旧键 {page->fd_, page->page_no_}
  // 3. page->ResetMemory()，再设置新的 fd_ / page_no_ / pin_count_ = 0
  // 4. page_table_[{fd, new_page_no}] = frame_id
  (void)page;
  (void)fd;
  (void)new_page_no;
  (void)frame_id;
}

Page* BufferPoolManager::FetchPage(int fd, PageId page_no) {
  // TODO(题1-1 缓冲池)：
  // 1. 命中：FrameOf(fd, page_no) 找到帧，++hits_，pages_[frame].Pin()，replacer_->Pin(frame)，返回
  // 2. 未命中：++misses_，FindVictimPage(&frame) 拿帧；失败返回 nullptr
  // 2.1 若该帧里是脏页，UpdatePage 会负责写回
  // 2.2 UpdatePage(&pages_[frame], fd, page_no, frame)
  // 2.3 disk_manager_->ReadPage(fd, page_no, pages_[frame].GetData(), kPageSize)，非 kOk 返回 nullptr
  // 3. 返回前 Pin() 并 replacer_->Pin(frame)；pin_count_ 从 0 变 1
  // 注意：hits_/misses_ 只在一次 FetchPage 里各加一次，别在子函数里重复统计
  (void)fd;
  (void)page_no;
  LOG_WARN_MSG("BufferPoolManager::FetchPage 尚未实现（W1 题1-1）");
  return nullptr;
}

bool BufferPoolManager::UnpinPage(int fd, PageId page_no, bool is_dirty) {
  // TODO(题1-1 缓冲池)：
  // 1. 页表里没有该页 -> 返回 false（调用方 fetch 后必须 unpin 同一个页）
  // 2. pin_count_ <= 0 -> 返回 false（重复 unpin 是调用方的 bug）
  // 3. pin_count_ 自减；归零后 replacer_->Unpin(frame)，这时它才允许被淘汰
  // 4. is_dirty == true 时置脏位；**只置脏，绝不清脏**（清脏只能发生在成功刷盘之后）
  (void)fd;
  (void)page_no;
  (void)is_dirty;
  LOG_WARN_MSG("BufferPoolManager::UnpinPage 尚未实现（W1 题1-1）");
  return false;
}

bool BufferPoolManager::FlushPage(int fd, PageId page_no) {
  // TODO(题1-1 缓冲池)：
  // 1. 页表没有该页返回 false
  // 2. 无论脏不脏都写回（赛题要求），写成功才 ClearDirty
  (void)fd;
  (void)page_no;
  LOG_WARN_MSG("BufferPoolManager::FlushPage 尚未实现（W1 题1-1）");
  return false;
}

Page* BufferPoolManager::NewPage(int fd, PageId* page_no_out) {
  // TODO(题1-1 缓冲池)：
  // 1. FindVictimPage(&frame)，失败返回 nullptr
  // 2. page_no = disk_manager_->AllocatePage(fd)，拿到不合法页号返回 nullptr
  // 3. UpdatePage(&pages_[frame], fd, page_no, frame)：脏页先刷盘，再重置为新页
  // 4. Pin() 并 replacer_->Pin(frame)；把页号写给调用方
  // 注意：新页此时只在内存里，磁盘上还没有；第一次刷盘后才真正占文件空间
  if (page_no_out != nullptr) *page_no_out = kInvalidPageId;
  (void)fd;
  LOG_WARN_MSG("BufferPoolManager::NewPage 尚未实现（W1 题1-1）");
  return nullptr;
}

bool BufferPoolManager::DeletePage(int fd, PageId page_no) {
  // TODO(题1-1 缓冲池)：
  // 1. 页表没有该页 -> true（本来就不在，删成功）
  // 2. pin_count_ != 0 -> false（正被别人用，不能删）
  // 3. 脏页先刷盘，从页表摘除，page->ResetMemory()，帧号放回 free_list_
  // 4. 注意页号不回收（DiskManager::DeallocatePage 是空实现），RID 指向它时靠 used=0 判失效
  (void)fd;
  (void)page_no;
  LOG_WARN_MSG("BufferPoolManager::DeletePage 尚未实现（W1 题1-1）");
  return false;
}

void BufferPoolManager::FlushAll() {
  // TODO(题1-1 缓冲池)：遍历页表，把所有脏页写回；W2 的检查点会复用它
  LOG_WARN_MSG("BufferPoolManager::FlushAll 尚未实现（W1 题1-1）");
}

void BufferPoolManager::FlushAll(int fd) {
  // TODO(题1-1 缓冲池)：只刷某个文件的页（一个缓冲池管九个表文件，关单表时用这个）
  (void)fd;
  LOG_WARN_MSG("BufferPoolManager::FlushAll(fd) 尚未实现（W1 题1-1）");
}

// ==================== HeapFile ====================

HeapFile::HeapFile(const std::string& path, BufferPoolManager* buffer_pool, uint16_t record_size)
    : path_(path), buffer_pool_(buffer_pool), record_size_(record_size) {
  if (buffer_pool_ == nullptr || buffer_pool_->Disk() == nullptr) {
    LOG_ERROR_MSG("HeapFile 构造失败：缓冲池为空 " + path_);
    return;
  }
  fd_ = buffer_pool_->Disk()->FileFd(path_);
  if (fd_ < 0) {
    LOG_ERROR_MSG("HeapFile 构造失败：无法打开 " + path_);
    return;
  }
  num_pages_ = buffer_pool_->Disk()->NumPages(fd_);  // 以文件长度为准，不信内存计数
  LOG_INFO_MSG("HeapFile 挂载：" + path_ + " fd=" + std::to_string(fd_) +
               " record_size=" + std::to_string(record_size_) +
               " pages=" + std::to_string(num_pages_));
}

HeapFile::~HeapFile() {
  // 关库顺序：Engine::Close 先 FlushAll 再析构 HeapFile，这里只关文件句柄
  if (buffer_pool_ != nullptr && buffer_pool_->Disk() != nullptr && fd_ >= 0) {
    buffer_pool_->Disk()->CloseFile(fd_);
  }
  fd_ = -1;
}

Status HeapFile::Insert(const RecBytes& record, RID* out) {
  // TODO(题1-2 页式存储 / slotted page)：
  // 1. record.size() != record_size_ 直接 kInvalidArgument（定长方案没有例外）
  // 2. 从 0 到 num_pages_ 逐页 FetchPage，调用页内插入辅助函数；成功则 UnpinPage(dirty=true)
  // 3. 所有页都满：NewPage(fd_) 追加一页再插，写成功后 ++num_pages_
  // 4. 页内布局：页头 4B（slot_count + free_low）从 Page::kHeaderOffset 开始，
  //    槽目录向下长、记录从页尾向上长，相撞即页满；删除槽可复用
  // 5. 完成后删掉下面两行占位
  (void)record;
  if (out != nullptr) *out = RID{};
  LOG_WARN_MSG("HeapFile::Insert 尚未实现（W1）");
  return Status::kNotImplemented;
}

Status HeapFile::Get(RID rid, RecBytes* out) const {
  // TODO：FetchPage(fd_, rid.page) -> 校验 rid.slot < slot_count 且 slot.used != 0
  //      -> 按 slot.offset 拷 record_size_ 字节 -> UnpinPage(dirty=false)
  // 注意：Get 是只读，任何情况下都不许标脏
  (void)rid;
  (void)out;
  LOG_WARN_MSG("HeapFile::Get 尚未实现（W1）");
  return Status::kNotImplemented;
}

Status HeapFile::Update(RID rid, const RecBytes& record) {
  // TODO：定长记录原地覆盖（偏移不变，RID 不变，索引不用改），写完 UnpinPage(dirty=true)
  (void)rid;
  (void)record;
  LOG_WARN_MSG("HeapFile::Update 尚未实现（W1）");
  return Status::kNotImplemented;
}

Status HeapFile::Remove(RID rid) {
  // TODO：只把 slot.used 置 0，不动 free_low（不 compact）；RID 依然指向该槽，读取时判失效
  (void)rid;
  LOG_WARN_MSG("HeapFile::Remove 尚未实现（W1）");
  return Status::kNotImplemented;
}

HeapFile::Iter::Iter(const HeapFile* file) : file_(file) {}

bool HeapFile::Iter::Next(RID* rid, RecBytes* record) {
  // TODO：跨页推进 + 跳过 used=0 的槽；每拿到一条记录就立刻 Unpin 当前页再返回
  (void)rid;
  (void)record;
  (void)file_;
  return false;
}

}  // namespace db
