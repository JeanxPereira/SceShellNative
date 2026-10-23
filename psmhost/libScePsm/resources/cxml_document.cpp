#include "libScePsm/resources/cxml_document.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>

#include "libScePsm/imaging/psm_common.h"

namespace ssn::psm::resources {

// ---- FileSource: the fd + pread the native reader uses (sceKernelOpen O_RDONLY) ----

FileSource::~FileSource() {
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

std::unique_ptr<FileSource> FileSource::open(const std::string& host_path) {
  const std::wstring w = std::filesystem::path(host_path).wstring();
  HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return nullptr;
  auto f = std::unique_ptr<FileSource>(new FileSource());
  f->handle_ = h;
  return f;
}

size_t FileSource::pread(void* dst, size_t size, uint64_t offset) const {
  size_t done = 0;
  while (done < size) {
    OVERLAPPED ov{};
    const uint64_t at = offset + done;
    ov.Offset = static_cast<DWORD>(at);
    ov.OffsetHigh = static_cast<DWORD>(at >> 32);
    const DWORD chunk = static_cast<DWORD>(size - done > 0x40000000u ? 0x40000000u : size - done);
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), static_cast<uint8_t*>(dst) + done, chunk, &got, &ov) || got == 0) break;
    done += got;
  }
  return done;
}

uint64_t FileSource::size() const {
  LARGE_INTEGER s{};
  return GetFileSizeEx(static_cast<HANDLE>(handle_), &s) ? static_cast<uint64_t>(s.QuadPart) : 0;
}

namespace {

// The eight in-memory tables in header order (the file table is handled separately).
struct TableRef {
  const char* name;
  uint32_t CxmlHeader::*off;
  uint32_t CxmlHeader::*size;
  std::vector<uint8_t> CxmlDocument::*data;
};
constexpr TableRef kTables[] = {
    {"tree", &CxmlHeader::tree_off, &CxmlHeader::tree_size, &CxmlDocument::tree},
    {"idtable", &CxmlHeader::idtable_off, &CxmlHeader::idtable_size, &CxmlDocument::idtable},
    {"idhashtable", &CxmlHeader::idhash_off, &CxmlHeader::idhash_size, &CxmlDocument::idhash},
    {"stringtable", &CxmlHeader::string_off, &CxmlHeader::string_size, &CxmlDocument::strings},
    {"wstringtable", &CxmlHeader::wstring_off, &CxmlHeader::wstring_size, &CxmlDocument::wstrings},
    {"hashtable", &CxmlHeader::hash_off, &CxmlHeader::hash_size, &CxmlDocument::hash},
    {"intarraytable", &CxmlHeader::intarray_off, &CxmlHeader::intarray_size, &CxmlDocument::intarray},
    {"floatarraytable", &CxmlHeader::floatarray_off, &CxmlHeader::floatarray_size, &CxmlDocument::floatarray},
};

// The two termination checks both readers apply after loading the string tables.
bool strings_terminated(const CxmlDocument& d) { return d.strings.empty() || d.strings.back() == 0; }
bool wstrings_terminated(const CxmlDocument& d) {
  if (d.wstrings.empty()) return true;
  const size_t end = d.wstrings.size() & ~size_t(1);  // *(u16*)(ptr + (size & ~1) - 2)
  if (end < 2) return false;
  return d.wstrings[end - 2] == 0 && d.wstrings[end - 1] == 0;
}

}  // namespace

// The CXML reader inlined in CxmlResources/Native::Create (0x2f7c80, extractsOnMemory =
// false): mode 1; pread the 80-byte header at 0 ("CXML: header read error from file"
// -> 0x80B30501); (version | 0x10) must be 0x110 (0x80B30506); int/float array table sizes
// must be multiples of 4 (0x80B30505); each non-empty table is allocated (0x80B30504) and
// pread whole ("CXML: <table> read error from file" -> 0x80B30501); the string table must
// end with NUL and the wstring table with a UTF-16 NUL (0x80B30505). The file table stays
// on disk. The magic is not checked.
int32_t CxmlDocument::load_from_file(const FileSource& file) {
  mode = kFile;
  if (file.pread(&header, sizeof header, 0) != sizeof header) {
    std::fprintf(stderr, "CXML: header read error from file\n");
    return err::kCxmlRead;
  }
  if ((header.version | 0x10u) != 0x110u) return err::kCxmlVersion;
  if ((header.intarray_size & 3u) != 0 || (header.floatarray_size & 3u) != 0) return err::kCxmlCorrupt;
  for (const TableRef& t : kTables) {
    const uint32_t size = header.*t.size;
    if (size == 0) continue;
    std::vector<uint8_t>& v = this->*t.data;
    try {
      v.resize(size);
    } catch (const std::bad_alloc&) {
      return err::kCxmlAlloc;
    }
    const size_t got = file.pread(v.data(), size, header.*t.off);
    if (got != size) {
      std::fprintf(stderr, "CXML: %s read error from file (%zu != %u)\n", t.name, got, size);
      return err::kCxmlRead;
    }
    if (&v == &strings && !strings_terminated(*this)) return err::kCxmlCorrupt;
    if (&v == &wstrings && !wstrings_terminated(*this)) return err::kCxmlCorrupt;
  }
  has_files = false;
  return 0;
}

// sub_2F7120 — build a document from a memory image of `size` bytes. mode = copy ? 0 : 2.
// size < 80 -> "CXML: header read error from memory" 0x80B30501; version 0x80B30506; array
// alignment 0x80B30505; per table: offset >= size -> 0x80B30503, offset + length > size ->
// 0x80B30501, allocation 0x80B30504; string / wstring termination 0x80B30505; the file
// table (same bounds) is kept in memory (+216).
int32_t CxmlDocument::load_from_memory(const uint8_t* data, size_t size, bool copy) {
  mode = copy ? kMemoryCopy : kMemoryRef;
  if (size <= 0x4F) {
    std::fprintf(stderr, "CXML: header read error from memory (80 > %zu)\n", size);
    return err::kCxmlRead;
  }
  std::memcpy(&header, data, sizeof header);
  if ((header.version | 0x10u) != 0x110u) return err::kCxmlVersion;
  if ((header.intarray_size & 3u) != 0 || (header.floatarray_size & 3u) != 0) return err::kCxmlCorrupt;
  for (const TableRef& t : kTables) {
    const uint32_t len = header.*t.size;
    if (len == 0) continue;
    const uint64_t off = header.*t.off;
    if (off >= size) return err::kCxmlOffset;
    if (off + len > size) {
      std::fprintf(stderr, "CXML: %s read error from memory (%llu > %zu)\n", t.name,
                   static_cast<unsigned long long>(off + len), size);
      return err::kCxmlRead;
    }
    try {
      (this->*t.data).assign(data + off, data + off + len);
    } catch (const std::bad_alloc&) {
      return err::kCxmlAlloc;
    }
    if (t.data == &CxmlDocument::strings && !strings_terminated(*this)) return err::kCxmlCorrupt;
    if (t.data == &CxmlDocument::wstrings && !wstrings_terminated(*this)) return err::kCxmlCorrupt;
  }
  if (header.file_size != 0) {
    const uint64_t off = header.file_off;
    if (off >= size) return err::kCxmlOffset;
    if (off + header.file_size > size) {
      std::fprintf(stderr, "CXML: filetable read error from memory\n");
      return err::kCxmlRead;
    }
    try {
      files.assign(data + off, data + off + header.file_size);
    } catch (const std::bad_alloc&) {
      return err::kCxmlAlloc;
    }
    has_files = true;
  }
  return 0;
}

uint32_t CxmlDocument::u32(uint32_t off) const {
  uint32_t v;
  std::memcpy(&v, tree.data() + off, 4);
  return v;
}

bool CxmlDocument::element_ok(int32_t off) const {
  if (off < 0) return false;
  const uint64_t o = static_cast<uint32_t>(off);
  if (o + 28 > tree.size()) return false;
  return o + 28 + 16ull * u32(static_cast<uint32_t>(o + 4)) <= tree.size();
}

const char* CxmlDocument::string_at(int32_t off) const {
  if (off < 0 || static_cast<uint32_t>(off) >= strings.size()) return nullptr;
  return reinterpret_cast<const char*>(strings.data()) + off;
}

const char* CxmlDocument::element_name(int32_t off) const {
  if (!element_ok(off)) return nullptr;
  return string_at(static_cast<int32_t>(u32(static_cast<uint32_t>(off))));
}

int32_t CxmlDocument::root() const {
  if (tree.size() < 28) return -1;
  return 16ull * u32(4) + 28 <= tree.size() ? 0 : -1;
}

int32_t CxmlDocument::first_child(int32_t off) const {
  if (!element_ok(off)) return -1;
  const int32_t c = static_cast<int32_t>(u32(static_cast<uint32_t>(off) + 20));
  return element_ok(c) ? c : -1;
}

int32_t CxmlDocument::next_sibling(int32_t off) const {
  if (!element_ok(off)) return -1;
  const int32_t n = static_cast<int32_t>(u32(static_cast<uint32_t>(off) + 16));
  return element_ok(n) ? n : -1;
}

int32_t CxmlDocument::find_child_named(int32_t parent, const char* name) const {
  for (int32_t c = first_child(parent); c >= 0; c = next_sibling(c)) {
    const char* n = element_name(c);
    if (n && std::strcmp(n, name) == 0) return c;
  }
  return -1;
}

uint32_t CxmlDocument::find_attribute(int32_t elem, const char* name) const {
  const uint32_t count = u32(static_cast<uint32_t>(elem) + 4);
  for (uint32_t k = 0; k < count; ++k) {
    const char* an = string_at(static_cast<int32_t>(u32(static_cast<uint32_t>(elem) + 28 + 16 * k)));
    if (an && std::strcmp(an, name) == 0) return k;
  }
  return count;
}

// sub_2F7960. When the element has no "id" attribute the native still evaluates the slot
// just past its attributes (elem + 28 + 16 * count) as if it were one; that read is kept.
int32_t CxmlDocument::find_element(int32_t parent, const char* name, const char* id) const {
  int32_t c = first_child(parent);
  while (c >= 0) {
    const char* en = string_at(static_cast<int32_t>(u32(static_cast<uint32_t>(c))));
    if (en && std::strcmp(en, name) == 0) {
      if (!id) return c;
      const uint32_t count = u32(static_cast<uint32_t>(c) + 4);
      if (count != 0) {
        const uint32_t k = find_attribute(c, "id");
        const int64_t slot = int64_t(c) + 28 + 16 * int64_t(k);
        if (slot <= 0x7FFFFFFF && slot + 16 <= int64_t(tree.size())) {
          const uint32_t s = static_cast<uint32_t>(slot);
          const uint32_t type = u32(s + 4);
          const char* text = nullptr;
          if (type == 9) {
            const int32_t io = static_cast<int32_t>(u32(s + 8));
            if (io >= 0 && static_cast<uint32_t>(io) < idtable.size() && uint64_t(io) + 4 < idtable.size())
              text = reinterpret_cast<const char*>(idtable.data()) + io + 4;
          } else if (type == 3) {
            const int32_t so = static_cast<int32_t>(u32(s + 8));
            const uint32_t len = u32(s + 12);
            if (so >= 0 && static_cast<uint32_t>(so) < strings.size() && uint64_t(so) + len < strings.size() &&
                strings[static_cast<size_t>(so) + len] == 0)
              text = reinterpret_cast<const char*>(strings.data()) + so;
          }
          if (text && std::strcmp(text, id) == 0) return c;
        }
      }
    }
    c = next_sibling(c);
  }
  return -1;
}

}  // namespace ssn::psm::resources
