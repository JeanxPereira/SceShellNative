// ShellUI eboot libSceAppDbWrapper: the AppInfo reads of the managed Sce.Vsh.AppDb.AppDbWrapper
// (GetAppInfo / GetAppInfoWithErrorCode). Addresses are eboot.bin offsets (FW 4.03).
//
// ReadAppInfoStep1 (0x561BC0) reads the title's AppInfo from SceShellCore through the AppInfo
// client (0x2749D0) and serialises it (AppInfo vtable +0, 0x268F70) into a buffer it hands back
// as a handle with the text length + 1; ReadAppInfoStep2 (0x562CC0) copies the text into the
// caller's buffer and frees it.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "eboot/app_browse.h"
#include "host/boundary.h"

namespace ssn::eboot::appbrowse {
namespace appdb = ssn::shellcore::appdb;

// ReadAppInfoStep1(string titleId, string metadataId, string[] itemProps, int itemLength,
// out long handleJsonText, out int length).
SSN_PINVOKE("libSceAppDbWrapper.a", "ReadAppInfoStep1", int32_t, ssn_AppDbWrapper_ReadAppInfoStep1,
            (const char* title_id, const char* metadata_id, const char** item_props, int32_t item_length,
             int64_t* handle, int32_t* length)) {
  if (!title_id || !handle || !length || std::strlen(title_id) > 0x20) return kErrInvalidArg;
  // The requested keys: TITLE stands for TITLE_00..TITLE_29. The client's key translation
  // (0x26EEE0, with _metadata_path added once for a translated key) and the content path
  // post-processing of the _path_* keys are not reproduced: other keys are asked as given.
  std::vector<std::string> keys;
  for (int32_t i = 0; i < item_length && item_props; ++i) {
    if (!item_props[i]) continue;
    const std::string k = item_props[i];
    if (k == "TITLE") {
      for (int n = 0; n < 30; ++n) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "TITLE_%02d", n);
        keys.emplace_back(buf);
      }
      continue;
    }
    keys.push_back(k);
  }
  if (item_length > 0)
    host::note_unimplemented("eboot ReadAppInfoStep1 key translation (0x26EEE0) and _path_* post-processing not reproduced");
  if (!metadata_id) {
    host::note_unimplemented("eboot ReadAppInfoStep1 without a metadata id (prioritised row read) not reproduced");
    return kErrInvalidArg;
  }
  AppInfo info;
  const int32_t r = appdb::get_app_info_value_list(title_id, metadata_id, keys, 0, &info);
  if (r != 0) return r;
  auto* text = new std::string(appinfo_json(info));
  *handle = reinterpret_cast<int64_t>(text);
  *length = static_cast<int32_t>(text->size()) + 1;
  return 0;
}

// ReadAppInfoStep2(long handleJsonText, StringBuilder jsonText).
SSN_PINVOKE("libSceAppDbWrapper.a", "ReadAppInfoStep2", int32_t, ssn_AppDbWrapper_ReadAppInfoStep2,
            (int64_t handle, char* json_text)) {
  if (handle == 0 || !json_text) return kErrInvalidArg;
  auto* text = reinterpret_cast<std::string*>(handle);
  std::memcpy(json_text, text->data(), text->size());
  json_text[text->size()] = '\0';
  delete text;
  return 0;
}

}  // namespace ssn::eboot::appbrowse
