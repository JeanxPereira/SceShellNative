// "libSceMediaEventListener" GetDiscInfoNative — the eboot's MediaEventListener client (FW 4.03):
// {name, fn} entry 0xd03130 (devkit build 0xd06b40) -> 0x722F30 (devkit 0x724F20).
//
// Client 0x722F30: buff, buffSize and outSize must be non-null / non-zero, else 0x81A80000; under
// its mutex it connects to the "SceMediaEventListenerIpc" IPMI server (0x725560 devkit) and calls
// method 0x150002 (0x723BD0; devkit 0x725BC0) with buff / buffSize as output and outSize (4 bytes);
// the server's result is returned.
//
// Server (SceShellCore; devkit build addresses in parentheses): GetDiscInfoIpc 0x49E010 (0x4A7100)
// waits for the listener's ready flag (0x4A3600 / 0x4AC6F0), then GetDiscInfo 0x4A2630 (0x4AB720):
// a JSON object is filled by makeDiscInfo 0x4A1800 (0x4AA8F0) and serialised (libSceJson2);
// *outSize = length + 1; when that fits buffSize the text is copied (strncpy(buff, text,
// buffSize)) and the result is 0, else 0x81A80004 ("Insufficient buffer size"). makeDiscInfo adds
// "titleId", "localConceptId" and "gameDiscRoleType" only while a disc is mounted (its disc record
// non-null; a mounted disc without a concept id answers 0x81A80003); with no disc the object stays
// empty and serialises as "{}".
//
// The host has no disc drive content (see SceShellCore AppPromote: no disc), so the answer is the
// no-disc one: "{}", outSize 3.
#include <cstdint>
#include <cstring>

#include "host/boundary.h"

namespace ssn::eboot {

SSN_PINVOKE("libSceMediaEventListener", "GetDiscInfoNative", int32_t, ssn_MediaEventListener_GetDiscInfo,
            (char* buff, uint32_t buff_size, uint32_t* out_size)) {
  if (!buff || buff_size == 0 || !out_size) return static_cast<int32_t>(0x81A80000u);
  static const char kText[] = "{}";  // the empty object makeDiscInfo leaves with no disc
  *out_size = static_cast<uint32_t>(sizeof kText);  // length + 1
  if (*out_size > buff_size) return static_cast<int32_t>(0x81A80004u);
  std::strncpy(buff, kText, buff_size);
  return 0;
}

}  // namespace ssn::eboot
