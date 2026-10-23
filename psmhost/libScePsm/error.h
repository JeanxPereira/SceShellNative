// SCE_PSM_ERROR_* result codes (constants of Sce.PlayStation.Core.Error, Core.dll).
#pragma once
#include <cstdint>

namespace ssn::psm {

constexpr int32_t kOk = 0;
constexpr int32_t kErrorInvalidParameter = static_cast<int32_t>(0x80b30001u);   // SCE_PSM_ERROR_COMMON_ARGUMENT
constexpr int32_t kErrorArgumentNull = static_cast<int32_t>(0x80b30002u);       // SCE_PSM_ERROR_COMMON_ARGUMENT_NULL
constexpr int32_t kErrorOutOfRange = static_cast<int32_t>(0x80b30003u);         // SCE_PSM_ERROR_COMMON_ARGUMENT_OUT_OF_RANGE
constexpr int32_t kErrorInvalidOperation = static_cast<int32_t>(0x80b30004u);   // SCE_PSM_ERROR_COMMON_INVALID_OPERATION
constexpr int32_t kErrorObjectDisposed = static_cast<int32_t>(0x80b30005u);     // SCE_PSM_ERROR_COMMON_OBJECT_DISPOSED
constexpr int32_t kErrorNotSupported = static_cast<int32_t>(0x80b30006u);       // SCE_PSM_ERROR_COMMON_NOT_SUPPORTED
constexpr int32_t kErrorInvalidFormat = static_cast<int32_t>(0x80b30007u);      // SCE_PSM_ERROR_COMMON_INVALID_FORMAT
constexpr int32_t kErrorInsufficientMemory = static_cast<int32_t>(0x80b30008u); // SCE_PSM_ERROR_COMMON_INSUFFICIENT_MEMORY
constexpr int32_t kErrorIo = static_cast<int32_t>(0x80b30010u);                 // SCE_PSM_ERROR_COMMON_IO
constexpr int32_t kErrorFileNotFound = static_cast<int32_t>(0x80b30011u);       // SCE_PSM_ERROR_COMMON_FILE_NOT_FOUND
constexpr int32_t kErrorFileLoad = static_cast<int32_t>(0x80b30012u);           // SCE_PSM_ERROR_COMMON_FILE_LOAD
constexpr int32_t kErrorOutOfMemory = static_cast<int32_t>(0x80b30013u);        // SCE_PSM_ERROR_COMMON_OUT_OF_MEMORY
constexpr int32_t kErrorGraphicsSystem = static_cast<int32_t>(0x80b30021u);     // SCE_PSM_ERROR_GRAPHICS_SYSTEM
constexpr int32_t kErrorImageSystem = static_cast<int32_t>(0x80b30023u);        // SCE_PSM_ERROR_IMAGE_SYSTEM
constexpr int32_t kErrorFontSystem = static_cast<int32_t>(0x80b30024u);         // SCE_PSM_ERROR_FONT_SYSTEM
constexpr int32_t kErrorResources = static_cast<int32_t>(0x80b30030u);          // SCE_PSM_ERROR_RESOURCES

}  // namespace ssn::psm
