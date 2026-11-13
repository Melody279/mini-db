// 手搓热身 A：8KB 页 + 槽目录（slotted page）的最小实现
// 目标：考完试直接长成 src/storage 的 Page/HeapFile，所以这里用和正式代码一致的布局约定：
//   [Header 4B][Slot 目录 4B/槽][...... 空闲 ......][记录区，自页尾向下生长]
// 每条记录定长 64B，槽目录只记 (offset, used)，删除只把 used 置 0（页内碎片留给正式实现 compact）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kPageSize = 8192;
constexpr uint16_t kRecordSize = 64;

struct Header {
  uint16_t num_slots;  // 已分配的槽数（含已删除）
  uint16_t free_low;   // 记录区下界，新记录放在 free_low - kRecordSize
};

struct Slot {
  uint16_t offset;
  uint16_t used;
};

class Page {
 public:
  Page() : buf_(kPageSize, 0) {
    auto* h = header();
    h->num_slots = 0;
    h->free_low = static_cast<uint16_t>(kPageSize);
  }

  int Insert(const void* data) {
    if (CanHoldOneMore() == false) return -1;
    int slot_index = FindFreeSlot();
    const uint16_t new_low = static_cast<uint16_t>(header()->free_low - kRecordSize);
    std::memcpy(buf_.data() + new_low, data, kRecordSize);
    header()->free_low = new_low;

    if (slot_index < 0) {
      slot_index = static_cast<int>(header()->num_slots);
      header()->num_slots = static_cast<uint16_t>(header()->num_slots + 1);
    }
    auto* s = slot_at(slot_index);
    s->offset = new_low;
    s->used = 1;
    return slot_index;
  }

  bool Get(int slot_index, void* out) const {
    if (slot_index < 0 || slot_index >= static_cast<int>(header()->num_slots)) return false;
    const auto* s = slot_at(slot_index);
    if (s->used == 0) return false;
    std::memcpy(out, buf_.data() + s->offset, kRecordSize);
    return true;
  }

  bool Erase(int slot_index) {
    if (slot_index < 0 || slot_index >= static_cast<int>(header()->num_slots)) return false;
    auto* s = slot_at(slot_index);
    if (s->used == 0) return false;
    s->used = 0;
    return true;
  }

  uint16_t NumSlots() const { return header()->num_slots; }

  uint16_t UsedSlots() const {
    uint16_t used = 0;
    for (uint16_t i = 0; i < header()->num_slots; ++i) {
      if (slot_at(i)->used != 0) ++used;
    }
    return used;
  }

  uint16_t FreeBytes() const {
    const uint16_t slot_area_end = static_cast<uint16_t>(sizeof(Header) + header()->num_slots * sizeof(Slot));
    return static_cast<uint16_t>(header()->free_low - slot_area_end);
  }

 private:
  const Header* header() const { return reinterpret_cast<const Header*>(buf_.data()); }
  Header* header() { return reinterpret_cast<Header*>(buf_.data()); }
  const Slot* slot_at(int i) const { return reinterpret_cast<const Slot*>(buf_.data() + sizeof(Header) + i * sizeof(Slot)); }
  Slot* slot_at(int i) { return reinterpret_cast<Slot*>(buf_.data() + sizeof(Header) + i * sizeof(Slot)); }

  int FindFreeSlot() const {
    for (uint16_t i = 0; i < header()->num_slots; ++i) {
      if (slot_at(i)->used == 0) return static_cast<int>(i);
    }
    return -1;
  }

  // 预判：新记录放得下，且若还需要新槽，槽目录也不能撞上记录区
  bool CanHoldOneMore() const {
    int free_slot = FindFreeSlot();
    const uint16_t slot_area_end =
        static_cast<uint16_t>(sizeof(Header) + header()->num_slots * sizeof(Slot) +
                              (free_slot < 0 ? sizeof(Slot) : 0));
    return header()->free_low >= static_cast<uint16_t>(slot_area_end + kRecordSize);
  }

  std::vector<uint8_t> buf_;
};

struct Record {
  uint32_t id;
  char payload[60];
};

}  // namespace

int main() {
  Page page;
  Record rec{};
  int inserted = 0;

  for (int i = 0; i < 100; ++i) {
    rec.id = static_cast<uint32_t>(i);
    std::snprintf(rec.payload, sizeof(rec.payload), "order-%03d", i);
    if (page.Insert(&rec) < 0) break;
    ++inserted;
  }
  std::printf("inserted=%d slots=%u used=%u free=%u bytes\n", inserted, page.NumSlots(),
              page.UsedSlots(), page.FreeBytes());
  if (inserted != 100) {
    std::printf("FAIL: 100 条定长记录应能塞进 8KB 页\n");
    return 1;
  }

  for (int i = 0; i < 100; ++i) {
    Record read_back{};
    if (!page.Get(i, &read_back) || read_back.id != static_cast<uint32_t>(i)) {
      std::printf("FAIL: 第 %d 槽读回不一致\n", i);
      return 1;
    }
  }
  std::printf("ok   read back 100/100\n");

  for (int i = 0; i < 10; ++i) page.Erase(i);
  if (page.UsedSlots() != 90) {
    std::printf("FAIL: 删除 10 条后 used 应为 90\n");
    return 1;
  }
  std::printf("ok   erase 10, used=%u\n", page.UsedSlots());

  int reused = 0;
  for (int i = 0; i < 5; ++i) {
    rec.id = static_cast<uint32_t>(1000 + i);
    std::snprintf(rec.payload, sizeof(rec.payload), "new-%03d", i);
    if (page.Insert(&rec) >= 0) ++reused;
  }
  std::printf("ok   reuse %d erased slots, slots=%u used=%u\n", reused, page.NumSlots(),
              page.UsedSlots());

  for (int i = 0; i < 5; ++i) {
    Record read_back{};
    if (!page.Get(i, &read_back) || read_back.id != static_cast<uint32_t>(1000 + i)) {
      std::printf("FAIL: 复用槽 %d 内容错误\n", i);
      return 1;
    }
  }
  std::printf("RESULT: PASS (warmup_a_slotted_page)\n");
  return 0;
}
