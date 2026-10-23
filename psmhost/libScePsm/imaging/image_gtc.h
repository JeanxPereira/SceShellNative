// libScePsm's GPU texture compressor ("GTC", sub_296B70), used by Image::ConvertNative with
// ImageConvertOption.DXTCompressByGPU. The console runs one of the embedded compressor shaders
// (table 0x4A23C0: BC1, BC2, BC3, BC4, BC5, BC6, BC7) over the RGBA8888 frame, one shader
// invocation per 4x4 block. The host runs the same shaders, transcribed from their ISA
// (tools/shaders/isa2glsl.py, launcher/shaders/psm/gtc), on its Vulkan device.
#pragma once
#include <cstdint>
#include <vector>

namespace ssn::psm::imaging::gtc {

// Compresses `rgba` (width x height RGBA8888, tight rows) to the block format of ImageFormat
// `format` (2 Dxt1 .. 8 Bc7U). `out` receives the blocks, row-major, over the size rounded up
// to a multiple of 4. False when the format's compressor is not available on the host.
bool compress(uint32_t format, const uint8_t* rgba, int32_t width, int32_t height, std::vector<uint8_t>* out);

}  // namespace ssn::psm::imaging::gtc
