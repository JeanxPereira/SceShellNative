// libNativeExtensions.sprx (FW 4.03) — Performance module JS hooks (0x12bc0..0x145a0).
//
// Managed PerformanceModule boxes its delegates with CreatePerformanceModule and installs
// the hook tables below through JSCExecutorInstallNativeHooks: GetPerformanceHooks under
// `performance` (class "PerformanceModuleHook") and GetPerformanceGlobalHooks on the global
// object. Every shim's private data is the boxed PerformanceModuleHooks.
#include <cstring>
#include <new>

#include "host/boundary.h"
#include "libNativeExtensions/jsc/jsc_common.h"

namespace ssn::native_ext::jsc {
namespace {

constexpr const char* kLib = "libNativeExtensions.sprx";

// Managed PerformanceModuleHooks (88 bytes).
struct PerformanceModuleHooks {
  void* user_arg;  // +0x00
  double (*now)(void* ua);  // +0x08
  void (*measure)(const char* measure_name, const char* start_mark, const char* end_mark, const char* options,
                  JSCExecutorString* result, JSCExecutorString* error, void* ua);  // +0x10
  void (*clear_measures)(const char* measure_name, void* ua);  // +0x18
  void (*mark)(const char* mark_name, double start_time, const char* detail, JSCExecutorString* result,
               JSCExecutorString* error, void* ua);  // +0x20
  void (*clear_marks)(const char* mark_name, void* ua);  // +0x28
  void (*set_resource_timing_buffer_size)(int size, void* ua);  // +0x30
  void (*clear_resource_timings)(void* ua);  // +0x38
  void (*get_entries)(JSCExecutorString* result, JSCExecutorString* error, void* ua);  // +0x40
  void (*get_entries_by_name)(const char* name, const char* entry_type, JSCExecutorString* result,
                              JSCExecutorString* error, void* ua);  // +0x48
  void (*get_entries_by_type)(const char* entry_type, JSCExecutorString* result, JSCExecutorString* error,
                              void* ua);  // +0x50
};
static_assert(sizeof(PerformanceModuleHooks) == 88);

const PerformanceModuleHooks* hooks_of(JSObjectRef function) {
  return static_cast<const PerformanceModuleHooks*>(JSObjectGetPrivate(function));
}

// The common tail of the hooks that return an entry list: error -> thrown value, else the
// result JSON as a value.
JSValueRef finish_result(JSContextRef ctx, JSCExecutorString* result, JSCExecutorString* error,
                         JSValueRef* exception, const char* caller) {
  JSValueRef ret;
  if (error->ptr) {
    throw_hook_error(ctx, error->ptr, exception, caller);
    ret = JSValueMakeUndefined(ctx);
  } else {
    ret = convert_json_str_to_value(ctx, result->ptr, exception, caller);
  }
  release_executor_string(error);
  release_executor_string(result);
  return ret;
}

// "<name> must be a string.\n%s\n" with the conversion exception's text.
void throw_name_not_string(JSContextRef ctx, const char* fmt, JSValueRef* exception) {
  char* text = jsstring_to_new_array(JSValueToStringCopy(ctx, *exception, nullptr));
  char* msg = format(fmt, text);
  const char* m = msg;
  *exception = create_js_error(ctx, &m);
  delete[] msg;
  delete[] text;
}

void throw_message(JSContextRef ctx, const char* message, JSValueRef* exception) {
  *exception = create_js_error(ctx, &message);
}

bool is_undefined_or_null(JSContextRef ctx, JSValueRef v) { return JSValueIsUndefined(ctx, v) || JSValueIsNull(ctx, v); }

JSValueRef get_property(JSContextRef ctx, JSObjectRef object, const char* name, JSValueRef* exception) {  // 0xcdc0
  JSStringRef s = JSStringCreateWithUTF8CString(name);
  JSValueRef v = JSObjectGetProperty(ctx, object, s, exception);
  if (s) JSStringRelease(s);
  return v;
}

// performance.getEntries() — 0x12c50.
JSValueRef get_entries(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t, const JSValueRef[],
                       JSValueRef* exception) {
  JSCExecutorString result{nullptr, false};
  JSCExecutorString error{nullptr, false};
  const PerformanceModuleHooks* h = hooks_of(function);
  h->get_entries(&result, &error, h->user_arg);
  return finish_result(ctx, &result, &error, exception, "GetEntriesJsHook");
}

// performance.getEntriesByName(name[, type]) — 0x12da0.
JSValueRef get_entries_by_name(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argument_count,
                               const JSValueRef arguments[], JSValueRef* exception) {
  if (argument_count == 0) {
    *exception = create_arg_count_js_error(ctx, "getEntriesByName", 1, 0);
    return JSValueMakeUndefined(ctx);
  }
  char* name = jsstring_to_new_array(JSValueToStringCopy(ctx, arguments[0], exception));
  if (*exception) {
    JSValueRef ret = JSValueMakeUndefined(ctx);
    delete[] name;
    return ret;
  }
  char* type = nullptr;
  if (argument_count >= 2) {
    type = jsstring_to_new_array(JSValueToStringCopy(ctx, arguments[1], exception));
    if (*exception) {
      JSValueRef ret = JSValueMakeUndefined(ctx);
      delete[] type;
      delete[] name;
      return ret;
    }
  }
  JSCExecutorString result{nullptr, false};
  JSCExecutorString error{nullptr, false};
  const PerformanceModuleHooks* h = hooks_of(function);
  h->get_entries_by_name(name, type, &result, &error, h->user_arg);
  JSValueRef ret = finish_result(ctx, &result, &error, exception, "GetEntriesByNameJsHook");
  if (type) delete[] type;
  delete[] name;
  return ret;
}

// performance.getEntriesByType(type) — 0x130c0.
JSValueRef get_entries_by_type(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argument_count,
                               const JSValueRef arguments[], JSValueRef* exception) {
  if (argument_count == 0) {
    *exception = create_arg_count_js_error(ctx, "getEntriesByType", 1, 0);
    return JSValueMakeUndefined(ctx);
  }
  char* type = jsstring_to_new_array(JSValueToStringCopy(ctx, arguments[0], exception));
  JSValueRef ret;
  if (*exception) {
    ret = JSValueMakeUndefined(ctx);
  } else {
    JSCExecutorString result{nullptr, false};
    JSCExecutorString error{nullptr, false};
    const PerformanceModuleHooks* h = hooks_of(function);
    h->get_entries_by_type(type, &result, &error, h->user_arg);
    ret = finish_result(ctx, &result, &error, exception, "GetEntriesByTypeJsHook");
  }
  delete[] type;
  return ret;
}

// performance.clearResourceTimings() — 0x13330.
JSValueRef clear_resource_timings(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t, const JSValueRef[],
                                  JSValueRef*) {
  const PerformanceModuleHooks* h = hooks_of(function);
  h->clear_resource_timings(h->user_arg);
  return JSValueMakeUndefined(ctx);
}

// performance.setResourceTimingBufferSize(size) — 0x13360.
JSValueRef set_resource_timing_buffer_size(JSContextRef ctx, JSObjectRef function, JSObjectRef,
                                           size_t argument_count, const JSValueRef arguments[],
                                           JSValueRef* exception) {
  if (argument_count == 0) {
    *exception = create_arg_count_js_error(ctx, "setResourceTimingBufferSize", 1, 0);
    return JSValueMakeUndefined(ctx);
  }
  const double size = JSValueToNumber(ctx, arguments[0], exception);
  if (!*exception) {
    const PerformanceModuleHooks* h = hooks_of(function);
    h->set_resource_timing_buffer_size(static_cast<int>(size), h->user_arg);
  }
  return JSValueMakeUndefined(ctx);
}

// performance.now() and global nativePerformanceNow() — 0x13490.
JSValueRef now(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
  const PerformanceModuleHooks* h = hooks_of(function);
  return JSValueMakeNumber(ctx, h->now(h->user_arg));
}

// performance.mark(name[, { detail, startTime }]) — 0x134c0. startTime defaults to Now(),
// which is sampled on entry.
JSValueRef mark(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argument_count,
                const JSValueRef arguments[], JSValueRef* exception) {
  const PerformanceModuleHooks* h = hooks_of(function);
  double start_time = h->now(h->user_arg);
  if (argument_count == 0) {
    *exception = create_arg_count_js_error(ctx, "mark", 1, 0);
    return JSValueMakeUndefined(ctx);
  }
  char* name = jsstring_to_new_array(JSValueToStringCopy(ctx, arguments[0], exception));
  if (*exception) {
    throw_name_not_string(ctx, "markName must be a string.\n%s\n", exception);
    JSValueRef ret = JSValueMakeUndefined(ctx);
    delete[] name;
    return ret;
  }
  char* detail = nullptr;
  if (argument_count >= 2 && !is_undefined_or_null(ctx, arguments[1])) {
    auto fail = [&]() {
      JSValueRef ret = JSValueMakeUndefined(ctx);
      delete[] name;
      if (detail) delete[] detail;
      return ret;
    };
    if (!JSValueIsObject(ctx, arguments[1])) {
      throw_message(ctx, "If passed, markOptions must be an object.", exception);
      return fail();
    }
    JSObjectRef options = JSValueToObject(ctx, arguments[1], exception);
    if (*exception) return fail();
    JSValueRef detail_value = get_property(ctx, options, "detail", exception);
    if (*exception) return fail();
    JSValueRef start_value = get_property(ctx, options, "startTime", exception);
    if (*exception) return fail();
    if (!is_undefined_or_null(ctx, detail_value)) {
      if (!JSValueIsObject(ctx, detail_value)) {
        throw_message(ctx, "If passed, markOptions.detail must be an object.", exception);
        return fail();
      }
      detail = jsstring_to_new_array(JSValueCreateJSONString(ctx, detail_value, 0, exception));
      if (*exception) return fail();
    }
    if (!is_undefined_or_null(ctx, start_value)) {
      if (!JSValueIsNumber(ctx, start_value)) {
        throw_message(ctx, "If passed, markOptions.startTime must be a number.", exception);
        return fail();
      }
      start_time = JSValueToNumber(ctx, start_value, exception);
      if (*exception) return fail();
    }
  }
  JSCExecutorString result{nullptr, false};
  JSCExecutorString error{nullptr, false};
  h->mark(name, start_time, detail, &result, &error, h->user_arg);
  JSValueRef ret;
  if (error.ptr) {
    throw_hook_error(ctx, error.ptr, exception, "MarkJsHook");
    ret = JSValueMakeUndefined(ctx);
  } else if (result.ptr) {
    ret = convert_json_str_to_value(ctx, result.ptr, exception, "MarkJsHook");
  } else {
    ret = JSValueMakeUndefined(ctx);
  }
  release_executor_string(&error);
  release_executor_string(&result);
  delete[] name;
  if (detail) delete[] detail;
  return ret;
}

// performance.clearMarks([name]) — 0x13ac0; performance.clearMeasures([name]) — 0x143c0.
template <void (*PerformanceModuleHooks::*Hook)(const char*, void*)>
JSValueRef clear_by_name(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argument_count,
                         const JSValueRef arguments[], JSValueRef* exception) {
  char* name = nullptr;
  if (argument_count != 0) name = jsstring_to_new_array(JSValueToStringCopy(ctx, arguments[0], exception));
  if (!*exception) {
    const PerformanceModuleHooks* h = hooks_of(function);
    (h->*Hook)(name, h->user_arg);
  }
  JSValueRef ret = JSValueMakeUndefined(ctx);
  if (name) delete[] name;
  return ret;
}

// performance.measure(name[, startMark | { detail, start, duration, end }[, endMark]]) — 0x13b80.
// A string second argument is the start mark; an object is validated and passed on as JSON.
JSValueRef measure(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argument_count,
                   const JSValueRef arguments[], JSValueRef* exception) {
  if (argument_count == 0) {
    *exception = create_arg_count_js_error(ctx, "measure", 1, 0);
    return JSValueMakeUndefined(ctx);
  }
  char* name = jsstring_to_new_array(JSValueToStringCopy(ctx, arguments[0], exception));
  if (*exception) {
    throw_name_not_string(ctx, "measureName must be a string.\n%s\n", exception);
    JSValueRef ret = JSValueMakeUndefined(ctx);
    delete[] name;
    return ret;
  }
  char* start_mark = nullptr;
  char* end_mark = nullptr;
  char* options = nullptr;
  auto finish = [&](JSValueRef ret) {
    delete[] name;
    if (options) delete[] options;
    if (end_mark) delete[] end_mark;
    if (start_mark) delete[] start_mark;
    return ret;
  };
  if (argument_count >= 2) {
    const JSValueRef arg = arguments[1];
    if (!is_undefined_or_null(ctx, arg)) {
      if (JSValueIsString(ctx, arg)) {
        start_mark = jsstring_to_new_array(JSValueToStringCopy(ctx, arg, exception));
        if (*exception) return finish(JSValueMakeUndefined(ctx));
      } else if (!JSValueIsObject(ctx, arg)) {
        throw_message(ctx, "startOrMeasureOptions must be a string or object.", exception);
        return finish(JSValueMakeUndefined(ctx));
      } else {
        JSObjectRef obj = JSValueToObject(ctx, arg, exception);
        if (*exception) return finish(JSValueMakeUndefined(ctx));
        JSValueRef detail = get_property(ctx, obj, "detail", exception);
        if (*exception) return finish(JSValueMakeUndefined(ctx));
        JSValueRef start = get_property(ctx, obj, "start", exception);
        if (*exception) return finish(JSValueMakeUndefined(ctx));
        JSValueRef duration = get_property(ctx, obj, "duration", exception);
        if (*exception) return finish(JSValueMakeUndefined(ctx));
        JSValueRef end = get_property(ctx, obj, "end", exception);
        if (*exception) return finish(JSValueMakeUndefined(ctx));
        const char* invalid = nullptr;
        if (!(is_undefined_or_null(ctx, detail) || JSValueIsObject(ctx, detail)))
          invalid = "If passed, measureOptions.detail must be an object.";
        else if (!(is_undefined_or_null(ctx, start) || JSValueIsNumber(ctx, start)))
          invalid = "If passed, measureOptions.start must be a number.";
        else if (!(is_undefined_or_null(ctx, duration) || JSValueIsNumber(ctx, duration)))
          invalid = "If passed, measureOptions.duration must be a number.";
        else if (!(is_undefined_or_null(ctx, end) || JSValueIsNumber(ctx, end)))
          invalid = "If passed, measureOptions.end must be a number.";
        if (invalid) {
          throw_message(ctx, invalid, exception);
          return finish(JSValueMakeUndefined(ctx));
        }
        options = jsstring_to_new_array(JSValueCreateJSONString(ctx, arg, 0, exception));
        if (*exception) return finish(JSValueMakeUndefined(ctx));
      }
    }
    if (argument_count >= 3) {
      end_mark = jsstring_to_new_array(JSValueToStringCopy(ctx, arguments[2], exception));
      if (*exception) return finish(JSValueMakeUndefined(ctx));
    }
  }
  JSCExecutorString result{nullptr, false};
  JSCExecutorString error{nullptr, false};
  const PerformanceModuleHooks* h = hooks_of(function);
  h->measure(name, start_mark, end_mark, options, &result, &error, h->user_arg);
  JSValueRef ret;
  if (error.ptr) {
    throw_hook_error(ctx, error.ptr, exception, "MeasureJsHook");
    ret = JSValueMakeUndefined(ctx);
  } else {
    ret = convert_json_str_to_value(ctx, result.ptr, exception, "MeasureJsHook");
  }
  release_executor_string(&error);
  release_executor_string(&result);
  return finish(ret);
}

// off_38020 — 10 entries, installed under `performance`.
const NativeHook kPerformanceHooks[10] = {
    {"getEntries", get_entries, nullptr},
    {"getEntriesByName", get_entries_by_name, nullptr},
    {"getEntriesByType", get_entries_by_type, nullptr},
    {"clearResourceTimings", clear_resource_timings, nullptr},
    {"setResourceTimingBufferSize", set_resource_timing_buffer_size, nullptr},
    {"now", now, nullptr},
    {"mark", mark, nullptr},
    {"clearMarks", clear_by_name<&PerformanceModuleHooks::clear_marks>, nullptr},
    {"measure", measure, nullptr},
    {"clearMeasures", clear_by_name<&PerformanceModuleHooks::clear_measures>, nullptr},
};

// off_38110 — 1 entry, installed on the global object.
const NativeHook kPerformanceGlobalHooks[1] = {
    {"nativePerformanceNow", now, nullptr},
};

}  // namespace

// CreatePerformanceModule(ref PerformanceModuleHooks) — 0x12bc0: operator new(88) copy.
SSN_PINVOKE(kLib, "CreatePerformanceModule", void*, ssn_NativeExtensions_CreatePerformanceModule,
            (const PerformanceModuleHooks* hooks)) {
  auto* copy = static_cast<PerformanceModuleHooks*>(::operator new(sizeof(PerformanceModuleHooks)));
  std::memcpy(copy, hooks, sizeof(PerformanceModuleHooks));
  return copy;
}

// FreePerformanceModule(handle) — 0x12c00.
SSN_PINVOKE(kLib, "FreePerformanceModule", void, ssn_NativeExtensions_FreePerformanceModule, (void* handle)) {
  if (handle) ::operator delete(handle);
}

// GetPerformanceHooks() / GetPerformanceHooksNum() — 0x12c10 / 0x12c20.
SSN_PINVOKE(kLib, "GetPerformanceHooks", const NativeHook*, ssn_NativeExtensions_GetPerformanceHooks, ()) {
  return kPerformanceHooks;
}
SSN_PINVOKE(kLib, "GetPerformanceHooksNum", uint64_t, ssn_NativeExtensions_GetPerformanceHooksNum, ()) { return 10; }

// GetPerformanceGlobalHooks() / GetPerformanceGlobalHooksNum() — 0x12c30 / 0x12c40.
SSN_PINVOKE(kLib, "GetPerformanceGlobalHooks", const NativeHook*, ssn_NativeExtensions_GetPerformanceGlobalHooks,
            ()) {
  return kPerformanceGlobalHooks;
}
SSN_PINVOKE(kLib, "GetPerformanceGlobalHooksNum", uint64_t, ssn_NativeExtensions_GetPerformanceGlobalHooksNum, ()) {
  return 1;
}

}  // namespace ssn::native_ext::jsc
