// libNativeExtensions.sprx (FW 4.03) — global `razor` (0x146a0..0x14cd0): Razor CPU
// profiler markers for JS. start/stop/pushMarker act only in internal development mode
// (flag byte_3836C, set from JSCExecutorCreate's argument); popMarker always calls through.
#include <cstdlib>
#include <string>

#include "host/boundary.h"
#include "libNativeExtensions/jsc/jsc_common.h"

namespace ssn::native_ext::jsc {
namespace {

bool g_is_internal_development_mode = false;  // byte_3836C

// dword_31300: marker colours selected by pushMarker's second argument (0..7).
constexpr uint32_t kMarkerColors[8] = {0x800000ff, 0x8000ff00, 0x80ff0000, 0x8000ffff,
                                       0x80ff00ff, 0x80ffff00, 0x80ffffff, 0x80000000};

// libSceRazorCpu is a console profiler service with no host counterpart. These stand-ins
// report SCE_OK and record the call through note_unimplemented.
int razor_cpu_start_capture() {
  host::note_unimplemented("libSceRazorCpu::sceRazorCpuStartCapture");
  return 0;
}
int razor_cpu_stop_capture() {
  host::note_unimplemented("libSceRazorCpu::sceRazorCpuStopCapture");
  return 0;
}
int razor_cpu_push_marker(const char*, uint32_t, uint32_t) {
  host::note_unimplemented("libSceRazorCpu::sceRazorCpuPushMarker");
  return 0;
}
int razor_cpu_pop_marker() {
  host::note_unimplemented("libSceRazorCpu::sceRazorCpuPopMarker");
  return 0;
}

void throw_razor_error(JSContextRef ctx, const char* fmt, int ret, JSValueRef* exception) {
  char* msg = format(fmt, ret);
  const char* m = msg;
  *exception = create_js_error(ctx, &m);
  delete[] msg;
}

// razor.start() — 0x148e0.
JSValueRef razor_start(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef* exception) {
  if (g_is_internal_development_mode) {
    const int ret = razor_cpu_start_capture();
    if (ret < 0) throw_razor_error(ctx, "sceRazorCpuStartCapture() failed ret=0x%X", ret, exception);
  }
  return JSValueMakeUndefined(ctx);
}

// razor.stop() — 0x14980.
JSValueRef razor_stop(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef* exception) {
  if (g_is_internal_development_mode) {
    const int ret = razor_cpu_stop_capture();
    if (ret < 0) throw_razor_error(ctx, "sceRazorCpuStopCapture() failed ret=0x%X", ret, exception);
  }
  return JSValueMakeUndefined(ctx);
}

// razor.pushMarker([name[, colorIndex]]) — 0x14a20. The colour index string is parsed with
// std::_Stoul(s, nullptr, 10) truncated to 32 bits; anything above 7 selects colour 0.
JSValueRef razor_push_marker(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t argument_count,
                             const JSValueRef arguments[], JSValueRef* exception) {
  if (!g_is_internal_development_mode) return JSValueMakeUndefined(ctx);
  char* name = nullptr;
  const char* marker = "unknown";
  uint32_t color_index = 0;
  if (argument_count != 0) {
    name = jsstring_to_new_array(JSValueToStringCopy(ctx, arguments[0], exception));
    if (*exception) {
      JSValueRef ret = JSValueMakeUndefined(ctx);
      delete[] name;
      return ret;
    }
    if (argument_count > 1) {
      char* color = jsstring_to_new_array(JSValueToStringCopy(ctx, arguments[1], exception));
      if (*exception) {
        JSValueRef ret = JSValueMakeUndefined(ctx);
        delete[] color;
        delete[] name;
        return ret;
      }
      const auto parsed = static_cast<uint32_t>(std::strtoull(color, nullptr, 10));
      delete[] color;
      color_index = parsed > 7 ? 0 : parsed;
    }
    marker = name;
  }
  const int ret = razor_cpu_push_marker(marker, kMarkerColors[color_index], 0);
  if (ret < 0) throw_razor_error(ctx, "sceRazorCpuPushMarker() failed ret=0x%X", ret, exception);
  JSValueRef result = JSValueMakeUndefined(ctx);
  if (name) delete[] name;
  return result;
}

// razor.popMarker() — 0x14c30 (not gated by the development flag).
JSValueRef razor_pop_marker(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[],
                            JSValueRef* exception) {
  const int ret = razor_cpu_pop_marker();
  if (ret < 0) throw_razor_error(ctx, "sceRazorCpuPopMarker() failed ret=0x%X", ret, exception);
  return JSValueMakeUndefined(ctx);
}

JSObjectRef make_function(JSContextRef ctx, const char* name, JSObjectCallAsFunctionCallback fn) {
  JSStringRef s = JSStringCreateWithUTF8CString(name);
  JSObjectRef f = JSObjectMakeFunctionWithCallback(ctx, s, fn);
  if (s) JSStringRelease(s);
  return f;
}

bool set_property(JSContextRef ctx, JSObjectRef object, const char* name, JSValueRef value, JSValueRef* exception) {
  JSStringRef s = JSStringCreateWithUTF8CString(name);
  JSObjectSetProperty(ctx, object, s, value, 0, exception);
  if (s) JSStringRelease(s);
  return *exception == nullptr;
}

}  // namespace

// 0x146a0
void install_razor_profiler(bool is_internal_development_mode, JSGlobalContextRef ctx, JSValueRef* exception) {
  g_is_internal_development_mode = is_internal_development_mode;
  JSObjectRef start = make_function(ctx, "start", razor_start);
  JSObjectRef stop = make_function(ctx, "stop", razor_stop);
  JSObjectRef push_marker = make_function(ctx, "pushMarker", razor_push_marker);
  JSObjectRef pop_marker = make_function(ctx, "popMarker", razor_pop_marker);
  JSObjectRef razor = JSObjectMake(ctx, nullptr, nullptr);
  JSObjectRef global = JSContextGetGlobalObject(ctx);
  if (!set_property(ctx, razor, "start", start, exception)) return;
  if (!set_property(ctx, razor, "pushMarker", push_marker, exception)) return;
  if (!set_property(ctx, razor, "popMarker", pop_marker, exception)) return;
  if (!set_property(ctx, razor, "stop", stop, exception)) return;
  set_property(ctx, global, "razor", razor, exception);
}

}  // namespace ssn::native_ext::jsc
