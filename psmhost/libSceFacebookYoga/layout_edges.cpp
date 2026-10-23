// libSceFacebook.Yoga.sprx (FW 4.03): YGNodeLayoutGetMargin / Border / Padding.
//
// Every other Yoga entry is passed through to the open-source Yoga the firmware library is
// built from (deps: Yoga 1.5, see README). These three differ: the firmware build accepts
// every edge up to YGEdgeEnd (0x9c40 / 0x9cc0 / 0x9d40 report "Cannot get layout properties
// of multi-edge shorthands" only for edge >= 6), the 1.5 DLL rejects YGEdgeEnd, which the
// RN layout asks for (ReactShadowNode.GetPadding -> YogaNode.LayoutPaddingEnd).
//
// Firmware body: idx = edge, except Left -> (layout direction RTL ? End : Start) and
// Right -> (RTL ? Start : End); result = layout.<array>[idx] (node +0x108 margin, +0x120
// border, +0x138 padding; direction = node +336 & 3, 2 = RTL). The host reads the same
// array slots through the 1.5 DLL: slots 1, 3 and 4 by their own edge, slot 5 (End)
// through the edge 1.5 maps to it for the node's direction (Right for LTR, Left for RTL).
#include <windows.h>

#include <cstdint>

#include "host/boundary.h"

namespace ssn::yoga {
namespace {

using GetFn = float (*)(void*, int32_t);
using DirFn = int32_t (*)(void*);

HMODULE dll() {
  static HMODULE h = [] {
    const std::string p = host::Boundary::get().library_path("SceFacebook.Yoga.sprx");
    return p.empty() ? nullptr : LoadLibraryA(p.c_str());
  }();
  return h;
}

float layout_edge(const char* name, void* node, int32_t edge) {
  static const auto dir = reinterpret_cast<DirFn>(GetProcAddress(dll(), "YGNodeLayoutGetDirection"));
  const auto get = reinterpret_cast<GetFn>(GetProcAddress(dll(), name));
  if (!get || !dir) return 0.0f;
  if (edge >= 6) return get(node, edge);  // the library's own multi-edge error path
  const bool rtl = dir(node) == 2;        // YGDirectionRTL
  int32_t slot = edge;
  if (edge == 0) slot = rtl ? 5 : 4;
  else if (edge == 2) slot = rtl ? 4 : 5;
  if (slot == 5) return get(node, rtl ? 0 : 2);
  return get(node, slot);
}

}  // namespace

SSN_PINVOKE("SceFacebook.Yoga.sprx", "YGNodeLayoutGetMargin", float, ssn_Yoga_YGNodeLayoutGetMargin, (void* node, int32_t edge)) {
  return layout_edge("YGNodeLayoutGetMargin", node, edge);
}
SSN_PINVOKE("SceFacebook.Yoga.sprx", "YGNodeLayoutGetBorder", float, ssn_Yoga_YGNodeLayoutGetBorder, (void* node, int32_t edge)) {
  return layout_edge("YGNodeLayoutGetBorder", node, edge);
}
SSN_PINVOKE("SceFacebook.Yoga.sprx", "YGNodeLayoutGetPadding", float, ssn_Yoga_YGNodeLayoutGetPadding, (void* node, int32_t edge)) {
  return layout_edge("YGNodeLayoutGetPadding", node, edge);
}

}  // namespace ssn::yoga
