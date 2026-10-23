// libNativeExtensions.sprx (FW 4.03) — DecryptRnpsBundle (0x16370, helper 0x16a20).
//
// Managed JavaScriptBundleDecryptor.Decrypt (ReactNative.PUI) calls it only for RNPSHEDR
// containers, passing (data, bundleOffset, bundleSize, releaseCertModulus, modulusLength): the
// whole container, header included; the native export reads only the first three. Plaintext
// bundles never reach this function. Afterwards managed takes the plaintext from the payload
// offset (header +0x1C) with the real payload size (header +0x24; +0x20 is the encrypted size).
//
// 0x16a20 decrypts in place through the SBL RNPS service:
//   fd = open("/dev/rnps", O_RDWR)          -> on failure 0x800F1213
//   ioctl(fd, 0xC0105203, {u64 addr; u32 size; i32 result = 0x800F1225})
//                                           -> on failure sceKernelError(errno)
//   close(fd); return result
//
// HOST VALUE (/dev/rnps): the PC has no SBL/SAMU. A firmware dump may carry, next to its app
// trees, what this very ioctl produced on a console for each container
// (<dump root>/rnps/<apps|bgs>/<titleId>/<bundle>_out/<bundle stem>_, the payload plaintext of
// exactly the header's real payload size). The host's device answers the ioctl for a container
// whose bytes equal that title's container in the dump by writing that output over the payload,
// result 0; the signature check the SBL does on the way is not reproduced (the output came from a
// console that did it). Any other container gets what the console gets when /dev/rnps cannot be
// opened: 0x800F1213.
#include "libNativeExtensions/jsc/rnps_decryption.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"

namespace ssn::native_ext::jsc {
namespace fs = std::filesystem;

namespace {

struct Output {
  fs::path container;  // the dump's RNPSHEDR container
  fs::path plaintext;  // the console's decryption output for it
  uint64_t container_size = 0;
};

// "<bundle>_out/<stem>_": application.ps.bundle -> application.ps.bundle_out/application.ps_.
fs::path output_of(const fs::path& title_dir, const std::string& bundle) {
  const fs::path b(bundle);
  return title_dir / (bundle + "_out") / (b.stem().string() + "_");
}

const std::vector<Output>& outputs() {
  static std::vector<Output> list;
  static std::once_flag once;
  std::call_once(once, [] {
    Firmware* fw = host::ctx().firmware;
    if (!fw) return;
    std::error_code ec;
    for (const std::string& dir : fw->rnps_decryption_dirs()) {
      for (const char* kind : {"apps", "bgs"}) {
        ec.clear();
        for (auto it = fs::directory_iterator(fs::path(dir) / kind, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
          if (!it->is_directory(ec)) continue;
          const std::string title = it->path().filename().string();
          std::error_code ec2;
          for (auto f = fs::directory_iterator(it->path(), ec2); !ec2 && f != fs::directory_iterator(); f.increment(ec2)) {
            const std::string name = f->path().filename().string();
            if (!f->is_directory(ec2) || name.size() <= 4 || name.compare(name.size() - 4, 4, "_out") != 0) continue;
            const std::string bundle = name.substr(0, name.size() - 4);
            Output o;
            o.plaintext = output_of(it->path(), bundle);
            std::error_code ec3;
            if (!fs::is_regular_file(o.plaintext, ec3)) continue;
            // The container the output belongs to: the console path's file, else the copy
            // next to the output.
            o.container = fw->host_path(std::string("/system_ex/rnps/") + kind + "/" + title + "/" + bundle);
            if (!fs::is_regular_file(o.container, ec3)) o.container = it->path() / bundle;
            o.container_size = fs::file_size(o.container, ec3);
            if (ec3) continue;
            list.push_back(o);
          }
        }
      }
    }
    log::info("rnps", "{} RNPS bundle decryption outputs in the dump", list.size());
  });
  return list;
}

bool same_bytes(const fs::path& file, const uint8_t* data, uint64_t size) {
  std::ifstream f(file, std::ios::binary);
  std::vector<char> buf(1 << 20);
  for (uint64_t at = 0; at < size;) {
    const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - at));
    if (!f.read(buf.data(), static_cast<std::streamsize>(n))) return false;
    if (std::memcmp(buf.data(), data + at, n) != 0) return false;
    at += n;
  }
  return true;
}

}  // namespace

bool rnps_decryption_available(const fs::path& container) {
  std::error_code ec;
  const uint64_t size = fs::file_size(container, ec);
  if (ec) return false;
  for (const Output& o : outputs())
    if (o.container.filename() == container.filename() &&
        o.container.parent_path().filename() == container.parent_path().filename() && size == o.container_size)
      return true;
  return false;
}

int32_t rnps_decrypt_in_place(uint8_t* addr, uint32_t size) {
  for (const Output& o : outputs()) {
    if (o.container_size != size || !same_bytes(o.container, addr, size)) continue;
    uint32_t payload_offset, real_size;
    std::memcpy(&payload_offset, addr + 0x1C, 4);
    std::memcpy(&real_size, addr + 0x24, 4);
    std::error_code ec;
    const uint64_t out_size = fs::file_size(o.plaintext, ec);
    if (ec || out_size != real_size || uint64_t(payload_offset) + real_size > size) {
      log::error("rnps", "decryption output {} does not fit its container's header (payload 0x{:X}, size {})",
                 o.plaintext.string(), payload_offset, real_size);
      break;
    }
    std::ifstream f(o.plaintext, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(addr + payload_offset), real_size)) break;
    host::note_unimplemented("HOST VALUE /dev/rnps: RNPSHEDR payloads replaced by the dump's console decryption outputs");
    log::info("rnps", "/dev/rnps: {} decrypted from {}", o.container.string(), o.plaintext.string());
    return 0;
  }
  host::note_unimplemented("libNativeExtensions::DecryptRnpsBundle /dev/rnps ioctl 0xC0105203 (no decryption output in the dump)");
  return static_cast<int32_t>(0x800F1213);  // open("/dev/rnps") failed
}

SSN_PINVOKE("NativeExtensions.sprx", "DecryptRnpsBundle", int32_t, ssn_NativeExtensions_DecryptRnpsBundle,
            (uint8_t * data, uint32_t start_offset, uint32_t size, const uint8_t* /*modulus*/,
             uint32_t /*modulus_size*/)) {
  if (((reinterpret_cast<uintptr_t>(data) + start_offset) & 0xF) != 0) {
    std::fprintf(stderr, "bundle is not 16B aligned! addr: %llx\n",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(data) + start_offset));
    return -1;
  }
  const int32_t err = rnps_decrypt_in_place(data + start_offset, size);
  if (err != 0) {
    std::fprintf(stderr, "sceSblRnpsVerifyDecryptRnpsBundle failed! err: %x\n", static_cast<unsigned>(err));
    return err;
  }
  return 0;
}

}  // namespace ssn::native_ext::jsc
