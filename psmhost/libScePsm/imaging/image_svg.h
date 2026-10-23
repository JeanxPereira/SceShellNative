// The SVG image codec of libScePsm.sprx (FW 4.03): file type 8 of the codec factory
// sub_246190 (signature sub_26F3E0), codec object of 40 bytes with vtable off_4A20C0,
// mutex "ImageSvgMutex" (+16), the parsed NSVGimage (+24) and an NSVGrasterizer (+32).
// The parser/rasterizer is Sony's patched nanosvg (third_party/nanosvg/nanosvg_sce.h,
// nanosvgrast_sce.h).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ssn::psm::imaging::svg {

struct Codec;
std::shared_ptr<Codec> create_codec();

// vtable[8] sub_2708A0 — header: parses the file once (cached in the codec) and reports
// width = (int)image->width, height = (int)image->height (truncation). The caller stores
// format 4 (RGBA8888), file type 8, one 1x1 frame table, row alignment 1 and attribute
// +144 = 1, which is the impl's alpha channel type (+216): Premultiplied.
bool read_header(Codec& codec, const uint8_t* data, size_t size, int32_t* width, int32_t* height);

// vtable[1] sub_26F470 — decode: rasterizes into width x height RGBA8888 (tight rows,
// alignment 1) at scale min(width / header_width, height / header_height) (header_* are
// the header sizes, impl +88; width/height the decode size, impl +96, which
// Image.SetDecodeSize changes), placed at the top-left corner. The rows are cleared first;
// the result is premultiplied, byte order R,G,B,A.
bool decode(Codec& codec, const uint8_t* data, size_t size, int32_t header_width, int32_t header_height, int32_t width,
            int32_t height, std::vector<uint8_t>* out);

}  // namespace ssn::psm::imaging::svg
