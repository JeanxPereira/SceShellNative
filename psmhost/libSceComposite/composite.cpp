// libSceComposite (FW 4.03, /system/priv/lib/libSceComposite.sprx): the compositor client
// call ShellUI's ReactNative.SystemMonitor makes (GpuUsageMonitor: usage = time / 16666666).
#include <cstdint>

#include "gfx/vk_device.h"
#include "host/boundary.h"

namespace ssn::composite {

// sceCompsoitorGetProcessRenderingTime(out uint) — NID EcTHemmz6n4 (the misspelt name the
// DllImport uses), libSceComposite 0x4560: with the compositor client connected (the shared
// block at 0x18030) *time = block +40, the process's rendering time the compositor server
// keeps, result 0; null -> 0x80D40006; not connected -> 0x80D40001 ("disconnected from
// compositor server").
// HOST VALUE: the host renderer is the process's compositor; the rendering time is the GPU
// time of the last retired frame measured with Vulkan timestamps (nanoseconds, the unit
// GpuUsageMonitor divides by 16666666).
SSN_PINVOKE("libSceComposite", "sceCompsoitorGetProcessRenderingTime", int32_t, ssn_Composite_GetProcessRenderingTime,
            (uint32_t* time)) {
  if (!time) return static_cast<int32_t>(0x80D40006u);
  static const bool noted =
      (host::note_unimplemented("HOST VALUE libSceComposite process rendering time: host renderer GPU frame time (Vulkan timestamps)"),
       true);
  (void)noted;
  *time = gfx::device().gpu_frame_ns();
  return 0;
}

}  // namespace ssn::composite
