// DbPreparation unit "async_storage" of the ShellUI eboot (FW 4.03): the internal
// implementation DbPreparation::DbPreparationAsyncStorageInternal (handle unk_EBF218,
// vtable 0xCC5320; Initialize 0x38E40, Terminate 0x39A20 returns 0).
//
// Initialize -> checkVersionFile(&isHighVersion):
//   dir  = "/" + sceKernelGetFsSandboxRandomWord() + "/priv/mms/nobackup/async_storage_inernal"
//   stat(dir): an error other than ENOENT (0x80020002) fails with that error.
//   file = dir + "/version.txt": stat; other than ENOENT fails.
//   When the file exists, open/read 4 bytes: a value of 1 means current — done.
//   Otherwise: mkDirs(dir) when the directory was missing, unlink the old file when it
//   existed, create the file (O_WRONLY|O_CREAT, 0777) and write the int 1; then
//   findDbFiles(dir) and report each database file found as corrupted to DbRecovery
//   (0x57FEE0 DbRecoveryReportCorruption(file, true)).
#include <cstdint>
#include <filesystem>
#include <fstream>

#include "core/log.h"
#include "host/boundary.h"
#include "host/context.h"
#include "shellcore/db_preparation_server_common.h"

namespace ssn::eboot::dbp {
namespace fs = std::filesystem;

int32_t async_storage_initialize(const ssn::dbp::Event& ev) {
  (void)ev;
  // HOST VALUE: the sandbox word's tree is <cache>/system_data (see
  // libScePsm/environment/system_parameters.cpp).
  const fs::path dir = fs::path(host::ctx().cache_dir) / "system_data" / "priv" / "mms" / "nobackup" / "async_storage_inernal";
  const fs::path file = dir / "version.txt";
  std::error_code ec;
  const bool dir_exists = fs::exists(dir, ec);
  const bool file_exists = fs::exists(file, ec);
  if (file_exists) {
    int32_t v = -1;
    std::ifstream in(file, std::ios::binary);
    in.read(reinterpret_cast<char*>(&v), 4);
    if (v == 1) return 0;
  }
  if (!dir_exists) {
    fs::create_directories(dir, ec);
    if (ec) {
      log::error("dbp", "ERROR DbOperationASI(checkVersionFile): AsyncStorageInternalAux::mkDirs(dirPath) returned {}", ec.message());
      return static_cast<int32_t>(0x80020002u);
    }
  }
  if (file_exists) fs::remove(file, ec);
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    const int32_t one = 1;
    out.write(reinterpret_cast<const char*>(&one), 4);
    if (!out) {
      log::error("dbp", "ERROR DbOperationASI(checkVersionFile): sceKernelWrite() failed");
      return static_cast<int32_t>(0x80020005u);
    }
  }
  // findDbFiles (AdTjrblPbkA / yLYSOLZmYso, libSceAsyncStorageInternal) and the corruption
  // report are not reproduced; a fresh directory holds no database.
  bool any = false;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.path().filename() != "version.txt") any = true;
  if (any) host::note_unimplemented("eboot DbPreparationAsyncStorageInternal findDbFiles + DbRecoveryReportCorruption not reproduced");
  return 0;
}

}  // namespace ssn::eboot::dbp
