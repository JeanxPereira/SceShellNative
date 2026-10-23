// libNativeExtensions.sprx (FW 4.03) — helpers shared by the JSC executor layer.
#include "libNativeExtensions/jsc/jsc_common.h"

#include <windows.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace ssn::native_ext::jsc {

BlobModuleRefCountFunc g_blob_increment_ref_count = nullptr;
BlobModuleRefCountFunc g_blob_remove = nullptr;

namespace {

// qword_38370: the module's private allocator, created on first use as
// sceLibcMspaceCreate("RNPS native", nullptr, 0, 6) (6 = growable + thread-safe).
// The host uses a growable private Win32 heap for the same role.
// The original's check-then-create on qword_38370 (0x14d70 / 0x14de0) is unsynchronised; on
// the console the first use comes from module start-up (CurlInitialize's certificate path),
// before any RNPS thread runs. On the host the first use comes from up to twelve app-pool JS
// threads at once; two of them each creating a heap sends an allocation to the other heap's
// HeapFree (STATUS_HEAP_CORRUPTION). The heap is therefore created exactly once.
HANDLE rnps_heap() {
  static const HANDLE heap = [] {
    HANDLE h = HeapCreate(0, 0, 0);
    if (!h) {
      std::fwrite("failed to create mspace\n", 24, 1, stderr);
      std::abort();
    }
    return h;
  }();
  return heap;
}

}  // namespace

void* rnps_heap_alloc(size_t size) { return HeapAlloc(rnps_heap(), 0, size); }

void rnps_heap_free(void* p) { HeapFree(rnps_heap(), 0, p); }

char* rnps_heap_strndup(const char* src, size_t n) {
  const size_t len = strnlen(src, n);
  auto* dst = static_cast<char*>(HeapAlloc(rnps_heap(), 0, len + 1));
  if (dst) {
    dst[len] = '\0';
    std::strncpy(dst, src, n);
  }
  return dst;
}

void set_static_error(JSCExecutorExceptionWrapper* e, const char* msg, bool is_js_exception) {
  if (!e->message.is_static && e->message.ptr) rnps_heap_free(e->message.ptr);
  e->message.ptr = const_cast<char*>(msg);
  e->message.is_static = true;
  e->is_js_exception = is_js_exception;
}

// 0xcb80
bool copy_jsstring(JSCExecutorString* dst, JSStringRef* str, JSCExecutorExceptionWrapper* error) {
  if (!dst->is_static) {
    if (dst->ptr) rnps_heap_free(dst->ptr);
    dst->ptr = nullptr;
  }
  if (!*str) return false;
  const size_t size = JSStringGetMaximumUTF8CStringSize(*str);
  auto* buf = static_cast<char*>(rnps_heap_alloc(size));
  if (buf) {
    JSStringGetUTF8CString(*str, buf, size);
    dst->ptr = buf;
    dst->is_static = false;
    return true;
  }
  std::fprintf(stderr, "[RNPS] %s() %s\n", "Copy", "Out of memory while converting JSString to C string");
  if (error) set_static_error(error, "Out of memory while converting JSString to C string", false);
  return false;
}

// 0xcc60
bool copy_value_json(JSCExecutorString* dst, JSContextRef ctx, JSValueRef value, JSCExecutorExceptionWrapper* error) {
  JSValueRef exception = nullptr;
  JSStringRef json = JSValueCreateJSONString(ctx, value, 2, &exception);
  bool ok;
  if (exception) {
    std::fprintf(stderr, "[RNPS] %s() %s\n", "Copy", "Failed to convert JSValue to JSString");
    JSStringRef exception_json = JSValueCreateJSONString(ctx, exception, 2, nullptr);
    if (exception_json) {
      if (error) {
        if (copy_jsstring(&error->message, &exception_json, error))
          error->is_js_exception = true;
        else
          std::fprintf(stderr, "[RNPS] %s() %s\n", "Copy", error->message.ptr);
      }
      JSStringRelease(exception_json);
    } else if (error) {
      set_static_error(error, "Failed to convert JSValue to JSString", true);
    }
    ok = false;
  } else {
    ok = copy_jsstring(dst, &json, error);
  }
  if (json) JSStringRelease(json);
  return ok;
}

char* jsstring_copy_new_array(JSStringRef s) {
  const size_t size = JSStringGetMaximumUTF8CStringSize(s);
  char* buf = new char[size];
  JSStringGetUTF8CString(s, buf, size);
  return buf;
}

char* jsstring_to_new_array(JSStringRef s) {
  char* buf = jsstring_copy_new_array(s);
  if (s) JSStringRelease(s);
  return buf;
}

void log_format_failure() {
  const int e = errno;
  std::fprintf(stderr, "[RNPS] %s() %s snprintf() failed: errno=%d strerror=%s\n", "Format", "Format", e,
               std::strerror(errno));
}

// 0xffe0
JSValueRef create_js_error(JSContextRef ctx, const char* const* message) {
  char* json = format("{ \"message\": \"%s\" }", *message);
  JSStringRef str = JSStringCreateWithUTF8CString(json);
  delete[] json;
  if (!str) std::fprintf(stderr, "[RNPS] %s() %s failed to create JSString\n", "CreateJSError", "CreateJSError");
  JSValueRef value = JSValueMakeFromJSONString(ctx, str);
  if (!value) {
    char* src = jsstring_copy_new_array(str);
    std::fprintf(stderr, "[RNPS] %s() %s failed to create error JS value. src=%s\n", "CreateJSError",
                 "CreateJSError", src);
    delete[] src;
    value = JSValueMakeString(ctx, str);
    if (!value) value = JSValueMakeUndefined(ctx);
  }
  if (str) JSStringRelease(str);
  return value;
}

// 0x10620
JSValueRef convert_json_str_to_value(JSContextRef ctx, const char* json, JSValueRef* exception, const char* caller) {
  if (!json || strnlen(json, 1) == 0) return nullptr;
  JSStringRef str = JSStringCreateWithUTF8CString(json);
  JSValueRef value = JSValueMakeFromJSONString(ctx, str);
  if (str) JSStringRelease(str);
  if (!value) {
    char* msg = format("Failed to make JSValue from JSONString: %s\n", json);
    std::fprintf(stderr, "[RNPS] %s() %s: %s\n", "ConvertJSONStrToJSValueRef", caller ? caller : "unknown", msg);
    if (exception) {
      const char* m = msg;
      *exception = create_js_error(ctx, &m);
    }
    delete[] msg;
    return nullptr;
  }
  return value;
}

// Inlined after every managed hook call that can report an error string.
void throw_hook_error(JSContextRef ctx, const char* error, JSValueRef* exception, const char* caller) {
  *exception = convert_json_str_to_value(ctx, error, nullptr, caller);
  if (!*exception) {
    JSStringRef str = JSStringCreateWithUTF8CString(error);
    *exception = JSValueMakeString(ctx, str);
    if (str) JSStringRelease(str);
    if (!*exception) {
      std::fprintf(stderr, "[RNPS] %s() %s: FATAL could not create exception JSValueRef: %s\n", caller, caller, error);
      *exception = JSValueMakeUndefined(ctx);
    }
  }
}

JSValueRef create_arg_count_js_error(JSContextRef ctx, const char* function, int required, int present) {
  char* msg = format("Failed to execute '{%s}': {%d} argument required, but only {%d} present.", function, required,
                     present);
  const char* m = msg;
  JSValueRef error = create_js_error(ctx, &m);
  if (!error)
    std::fprintf(stderr, "[RNPS] %s() %s Failed to create exception JSValue\n", "CreateArgCountJSError",
                 "CreateArgCountJSError");
  delete[] msg;
  return error;
}

namespace {

JSObjectRef cached_global(JSCExecutor* ex) {
  if (!ex->global) ex->global = JSContextGetGlobalObject(ex->ctx);
  return ex->global;
}

// The inlined "exception value -> new[] C string" sequence of SetObjectProperty's error paths.
char* exception_to_new_array(JSContextRef ctx, JSValueRef exception) {
  return jsstring_to_new_array(JSValueToStringCopy(ctx, exception, nullptr));
}

}  // namespace

// 0xd910
int set_object_property(JSCExecutor* ex, const char* name, const JSClassDefinition* def, void* private_data,
                        const char* parent_object, const char* parent_object_script) {
  JSClassRef cls = JSClassCreate(def);
  if (!cls) {
    std::fprintf(stderr, "[RNPS] %s() Failed to create JSClass\n", "SetObjectProperty");
    return -1;
  }
  JSObjectRef object = JSObjectMake(ex->ctx, cls, private_data);
  JSClassRelease(cls);

  JSValueRef exception = nullptr;
  JSObjectRef target;
  if (parent_object && strnlen(parent_object, 1) != 0) {
    JSObjectRef global = cached_global(ex);
    JSStringRef parent_name = JSStringCreateWithUTF8CString(parent_object);
    JSValueRef parent = JSObjectGetProperty(ex->ctx, global, parent_name, &exception);
    if (parent_name) JSStringRelease(parent_name);
    if (exception) {
      char* msg = exception_to_new_array(ex->ctx, exception);
      std::fprintf(stderr, "[RNPS] %s() Failed to get property %s from global object\n%s\n", "SetObjectProperty",
                   parent_object, msg);
      delete[] msg;
      return -1;
    }
    if (JSValueIsUndefined(ex->ctx, parent) || JSValueIsNull(ex->ctx, parent)) {
      target = JSObjectMake(ex->ctx, nullptr, nullptr);
      JSStringRef pname = JSStringCreateWithUTF8CString(parent_object);
      JSObjectSetProperty(ex->ctx, cached_global(ex), pname, target, 0, &exception);
      if (pname) JSStringRelease(pname);
      if (exception) {
        char* msg = exception_to_new_array(ex->ctx, exception);
        std::fprintf(stderr, "[RNPS] %s() Failed to set property %s to global object\n%s\n", "SetObjectProperty",
                     parent_object, msg);
        delete[] msg;
        return -1;
      }
    } else {
      target = JSValueToObject(ex->ctx, parent, &exception);
      if (exception) {
        char* msg = exception_to_new_array(ex->ctx, exception);
        std::fprintf(stderr, "[RNPS] %s() Failed to convert value of property %s to object\n%s\n",
                     "SetObjectProperty", parent_object, msg);
        delete[] msg;
        return -1;
      }
    }
  } else if (parent_object_script) {
    // Evaluates the script with sourceURL "JSCExecutor::SetObjectProperty". On success the
    // original continues with a null target object (r13 is cleared, 0xdaab); no caller in
    // the firmware passes a script, so the path is reproduced as is.
    JSStringRef script = JSStringCreateWithUTF8CString(parent_object_script);
    JSStringRef url = JSStringCreateWithUTF8CString("JSCExecutor::SetObjectProperty");
    if (!JSEvaluateScript(ex->ctx, script, nullptr, url, 0, &exception)) {
      char* msg = exception_to_new_array(ex->ctx, exception);
      std::fprintf(stderr, "[RNPS] %s() Failed to evaluate parentObject script: %s\n", "SetObjectProperty", msg);
      delete[] msg;
      if (url) JSStringRelease(url);
      if (script) JSStringRelease(script);
      return -1;
    }
    if (url) JSStringRelease(url);
    if (script) JSStringRelease(script);
    target = nullptr;
  } else {
    target = cached_global(ex);
  }

  JSStringRef prop = JSStringCreateWithUTF8CString(name);
  if (!prop) {
    std::fprintf(stderr, "[RNPS] %s() Failed to create propertyName(%s)\n", "SetObjectProperty", name);
    return -1;
  }
  int ret = 0;
  JSObjectSetProperty(ex->ctx, target, prop, object, 0, &exception);
  if (exception) {
    char* msg = exception_to_new_array(ex->ctx, exception);
    std::fprintf(stderr, "[RNPS] %s() JSObjectSetProperty failed. %s\n", "SetObjectProperty", msg);
    delete[] msg;
    ret = -1;
  }
  JSStringRelease(prop);
  return ret;
}

}  // namespace ssn::native_ext::jsc
