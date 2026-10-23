// libNativeExtensions.sprx (FW 4.03) — JSCExecutorInstallGlobalHooks (0x10b50 -> 0xef20) and
// the five JS-callable shims it installs (class "GlobalHook", private data = the GlobalHooks
// copy inside the executor):
//   nativeCallSyncHook 0xf190, nativeFlushQueueImmediate 0xf440, bundleRegistryLoad 0xf600,
//   nativeRequire 0xf7d0 (global), rnps._nativeLoggingHook 0xfdb0.
#include <cstring>
#include <string>

#include "host/boundary.h"
#include "libNativeExtensions/jsc/jsc_common.h"

namespace ssn::native_ext::jsc {
namespace {

const GlobalHooks* hooks_of(JSObjectRef function) {
  return static_cast<const GlobalHooks*>(JSObjectGetPrivate(function));
}

// nativeCallSyncHook(moduleId, methodId, args) — 0xf190.
JSValueRef native_call_sync_hook(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argument_count,
                                 const JSValueRef arguments[], JSValueRef* exception) {
  if (argument_count != 3) {
    char* msg = format("Wrong number of args  (given: %d, expected: 3)", static_cast<int>(argument_count));
    const char* m = msg;
    *exception = create_js_error(ctx, &m);
    delete[] msg;
    return JSValueMakeUndefined(ctx);
  }
  const double module_id = JSValueToNumber(ctx, arguments[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  const double method_id = JSValueToNumber(ctx, arguments[1], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  char* args = jsstring_to_new_array(JSValueCreateJSONString(ctx, arguments[2], 0, exception));
  JSValueRef ret;
  if (*exception) {
    ret = JSValueMakeUndefined(ctx);
  } else {
    JSCExecutorString result{nullptr, false};
    JSCExecutorString error{nullptr, false};
    const GlobalHooks* hooks = hooks_of(function);
    hooks->native_call_sync_hook(static_cast<int>(module_id), static_cast<int>(method_id), args, &result, &error,
                                 hooks->user_arg);
    if (error.ptr) {
      throw_hook_error(ctx, error.ptr, exception, "NativeCallSyncHook");
      ret = JSValueMakeUndefined(ctx);
    } else if (result.ptr) {
      ret = convert_json_str_to_value(ctx, result.ptr, exception, "NativeCallSyncHook");
    } else {
      ret = JSValueMakeUndefined(ctx);
    }
    release_executor_string(&error);
    release_executor_string(&result);
  }
  delete[] args;
  return ret;
}

// nativeFlushQueueImmediate(queue) — 0xf440. Any other argument count is a no-op.
JSValueRef native_flush_queue_immediate(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argument_count,
                                        const JSValueRef arguments[], JSValueRef* exception) {
  JSCExecutorString error{nullptr, false};
  const GlobalHooks* hooks = hooks_of(function);
  JSValueRef ret;
  if (argument_count == 1) {
    char* queue = jsstring_to_new_array(JSValueCreateJSONString(ctx, arguments[0], 0, exception));
    if (*exception) {
      ret = JSValueMakeUndefined(ctx);
      delete[] queue;
      release_executor_string(&error);
      return ret;
    }
    hooks->native_flush_queue_immediate(queue, &error, hooks->user_arg);
    if (error.ptr) {
      throw_hook_error(ctx, error.ptr, exception, "NativeFlushQueueImmediate");
      ret = JSValueMakeUndefined(ctx);
      delete[] queue;
      release_executor_string(&error);
      return ret;
    }
    delete[] queue;
  }
  ret = JSValueMakeUndefined(ctx);
  release_executor_string(&error);
  return ret;
}

// bundleRegistryLoad(name) — 0xf600. Returns undefined; the managed side evaluates the bundle.
JSValueRef bundle_registry_load(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argument_count,
                                const JSValueRef arguments[], JSValueRef* exception) {
  const char* msg;
  if (argument_count == 0) {
    msg = "Expected argument bundle but none was passed";
    *exception = create_js_error(ctx, &msg);
    return JSValueMakeUndefined(ctx);
  }
  if (!JSValueIsString(ctx, arguments[0])) {
    msg = "Invalid argument type, expected string. arg: \"bundle\".";
    *exception = create_js_error(ctx, &msg);
    return JSValueMakeUndefined(ctx);
  }
  char* name = jsstring_to_new_array(JSValueToStringCopy(ctx, arguments[0], exception));
  JSValueRef ret;
  if (*exception) {
    ret = JSValueMakeUndefined(ctx);
  } else {
    JSCExecutorString error{nullptr, false};
    const GlobalHooks* hooks = hooks_of(function);
    hooks->native_bundle_registry_load(name, &error, hooks->user_arg);
    if (error.ptr) throw_hook_error(ctx, error.ptr, exception, "NativeBundleRegistryLoad");
    ret = JSValueMakeUndefined(ctx);
    release_executor_string(&error);
  }
  delete[] name;
  return ret;
}

// nativeRequire(moduleId[, bundleName]) — 0xf7d0. With any other argument count it throws
// the version-mismatch error built from GetNativeVersionStr and global __ReactNativeVersion.
JSValueRef native_require(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argument_count,
                          const JSValueRef arguments[], JSValueRef* exception) {
  const GlobalHooks* hooks = hooks_of(function);
  if (argument_count - 1 < 2) {
    const int module_id = static_cast<int>(JSValueToNumber(ctx, arguments[0], exception));
    if (*exception) return JSValueMakeUndefined(ctx);
    if (module_id < 0) {
      char* msg = format("Received invalid module ID '{%d}'.", module_id);
      const char* m = msg;
      *exception = create_js_error(ctx, &m);
      delete[] msg;
      return JSValueMakeUndefined(ctx);
    }
    char* bundle_name = nullptr;
    if (argument_count == 2 && JSValueIsString(ctx, arguments[1])) {
      bundle_name = jsstring_to_new_array(JSValueToStringCopy(ctx, arguments[1], exception));
      if (*exception) {
        JSValueRef ret = JSValueMakeUndefined(ctx);
        delete[] bundle_name;
        return ret;
      }
    }
    JSCExecutorString error{nullptr, false};
    hooks->native_require(module_id, bundle_name, &error, hooks->user_arg);
    if (error.ptr) throw_hook_error(ctx, error.ptr, exception, "NativeRequire");
    JSValueRef ret = JSValueMakeUndefined(ctx);
    release_executor_string(&error);
    if (bundle_name) delete[] bundle_name;
    return ret;
  }

  JSCExecutorString native_version{nullptr, false};
  hooks->get_native_version_str(&native_version, hooks->user_arg);
  JSObjectRef global = JSContextGetGlobalObject(ctx);
  JSStringRef name = JSStringCreateWithUTF8CString("__ReactNativeVersion");
  JSValueRef js_version = JSObjectGetProperty(ctx, global, name, exception);
  if (name) JSStringRelease(name);
  JSValueRef ret;
  if (*exception) {
    ret = JSValueMakeUndefined(ctx);
  } else if (JSValueIsUndefined(ctx, js_version) || JSValueIsNull(ctx, js_version)) {
    const char* v = native_version.ptr;
    std::string msg(v, *v ? std::strlen(v) : 0);
    msg.insert(0, "Incompatible version detected\njsVersion: Unknown\nnativeVersion: ", 64);
    const char* m = msg.c_str();
    *exception = create_js_error(ctx, &m);
    ret = JSValueMakeUndefined(ctx);
  } else {
    char* js = jsstring_to_new_array(JSValueCreateJSONString(ctx, js_version, 0, exception));
    if (!*exception) {
      char* msg = format("Incompatible version detected\njsVersion: %s\nnativeVersion: %s", js, native_version.ptr);
      const char* m = msg;
      *exception = create_js_error(ctx, &m);
      delete[] msg;
    }
    ret = JSValueMakeUndefined(ctx);
    delete[] js;
  }
  release_executor_string(&native_version);
  return ret;
}

// rnps._nativeLoggingHook(msg, level) — 0xfdb0.
JSValueRef native_logging_hook(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argument_count,
                               const JSValueRef arguments[], JSValueRef* exception) {
  if (argument_count != 2) {
    char* msg = format("Wrong number of args (given: %llu, expected: 2)",
                       static_cast<unsigned long long>(argument_count));
    const char* m = msg;
    *exception = create_js_error(ctx, &m);
    delete[] msg;
    return JSValueMakeUndefined(ctx);
  }
  const double level = JSValueToNumber(ctx, arguments[1], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  char* msg = jsstring_to_new_array(JSValueToStringCopy(ctx, arguments[0], exception));
  JSValueRef ret;
  if (*exception) {
    ret = JSValueMakeUndefined(ctx);
  } else {
    JSCExecutorString error{nullptr, false};
    const GlobalHooks* hooks = hooks_of(function);
    hooks->console_log(msg, static_cast<int>(level), &error, hooks->user_arg);
    if (error.ptr) throw_hook_error(ctx, error.ptr, exception, "NativeLoggingHook");
    ret = JSValueMakeUndefined(ctx);
    release_executor_string(&error);
  }
  delete[] msg;
  return ret;
}

struct GlobalHookEntry {
  const char* name;
  JSObjectCallAsFunctionCallback fn;
  const char* parent;
  const char* failure;
};

constexpr GlobalHookEntry kGlobalHooks[] = {
    {"nativeCallSyncHook", native_call_sync_hook, nullptr,
     "[RNPS] %s() Failed to install nativeCallSyncHook\n"},
    {"nativeFlushQueueImmediate", native_flush_queue_immediate, nullptr,
     "[RNPS] %s() Failed to install nativeFlushQueueImmediate\n"},
    {"bundleRegistryLoad", bundle_registry_load, nullptr, "[RNPS] %s() Failed to install bundleRegistryLoad\n"},
    {"nativeRequire", native_require, nullptr, "[RNPS] %s() Failed to install nativeRequire\n"},
    {"_nativeLoggingHook", native_logging_hook, "rnps", "[RNPS] %s() Failed to install _nativeLoggingHook\n"},
};

}  // namespace

// JSCExecutorInstallGlobalHooks(executor, ref GlobalHooks) — 0x10b50 -> 0xef20. Copies the
// 56-byte hook table into the executor, then installs the shims in table order; stops at the
// first failure (-1).
SSN_PINVOKE("libNativeExtensions.sprx", "JSCExecutorInstallGlobalHooks", int32_t,
            ssn_NativeExtensions_JSCExecutorInstallGlobalHooks, (JSCExecutor * ex, const GlobalHooks* hooks)) {
  std::memcpy(&ex->hooks, hooks, sizeof(GlobalHooks));
  for (const GlobalHookEntry& e : kGlobalHooks) {
    JSClassDefinition def = kJSClassDefinitionEmpty;
    def.className = "GlobalHook";
    def.finalize = nullptr;
    def.callAsFunction = e.fn;
    if (set_object_property(ex, e.name, &def, &ex->hooks, e.parent, nullptr) < 0) {
      std::fprintf(stderr, e.failure, "InstallGlobalHooks");
      return -1;
    }
  }
  return 0;
}

}  // namespace ssn::native_ext::jsc
