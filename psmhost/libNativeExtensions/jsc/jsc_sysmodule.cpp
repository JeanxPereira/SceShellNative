// "/dl_sysmodule:SCE_SYSMODULE_INTERNAL_JSC" — entry points the managed RNPS code imports
// straight from libSceJsc (no libNativeExtensions wrapper on the console). The host forwards
// them to the same-named exports of JavaScriptCore.dll (Sony JSC-400 tree, JSBase.cpp /
// JSContextRefPrivatePlayStation.cpp / JSRemoteInspectorServer.cpp).
#include <JavaScriptCore/JSBase.h>
#include <JavaScriptCore/JSContextRefPrivatePlayStation.h>
#include <JavaScriptCore/JSRemoteInspectorServer.h>

#include <cstdint>

#include "host/boundary.h"

namespace ssn::native_ext::jsc {
namespace {
constexpr const char* kLib = "/dl_sysmodule:SCE_SYSMODULE_INTERNAL_JSC";
}

// JSSetOptions(string) — "gcMaxHeapSize=<n>" (manifest jscGcHeapMaxSize), "enableSingleThreadCheck=1".
SSN_PINVOKE(kLib, "JSSetOptions", void, ssn_SysmoduleJsc_JSSetOptions, (const char* args)) { JSSetOptions(args); }

// JSRunLoop() — runs the inspector run loop (RNPSCore.EnableJscRemoteInspector).
SSN_PINVOKE(kLib, "JSRunLoop", void, ssn_SysmoduleJsc_JSRunLoop, ()) { JSRunLoop(); }

// JSRunLoopIterateCurrentRunLoop() — after each JS-thread action while the inspector is on.
SSN_PINVOKE(kLib, "JSRunLoopIterateCurrentRunLoop", void, ssn_SysmoduleJsc_JSRunLoopIterateCurrentRunLoop, ()) {
  JSRunLoopIterateCurrentRunLoop();
}

// JSRemoteInspectorServerStart(host, port) — the C API returns the bound port (uint16_t);
// managed declares the result as bool (4-byte BOOL), so the port is returned zero-extended.
SSN_PINVOKE(kLib, "JSRemoteInspectorServerStart", int32_t, ssn_SysmoduleJsc_JSRemoteInspectorServerStart,
            (const char* host, uint16_t port)) {
  return static_cast<int32_t>(JSRemoteInspectorServerStart(host, port));
}

}  // namespace ssn::native_ext::jsc
