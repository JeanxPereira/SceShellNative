// The user-supplied firmware dump (decrypted), used like a BIOS image: the launcher
// reads the managed assemblies, resources and fonts from it and never modifies it.
//
// Expected layout of each root (as on the console's filesystem):
//   <root>/system/...                         system modules
//   <root>/system_ex/common_ex/lib/*.sprx     PSM framework + Mono class libraries
//   <root>/system_ex/app/NPXS40087/psm/Application/*.sprx   ShellUI (app.exe)
//   <root>/system_ex/vsh_asset/*.rco          shared UI resources
// The console mounts /system and /system_ex from one of two update slots (A/B); a dump taken
// from the partitions may name the slot directories system_a / system_b and system_ex_a /
// system_ex_b. A root without a plain system/ (system_ex/) directory maps that console path to
// its single slot directory; nothing on disk is renamed.
//
// A root may also hold the outputs of decrypting its RNPS bundles on a console
// (<root>/rnps/<apps|bgs>/<titleId>/<bundle>_out/<stem>_, next to a copy of the app directory):
// the plaintext /dev/rnps produced for that container (see libNativeExtensions DecryptRnpsBundle).
//
// A dump may be split over several roots (e.g. one with the managed code and one with
// the vsh_asset files); console paths resolve against the roots in order.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace ssn {

class Firmware {
 public:
  explicit Firmware(std::string root) { add_root(std::move(root)); }
  void add_root(std::string root);

  const std::string& root() const { return roots_.front().path; }
  std::vector<std::string> roots() const;
  bool looks_valid() const;

  // Maps a console path ("/system_ex/vsh_asset/x.rco") to the first existing host path
  // inside the roots (or the path under the primary root when none exists).
  std::string host_path(const std::string& console_path) const;

  // Directories holding decrypted RNPS bundle outputs (<root>/rnps), in root order.
  std::vector<std::string> rnps_decryption_dirs() const;

  // One line per root describing the slot mapping (for the log).
  std::string describe() const;

 private:
  struct Root {
    std::string path;
    std::vector<std::pair<std::string, std::string>> top;  // console top directory -> directory in the root
  };
  std::string map(const Root& r, const std::string& rel) const;
  std::vector<Root> roots_;
};

}  // namespace ssn
