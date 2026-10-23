// The CXML document reader libScePsm.sprx (FW 4.03) uses for .rco resource containers
// ("RCOF" files) and for the per-locale string documents embedded in them.
//
// Native document object (248 bytes, built by Create 0x2f7c80 / sub_2F7120):
//   +0   mode: 0 = tables copied from memory, 1 = file-backed (file table read on
//        demand from the fd), 2 = tables referencing a memory image
//   +4   80-byte header: magic, version, then (offset, size) of tree, idtable,
//        idhashtable, stringtable, wstringtable, hashtable, intarraytable,
//        floatarraytable, filetable
//   +88.. table pointers (+216 file table in the memory modes), +232 table allocator.
// Tree element: +0 name (stringtable offset), +4 attribute count, +8 parent, +12 prev,
// +16 next sibling, +20 first child, +24 last child, then 16-byte attributes
// {name, type, v1, v2}. Attribute types used here: 1 int, 2 float, 3 string (offset,
// byte length), 4 wstring (offset in UTF-16 units, byte length), 6 int array (index,
// count), 7 float array, 8 file (offset into the file table, size), 9 id (idtable offset;
// the id text is at +4).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ssn::psm::resources {

// The file a file-backed document reads from (the native keeps an fd and uses pread).
class FileSource {
 public:
  ~FileSource();
  // Opens a host file read-only; nullptr when it cannot be opened.
  static std::unique_ptr<FileSource> open(const std::string& host_path);
  // pread: returns the number of bytes read (short on EOF / error).
  size_t pread(void* dst, size_t size, uint64_t offset) const;
  uint64_t size() const;

 private:
  void* handle_ = nullptr;
};

struct CxmlHeader {
  uint32_t magic, version;
  uint32_t tree_off, tree_size;
  uint32_t idtable_off, idtable_size;
  uint32_t idhash_off, idhash_size;
  uint32_t string_off, string_size;
  uint32_t wstring_off, wstring_size;
  uint32_t hash_off, hash_size;
  uint32_t intarray_off, intarray_size;
  uint32_t floatarray_off, floatarray_size;
  uint32_t file_off, file_size;
};
static_assert(sizeof(CxmlHeader) == 80);

class CxmlDocument {
 public:
  enum Mode : uint32_t { kMemoryCopy = 0, kFile = 1, kMemoryRef = 2 };

  uint32_t mode = kFile;
  CxmlHeader header{};
  std::vector<uint8_t> tree, idtable, idhash, strings, wstrings, hash, intarray, floatarray;
  std::vector<uint8_t> files;  // file table (memory modes only)
  bool has_files = false;      // +216 non-null

  // The reader inlined in Create (file-backed, extractsOnMemory = false). Returns 0 or the
  // PSM error the native stores as the resource status.
  int32_t load_from_file(const FileSource& file);
  // sub_2F7120 (copy = true for mode 0, false for mode 2).
  int32_t load_from_memory(const uint8_t* data, size_t size, bool copy);

  // ---- element access, with the bounds tests libScePsm inlines at every use ----
  uint32_t u32(uint32_t off) const;  // tree dword (caller checked the range)
  // off >= 0, off + 28 <= tree size and the attributes fit.
  bool element_ok(int32_t off) const;
  // sub_2F7840: the name of a valid element (nullptr otherwise).
  const char* element_name(int32_t off) const;
  // stringtable text at `off` (nullptr when off < 0 or off >= string table size).
  const char* string_at(int32_t off) const;
  // The root element (0) when the tree holds it, else -1 (sub_2FF200 prologue).
  int32_t root() const;
  // sub_2F77D0 / sub_2F7760: first child / next sibling of a valid element, -1 if none.
  int32_t first_child(int32_t off) const;
  int32_t next_sibling(int32_t off) const;
  // sub_2F7890: first child of `parent` named `name`, -1 if none.
  int32_t find_child_named(int32_t parent, const char* name) const;
  // sub_2F7960: first child of `parent` named `name` whose "id" attribute (type 9 idtable
  // or type 3 string) equals `id`; `id` = nullptr matches any. -1 if none.
  int32_t find_element(int32_t parent, const char* name, const char* id) const;
  // Index of the first attribute of `elem` named `name`, or the attribute count when there
  // is none (callers decide what the native does in that case).
  uint32_t find_attribute(int32_t elem, const char* name) const;
};

}  // namespace ssn::psm::resources
