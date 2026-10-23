// The eboot's native LoginMgr (vsh/shell/shell_ui/src/login_mgr, FW 4.03) pieces shared
// between the LoginMgrProxy files.
#pragma once
#include <cstdint>

namespace ssn::eboot {

// 0x1D4620: function bits of an mbus client record's functions (enabled functions, state 0,
// unless `builtin`). login_mgr_proxy.cpp.
uint64_t device_function_bits(const uint8_t* rec, bool builtin);

// A LoginMgr device-search condition as 0x20C280 makes it: libSceMbus's condition
// initialiser (edYHYROxzx4) and the eleven function types (+24 + 16*i) set to -1.
void init_device_condition(uint8_t* cond);

// mbus_utility.cpp device search (0x20C4A0, also reached as 0x1F98F0): one
// sceMbusGetDeviceInfoByCondition(cond, 0, recs, 16) query; records with +17 == 1 (unless type
// 15) are skipped; with `assigned_only` the owner (+200) must be > 0 and != 255; with a
// function filter (`filter`: `nfilter` entries of 16 bytes {type +0, state flag +8}) a record
// is taken when its functions match the filter (`any` = 1: one matching entry suffices,
// otherwise every entry must match; `match_state`: the entry's flag must equal the function's
// state != 0). Taken records are copied into `out` (`max` slots of 256 bytes, initialised like
// a condition first). Returns the count, 0x80B60004 past `max`, 0x80B60003 for a null `out` or
// `max` > 256, or the mbus error. login_mgr_proxy.cpp.
int32_t device_search(const uint8_t* cond, uint8_t* out, int32_t max, const uint8_t* filter, int32_t nfilter, int32_t any,
                      bool match_state, bool assigned_only);

}  // namespace ssn::eboot
