// State libScePsm.sprx (FW 4.03) shares between its object tables (Image, CxmlResources,
// Sound, Bgm, Font, Camera, HttpCache...), as used by the imaging/ and resources/ modules:
//   * the result codes as this firmware's Core.dll defines them (Error.SCE_PSM_ERROR_*),
//   * the global object-handle generator,
//   * the "exception info" string the native side leaves for Error.GetExceptionInfoNative.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace ssn::psm {

// Sce.PlayStation.Core.Error (Core.dll): SCE_PSM_ERROR_BASE = 2159214592u = 0x80B30000.
namespace err {
constexpr int32_t kOk = 0;
constexpr int32_t kArgument = static_cast<int32_t>(0x80B30001u);            // COMMON_ARGUMENT
constexpr int32_t kArgumentNull = static_cast<int32_t>(0x80B30002u);        // COMMON_ARGUMENT_NULL
constexpr int32_t kOutOfRange = static_cast<int32_t>(0x80B30003u);          // COMMON_ARGUMENT_OUT_OF_RANGE
constexpr int32_t kInvalidOperation = static_cast<int32_t>(0x80B30004u);    // COMMON_INVALID_OPERATION
constexpr int32_t kObjectDisposed = static_cast<int32_t>(0x80B30005u);      // COMMON_OBJECT_DISPOSED
constexpr int32_t kNotSupported = static_cast<int32_t>(0x80B30006u);        // COMMON_NOT_SUPPORTED
constexpr int32_t kInvalidFormat = static_cast<int32_t>(0x80B30007u);       // COMMON_INVALID_FORMAT
constexpr int32_t kIo = static_cast<int32_t>(0x80B30010u);                  // COMMON_IO
constexpr int32_t kFileNotFound = static_cast<int32_t>(0x80B30011u);        // COMMON_FILE_NOT_FOUND
constexpr int32_t kOutOfMemory = static_cast<int32_t>(0x80B30013u);         // COMMON_OUT_OF_MEMORY
constexpr int32_t kImageSystem = static_cast<int32_t>(0x80B30023u);         // IMAGE_SYSTEM
constexpr int32_t kResources = static_cast<int32_t>(0x80B30030u);           // RESOURCES
// CXML reader codes (libScePsm CXML document loader, 0x2f7120 / Create 0x2f7c80).
constexpr int32_t kCxmlRead = static_cast<int32_t>(0x80B30501u);            // short read / header too small
constexpr int32_t kCxmlOffset = static_cast<int32_t>(0x80B30503u);          // table offset >= image size
constexpr int32_t kCxmlAlloc = static_cast<int32_t>(0x80B30504u);           // table allocation failed
constexpr int32_t kCxmlCorrupt = static_cast<int32_t>(0x80B30505u);         // unterminated string table, misaligned arrays
constexpr int32_t kCxmlVersion = static_cast<int32_t>(0x80B30506u);         // (version | 0x10) != 0x110
constexpr int32_t kCxmlBounds = static_cast<int32_t>(0x80B30507u);          // element / attribute outside the tree
constexpr int32_t kCxmlTooLarge = static_cast<int32_t>(0x80B30508u);        // file attribute larger than the buffer
constexpr int32_t kCxmlNotFileMode = static_cast<int32_t>(0x80B30509u);     // document is not file-backed
constexpr int32_t kCxmlAttrNotFound = static_cast<int32_t>(0x80B3050Au);    // attribute missing
constexpr int32_t kCxmlAttrType = static_cast<int32_t>(0x80B3050Bu);        // attribute has another type
constexpr int32_t kCxmlNoAllocator = static_cast<int32_t>(0x80B3050Du);     // document has no table allocator
}  // namespace err

// The libScePsm handle generator (inlined at every object creation, e.g. Image
// 0x2debec, CxmlResources::Create 0x2f965e): x = dword_5423F0++ ^ 0x512F967B, then the
// bytes of x are permuted (xmmword_34DC40 shifts {8,16,16,8}, mask xmmword_34E030):
//   h = (x>>8 & 0x00FF0000) | (x>>16 & 0x000000FF) | (x<<16 & 0xFF000000) | (x<<8 & 0x0000FF00)
// and the draw is repeated while h == 0 or h is already a key of the object's table.
// The counter is one for all object kinds; `in_use` is the caller's table lookup.
uint32_t generate_handle(const std::function<bool(uint32_t)>& in_use);

// The exception-info string (libScePsm qword_542400, guarded by qword_5423F8) that
// Error.GetExceptionInfoNative (0x2f5c70) hands to the managed side.
void set_exception_info(const std::string& message);      // replace (sub_2F5A70 / direct stores)
void append_exception_info(const std::string& suffix);    // append ("Unknown type" sites)
std::string get_exception_info();

}  // namespace ssn::psm
