// libNativeExtensions.sprx (FW 4.03) — JSCExecutor* exports (0xce70–0xeb30, thunks at
// 0x10a60–0x10bf0) and the builtins every RNPS context gets at creation.
//
// Managed caller: ReactNative.PUI JSCJavaScriptExecutor (one executor per RNPS app,
// every call on that app's JS queue thread).
#include <JavaScriptCore/JSBasePrivate.h>
#include <JavaScriptCore/JSContextRefPrivate.h>
#include <JavaScriptCore/JSContextRefPrivatePlayStation.h>
#include <JavaScriptCore/JSHeapFinalizerPrivate.h>

#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>

#include "host/boundary.h"
#include "libNativeExtensions/jsc/jsc_common.h"
#include "libNativeExtensions/jsc/settings_categories.h"

namespace ssn::native_ext::jsc {
namespace {

constexpr const char* kLib = "libNativeExtensions.sprx";

// ---------------------------------------------------------------------------
// Global object class setProperty (0xcfb0, "GlobalSetPropertyCallback"): assigning
// `onunhandledrejection` registers a forwarding function as the context's unhandled
// rejection callback. Always returns false, so the ordinary store also happens.
bool global_set_property(JSContextRef ctx, JSObjectRef, JSStringRef property_name, JSValueRef, JSValueRef* exception) {
  if (JSStringIsEqualToUTF8CString(property_name, "onunhandledrejection")) {
    JSStringRef script = JSStringCreateWithUTF8CString(
        "var _onunhandledrejection = function(promise, reason) { var global = this; if (global.onunhandledrejection) "
        "{ global.onunhandledrejection({ promise,reason }) } }; _onunhandledrejection");
    if (!script) {
      std::fprintf(stderr, "[RNPS] %s() %s\n", "GlobalSetPropertyCallback",
                   "Failed to create onunhandledrejection JSString");
      JSStringRef msg = JSStringCreateWithUTF8CString("Failed to create onunhandledrejection JSString");
      *exception = JSValueMakeString(ctx, msg);
      if (!msg) return false;
      JSStringRelease(msg);
      return false;
    }
    JSValueRef fn = JSEvaluateScript(ctx, script, nullptr, nullptr, 0, exception);
    if (*exception) {
      std::fprintf(stderr, "[RNPS] %s() Failed to evaluate onunhandledrejection script\n", "GlobalSetPropertyCallback");
    } else {
      JSObjectRef fn_object = JSValueToObject(ctx, fn, exception);
      if (*exception) {
        std::fprintf(stderr, "[RNPS] %s() JSValueToObject failed.\n", "GlobalSetPropertyCallback");
      } else {
        JSGlobalContextSetUnhandledRejectionCallback(JSContextGetGlobalContext(ctx), fn_object, exception);
        if (*exception)
          std::fprintf(stderr, "[RNPS] %s() SetUnhandledRejectionCallback failed.\n", "GlobalSetPropertyCallback");
      }
    }
    JSStringRelease(script);
  }
  return false;
}

// ---------------------------------------------------------------------------
// __Blob / __BlobHandle (0xde50 / 0xde00): `new __Blob(id)` returns a handle object
// owning a copy of the id; the managed BlobModule ref count follows the handle's life.

void blob_finalize(JSObjectRef object) {
  auto* blob_id = static_cast<char*>(JSObjectGetPrivate(object));
  if (!blob_id) {
    std::fprintf(stderr, "[RNPS] %s() Finalizing Blob JS object but blobId is missing!\n", "BlobFinalizeCallback");
    return;
  }
  g_blob_remove(blob_id);
  delete[] blob_id;
}

JSValueRef make_js_value_error(JSContextRef ctx, const char* message) {  // inlined "MakeJSValue"
  JSStringRef str = JSStringCreateWithUTF8CString(message);
  if (!str) {
    std::fprintf(stderr, "[RNPS] %s() Failed to create JSString\n", "MakeJSValue");
    return nullptr;
  }
  JSValueRef v = JSValueMakeString(ctx, str);
  JSStringRelease(str);
  return v;
}

JSObjectRef blob_construct(JSContextRef ctx, JSObjectRef, size_t argument_count, const JSValueRef arguments[],
                           JSValueRef* exception) {
  if (argument_count == 0) {
    *exception = make_js_value_error(ctx, "missing required argument");
    return nullptr;
  }
  JSClassDefinition def = kJSClassDefinitionEmpty;
  def.className = "__BlobHandle";
  def.finalize = blob_finalize;
  JSClassRef cls = JSClassCreate(&def);
  if (!cls) {
    *exception = make_js_value_error(ctx, "Failed to create JSClass");
    return nullptr;
  }
  JSObjectRef handle = nullptr;
  JSStringRef id = JSValueToStringCopy(ctx, arguments[0], exception);
  if (!*exception) {
    char* blob_id = jsstring_copy_new_array(id);
    handle = JSObjectMake(ctx, cls, blob_id);
    if (handle) g_blob_increment_ref_count(blob_id);
  }
  if (id) JSStringRelease(id);
  JSClassRelease(cls);
  return handle;
}

// ---------------------------------------------------------------------------
// Global `eval` replacement (0xeb50, class "NativeHookEvalIntercept"): always throws.
// `new Function(...)` is not intercepted (JSGlobalContextSetEvalEnabled is not used).
JSValueRef eval_intercept(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef arguments[],
                          JSValueRef* exception) {
  JSStringRef arg = JSValueToStringCopy(ctx, arguments[0], exception);
  char* source = jsstring_to_new_array(arg);
  JSValueRef ret;
  if (*exception) {
    ret = JSValueMakeUndefined(ctx);
  } else {
    std::string msg(source, *source ? std::strlen(source) : 0);
    msg.insert(0, "eval(", 5);
    msg.append(")\n", 2);
    msg.append("eval() is disabled. Please consider different implementation.", 61);
    const char* m = msg.c_str();
    *exception = create_js_error(ctx, &m);
    ret = JSValueMakeUndefined(ctx);
  }
  delete[] source;
  return ret;
}

// 0xd0d0: __Blob, eval interceptor, razor. Returns 0 or -1.
int install_builtins(JSCExecutor* ex, bool is_internal_development_mode) {
  JSClassDefinition blob = kJSClassDefinitionEmpty;
  blob.className = "__Blob";
  blob.callAsConstructor = blob_construct;
  if (set_object_property(ex, "__Blob", &blob, nullptr, nullptr, nullptr) < 0) {
    std::fprintf(stderr, "[RNPS] %s() Failed to InstallBlob()\n", "InstallGlobalHooks");
    return -1;
  }
  JSClassDefinition eval = kJSClassDefinitionEmpty;
  eval.className = "NativeHookEvalIntercept";
  eval.finalize = nullptr;
  eval.callAsFunction = eval_intercept;
  if (set_object_property(ex, "eval", &eval, nullptr, nullptr, nullptr) < 0) {
    std::fprintf(stderr, "[RNPS] %s() Failed to InstallNativeHook NativeEvalIntercept\n", "InstallGlobalHooks");
    return -1;
  }
  JSValueRef exception = nullptr;
  install_razor_profiler(is_internal_development_mode, ex->ctx, &exception);
  if (exception) {
    std::fprintf(stderr, "[RNPS] %s() Failed to install RazorProfiler hooks\n", "InstallGlobalHooks");
    return -1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Lookups shared by the bridge calls.

JSObjectRef cached_global(JSCExecutor* ex) {
  if (!ex->global) ex->global = JSContextGetGlobalObject(ex->ctx);
  return ex->global;
}

// 0xd330: object[name] converted to an object; errors go to `error` as JS exceptions.
JSObjectRef get_object_property(JSCExecutor* ex, JSObjectRef object, const char* name,
                                JSCExecutorExceptionWrapper* error) {
  JSValueRef exception = nullptr;
  JSStringRef str = JSStringCreateWithUTF8CString(name);
  JSValueRef value = JSObjectGetProperty(ex->ctx, object, str, &exception);
  if (exception) {
    report_js_exception(error, ex->ctx, exception);
    value = nullptr;
  }
  if (str) JSStringRelease(str);
  if (!value) return nullptr;
  exception = nullptr;
  JSObjectRef result = JSValueToObject(ex->ctx, value, &exception);
  if (!exception) return result;
  report_js_exception(error, ex->ctx, exception);
  return nullptr;
}

// 0xd480 / 0xd580 / 0xd680: __fbBatchedBridge.<method>, looked up once and cached.
JSObjectRef batched_bridge_method(JSCExecutor* ex, JSObjectRef JSCExecutor::*slot, const char* method,
                                  JSCExecutorExceptionWrapper* error) {
  if (ex->*slot) return ex->*slot;
  JSObjectRef bridge = ex->batched_bridge;
  if (!bridge) {
    JSObjectRef global = cached_global(ex);
    JSStringRef name = JSStringCreateWithUTF8CString(ex->batched_bridge_name);
    JSValueRef exception = nullptr;
    JSValueRef value = JSObjectGetProperty(ex->ctx, global, name, &exception);
    if (exception) value = nullptr;
    if (name) JSStringRelease(name);
    if (value) {
      exception = nullptr;
      bridge = JSValueToObject(ex->ctx, value, &exception);
      if (exception) bridge = nullptr;
    }
    ex->batched_bridge = bridge;
  }
  ex->*slot = get_object_property(ex, bridge, method, error);
  return ex->*slot;
}

// 0xe210: calls fn(args) and returns its JSON (indent 2) in `result`.
void call_and_return_json(JSCExecutor* ex, JSObjectRef fn, size_t argc, const JSValueRef* argv,
                          JSCExecutorString* result, JSCExecutorExceptionWrapper* error) {
  JSValueRef exception = nullptr;
  JSValueRef ret = JSObjectCallAsFunction(ex->ctx, fn, nullptr, argc, argv, &exception);
  if (exception) {
    report_js_exception(error, ex->ctx, exception);
    return;
  }
  if (!result) return;
  JSStringRef json = JSValueCreateJSONString(ex->ctx, ret, 2, &exception);
  if (exception)
    report_js_exception(error, ex->ctx, exception);
  else
    copy_jsstring(result, &json, error);
  if (json) JSStringRelease(json);
}

JSValueRef make_string_value(JSContextRef ctx, const char* utf8) {  // inlined in the bridge calls
  JSStringRef s = JSStringCreateWithUTF8CString(utf8);
  JSValueRef v = JSValueMakeString(ctx, s);
  if (s) JSStringRelease(s);
  return v;
}

JSValueRef make_json_value(JSContextRef ctx, const char* utf8) {  // inlined in the bridge calls
  JSStringRef s = JSStringCreateWithUTF8CString(utf8);
  JSValueRef v = JSValueMakeFromJSONString(ctx, s);
  if (s) JSStringRelease(s);
  return v;
}

// 0x112d0: property-name strings, created once per process (std::call_once, flag
// 0x38368, map 0x38350) and never released.
std::once_flag g_stats_names_once;
std::map<std::string, JSStringRef> g_stats_names;

constexpr const char* kStatsNames[] = {"heapSize",           "heapCapacity",         "extraMemorySize",
                                       "objectCount",        "protectedObjectCount", "globalObjectCount",
                                       "protectedGlobalObjectCount"};

JSCMemoryUsageStatistics memory_usage_statistics(JSContextRef ctx) {
  std::call_once(g_stats_names_once, [] {
    for (const char* n : kStatsNames) {
      JSStringRef s = JSStringCreateWithUTF8CString(n);
      g_stats_names[n] = s;
    }
  });
  JSObjectRef stats = JSGetMemoryUsageStatistics(ctx);
  JSValueProtect(ctx, stats);
  uint64_t v[7];
  for (int i = 0; i < 7; ++i) {
    JSValueRef value = JSObjectGetProperty(ctx, stats, g_stats_names[kStatsNames[i]], nullptr);
    v[i] = static_cast<uint64_t>(JSValueToNumber(ctx, value, nullptr));
  }
  JSValueUnprotect(ctx, stats);
  return JSCMemoryUsageStatistics{v[0], v[1], v[2], v[3], v[4], v[5], v[6]};
}

}  // namespace

// ===========================================================================
// Exports

// JSCExecutorCreate(bool isInternalDevelopmentMode) — 0x10a60 -> 0xce70. The managed bool
// arrives as a 4-byte BOOL; the original reads its low byte.
SSN_PINVOKE(kLib, "JSCExecutorCreate", void*, ssn_NativeExtensions_JSCExecutorCreate,
            (int32_t is_internal_development_mode)) {
  JSClassDefinition def = kJSClassDefinitionEmpty;
  def.setProperty = global_set_property;
  JSClassRef cls = JSClassCreate(&def);
  if (!cls) {
    std::fprintf(stderr, "[RNPS] %s() Failed to create JSClass for global object\n", "Create");
    return nullptr;
  }
  JSCExecutor* ex = nullptr;
  JSGlobalContextRef ctx = JSGlobalContextCreateInGroup(nullptr, cls);
  if (ctx) {
    ex = static_cast<JSCExecutor*>(::operator new(sizeof(JSCExecutor)));
    std::memset(ex, 0, sizeof(JSCExecutor));
    ex->batched_bridge_name = "__fbBatchedBridge";
    ex->ctx = ctx;
    if (install_builtins(ex, static_cast<uint8_t>(is_internal_development_mode) != 0) < 0) {
      JSGlobalContextUnsetDebuggerRunLoop(ex->ctx);
      JSGlobalContextRelease(ex->ctx);
      ::operator delete(ex);
      ex = nullptr;
    } else {
      JSGlobalContextSetDebuggerRunLoopWithCurrentRunLoop(ctx);
    }
  }
  JSClassRelease(cls);
  return ex;
}

// JSCExecutorDelete(executor) — 0x10a70 (0xce40 + operator delete).
SSN_PINVOKE(kLib, "JSCExecutorDelete", void, ssn_NativeExtensions_JSCExecutorDelete, (JSCExecutor * ex)) {
  if (!ex) return;
  JSGlobalContextUnsetDebuggerRunLoop(ex->ctx);
  JSGlobalContextRelease(ex->ctx);
  ::operator delete(ex);
}

// JSCExecutorSetContextName(executor, name) — 0x10aa0 -> 0xd230.
SSN_PINVOKE(kLib, "JSCExecutorSetContextName", void, ssn_NativeExtensions_JSCExecutorSetContextName,
            (JSCExecutor * ex, const char* name)) {
  JSGlobalContextRef ctx = ex->ctx;
  JSStringRef str = JSStringCreateWithUTF8CString(name);
  JSGlobalContextSetName(ctx, str);
  if (str) JSStringRelease(str);
}

// JSCExecutorEvaluateScript(executor, script, sourceURL, exception) — 0x10ab0 -> 0xe060.
SSN_PINVOKE(kLib, "JSCExecutorEvaluateScript", void, ssn_NativeExtensions_JSCExecutorEvaluateScript,
            (JSCExecutor * ex, const char* script, const char* source_url, JSCExecutorExceptionWrapper* error)) {
  // HOST POLICY: the Settings root categories a host keeps (settings_categories.h).
  const std::string patched = settings_categories_patch(script);
  if (!patched.empty()) script = patched.c_str();
  JSStringRef script_str = JSStringCreateWithUTF8CString(script);
  if (!script_str) {
    std::fprintf(stderr, "[RNPS] %s() %s\n", "EvaluateScript", "Failed to create script JSString");
    if (error) set_static_error(error, "Failed to create script JSString", false);
    return;
  }
  JSStringRef url = JSStringCreateWithUTF8CString(source_url);
  if (!source_url || url) {
    JSValueRef exception = nullptr;
    JSEvaluateScript(ex->ctx, script_str, nullptr, url, 0, &exception);
    if (exception) {
      std::fprintf(stderr, "[RNPS] %s() Failed to eval script\n", "EvaluateScript");
      report_js_exception(error, ex->ctx, exception);
    }
  } else {
    std::fprintf(stderr, "[RNPS] %s() %s\n", "EvaluateScript", "Failed to create sourceURL JSString");
    if (error) set_static_error(error, "Failed to create sourceURL JSString", false);
  }
  if (url) JSStringRelease(url);
  JSStringRelease(script_str);
}

// JSCExecutorCallFunctionReturnFlushedQueue(executor, module, method, argsJSON, result, exception)
// — 0x10ac0 -> 0xe310: __fbBatchedBridge.callFunctionReturnFlushedQueue(module, method, args).
SSN_PINVOKE(kLib, "JSCExecutorCallFunctionReturnFlushedQueue", void,
            ssn_NativeExtensions_JSCExecutorCallFunctionReturnFlushedQueue,
            (JSCExecutor * ex, const char* module_name, const char* method_name, const char* args,
             JSCExecutorString* result, JSCExecutorExceptionWrapper* error)) {
  JSValueRef argv[3];
  argv[0] = make_string_value(ex->ctx, module_name);
  argv[1] = make_string_value(ex->ctx, method_name);
  argv[2] = make_json_value(ex->ctx, args);
  JSObjectRef fn = batched_bridge_method(ex, &JSCExecutor::call_function_return_flushed_queue,
                                         "callFunctionReturnFlushedQueue", nullptr);
  call_and_return_json(ex, fn, 3, argv, result, error);
}

// JSCExecutorInvokeCallbackAndReturnFlushedQueue(executor, callbackId, argsJSON, result, exception)
// — 0x10ad0 -> 0xe410.
SSN_PINVOKE(kLib, "JSCExecutorInvokeCallbackAndReturnFlushedQueue", void,
            ssn_NativeExtensions_JSCExecutorInvokeCallbackAndReturnFlushedQueue,
            (JSCExecutor * ex, int callback_id, const char* args, JSCExecutorString* result,
             JSCExecutorExceptionWrapper* error)) {
  JSValueRef argv[2];
  argv[0] = JSValueMakeNumber(ex->ctx, static_cast<double>(callback_id));
  argv[1] = make_json_value(ex->ctx, args);
  JSObjectRef fn = batched_bridge_method(ex, &JSCExecutor::invoke_callback_and_return_flushed_queue,
                                         "invokeCallbackAndReturnFlushedQueue", nullptr);
  call_and_return_json(ex, fn, 2, argv, result, error);
}

// JSCExecutorFlushedQueue(executor, result, exception) — 0x10ae0 -> 0xe4d0.
SSN_PINVOKE(kLib, "JSCExecutorFlushedQueue", void, ssn_NativeExtensions_JSCExecutorFlushedQueue,
            (JSCExecutor * ex, JSCExecutorString* result, JSCExecutorExceptionWrapper* error)) {
  JSObjectRef fn = batched_bridge_method(ex, &JSCExecutor::flushed_queue, "flushedQueue", nullptr);
  call_and_return_json(ex, fn, 0, nullptr, result, error);
}

// JSCExecutorGetJSVersion(executor, result, exception) — 0x10af0 -> 0xe510: JSON (indent 2)
// of global __ReactNativeVersion.
SSN_PINVOKE(kLib, "JSCExecutorGetJSVersion", void, ssn_NativeExtensions_JSCExecutorGetJSVersion,
            (JSCExecutor * ex, JSCExecutorString* result, JSCExecutorExceptionWrapper* error)) {
  if (!result) return;
  JSObjectRef global = cached_global(ex);
  JSValueRef exception = nullptr;
  JSStringRef name = JSStringCreateWithUTF8CString("__ReactNativeVersion");
  JSValueRef value = JSObjectGetProperty(ex->ctx, global, name, &exception);
  if (exception) {
    report_js_exception(error, ex->ctx, exception);
    value = nullptr;
  }
  if (name) JSStringRelease(name);
  if (value) {
    copy_value_json(result, ex->ctx, value, error);
  } else if (!result->is_static) {
    if (result->ptr) rnps_heap_free(result->ptr);
    result->ptr = nullptr;
  }
}

// JSCExecutorCallGlobal(executor, name, optional, exception) — 0x10b00 -> 0xe630.
SSN_PINVOKE(kLib, "JSCExecutorCallGlobal", void, ssn_NativeExtensions_JSCExecutorCallGlobal,
            (JSCExecutor * ex, const char* function, int32_t optional, JSCExecutorExceptionWrapper* error)) {
  JSObjectRef global = cached_global(ex);
  JSObjectRef fn = get_object_property(ex, global, function, error);
  if (fn && JSObjectIsFunction(ex->ctx, fn)) {
    JSValueRef exception = nullptr;
    JSObjectCallAsFunction(ex->ctx, fn, nullptr, 0, nullptr, &exception);
    if (error && exception) report_js_exception(error, ex->ctx, exception);
  } else if (error && static_cast<uint8_t>(optional) == 0) {
    set_static_error(error, "Trying to call undefined JS function", false);
  }
}

// JSCExecutorSetGlobalVariableFromJSON(executor, propertyName, jsonEncodedValue, exception)
// — 0x10b10 -> 0xe740.
SSN_PINVOKE(kLib, "JSCExecutorSetGlobalVariableFromJSON", void,
            ssn_NativeExtensions_JSCExecutorSetGlobalVariableFromJSON,
            (JSCExecutor * ex, const char* property_name, const char* json, JSCExecutorExceptionWrapper* error)) {
  if (!property_name) {
    if (error) set_static_error(error, "Required argument propertyName is null", false);
    return;
  }
  if (!json) {
    if (error) set_static_error(error, "Required argument jsonEncodedValue is null", false);
    return;
  }
  JSStringRef name = JSStringCreateWithUTF8CString(property_name);
  if (!name) {
    if (error) set_static_error(error, "Failed to create propertyName JSString", false);
    return;
  }
  JSStringRef json_str = JSStringCreateWithUTF8CString(json);
  if (json_str) {
    JSValueRef value = JSValueMakeFromJSONString(ex->ctx, json_str);
    if (value) {
      JSValueRef exception = nullptr;
      JSObjectSetProperty(ex->ctx, cached_global(ex), name, value, 0, &exception);
      if (error && exception) report_js_exception(error, ex->ctx, exception);
    } else if (error) {
      set_static_error(error, "Invalid JSON", false);
    }
    JSStringRelease(json_str);
  } else if (error) {
    set_static_error(error, "Failed to create jsonEncodedValue JSString", false);
  }
  JSStringRelease(name);
}

// JSCExecutorGetJSCodeCovarage(executor, result) [sic] — 0x10b20 -> 0xe950: JSON (indent 0)
// of global __coverage__, which is then reset to undefined. The managed 3-argument
// JSCExecutorGetJSCodeCoverage is declared but not exported by the firmware module.
//
// Note: the original hands managed a buffer from operator new[] here although managed
// releases it through JSCExecutorFreeJSCStr (the RNPS heap); reproduced as is. Only
// coverage-instrumented bundles define __coverage__.
SSN_PINVOKE(kLib, "JSCExecutorGetJSCodeCovarage", void, ssn_NativeExtensions_JSCExecutorGetJSCodeCovarage,
            (JSCExecutor * ex, JSCExecutorString* result)) {
  JSGlobalContextRef ctx = ex->ctx;
  JSObjectRef global = JSContextGetGlobalObject(ctx);
  JSValueRef exception = nullptr;
  JSStringRef name = JSStringCreateWithUTF8CString("__coverage__");
  JSValueRef coverage = JSObjectGetProperty(ctx, global, name, &exception);
  if (name) JSStringRelease(name);
  if (JSValueIsUndefined(ctx, coverage) || JSValueIsNull(ctx, coverage)) return;
  JSStringRef json = JSValueCreateJSONString(ctx, coverage, 0, &exception);
  char* buf = jsstring_to_new_array(json);
  if (exception) {
    delete[] buf;
    return;
  }
  if (!result->is_static && result->ptr) rnps_heap_free(result->ptr);
  result->ptr = buf;
  result->is_static = false;
  JSValueRef undefined = JSValueMakeUndefined(ex->ctx);
  JSStringRef reset = JSStringCreateWithUTF8CString("__coverage__");
  JSObjectSetProperty(ctx, global, reset, undefined, 0, nullptr);
  if (reset) JSStringRelease(reset);
}

// JSCExecutorRegisterHeapFinalizerCallback(executor, cb, userData) — 0x10b60 -> 0xead0.
SSN_PINVOKE(kLib, "JSCExecutorRegisterHeapFinalizerCallback", void,
            ssn_NativeExtensions_JSCExecutorRegisterHeapFinalizerCallback,
            (JSCExecutor * ex, JSHeapFinalizer cb, void* user_data)) {
  JSContextGroupAddHeapFinalizer(JSContextGetGroup(ex->ctx), cb, user_data);
}

// JSCExecutorUnregisterHeapFinalizerCallback(executor, cb, userData) — 0x10b70 -> 0xeb00.
SSN_PINVOKE(kLib, "JSCExecutorUnregisterHeapFinalizerCallback", void,
            ssn_NativeExtensions_JSCExecutorUnregisterHeapFinalizerCallback,
            (JSCExecutor * ex, JSHeapFinalizer cb, void* user_data)) {
  JSContextGroupRemoveHeapFinalizer(JSContextGetGroup(ex->ctx), cb, user_data);
}

// JSCExecutorGetMemoryUsageStatistics(executor) -> JSCMemoryUsageStatistics — 0x10b80 -> 0x112d0.
SSN_PINVOKE(kLib, "JSCExecutorGetMemoryUsageStatistics", JSCMemoryUsageStatistics,
            ssn_NativeExtensions_JSCExecutorGetMemoryUsageStatistics, (JSCExecutor * ex)) {
  return memory_usage_statistics(ex->ctx);
}

// JSCExecutorInstallNativeHooks(executor, ref NativeHookParameters, hooks, hooksNum) — 0x10b30
// -> 0xd780: one object per entry (class params.ClassName, callAsFunction/finalize from the
// entry, private params.PrivateData) stored on global or global[params.ParentObject].
SSN_PINVOKE(kLib, "JSCExecutorInstallNativeHooks", int32_t, ssn_NativeExtensions_JSCExecutorInstallNativeHooks,
            (JSCExecutor * ex, const NativeHookParameters* params, const NativeHook* hooks, uint64_t hooks_num)) {
  for (uint64_t i = 0; i < hooks_num; ++i) {
    JSClassDefinition def = kJSClassDefinitionEmpty;
    def.className = params->class_name;
    def.finalize = hooks[i].finalize;
    def.callAsFunction = hooks[i].call_as_function;
    if (set_object_property(ex, hooks[i].name, &def, params->private_data, params->parent_object, nullptr) < 0)
      return -1;
  }
  return 0;
}

// JSCExecutorRegisterBlobModuleRefCountIncrementFunc(func) — 0x10ba0.
SSN_PINVOKE(kLib, "JSCExecutorRegisterBlobModuleRefCountIncrementFunc", void,
            ssn_NativeExtensions_JSCExecutorRegisterBlobModuleRefCountIncrementFunc, (BlobModuleRefCountFunc func)) {
  g_blob_increment_ref_count = func;
}

// JSCExecutorRegisterBlobModuleRemoveFunc(func) — 0x10bb0.
SSN_PINVOKE(kLib, "JSCExecutorRegisterBlobModuleRemoveFunc", void,
            ssn_NativeExtensions_JSCExecutorRegisterBlobModuleRemoveFunc, (BlobModuleRefCountFunc func)) {
  g_blob_remove = func;
}

// JSCExecutorCopyToJSCStr(str, n, dst) — 0x10bc0: *dst = RNPS-heap copy; -1 when out of memory.
SSN_PINVOKE(kLib, "JSCExecutorCopyToJSCStr", int32_t, ssn_NativeExtensions_JSCExecutorCopyToJSCStr,
            (const char* str, int32_t n, char** dst)) {
  if (!str || !dst) return 0;
  char* p = rnps_heap_strndup(str, static_cast<uint32_t>(n));
  *dst = p;
  return p ? 0 : -1;
}

// JSCExecutorFreeJSCStr(ptr) — 0x10bf0.
SSN_PINVOKE(kLib, "JSCExecutorFreeJSCStr", void, ssn_NativeExtensions_JSCExecutorFreeJSCStr, (char* ptr)) {
  if (ptr) rnps_heap_free(ptr);
}

}  // namespace ssn::native_ext::jsc
