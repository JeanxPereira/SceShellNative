// Managed entry points onto the registry (FW 4.03):
//  * Sce.Vsh.Registry internal calls, registered by the ShellUI eboot (0x4c5750..0x4c59c0);
//    each forwards to a libSceRegMgr export (named by NID below).
//  * Sce.Vsh.RegistryInternal P/Invokes into "libSceRegistry" (Sce.Vsh.Registry.dll),
//    the libSceRegMgr exports of the same names.
// The libSceRegMgr exports themselves are regmgr_call wrappers (libSceRegMgr.sprx
// 0xd0..0x21f0: status = regmgr_call(cmd, arg, &result, buf, size); a failed system call
// becomes sceKernelError(errno), otherwise the kernel's result is returned) — the store
// and its checks live in regmgr.cpp.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/mono_array.h"
#include "common/mono_util.h"
#include "host/boundary.h"
#include "libSceRegMgr/regmgr.h"

namespace ssn::regmgr {
namespace {

using psm::new_string;
using psm::store_ref;
using psm::utf8;

// ---- Sce.Vsh.Registry icalls (eboot.bin) ----------------------------------------------

// 0x4c5750: thunk to sceRegMgrSetInt (khaYelw1Ytc).
int32_t SetIntNative(uint32_t key, int32_t value) { return set_int(key, value); }
SSN_ICALL("Sce.Vsh.Registry::SetIntNative", SetIntNative);

// 0x4c5760: thunk to sceRegMgrGetInt (mPYKD12UDQI).
int32_t GetIntNative(uint32_t key, int32_t* value) { return get_int(key, value); }
SSN_ICALL("Sce.Vsh.Registry::GetIntNative", GetIntNative);

// 0x4c5770: null string -> SCE_REGMGR_ERROR_PARAM; otherwise the UTF-8 copy of the string
// (empty when the conversion yields nothing) goes to sceRegMgrSetStr(key, s, size)
// (ffInidSqRss) with the caller's size.
int32_t SetStrNative(uint32_t key, MonoString* str, uint32_t size) {
  if (!str) return kErrParam;
  const std::string s = utf8(str);
  return set_str(key, s.c_str(), size);
}
SSN_ICALL("Sce.Vsh.Registry::SetStrNative", SetStrNative);

// 0x4c5880: size 0 -> out "" and SCE_REGMGR_ERROR_PRM_SIZE. Otherwise a size-byte buffer,
// first byte cleared, sceRegMgrGetStr(key, buf, size) (CTplLrrndUg); on failure the buffer
// is emptied again; the out string is made from the buffer either way.
int32_t GetStrNative(uint32_t key, MonoString** str, uint32_t size) {
  if (size == 0) {
    store_ref(str, new_string(""));
    return kErrPrmSize;
  }
  std::vector<char> buf(size + 1, 0);  // +1: host-side terminator guard
  const int32_t r = get_str(key, buf.data(), size);
  if (r != 0) buf[0] = 0;
  store_ref(str, new_string(buf.data()));
  return r;
}
SSN_ICALL("Sce.Vsh.Registry::GetStrNative", GetStrNative);

// 0x4c5900 / 0x4c5930: the array's data pointer and the caller's size go straight to
// sceRegMgrSetBin (eXxrXkrLXa4) / sceRegMgrGetBin (NqxMleeTiLs). The console copies `size`
// bytes whatever the array length; the host refuses a size beyond the array instead of
// reading or writing past it.
int32_t SetBinNative(uint32_t key, MonoArray* bin, uint32_t size) {
  const psm::ArrayView v = psm::view(bin);
  if (bin && size > v.bytes()) {
    host::note_unimplemented("Sce.Vsh.Registry::SetBinNative size beyond the array");
    return kErrPrmSize;
  }
  return set_bin(key, bin ? v.data : nullptr, size);
}
SSN_ICALL("Sce.Vsh.Registry::SetBinNative", SetBinNative);

int32_t GetBinNative(uint32_t key, MonoArray* bin, uint32_t size) {
  const psm::ArrayView v = psm::view(bin);
  if (bin && size > v.bytes()) {
    host::note_unimplemented("Sce.Vsh.Registry::GetBinNative size beyond the array");
    return kErrPrmSize;
  }
  return get_bin(key, bin ? v.data : nullptr, size);
}
SSN_ICALL("Sce.Vsh.Registry::GetBinNative", GetBinNative);

// 0x4c5960: thunk to sceRegMgrResetVal (eL7sMZaIdac).
int32_t ResetValNative(uint32_t key, int32_t init_level) { return reset_val(key, init_level); }
SSN_ICALL("Sce.Vsh.Registry::ResetValNative", ResetValNative);

// 0x4c5970: thunk to sceRegMgrCheckError (LZBANaFbPqM).
int32_t CheckErrorNative(int32_t error) { return check_error(error); }
SSN_ICALL("Sce.Vsh.Registry::CheckErrorNative", CheckErrorNative);

// Console identity (kernel 0xffffffff8080bd70 / 0xffffffff8080bfc0 / 0xffffffff8080c060
// read it from the console's own data): there is none on the host. Each call reports the
// gap and answers 0, which the eboot wrappers below already produce for any value they do
// not recognise (region outside 1..13 -> 0, QAF != 1 -> 0); for the machine type 0 is
// Registry.MachineType.CEX.
int32_t untraced_identity(const char* what) {
  host::note_unimplemented(std::string("libSceRegMgr ") + what + " (console identity not available on the host); returns 0");
  return 0;
}

// 0x4c5980: r = sceRegMgrSrvGetRegion() (sywg_RnhZMA, regmgr_call 18); r in 1..13 ? r : 0.
int32_t GetRegionNative() {
  const int32_t r = untraced_identity("SrvGetRegion");
  return (uint32_t(r - 1) < 13) ? r : 0;
}
SSN_ICALL("Sce.Vsh.Registry::GetRegionNative", GetRegionNative);

// 0x4c59a0: sceRegMgrSrvGetQAFforReg() (wQlp94zzrWg, regmgr_call 23) == 1.
int32_t GetQAFforRegNative() { return untraced_identity("SrvGetQAFforReg") == 1; }
SSN_ICALL("Sce.Vsh.Registry::GetQAFforRegNative", GetQAFforRegNative);

// 0x4c59c0: thunk to sceRegMgrSrvGetMachineType (_04zv_0zNJkg, regmgr_call 22).
int32_t GetMachineTypeNative() { return untraced_identity("SrvGetMachineType"); }
SSN_ICALL("Sce.Vsh.Registry::GetMachineTypeNative", GetMachineTypeNative);

}  // namespace

// ---- libSceRegistry P/Invokes (Sce.Vsh.RegistryInternal) ------------------------------

// sceRegMgrSetInitLevel (YRPHqS8TcnI, regmgr_call 10): the kernel stores the level
// (0xffffffff80ca2a90, effect not traced) and returns 0.
SSN_PINVOKE("libSceRegistry", "sceRegMgrSetInitLevel", int32_t, ssn_RegMgr_SetInitLevel, (int32_t level)) {
  host::note_unimplemented("libSceRegMgr sceRegMgrSetInitLevel " + std::to_string(level) + " (effect not traced)");
  return 0;
}

// sceRegMgrGetVersion (q_lPgqZniW4, regmgr_call 0 with 1).
SSN_PINVOKE("libSceRegistry", "sceRegMgrGetVersion", int32_t, ssn_RegMgr_GetVersion, ()) { return get_version(); }

// sceRegMgrGetVersionForPS4 (7yIpFl9KknY, regmgr_call 0 with 2).
SSN_PINVOKE("libSceRegistry", "sceRegMgrGetVersionForPS4", int32_t, ssn_RegMgr_GetVersionForPS4, ()) {
  return get_version_for_ps4();
}

// sceRegMgrCheckError (LZBANaFbPqM, regmgr_call 12).
SSN_PINVOKE("libSceRegistry", "sceRegMgrCheckError", int32_t, ssn_RegMgr_CheckError, (int32_t error)) {
  return check_error(error);
}

// sceRegMgrIsInitOK (rebo0q4yREE, regmgr_call 17).
SSN_PINVOKE("libSceRegistry", "sceRegMgrIsInitOK", int32_t, ssn_RegMgr_IsInitOK, ()) { return is_init_ok(); }

// sceRegMgrGetInitError (nfAfS7f2gR0, regmgr_call 33).
SSN_PINVOKE("libSceRegistry", "sceRegMgrGetInitError", int32_t, ssn_RegMgr_GetInitError, ()) { return get_init_error(); }

// sceRegMgrStrncpy (BnSHkcDlM_8, 0x21b0): strncpy(dst, src, n) when all three are non-zero.
SSN_PINVOKE("libSceRegistry", "sceRegMgrStrncpy", void, ssn_RegMgr_Strncpy, (char* dst, const char* src, size_t n)) {
  if (!dst || !src || !n) return;
  size_t i = 0;  // strncpy: copy up to n characters, zero-fill the rest
  for (; i < n && src[i]; ++i) dst[i] = src[i];
  for (; i < n; ++i) dst[i] = 0;
}

// sceRegMgrMemset (7WBGJ8aph1M, 0x21d0): memset(dst, fill, n) when dst and n are non-zero.
SSN_PINVOKE("libSceRegistry", "sceRegMgrMemset", void, ssn_RegMgr_Memset, (void* dst, uint8_t fill, size_t n)) {
  if (dst && n) std::memset(dst, fill, n);
}

// sceRegMgrSetbin (LhiUdPKY2qo, 0x21f0): parses "b'<2n hex digits>" into n bytes. Requires
// strnlen(str, 4098) == 2n + 2 and the "b'" prefix; stops at the first non-hex digit (the
// byte being built keeps its high nibble).
SSN_PINVOKE("libSceRegistry", "sceRegMgrSetbin", void, ssn_RegMgr_Setbin, (uint8_t* dst, const char* str, size_t n)) {
  if (!dst || !str) return;
  if (strnlen(str, 4098) != 2 * n + 2 || str[0] != 'b' || str[1] != '\'' || n == 0) return;
  auto nib = [](char c) -> int {
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= '0' && c <= '9') return c - '0';
    return -1;
  };
  for (size_t i = 0; i < n; ++i) {
    const int hi = nib(str[2 + 2 * i]);
    if (hi < 0) return;
    dst[i] = uint8_t(hi << 4);
    const int lo = nib(str[3 + 2 * i]);
    if (lo < 0) return;
    dst[i] = uint8_t((hi << 4) + lo);
  }
}

// Region / machine-type services: console identity, not available on the host.
SSN_PINVOKE("libSceRegistry", "sceRegMgrSrvGetRegion", int32_t, ssn_RegMgr_SrvGetRegion, ()) {
  return untraced_identity("SrvGetRegion");
}
SSN_PINVOKE("libSceRegistry", "sceRegMgrSrvGetMachineType", int32_t, ssn_RegMgr_SrvGetMachineType, ()) {
  return untraced_identity("SrvGetMachineType");
}
SSN_PINVOKE("libSceRegistry", "sceRegMgrSrvGetRealMachineType", int32_t, ssn_RegMgr_SrvGetRealMachineType, ()) {
  return untraced_identity("SrvGetRealMachineType");
}
SSN_PINVOKE("libSceRegistry", "sceRegMgrSrvGetQAFforReg", int32_t, ssn_RegMgr_SrvGetQAFforReg, ()) {
  return untraced_identity("SrvGetQAFforReg");
}
// Region strings (regmgr_call 19/20/21, kernel 0xffffffff8080bde0/be80/bf40): not traced.
SSN_PINVOKE("libSceRegistry", "sceRegMgrSrvGetRegionStr", int32_t, ssn_RegMgr_SrvGetRegionStr, (char* str, size_t size)) {
  (void)str;
  (void)size;
  return untraced_identity("SrvGetRegionStr");
}
SSN_PINVOKE("libSceRegistry", "sceRegMgrSrvCnvRegionStr", int32_t, ssn_RegMgr_SrvCnvRegionStr,
            (int32_t region, char* str, size_t size)) {
  (void)region;
  (void)str;
  (void)size;
  return untraced_identity("SrvCnvRegionStr");
}
SSN_PINVOKE("libSceRegistry", "sceRegMgrSrvCnvRegionInt", int32_t, ssn_RegMgr_SrvCnvRegionInt, (const char* str)) {
  (void)str;
  return untraced_identity("SrvCnvRegionInt");
}

}  // namespace ssn::regmgr
