// Sce.PlayStation.Core.Resources.CxmlResources native side (libScePsm.sprx, FW 4.03).
#include "libScePsm/resources/cxml_resource.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#include "host/boundary.h"
#include "libScePsm/imaging/image_core.h"
#include "libScePsm/imaging/psm_common.h"
#include "libScePsm/resources/cxml_document.h"

namespace ssn::psm::resources {
namespace {

// The ResourceObj (see cxml_resource.h for the native layout).
struct ResourceObj {
  std::unique_ptr<FileSource> file;          // +0 fd (null = -1)
  std::string filename;                      // +8
  std::unique_ptr<CxmlDocument> doc;         // +32
  std::unique_ptr<CxmlDocument> locale_doc;  // +40 (references +48)
  int32_t status = 0;                        // +56
  bool extract_on_memory = false;            // +60
  std::mutex mutex;                          // +64 "ScePsmResourceStr"
};

struct Holder {  // the 40-byte refcounted object the handle table points at
  std::shared_ptr<ResourceObj> res;          // +32
};

struct Entry {
  std::shared_ptr<Holder> holder;
  int32_t refs = 0;
};

std::mutex g_table_mutex;                         // X6AKrGoJOU0_h_A
std::unordered_map<uint32_t, Entry> g_table;      // xmmword_542450

std::shared_ptr<Holder> lookup(uint32_t handle) {
  std::lock_guard<std::mutex> lock(g_table_mutex);
  auto it = g_table.find(handle);
  return it == g_table.end() ? nullptr : it->second.holder;
}

uint32_t register_holder(std::shared_ptr<Holder> h) {
  std::lock_guard<std::mutex> lock(g_table_mutex);
  const uint32_t k = generate_handle([](uint32_t x) { return g_table.count(x) != 0; });
  g_table[k] = Entry{std::move(h), 1};
  return k;
}

// Resource table names (sub_2FF200) and element names (off_4A52F0, indexed by type - 3).
const char* table_name(uint32_t type) {
  switch (type) {
    case 2: return "stringtable";
    case 3: return "texturetable";
    case 6: return "sounddatatable";
    case 8: return "soundgrouptable";
    case 9: return "soundscripttable";
    case 10: return "filetable";
    case 11: return "layouttable";
    case 13: return "msgidtable";
    default: return nullptr;
  }
}
const char* const kElementNames[11] = {"texture", "texture", "texture", "sounddata", "texture", "soundgroup",
                                       "soundscript", "file", "layout", "texture", "locale"};
// Types accepted by the typed accessors: type - 3 < 11 and bit set in 1513 (3, 6, 8, 9,
// 10, 11, 13).
bool valid_type(uint32_t type) { return type - 3u < 11u && ((1513u >> (type - 3u)) & 1u); }

// sub_2FF200: the table element for `type`. For types without a table name the root
// element itself is returned; the result is -1 when the tree has no valid root or the
// named table is missing.
int32_t find_table(const CxmlDocument& doc, uint32_t type) {
  const int32_t root = doc.root();
  const char* name = table_name(type);
  if (!name) return root;
  return root < 0 ? -1 : doc.find_child_named(root, name);
}

// sub_2FF2F0: "<what> '<value>' not found" as the exception info.
void not_found(const char* what, const std::string& value) {
  set_exception_info(std::string(what) + " '" + value + "' not found");
}

// An element's attribute slot (elem + 28 + 16 * k) when it lies inside the tree.
bool slot_in_tree(const CxmlDocument& d, int32_t elem, uint32_t k, uint32_t* slot) {
  const int64_t s = int64_t(elem) + 28 + 16 * int64_t(k);
  if (s > 0x7FFFFFFF || s + 16 > int64_t(d.tree.size())) return false;
  *slot = static_cast<uint32_t>(s);
  return true;
}

// The element header bounds test of the attribute scans (elem + 28 + 16 * count fits).
bool attrs_in_tree(const CxmlDocument& d, int32_t elem) { return d.element_ok(elem); }

// A file attribute reference {document, offset, size} (the native passes {doc, int, u64}).
struct FileRef {
  const CxmlDocument* doc = nullptr;
  int32_t offset = -1;
  uint64_t size = 0;
};

// The "src"-style lookup shared by sub_2FF4B0 / sub_2FF740 and GetString: find attribute
// `name` of `elem`, require type `want`. 0 or 0x80B30507 / 0x80B3050A / 0x80B3050B.
int32_t typed_attribute(const CxmlDocument& d, int32_t elem, const char* name, uint32_t want, uint32_t* slot) {
  if (!attrs_in_tree(d, elem)) return err::kCxmlBounds;
  const uint32_t count = d.u32(static_cast<uint32_t>(elem) + 4);
  if (count == 0) return err::kCxmlAttrNotFound;
  const uint32_t k = d.find_attribute(elem, name);
  if (k == count) return err::kCxmlAttrNotFound;
  if (!slot_in_tree(d, elem, k, slot)) return err::kCxmlBounds;
  if (d.u32(*slot + 4) != want) return err::kCxmlAttrType;
  return 0;
}

// sub_2FFBD0 — load a file attribute's bytes into a new allocation (malloc here; the
// native uses the PSM allocator qword_53FBD8). size 0 -> "Size is zero", -1; memory
// documents copy from the in-memory file table ("GetAddr failed", -1 when out of range);
// file documents need mode 1 (0x80B30509) and pread the bytes at file_off + offset
// (0x80B30501 "Load failed" when short).
int32_t load_file_ref(const ResourceObj& res, const FileRef& ref, UnmanagedBuffer* out) {
  if (!ref.doc || ref.offset < 0) return err::kInvalidOperation;
  if (ref.size == 0) {
    set_exception_info("Size is zero");
    return -1;
  }
  auto* buf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(ref.size)));
  if (!buf) return err::kOutOfMemory;
  if (!res.file) {
    const CxmlDocument& d = *ref.doc;
    if ((d.mode | 2u) == 2u && d.has_files && int32_t(d.header.file_size) > ref.offset &&
        uint32_t(ref.offset) + uint32_t(ref.size) <= d.header.file_size) {
      std::memcpy(buf, d.files.data() + ref.offset, static_cast<size_t>(ref.size));
      out->address = reinterpret_cast<uintptr_t>(buf);
      out->size = static_cast<uint32_t>(ref.size);
      return 0;
    }
    set_exception_info("GetAddr failed");
    std::free(buf);
    return -1;
  }
  int32_t r = 0;
  if (ref.doc->mode != CxmlDocument::kFile) {
    r = err::kCxmlNotFileMode;
  } else if (ref.size > 0xFFFFFFFFull) {
    r = err::kCxmlTooLarge;
  } else {
    const uint64_t pos = uint64_t(ref.doc->header.file_off) + uint32_t(ref.offset);
    const size_t got = res.file->pread(buf, static_cast<size_t>(ref.size), pos);
    if (got == ref.size) {
      out->address = reinterpret_cast<uintptr_t>(buf);
      out->size = static_cast<uint32_t>(ref.size);
      return 0;
    }
    std::fprintf(stderr, "CXML: read error: pos=%llu, size to read(%zu != %llu)\n", static_cast<unsigned long long>(pos), got,
                 static_cast<unsigned long long>(ref.size));
    r = err::kCxmlRead;
  }
  set_exception_info("Load failed");
  std::free(buf);
  return r;
}

// sub_2FF4B0 / sub_2FF740 prologue: type check, table, element by id, "src" file attribute.
int32_t find_src(const ResourceObj& res, uint32_t type, const std::string& id, FileRef* ref) {
  if (!valid_type(type)) {
    append_exception_info("Unknown type");
    return err::kResources;
  }
  const char* child = kElementNames[type - 3];
  const CxmlDocument& d = *res.doc;
  const int32_t table = find_table(d, type);
  if (table < 0) {
    not_found("type", child);
    return err::kResources;
  }
  const int32_t elem = d.find_element(table, child, id.c_str());
  if (elem < 0) {
    not_found("id", id);
    return err::kResources;
  }
  uint32_t slot = 0;
  const int32_t r = typed_attribute(d, elem, "src", 8, &slot);
  if (r != 0) {
    not_found("src", id);
    return r;
  }
  ref->doc = &d;
  ref->offset = static_cast<int32_t>(d.u32(slot + 8));
  ref->size = d.u32(slot + 12);
  return 0;
}

// Opens and reads a file-backed resource (the loop body of Create): status 0x80B30011 and
// "Cannot open <name>" when the file cannot be opened; extractsOnMemory reads the whole
// file and parses it as a memory document (mode 0), dropping the fd.
std::shared_ptr<ResourceObj> open_resource(const std::string& console_path, const std::string& host_path,
                                           bool extracts_on_memory) {
  auto res = std::make_shared<ResourceObj>();
  res->filename = console_path;
  res->doc = std::make_unique<CxmlDocument>();
  res->file = FileSource::open(host_path);
  if (!res->file) {
    set_exception_info("Cannot open " + console_path);
    std::fprintf(stderr, "fopen(\"%s\", \"rb\") failed in ResourceObj\n", console_path.c_str());
    res->status = err::kFileNotFound;
    return res;
  }
  if (extracts_on_memory) {
    res->extract_on_memory = true;
    const uint64_t size = res->file->size();
    std::vector<uint8_t> image;
    int32_t r = 0;
    try {
      image.resize(static_cast<size_t>(size));
    } catch (const std::bad_alloc&) {
      std::fprintf(stderr, "[PSM] Not enough memory (%llu bytes) in ResourceObj\n", static_cast<unsigned long long>(size));
      set_exception_info("Not enough memory");
      res->file.reset();
      res->status = err::kOutOfMemory;
      return res;
    }
    if (res->file->pread(image.data(), image.size(), 0) == image.size()) r = res->doc->load_from_memory(image.data(), image.size(), true);
    res->file.reset();
    if (r != 0) {
      std::fprintf(stderr, "CreateFromFile() extractOnMemory failed 0x%x in ResourceObj\n", r);
      set_exception_info("Buffer is corrupted");
      res->doc = std::make_unique<CxmlDocument>();
      res->status = r;
    }
    return res;
  }
  const int32_t r = res->doc->load_from_file(*res->file);
  if (r != 0) {
    std::fprintf(stderr, "CreateFromFile() failed 0x%x in ResourceObj\n", r);
    res->doc = std::make_unique<CxmlDocument>();
    res->file.reset();
    res->status = r;
  }
  return res;
}

}  // namespace

// sub_2F6840 — path normalisation: empty segments are dropped once something was
// emitted, ".." removes the last emitted directory (never above "/").
std::string normalize_path(const std::string& s) {
  std::string out;
  auto up = [&out]() {
    if (out.size() <= 1) {
      out = "/";
      return;
    }
    size_t k = out.size() - 2;
    if (out[k] != '/') {
      bool found = false;
      while (k != 0) {
        if (out[k - 1] == '/') {
          --k;
          found = true;
          break;
        }
        --k;
      }
      if (!found) {
        out = "/";
        return;
      }
    }
    out.resize(k + 1);
  };
  size_t pos = 0;
  while (pos < s.size()) {
    const size_t i = s.find('/', pos);
    if (i == std::string::npos) {
      if (s.size() == pos + 2 && s[pos] == '.' && s[pos + 1] == '.') {
        up();
      } else {
        out.append(s, pos, std::string::npos);
      }
      return out;
    }
    if (i == pos && !out.empty()) {
      pos = i + 1;
      continue;
    }
    if (i == pos + 2 && s[pos] == '.' && s[pos + 1] == '.') {
      up();
    } else {
      out.append(s, pos, i + 1 - pos);
    }
    pos = i + 1;
  }
  return out;
}

// CxmlResources/Native::Create(out int, string, bool extractsOnMemory, bool
// systemCommonPath) — 0x2f7c80.
// Path resolution:
//   1. (Not reproduced, empty here) first, each directory of the override list
//      qword_542438 (filled by PsmFramework::Initialize from a boot parameter) is tried
//      with the file's base name; the first existing one wins.
//   2. systemCommonPath: "/system_ex/vsh_asset/" + filename, normalised (sub_2F6840);
//      (when the process was booted from /hostapp, byte_53FCA6, an existing
//      "/hostapp/system_ex/vsh_asset/" + filename replaces it — retail boots do not).
//      Up to 3 open attempts.
//   3. otherwise the PSS sandbox mapping (PssPathToSystemPath): the name must start with
//      /Application, /Documents or /Temp (or be "X:\..."), have fewer than 8 separators,
//      and is prefixed with the sandbox root byte_540DF0 — the root is not traced, so this
//      route reports PssPathToSystemPath failure (0x80B30011).
// The file is opened read-only and its CXML tables read (cxml_document.cpp). A failed
// open/parse is retried while attempts remain; the final status is returned (no handle,
// *handle = 0) or a handle from the shared generator is stored.
int32_t create(uint32_t* handle, const std::string& filename, bool extracts_on_memory, bool system_common_path,
               const PathResolver& resolve) {
  std::string console_path;
  int attempts = 1;
  if (system_common_path) {
    console_path = normalize_path("/system_ex/vsh_asset/" + filename);
    attempts = 3;
  } else {
    const std::string p = normalize_path(filename);
    auto starts = [&p](const char* pre) { return p.compare(0, std::strlen(pre), pre) == 0; };
    const bool pss = starts("/Application/") || starts("/Documents/") || starts("/Temp/") || starts("/Application") ||
                     starts("/Documents") || starts("/Temp") || (p.size() >= 3 && p.compare(1, 2, ":\\") == 0);
    int depth = 0;
    for (char c : p) depth += (c == '/' || c == '\\') ? 1 : 0;
    if (pss && depth >= 8) std::fprintf(stderr, "\"%s\" is too deep subdirectory depth\n", p.c_str());
    if (!pss || depth >= 8) {
      std::fprintf(stderr, "[PSM] PssPathToSystemPath(%s) returned error: 0x%x \n", filename.c_str(), uint32_t(err::kIo));
      *handle = 0;
      return err::kFileNotFound;
    }
    // Mapping (traced): root + "/" + the normalised path without its leading '/'.
    // The root is byte_540DF0, copied by PsmFramework::Initialize (0x2c9460) from its
    // init parameter +176; the ShellUI eboot passes "/app0/psm" (eboot 0xe500). /app0 is
    // the application's own mount, /system_ex/app/NPXS40087 for the ShellUI.
    static const std::string kSandboxRoot = "/system_ex/app/NPXS40087/psm";
    console_path = kSandboxRoot + "/" + (p.empty() || p[0] != '/' ? p : p.substr(1));
    attempts = 1;
  }
  const std::string host_path = resolve ? resolve(console_path) : console_path;
  std::shared_ptr<ResourceObj> res;
  for (int i = 0; i < attempts; ++i) {
    res = open_resource(console_path, host_path, extracts_on_memory);
    if (res->status >= 0) break;
  }
  if (res->status < 0) {
    *handle = 0;
    return res->status;
  }
  auto holder = std::make_shared<Holder>();
  holder->res = std::move(res);
  *handle = register_holder(std::move(holder));
  return 0;
}

// CxmlResources/Native::CreateFromBuffer(out int, byte[]) — 0x2f97b0: a memory document
// copied from the array (sub_2F7120, copy), file name "from buffer", extract-on-memory
// set, no fd. Parse errors: "CreateFromBuffer() failed", exception "Buffer is corrupted",
// status returned and *handle = 0.
int32_t create_from_buffer(uint32_t* handle, const uint8_t* data, size_t size) {
  auto res = std::make_shared<ResourceObj>();
  res->filename = "from buffer";
  res->extract_on_memory = true;
  res->doc = std::make_unique<CxmlDocument>();
  const int32_t r = data ? res->doc->load_from_memory(data, size, true) : err::kCxmlRead;
  if (r != 0) {
    std::fprintf(stderr, "CreateFromBuffer() failed 0x%x in ResourceObj\n", r);
    set_exception_info("Buffer is corrupted");
    *handle = 0;
    return r;
  }
  auto holder = std::make_shared<Holder>();
  holder->res = std::move(res);
  *handle = register_holder(std::move(holder));
  return 0;
}

// CxmlResources/Native::Delete(int) — 0x2f9b20: refcount -1, entry removed at zero;
// always 0.
int32_t destroy(uint32_t handle) {
  std::lock_guard<std::mutex> lock(g_table_mutex);
  auto it = g_table.find(handle);
  if (it != g_table.end() && --it->second.refs == 0) g_table.erase(it);
  return 0;
}

// CxmlResources/Native::SetLocale(int, string) — 0x2f9c10. Under the resource mutex:
// find the root's "stringtable" child, then its <locale id="..."> child (sub_2F7960); when
// missing ("GetLocaleElement() failed in SetLocale") retry with "en"; when that is missing
// too, return 0 and keep the current string document. The locale element's "src" file
// attribute (else "GetFile() failed", exception "Locale not found", 0x80B30030) is loaded:
// file-backed resources pread it into a new buffer (doc mode must be 1, else 0x80B30509;
// short read 0x80B30501; "Load() failed"); extract-on-memory resources point into the
// in-memory file table (else "Failed to create stringtable", 0x80B30013). The bytes become
// a new string document (sub_2F7120, mode 2); a parse error clears it and is returned.
int32_t set_locale(uint32_t handle, const std::string& locale) {
  auto h = lookup(handle);
  if (!h || !h->res) return err::kObjectDisposed;
  ResourceObj& res = *h->res;
  std::lock_guard<std::mutex> lock(res.mutex);
  const CxmlDocument& d = *res.doc;
  const int32_t root = d.root();
  const int32_t table = root < 0 ? -1 : d.find_child_named(root, "stringtable");
  int32_t loc = table < 0 ? -1 : d.find_element(table, "locale", locale.c_str());
  if (loc < 0) {
    std::fprintf(stderr, "GetLocaleElement() failed in %s\n", "SetLocale");
    loc = table < 0 ? -1 : d.find_element(table, "locale", "en");
    if (loc < 0) return 0;
  }
  uint32_t slot = 0;
  const int32_t a = typed_attribute(d, loc, "src", 8, &slot);
  if (a != 0) {
    std::fprintf(stderr, "GetFile() failed 0x%x in %s\n", a, "SetLocale");
    set_exception_info("Locale not found");
    return err::kResources;
  }
  const int32_t off = static_cast<int32_t>(d.u32(slot + 8));
  const uint32_t size = d.u32(slot + 12);
  std::vector<uint8_t> bytes;
  if (!res.extract_on_memory) {
    try {
      bytes.resize(size);
    } catch (const std::bad_alloc&) {
      std::fprintf(stderr, "rscallocator->Alloc() failed in %s\n", "SetLocale");
      return err::kOutOfMemory;
    }
    int32_t r = err::kCxmlNotFileMode;
    if (d.mode == CxmlDocument::kFile && res.file) {
      const uint64_t pos = uint64_t(d.header.file_off) + uint32_t(off);
      r = res.file->pread(bytes.data(), size, pos) == size ? 0 : err::kCxmlRead;
    }
    if (r != 0) {
      std::fprintf(stderr, "Load() failed 0x%08x in %s\n", r, "SetLocale");
      return r;
    }
  } else {
    if (!((d.mode | 2u) == 2u && d.has_files && int32_t(d.header.file_size) > off &&
          uint32_t(off) + size <= d.header.file_size)) {
      set_exception_info("Failed to create stringtable");
      return err::kOutOfMemory;
    }
    bytes.assign(d.files.begin() + off, d.files.begin() + off + size);
  }
  auto ld = std::make_unique<CxmlDocument>();
  const int32_t r = ld->load_from_memory(bytes.data(), bytes.size(), false);
  if (r != 0) {
    std::fprintf(stderr, "create cxml document failed (0x%08x) in %s\n", r, "SetLocale");
    res.locale_doc.reset();
    return r;
  }
  res.locale_doc = std::move(ld);
  return 0;
}

// CxmlResources/Native::HasResourceType(int, uint, out uint) — 0x2fafb0: result = the
// table element for `type` exists (sub_2FF200; types without a table name test the root).
// Null out -> 0x80B30002.
int32_t has_resource_type(uint32_t handle, uint32_t type, uint32_t* result) {
  auto h = lookup(handle);
  if (!h) return err::kObjectDisposed;
  if (!result) return err::kArgumentNull;
  if (!h->res) return err::kObjectDisposed;
  *result = find_table(*h->res->doc, type) >= 0 ? 1u : 0u;
  return 0;
}

// CxmlResources/Native::GetIds(int, uint, out string) — 0x2fa840: walks the children of
// the table element (any element name) and joins their "id" attribute texts (idtable,
// type 9) with ','. The walk stops at the first child without an "id" attribute or whose
// "id" is not of type 9. A missing table gives "". Always 0.
int32_t get_ids(uint32_t handle, uint32_t type, std::string* ids) {
  auto h = lookup(handle);
  if (!h || !h->res) return err::kObjectDisposed;
  const CxmlDocument& d = *h->res->doc;
  std::string out;
  const int32_t table = find_table(d, type);
  bool first = true;
  for (int32_t c = table < 0 ? -1 : d.first_child(table); c >= 0;) {
    const uint32_t count = d.u32(static_cast<uint32_t>(c) + 4);
    if (count == 0) break;
    const uint32_t k = d.find_attribute(c, "id");
    if (k == count) break;
    uint32_t slot = 0;
    if (!slot_in_tree(d, c, k, &slot) || d.u32(slot + 4) != 9) break;
    const int32_t io = static_cast<int32_t>(d.u32(slot + 8));
    const char* text = (io >= 0 && uint64_t(io) + 4 < d.idtable.size())
                           ? reinterpret_cast<const char*>(d.idtable.data()) + io + 4
                           : "";
    if (!first) out += ',';
    out += text;
    first = false;
    c = d.next_sibling(c);
  }
  *ids = std::move(out);
  return 0;
}

// CxmlResources/Native::GetIdCount(int, uint, out uint) — 0x2fac70: number of children of
// the type's table named like the type's element (off_4A52F0). Null out -> 0x80B30002;
// unsupported type -> exception "...Unknown type", 0x80B30030; missing table -> "type
// '<element>' not found", 0x80B30030.
int32_t get_id_count(uint32_t handle, uint32_t type, uint32_t* count) {
  auto h = lookup(handle);
  if (!h || !h->res) return err::kObjectDisposed;
  if (!count) return err::kArgumentNull;
  if (!valid_type(type)) {
    append_exception_info("Unknown type");
    return err::kResources;
  }
  const char* child = kElementNames[type - 3];
  const CxmlDocument& d = *h->res->doc;
  const int32_t table = find_table(d, type);
  if (table < 0) {
    not_found("type", child);
    return err::kResources;
  }
  *count = 0;
  uint32_t n = 0;
  for (int32_t c = d.first_child(table); c >= 0; c = d.next_sibling(c)) {
    const char* en = d.string_at(static_cast<int32_t>(d.u32(static_cast<uint32_t>(c))));
    if (en && std::strcmp(en, child) == 0) *count = ++n;
  }
  return 0;
}

// CxmlResources/Native::GetTextureResource(int, uint, string, ref TextureResourceOption,
// out TextureResourceNative) — 0x2fb680.
//  * out zeroed (56 bytes); type check / table / element-by-id as GetIdCount (errors
//    0x80B30030 with "type '...' not found" / "id '...' not found");
//  * "ninepatch" int-array attribute (type 6, index + count within the int array table)
//    -> flag 2, up to 16 bytes into NinePatchMargin; "margin" likewise -> flag 1, Margin;
//    "alpha8" string attribute equal to "on" (length 2) -> flag 4;
//  * resize = option && (Width > 0 || Height > 0); is4K = option && PixelDensity > 1.5;
//  * no resize: the file attribute "src_4k" (is4K) or "src"; an is4K miss falls back to
//    "src" (then not 4K). Missing / wrong type -> "%s '%s' not found." with the requested
//    name, 0x80B3050A / 0x80B3050B (0x80B30507 outside the tree);
//  * resize: candidates "src", "src_lv1", "src_lv2", "src_lv3" in order; a later candidate
//    replaces the current one unless the current one's decoded size * 1.1 already covers
//    the requested Width and Height (the current one is loaded and header-decoded, sub_295880;
//    a load or decode failure keeps the current one);
//  * the chosen bytes are loaded into a new buffer (sub_2FFBD0) -> UBuffer;
//  * without resize: chosen src_4k -> flags |= 8|0x10; chosen "src" for a non-4K request
//    and a file "src_4k" present -> flags |= 0x10.
int32_t get_texture_resource(uint32_t handle, uint32_t type, const std::string& id, const TextureResourceOption* option,
                             TextureResourceNative* out) {
  auto h = lookup(handle);
  if (!h || !h->res) return err::kObjectDisposed;
  const ResourceObj& res = *h->res;
  std::memset(out, 0, sizeof *out);
  if (!valid_type(type)) {
    append_exception_info("Unknown type");
    return err::kResources;
  }
  const char* child = kElementNames[type - 3];
  const CxmlDocument& d = *res.doc;
  const int32_t table = find_table(d, type);
  if (table < 0) {
    not_found("type", child);
    return err::kResources;
  }
  const int32_t elem = d.find_element(table, child, id.c_str());
  if (elem < 0) {
    not_found("id", id);
    return err::kResources;
  }

  // Margins and the alpha8 switch.
  auto int_array = [&](const char* name, int32_t* dst, int32_t flag) {
    if (!attrs_in_tree(d, elem)) return;
    const uint32_t count = d.u32(static_cast<uint32_t>(elem) + 4);
    const uint32_t k = d.find_attribute(elem, name);
    if (count == 0 || k == count) return;
    uint32_t slot = 0;
    if (!slot_in_tree(d, elem, k, &slot) || d.u32(slot + 4) != 6) return;
    const int32_t idx = static_cast<int32_t>(d.u32(slot + 8));
    const uint32_t n = d.u32(slot + 12);
    if (idx < 0 || 4ull * (uint64_t(uint32_t(idx)) + n) > d.header.intarray_size || d.intarray.empty()) return;
    out->attributes |= flag;
    const size_t bytes = 4ull * n < 16 ? size_t(4ull * n) : 16;
    std::memcpy(dst, d.intarray.data() + 4ull * uint32_t(idx), bytes);
  };
  int_array("ninepatch", out->nine_patch, 2);
  int_array("margin", out->margin, 1);
  if (attrs_in_tree(d, elem)) {
    const uint32_t count = d.u32(static_cast<uint32_t>(elem) + 4);
    const uint32_t k = d.find_attribute(elem, "alpha8");
    uint32_t slot = 0;
    if (count != 0 && k != count && slot_in_tree(d, elem, k, &slot) && d.u32(slot + 4) == 3) {
      const int32_t so = static_cast<int32_t>(d.u32(slot + 8));
      const uint32_t len = d.u32(slot + 12);
      if (so >= 0 && uint64_t(so) + len < d.strings.size() && d.strings[size_t(so) + len] == 0 && len == 2 &&
          std::strcmp(reinterpret_cast<const char*>(d.strings.data()) + so, "on") == 0)
        out->attributes |= 4;
    }
  }

  const bool resize = option && (option->width > 0 || option->height > 0);
  const bool want4k = option && option->pixel_density > 1.5f;

  // A file attribute of the element: 0 and the reference, or the native error code.
  auto file_attr = [&](const char* name, FileRef* ref) -> int32_t {
    if (!attrs_in_tree(d, elem)) return err::kCxmlBounds;
    const uint32_t count = d.u32(static_cast<uint32_t>(elem) + 4);
    if (count == 0) return err::kCxmlAttrNotFound;
    const uint32_t k = d.find_attribute(elem, name);
    if (k == count) return err::kCxmlAttrNotFound;
    uint32_t slot = 0;
    if (!slot_in_tree(d, elem, k, &slot)) return err::kCxmlBounds;
    if (d.u32(slot + 4) != 8) return err::kCxmlAttrType;
    ref->doc = &d;
    ref->offset = static_cast<int32_t>(d.u32(slot + 8));
    ref->size = d.u32(slot + 12);
    return 0;
  };

  FileRef chosen;
  int32_t r = 0;
  bool is4k = want4k;
  if (!resize) {
    r = file_attr(want4k ? "src_4k" : "src", &chosen);
    if (r != 0 && want4k) {
      chosen = FileRef{};
      r = file_attr("src", &chosen);
      if (r == 0) is4k = false;
    }
  } else {
    static const char* const kLevels[4] = {"src", "src_lv1", "src_lv2", "src_lv3"};
    for (const char* lv : kLevels) {
      FileRef cand;
      const int32_t a = file_attr(lv, &cand);
      if (a != 0) {
        r = a;
        continue;
      }
      r = 0;
      if (!chosen.doc || chosen.offset < 0) {
        chosen = cand;
        continue;
      }
      UnmanagedBuffer probe{};
      if (load_file_ref(res, chosen, &probe) != 0) break;  // keep the current candidate
      int32_t w = 0, hgt = 0;
      const bool ok = imaging::probe_file_image_size(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(probe.address)),
                                                     probe.size, &w, &hgt);
      std::free(reinterpret_cast<void*>(static_cast<uintptr_t>(probe.address)));
      if (!ok) break;
      if (float(w) * 1.1f >= float(option->width) && float(hgt) * 1.1f >= float(option->height)) break;
      chosen = cand;
    }
  }
  if (!chosen.doc || chosen.offset < 0) {
    // (A found attribute with a negative file offset also lands here, with r == 0.)
    set_exception_info(std::string(want4k ? "src_4k" : "src") + " '" + id + "' not found.");
    return r;
  }
  r = load_file_ref(res, chosen, &out->ubuffer);
  if (r != 0 || resize) return r;
  if (is4k) {
    out->attributes |= 0x18;
  } else if (!want4k) {
    FileRef k4;
    if (file_attr("src_4k", &k4) == 0) out->attributes |= 0x10;
  }
  return 0;
}

// CxmlResources/Native::GetString(int, string, out string) — 0x2fa4a0: under the resource
// mutex, the <string id="..."> child of the string document's root (sub_2F7960) and its
// "src" wstring attribute (type 4; offset in UTF-16 units within the wstring table, length
// in bytes, NUL-terminated at offset + length). No string document -> "No string
// document", 0x80B30030; no element -> "id '...' not found", 0x80B30030; then 0x80B30507 /
// 0x80B3050A / 0x80B3050B / 0x80B30505 for a bad attribute. Errors return "".
int32_t get_string(uint32_t handle, const std::string& id, std::u16string* out, bool* out_set) {
  *out_set = false;
  auto h = lookup(handle);
  if (!h) return err::kObjectDisposed;
  *out_set = true;
  out->clear();
  if (!h->res) return err::kObjectDisposed;
  ResourceObj& res = *h->res;
  std::lock_guard<std::mutex> lock(res.mutex);
  if (!res.locale_doc) {
    set_exception_info("No string document");
    return err::kResources;
  }
  const CxmlDocument& d = *res.locale_doc;
  const int32_t elem = d.root() < 0 ? -1 : d.find_element(d.root(), "string", id.c_str());
  if (elem < 0) {
    not_found("id", id);
    return err::kResources;
  }
  uint32_t slot = 0;
  int32_t r = typed_attribute(d, elem, "src", 4, &slot);
  if (r != 0) return r;
  const int32_t wo = static_cast<int32_t>(d.u32(slot + 8));
  const uint32_t len = d.u32(slot + 12);
  if (wo < 0 || 2ull * uint32_t(wo) >= d.header.wstring_size || d.wstrings.empty()) return err::kCxmlBounds;
  const size_t base = 2ull * uint32_t(wo);
  const size_t term = base + (len & ~1u);
  if (term + 2 > d.wstrings.size() || d.wstrings[term] != 0 || d.wstrings[term + 1] != 0) return err::kCxmlCorrupt;
  for (size_t p = base; p + 1 < d.wstrings.size(); p += 2) {
    const char16_t c = char16_t(d.wstrings[p] | (d.wstrings[p + 1] << 8));
    if (c == 0) break;
    out->push_back(c);
  }
  return 0;
}

// CxmlResources/Native::GetResourceSize(int, uint, string, out uint) — 0x2fb0e0 (sub_2FF4B0):
// the "src" file attribute's size. A resource without document: *size = 0, 0x80B30005.
int32_t get_resource_size(uint32_t handle, uint32_t type, const std::string& id, uint32_t* size) {
  auto h = lookup(handle);
  if (!h) return err::kObjectDisposed;
  if (!h->res) {
    *size = 0;
    return err::kObjectDisposed;
  }
  FileRef ref;
  const int32_t r = find_src(*h->res, type, id, &ref);
  if (r == 0) *size = static_cast<uint32_t>(ref.size);
  return r;
}

// sub_2FF740 — copy the "src" bytes into `buffer` (length `length`): memory documents copy
// `length` bytes from the in-memory file table ("GetAddr failed", -1 when out of range);
// file documents need mode 1 (0x80B30509) and size <= length (0x80B30508), then pread
// (0x80B30501); failures set "Load failed".
static int32_t copy_src(const ResourceObj& res, uint32_t type, const std::string& id, uint8_t* buffer, uint32_t length) {
  FileRef ref;
  int32_t r = find_src(res, type, id, &ref);
  if (r != 0) return r;
  const CxmlDocument& d = *ref.doc;
  if (!res.file) {
    if ((d.mode | 2u) == 2u && d.has_files && int32_t(d.header.file_size) > ref.offset &&
        uint32_t(ref.offset) + uint32_t(ref.size) <= d.header.file_size) {
      // The native copies `length` bytes; clamp to the file table so the host stays in bounds.
      const size_t avail = d.files.size() - size_t(ref.offset);
      std::memcpy(buffer, d.files.data() + ref.offset, length < avail ? length : avail);
      return 0;
    }
    set_exception_info("GetAddr failed");
    return -1;
  }
  if (d.mode != CxmlDocument::kFile) {
    r = err::kCxmlNotFileMode;
  } else if (ref.size > length) {
    r = err::kCxmlTooLarge;
  } else {
    const uint64_t pos = uint64_t(d.header.file_off) + uint32_t(ref.offset);
    if (res.file->pread(buffer, static_cast<size_t>(ref.size), pos) == ref.size) return 0;
    r = err::kCxmlRead;
  }
  set_exception_info("Load failed");
  return r;
}

// CxmlResources/Native::GetResourceBuffer(int, uint, string, byte[]) — 0x2fb1e0.
int32_t get_resource_buffer(uint32_t handle, uint32_t type, const std::string& id, uint8_t* buffer, uint32_t buffer_length) {
  auto h = lookup(handle);
  if (!h || !h->res) return err::kObjectDisposed;
  if (!buffer) return err::kArgumentNull;  // the native dereferences the array unchecked
  return copy_src(*h->res, type, id, buffer, buffer_length);
}

// CxmlResources/Native::GetResourceBufferPtr(int, uint, string, ref UnmanagedBuffer) —
// 0x2fb300: null ref -> 0x80B30030; Size = the "src" size (sub_2FF4B0), a new buffer of
// that size (0x80B30013), filled by sub_2FF740; Address set on success.
int32_t get_resource_buffer_ptr(uint32_t handle, uint32_t type, const std::string& id, UnmanagedBuffer* buffer) {
  auto h = lookup(handle);
  if (!h || !h->res) return err::kObjectDisposed;
  if (!buffer) return err::kResources;
  FileRef ref;
  int32_t r = find_src(*h->res, type, id, &ref);
  if (r < 0) return r;
  buffer->size = static_cast<uint32_t>(ref.size);
  auto* mem = static_cast<uint8_t*>(std::malloc(buffer->size ? buffer->size : 1));
  if (!mem) return err::kOutOfMemory;
  r = copy_src(*h->res, type, id, mem, buffer->size);
  if (r < 0) {
    std::free(mem);  // the native leaks this block
    return r;
  }
  buffer->address = reinterpret_cast<uintptr_t>(mem);
  return 0;
}

// CxmlResources/Native::GetFromResourceBufferPtr(int, ref UnmanagedBuffer, byte[]) —
// 0x2fb570: null ref / array -> 0x80B30002; Address 0 or Size 0 -> 0x80B30005; array
// length != Size -> 0x80B30003; else the bytes are copied.
int32_t get_from_resource_buffer_ptr(uint32_t handle, const UnmanagedBuffer* buffer, uint8_t* managed,
                                     uint32_t managed_length, bool managed_null) {
  auto h = lookup(handle);
  if (!h) return err::kObjectDisposed;
  if (!buffer || managed_null) return err::kArgumentNull;
  if (buffer->address == 0 || buffer->size == 0) return err::kObjectDisposed;
  if (buffer->size != managed_length) return err::kOutOfRange;
  std::memcpy(managed, reinterpret_cast<const void*>(static_cast<uintptr_t>(buffer->address)), managed_length);
  return 0;
}

// CxmlResources/Native::DisposeResourceBufferPtr(int, ref UnmanagedBuffer) — 0x2fb460:
// frees Address (null ref / Address 0 -> 0x80B30030; resource without document ->
// 0x80B30005). The managed struct is not modified.
int32_t dispose_resource_buffer_ptr(uint32_t handle, const UnmanagedBuffer* buffer) {
  auto h = lookup(handle);
  if (!h) return err::kObjectDisposed;
  if (!h->res) return err::kObjectDisposed;
  if (!buffer || buffer->address == 0) return err::kResources;
  std::free(reinterpret_cast<void*>(static_cast<uintptr_t>(buffer->address)));
  return 0;
}

// ---- sound scripts ----

namespace {

// A string attribute (type 3) of a valid element, as both sound-script readers test it:
// offset inside the string table and NUL at offset + length; GetSoundScriptAll also
// requires length != 0 (`nonempty`). nullptr otherwise.
const char* string_attr(const CxmlDocument& d, int32_t elem, const char* name, bool nonempty) {
  if (!d.element_ok(elem)) return nullptr;
  const uint32_t count = d.u32(static_cast<uint32_t>(elem) + 4);
  const uint32_t k = d.find_attribute(elem, name);
  uint32_t slot = 0;
  if (count == 0 || k == count || !slot_in_tree(d, elem, k, &slot) || d.u32(slot + 4) != 3) return nullptr;
  const int32_t so = static_cast<int32_t>(d.u32(slot + 8));
  const uint32_t len = d.u32(slot + 12);
  if (so < 0 || static_cast<uint32_t>(so) >= d.strings.size()) return nullptr;
  if (uint64_t(so) + len >= d.strings.size() || d.strings[size_t(so) + len] != 0) return nullptr;
  if (nonempty && len == 0) return nullptr;
  return reinterpret_cast<const char*>(d.strings.data()) + so;
}

// An int-array attribute (type 6) whose index + count lies inside the int array table:
// copies min(count, cap) values to `dst` and returns that number (0 when absent).
uint32_t int_array_attr(const CxmlDocument& d, int32_t elem, const char* name, int32_t* dst, uint32_t cap) {
  if (!d.element_ok(elem)) return 0;
  const uint32_t count = d.u32(static_cast<uint32_t>(elem) + 4);
  const uint32_t k = d.find_attribute(elem, name);
  uint32_t slot = 0;
  if (count == 0 || k == count || !slot_in_tree(d, elem, k, &slot) || d.u32(slot + 4) != 6) return 0;
  const int32_t idx = static_cast<int32_t>(d.u32(slot + 8));
  const uint32_t n = d.u32(slot + 12);
  if (idx < 0 || 4ull * (uint64_t(n) + uint32_t(idx)) > d.header.intarray_size || d.intarray.empty()) return 0;
  const uint32_t m = n < cap ? n : cap;
  if (m) std::memcpy(dst, d.intarray.data() + 4ull * uint32_t(idx), 4ull * m);
  return m;
}

// An int attribute (type 1): its value, or -1 (the "direction" default of both readers).
int32_t int_attr(const CxmlDocument& d, int32_t elem, const char* name) {
  if (!d.element_ok(elem)) return -1;
  const uint32_t count = d.u32(static_cast<uint32_t>(elem) + 4);
  const uint32_t k = d.find_attribute(elem, name);
  uint32_t slot = 0;
  if (count == 0 || k == count || !slot_in_tree(d, elem, k, &slot) || d.u32(slot + 4) != 1) return -1;
  return static_cast<int32_t>(d.u32(slot + 8));
}

}  // namespace

int32_t check_resource(uint32_t handle) {
  auto h = lookup(handle);
  return h && h->res ? 0 : err::kObjectDisposed;
}

// CxmlResources/Native::GetSoundScriptAll(int, SoundScriptNative[], out uint) — 0x2fe080.
// Walks the children of the root's "soundscripttable"; each child named "soundscript"
// whose "id" attribute is an idtable id (type 9) produces one entry (*count is bumped
// before it is filled): Id = the id text; every child named "control" increments
// CommandCount and, for the first three, CommandN gets
//   Id / Command / Target / ValueString <- string attributes "id" / "command" / "tgt" /
//     "value_str" (non-empty, NUL-terminated; left untouched when absent),
//   Ratio (<= 3) / ValueFrom / Value / Duration (<= 2 each) <- int arrays "ratio" /
//     "value_from" / "value" / "duration", with their *Size (0 when absent),
//   Direction <- int attribute "direction" (-1 when absent).
// Other children are skipped; the walk ends at the last sibling or once `capacity`
// entries were produced. A soundscript whose "id" is missing or not of type 9 makes the
// native spin forever on that element; it is skipped here (logged).
int32_t get_sound_script_all(uint32_t handle, size_t capacity, std::vector<SoundScript>* scripts, uint32_t* count) {
  auto h = lookup(handle);
  if (!h || !h->res) return err::kObjectDisposed;
  const CxmlDocument& d = *h->res->doc;
  scripts->clear();
  *count = 0;
  const int32_t table = find_table(d, 9);
  for (int32_t c = table < 0 ? -1 : d.first_child(table); c >= 0 && scripts->size() < capacity; c = d.next_sibling(c)) {
    const char* en = d.element_name(c);
    if (!en || std::strcmp(en, "soundscript") != 0) continue;
    const uint32_t ac = d.u32(static_cast<uint32_t>(c) + 4);
    const uint32_t k = d.find_attribute(c, "id");
    uint32_t slot = 0;
    if (ac == 0 || k == ac || !slot_in_tree(d, c, k, &slot) || d.u32(slot + 4) != 9) {
      host::note_unimplemented("libScePsm CxmlResources::GetSoundScriptAll soundscript without idtable id (native spins)");
      continue;
    }
    SoundScript s;
    const int32_t io = static_cast<int32_t>(d.u32(slot + 8));
    s.id = (io >= 0 && uint64_t(io) + 4 < d.idtable.size()) ? reinterpret_cast<const char*>(d.idtable.data()) + io + 4 : nullptr;
    for (int32_t ctl = d.first_child(c); ctl >= 0; ctl = d.next_sibling(ctl)) {
      const char* cn = d.element_name(ctl);
      if (!cn || std::strcmp(cn, "control") != 0) continue;
      if (s.command_count < 3) {
        SoundScriptCommand& m = s.commands[s.command_count];
        m.id = string_attr(d, ctl, "id", true);
        m.command = string_attr(d, ctl, "command", true);
        m.target = string_attr(d, ctl, "tgt", true);
        m.ratio_size = int_array_attr(d, ctl, "ratio", m.ratio, 3);
        m.value_from_size = int_array_attr(d, ctl, "value_from", m.value_from, 2);
        m.value_size = int_array_attr(d, ctl, "value", m.value, 2);
        m.value_str = string_attr(d, ctl, "value_str", true);
        m.direction = int_attr(d, ctl, "direction");
        m.duration_size = int_array_attr(d, ctl, "duration", m.duration, 2);
      }
      ++s.command_count;
    }
    scripts->push_back(s);
    *count = static_cast<uint32_t>(scripts->size());
  }
  return 0;
}

// CxmlResources/Native::GetSoundScriptCommand(int, uint, string, int index, out string id,
// out string command, out string tgt, int[] ratio, out int, int[] valueFrom, out int,
// int[] value, out int, out string valueStr, out int direction, int[] duration, out int)
// — 0x2fd180. Failures return 0x80B36001: resource without document, unknown type
// ("Unknown type" appended), missing table ("type '<element>' not found"), missing
// element or missing child `index` ("id '<id>' not found"). The element is found by id as
// in GetTextureResource; its child number `index` (any element name) is read: strings
// "id"/"command"/"tgt"/"value_str" (NUL-terminated, "" when absent), int arrays clamped to
// the managed array lengths, "direction" (-1 when absent).
int32_t get_sound_script_command(uint32_t handle, uint32_t type, const std::string& id, int32_t index,
                                 const uint32_t caps[4], SoundScriptCommandResult* out) {
  constexpr int32_t kFail = static_cast<int32_t>(0x80B36001u);
  auto h = lookup(handle);
  if (!h) return err::kObjectDisposed;
  if (!h->res) return kFail;
  if (!valid_type(type)) {
    append_exception_info("Unknown type");
    return kFail;
  }
  const char* child = kElementNames[type - 3];
  const CxmlDocument& d = *h->res->doc;
  const int32_t table = find_table(d, type);
  if (table < 0) {
    not_found("type", child);
    return kFail;
  }
  const int32_t elem = d.find_element(table, child, id.c_str());
  int32_t ctl = elem < 0 ? -1 : d.first_child(elem);
  for (int32_t i = index; i > 0 && ctl >= 0; --i) ctl = d.next_sibling(ctl);
  if (ctl < 0) {
    not_found("id", id);
    return kFail;
  }
  auto str = [&](const char* name) {
    const char* s = string_attr(d, ctl, name, false);
    return std::string(s ? s : "");
  };
  out->id = str("id");
  out->command = str("command");
  out->target = str("tgt");
  out->value_str = str("value_str");
  out->direction = int_attr(d, ctl, "direction");
  std::vector<int32_t>* arrays[4] = {&out->ratio, &out->value_from, &out->value, &out->duration};
  static const char* const kNames[4] = {"ratio", "value_from", "value", "duration"};
  for (int i = 0; i < 4; ++i) {
    arrays[i]->assign(caps[i], 0);
    arrays[i]->resize(int_array_attr(d, ctl, kNames[i], arrays[i]->data(), caps[i]));
  }
  return 0;
}

// CxmlResources/Native::GetSoundGroup(int, uint, string, out int limitMode, out int
// voiceNum) — 0x2fcd30. Handle not found -> 0x80B30005; resource without document, unknown
// type ("Unknown type" appended), missing table ("type '<element>' not found") or missing
// element ("id '<id>' not found") -> 0x80B36001. The element (found by id as in
// GetTextureResource) gives limit_mode and voice_num: int attributes (type 1), -1 otherwise.
int32_t get_sound_group(uint32_t handle, uint32_t type, const std::string& id, int32_t* limit_mode, int32_t* voice_num) {
  constexpr int32_t kFail = static_cast<int32_t>(0x80B36001u);
  auto h = lookup(handle);
  if (!h) return err::kObjectDisposed;
  if (!h->res) return kFail;
  if (!valid_type(type)) {
    append_exception_info("Unknown type");
    return kFail;
  }
  const char* child = kElementNames[type - 3];
  const CxmlDocument& d = *h->res->doc;
  const int32_t table = find_table(d, type);
  if (table < 0) {
    not_found("type", child);
    return kFail;
  }
  const int32_t elem = d.find_element(table, child, id.c_str());
  if (elem < 0) {
    not_found("id", id);
    return kFail;
  }
  *limit_mode = int_attr(d, elem, "limit_mode");
  *voice_num = int_attr(d, elem, "voice_num");
  return 0;
}

}  // namespace ssn::psm::resources
