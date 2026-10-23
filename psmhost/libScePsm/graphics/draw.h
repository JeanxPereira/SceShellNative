// Clear and draw execution for the PSM graphics context (Vulkan backend).
#pragma once
#include <cstdint>

namespace ssn::psm::gfx {

int32_t clear_target(uint32_t mask);
int32_t draw_arrays(uint32_t mode, int32_t first, int32_t count, int32_t repeat);

}  // namespace ssn::psm::gfx
