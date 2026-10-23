// libNativeExtensions.sprx (FW 4.03) — JavaScriptCore executor layer, shared internals.
//
// The firmware module wraps libSceJsc's C API for the managed RNPS runtime
// (ReactNative.PUI JSCJavaScriptExecutor / PerformanceModule). This directory
// reproduces that wrapper over the same C API of a Windows JavaScriptCore built
// from Sony's JSC-400 source. Addresses in comments are libNativeExtensions.elf
// offsets (IDA DB work/ida_jscexec).
//
// Diagnostics go to stderr with the firmware's exact text (the original writes
// them with fprintf to stderr). Where a format uses `long` (64-bit on the
// console, 32-bit on Windows) the host uses the `long long` conversion so the
// printed text is identical.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <JavaScriptCore/JavaScript.h>

namespace ssn::native_ext::jsc {

// ---------------------------------------------------------------------------
// Marshalling structs shared with managed code (ReactNative.PUI.cs).

// JSCExecutorString { IntPtr ptr; bool isStatic; } — the native side writes the
// flag as one byte at +8. Heap strings come from the "RNPS native" heap
// (rnps_heap_alloc) and are released by managed through JSCExecutorFreeJSCStr
// unless isStatic is set (static literals of this module).
struct JSCExecutorString {
  char* ptr;
  bool is_static;
};
static_assert(sizeof(JSCExecutorString) == 16);

// JSCExecutorExceptionWrapper { JSCExecutorString message; bool isJSException; }
// isJSException (+16): 1 = the message is a JS exception value (JSON), 0 = a
// native error string.
struct JSCExecutorExceptionWrapper {
  JSCExecutorString message;
  bool is_js_exception;
};
static_assert(offsetof(JSCExecutorExceptionWrapper, is_js_exception) == 16);

// Managed GlobalHooks (7 pointers, copied into the executor at +8).
using ConsoleLogFn = void (*)(const char* msg, int log_level, JSCExecutorString* error, void* user_arg);
using GetNativeVersionStrFn = void (*)(JSCExecutorString* result, void* user_arg);
using NativeBundleRegistryLoadFn = void (*)(const char* name, JSCExecutorString* error, void* user_arg);
using NativeCallSyncHookFn = void (*)(int module_id, int method_id, const char* arguments, JSCExecutorString* result,
                                      JSCExecutorString* error, void* user_arg);
using NativeFlushQueueImmediateFn = void (*)(const char* flushed_queue, JSCExecutorString* error, void* user_arg);
using NativeRequireFn = void (*)(int module_id, const char* bundle_name, JSCExecutorString* error, void* user_arg);

struct GlobalHooks {
  void* user_arg;                                         // +0x00
  ConsoleLogFn console_log;                               // +0x08
  GetNativeVersionStrFn get_native_version_str;           // +0x10
  NativeBundleRegistryLoadFn native_bundle_registry_load; // +0x18
  NativeCallSyncHookFn native_call_sync_hook;             // +0x20
  NativeFlushQueueImmediateFn native_flush_queue_immediate; // +0x28
  NativeRequireFn native_require;                         // +0x30
};
static_assert(sizeof(GlobalHooks) == 56);

// Managed NativeHookParameters { string ClassName; string ParentObject; IntPtr PrivateData; }.
struct NativeHookParameters {
  const char* class_name;
  const char* parent_object;
  void* private_data;
};

// One entry of a hook table (GetPerformanceHooks & co., 24 bytes).
struct NativeHook {
  const char* name;
  JSObjectCallAsFunctionCallback call_as_function;
  JSObjectFinalizeCallback finalize;
};
static_assert(sizeof(NativeHook) == 24);

// Managed JSCMemoryUsageStatistics, returned by value.
struct JSCMemoryUsageStatistics {
  uint64_t heap_size;
  uint64_t heap_capacity;
  uint64_t extra_memory_size;
  uint64_t object_count;
  uint64_t protected_object_count;
  uint64_t global_object_count;
  uint64_t protected_global_object_count;
};
static_assert(sizeof(JSCMemoryUsageStatistics) == 56);

using BlobModuleRefCountFunc = void (*)(char* blob_id);

// ---------------------------------------------------------------------------
// The executor (operator new(112) in JSCExecutorCreate, 0xce70).
struct JSCExecutor {
  const char* batched_bridge_name;           // +0x00 "__fbBatchedBridge"
  GlobalHooks hooks;                         // +0x08 (InstallGlobalHooks copy; hook private data)
  JSGlobalContextRef ctx;                    // +0x40
  JSObjectRef global;                        // +0x48 cached global object
  JSObjectRef batched_bridge;                // +0x50 cached __fbBatchedBridge
  JSObjectRef call_function_return_flushed_queue;     // +0x58
  JSObjectRef invoke_callback_and_return_flushed_queue; // +0x60
  JSObjectRef flushed_queue;                 // +0x68
};
static_assert(sizeof(JSCExecutor) == 112);
static_assert(offsetof(JSCExecutor, ctx) == 0x40);

// Blob module callbacks registered by managed (qword_38340 / qword_38348).
extern BlobModuleRefCountFunc g_blob_increment_ref_count;
extern BlobModuleRefCountFunc g_blob_remove;

// ---------------------------------------------------------------------------
// Helpers (each mirrors one routine of the original).

// "RNPS native" mspace (0x14d70 alloc / 0x14de0 free), created on first use.
void* rnps_heap_alloc(size_t size);
void rnps_heap_free(void* p);
// 0x14f30: strnlen(src, n) bytes + NUL from the RNPS heap; strncpy(dst, src, n).
char* rnps_heap_strndup(const char* src, size_t n);

// Frees a JSCExecutorString's heap pointer unless it is static (inlined everywhere).
inline void release_executor_string(JSCExecutorString* s) {
  if (!s->is_static && s->ptr) rnps_heap_free(s->ptr);
}
// Stores a static error literal into an exception wrapper (inlined everywhere).
void set_static_error(JSCExecutorExceptionWrapper* e, const char* msg, bool is_js_exception);

// 0xcb80: moves a JSString into *dst as a heap UTF-8 string.
bool copy_jsstring(JSCExecutorString* dst, JSStringRef* str, JSCExecutorExceptionWrapper* error);
// 0xcc60: JSON (indent 2) of value into *dst; a conversion exception is reported in *error.
bool copy_value_json(JSCExecutorString* dst, JSContextRef ctx, JSValueRef value, JSCExecutorExceptionWrapper* error);
// 0xcc60 called on the exception itself, as every caller does: on success marks it a JS exception.
inline void report_js_exception(JSCExecutorExceptionWrapper* error, JSContextRef ctx, JSValueRef exception) {
  if (error && copy_value_json(&error->message, ctx, exception, error)) error->is_js_exception = true;
}

// Inlined pattern: JSStringGetMaximumUTF8CStringSize + operator new[] + JSStringGetUTF8CString,
// then JSStringRelease when the string is non-null.
char* jsstring_to_new_array(JSStringRef s);
// The same copy without the release.
char* jsstring_copy_new_array(JSStringRef s);

// 0xffe0 CreateJSError: JSValueMakeFromJSONString("{ \"message\": \"%s\" }") with the
// message spliced in unescaped; falls back to a plain string value, then undefined.
JSValueRef create_js_error(JSContextRef ctx, const char* const* message);
// 0x10620 ConvertJSONStrToJSValueRef.
JSValueRef convert_json_str_to_value(JSContextRef ctx, const char* json, JSValueRef* exception, const char* caller);
// Inlined in every hook shim: turns a managed error string into the thrown value.
void throw_hook_error(JSContextRef ctx, const char* error, JSValueRef* exception, const char* caller);
// Performance shims' CreateArgCountJSError.
JSValueRef create_arg_count_js_error(JSContextRef ctx, const char* function, int required, int present);

// 0xd910 SetObjectProperty: creates an object of class `def` with `private_data`
// and stores it as `name` on the global object or on global[parent_object]
// (created as {} when missing). Returns 0 or -1.
int set_object_property(JSCExecutor* ex, const char* name, const JSClassDefinition* def, void* private_data,
                        const char* parent_object, const char* parent_object_script);

// 0x146a0: installs global `razor`.
void install_razor_profiler(bool is_internal_development_mode, JSGlobalContextRef ctx, JSValueRef* exception);

// The snprintf-into-new[] helpers (0x10420, 0x10520, 0x10740, 0x10840, 0x10950,
// 0x14480, 0x145a0 — one instantiation per argument list). nullptr when the
// result is empty or snprintf fails.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#pragma clang diagnostic ignored "-Wformat-security"
#endif
void log_format_failure();
template <typename... Args>
char* format(const char* fmt, Args... args) {
  const int n = std::snprintf(nullptr, 0, fmt, args...);
  if (n < 0) {
    log_format_failure();
    return nullptr;
  }
  if (n == 0) return nullptr;
  const size_t size = static_cast<size_t>(n) + 1;
  char* buf = new char[size];
  if (std::snprintf(buf, size, fmt, args...) < 0) {
    log_format_failure();
    delete[] buf;
    return nullptr;
  }
  return buf;
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

}  // namespace ssn::native_ext::jsc
