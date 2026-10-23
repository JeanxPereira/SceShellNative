// libSceVnaInternal (FW 4.03): the Voice and Agent client the managed
// Sce.Vsh.VoiceAndAgent.Critical.API / UtilAPI call. Addresses are libSceVnaInternal.sprx
// offsets (export thunk -> implementation).
//
// HOST VALUE: no Voice and Agent daemon runs on the host. On the console SceShellCore starts
// it ("Voice and Agent", NPXS40094, daemon table 0x19F5050, run-level condition 0x768FB0:
// above run level 79 or 99) and it registers the IPMI server "SceVnaIpcServer" and the
// event flags "SceVnaStatVOIN" / "SceVnaVoLang". The host is the console before that daemon
// exists (the state ShellUI is in until ShellCore launches it), and every call takes the
// client's own path for it:
//   * sceVnaInitialize (0xD40) creates the IPMI client and connects with a zero wait time
//     (sub_6940: libSceIpmi connect 0x4D0 -> 0x520 -> sub_4110, ipmimgr call 1024). With no
//     server of that name the kernel's syscallConnectWithWaitServer (kernel 7.00
//     0xFFFFFFFF805D5B50; server lookup 0xFFFFFFFF805D7140) fails with ESRCH, which
//     libSceIpmi 0x520 passes through as 0x80020003; the client is destroyed, *context = 0
//     and 0x80020003 is returned.
//   * no context therefore exists, and every per-context call (the context-list lookup at the
//     top of each implementation, qword_10068) answers SCE_VNA_ERROR_NOT_INITIALIZED
//     (0x816F8007), sceVnaCheckCallback included.
//   * sceVnaUtilInitialize (0x6C90) counts the reference and opens the daemon's event flags,
//     which do not exist (ESRCH); the other Util calls then poll the unopened handles.
// What the managed side does with that is the firmware's own: AgentLauncher.Core logs the
// Initialize error and retries from its error state once a second (CoreStateSm.Error ->
// StartRecoveryCheckTimer), VnaCollector keeps a 0 context per user and never polls it
// (no "daemon crash" report), Manager's common-context calls fail and are dropped.
#include <cstdint>
#include <cstring>
#include <mutex>

#include "host/boundary.h"
#include "libkernel/event_flag.h"

namespace ssn::vna {
namespace {

constexpr int32_t kErrInvalidArgument = static_cast<int32_t>(0x816F8004u);  // SCE_VNA_ERROR_INVALID_ARGUMENT
constexpr int32_t kErrNotInitialized = static_cast<int32_t>(0x816F8007u);   // SCE_VNA_ERROR_NOT_INITIALIZED
constexpr int32_t kErrIpmiNoServer = kernel::kErrorEsrch;                   // 0x80020003

void note_absent() {
  static std::once_flag once;
  std::call_once(once, [] {
    host::note_unimplemented(
        "HOST VALUE Voice and Agent daemon (NPXS40094) absent: SceVnaIpcServer not registered, sceVnaInitialize -> "
        "0x80020003, per-context calls -> 0x816F8007");
  });
}

// Per-context calls: the context is never in the client's list.
int32_t no_context() {
  note_absent();
  return kErrNotInitialized;
}

// sceVnaUtil* state (0x10478 mutex, 0x10480 reference count, qword_10038 / qword_10040 the
// "SceVnaStatVOIN" / "SceVnaVoLang" event flags, -1 = not opened).
struct Util {
  std::mutex mu;
  int32_t refs = 0;
  kernel::EventFlag* voin = nullptr;
  kernel::EventFlag* lang = nullptr;
};
Util& util() {
  static Util u;
  return u;
}
int32_t open_flag(const char* name, kernel::EventFlag** out) {
  kernel::EventFlag* f = kernel::open_event_flag(name);
  if (!f) return kernel::kErrorEsrch;
  *out = f;
  return 0;
}
int32_t poll_all(kernel::EventFlag* f, uint64_t* bits) {
  return kernel::poll_event_flag(f, ~0ull, kernel::kEvfWaitModeOr, bits);
}

struct InitializeParam {  // SceVnaInitializeParam
  int32_t client_type;
  char name[32];
  int32_t user_id;
};

}  // namespace

// XCD6zBOkiIM (0x1E0 -> 0xD40).
SSN_PINVOKE("libSceVnaInternal", "sceVnaInitialize", int32_t, ssn_Vna_Initialize, (const InitializeParam* param, int64_t* context)) {
  note_absent();
  if (!param || !context || static_cast<uint32_t>(param->client_type) > 5 || strnlen(param->name, 32) > 31)
    return kErrInvalidArgument;
  // sub_6940: IPMI client to "SceVnaIpcServer", connect without waiting -> no server.
  *context = 0;
  return kErrIpmiNoServer;
}

// qLIzHK7b8ow (0x1F0 -> 0xF20).
SSN_PINVOKE("libSceVnaInternal", "sceVnaTerminate", int32_t, ssn_Vna_Terminate, (int64_t)) { return no_context(); }
// KFez0hNxLvY (0x480 -> 0x2EF0).
SSN_PINVOKE("libSceVnaInternal", "sceVnaCheckCallback", int32_t, ssn_Vna_CheckCallback, (int64_t)) { return no_context(); }

// The other per-context calls (export -> implementation), all context-lookup first:
#define SSN_VNA_CTX(entry, sym, ...) \
  SSN_PINVOKE("libSceVnaInternal", entry, int32_t, sym, (int64_t, __VA_ARGS__)) { return no_context(); }
#define SSN_VNA_CTX0(entry, sym) \
  SSN_PINVOKE("libSceVnaInternal", entry, int32_t, sym, (int64_t)) { return no_context(); }

SSN_VNA_CTX("sceVnaSetupKeyPhraseDetection", ssn_Vna_SetupKeyPhraseDetection, void*, void*)   // 0x3B0 -> 0x1060
SSN_VNA_CTX0("sceVnaStartKeyPhraseDetection", ssn_Vna_StartKeyPhraseDetection)                // 0x3C0 -> 0x11E0
SSN_VNA_CTX("sceVnaStopKeyPhraseDetection", ssn_Vna_StopKeyPhraseDetection, void*)            // 0x3D0 -> 0x12D0
SSN_VNA_CTX("sceVnaStartAnalyze", ssn_Vna_StartAnalyze, void*)                                // 0x260 -> 0x1410
SSN_VNA_CTX("sceVnaStartAnalyzeInternal", ssn_Vna_StartAnalyzeInternal, void*)                // 0x270 -> 0x1540
SSN_VNA_CTX("sceVnaStopAnalyze", ssn_Vna_StopAnalyze, void*)                                  // 0x280 -> 0x1730
SSN_VNA_CTX("sceVnaDevideAnalyze", ssn_Vna_DevideAnalyze, void*)                              // 0x290 -> 0x1850
SSN_VNA_CTX("sceVnaRegisterCallback", ssn_Vna_RegisterCallback, uint32_t, void*, int64_t)     // 0x460 -> 0x2C70
SSN_VNA_CTX0("sceVnaUnregisterCallback", ssn_Vna_UnregisterCallback)                          // 0x300 -> TvfmbplEsp4
SSN_VNA_CTX("sceVnaGetMicStatus", ssn_Vna_GetMicStatus, void*)                                // 0x490 -> 0x3330
SSN_VNA_CTX("sceVnaGetFeedbackStatus", ssn_Vna_GetFeedbackStatus, void*)                      // 0x4A0 -> 0x3470
SSN_VNA_CTX("sceVnaGetVadState", ssn_Vna_GetVadState, void*)                                  // 0x4B0 -> 0x3590
SSN_VNA_CTX("sceVnaGetTtsVolume", ssn_Vna_GetTtsVolume, void*)                                // 0x4C0 -> 0x3690
SSN_VNA_CTX("sceVnaNotifyStatus", ssn_Vna_NotifyStatus, int32_t, void*)                       // 0x4D0 -> 0x4D60
SSN_VNA_CTX("sceVnaNotify", ssn_Vna_Notify, const char*)                                      // 0x4E0 -> 0x4E90
SSN_VNA_CTX("sceVnaRequestTts", ssn_Vna_RequestTts, void*)                                    // 0x3E0 -> 0x1970
SSN_VNA_CTX("sceVnaRequestDialogTts", ssn_Vna_RequestDialogTts, void*)                        // 0x3F0 -> 0x1B20
SSN_VNA_CTX("sceVnaCancelTts", ssn_Vna_CancelTts, int32_t)                                    // 0x400 -> 0x1C60
SSN_VNA_CTX("sceVnaRequestPlayCachedTts", ssn_Vna_RequestPlayCachedTts, void*)                // 0x410 -> 0x2520
SSN_VNA_CTX("sceVnaRequestDownloadCahcedTts", ssn_Vna_RequestDownloadCahcedTts, void*)        // 0x420 -> 0x26A0
SSN_VNA_CTX("sceVnaRequestDownloadTts", ssn_Vna_RequestDownloadTts, void*, uint32_t)          // 0x2A0 -> 0x1D70
SSN_VNA_CTX("sceVnaCancelDownloadTts", ssn_Vna_CancelDownloadTts, int32_t)                    // 0x2B0 -> 0x2010
SSN_VNA_CTX("sceVnaOpenTtsStream", ssn_Vna_OpenTtsStream, int32_t, void*)                     // 0x2C0 -> 0x2130
SSN_VNA_CTX("sceVnaReadTtsStream", ssn_Vna_ReadTtsStream, int32_t, void*, uint32_t)           // 0x2D0 -> 0x22F0
SSN_VNA_CTX("sceVnaCloseTtsStream", ssn_Vna_CloseTtsStream, int32_t)                          // 0x2E0 -> 0x23D0
SSN_VNA_CTX("sceVnaSetDucking", ssn_Vna_SetDucking, int32_t)                                  // 0x430 -> 0x28E0
SSN_VNA_CTX("sceVnaSetTtsDuckingLevel", ssn_Vna_SetTtsDuckingLevel, int32_t)                  // 0x440 -> 0x29F0
SSN_VNA_CTX("sceVnaSetVadVolumeThreshold", ssn_Vna_SetVadVolumeThreshold, void*)              // 0x450 -> 0x2B10
SSN_VNA_CTX("sceVnaGetKeyPhraseInfoCount", ssn_Vna_GetKeyPhraseInfoCount, int32_t)            // 0x510 -> 0x5840
SSN_VNA_CTX("sceVnaGetKeyPhraseInfo", ssn_Vna_GetKeyPhraseInfo, int32_t, int32_t, void*)      // 0x520 -> 0x5960
SSN_VNA_CTX("sceVnaSetKeyPhrase", ssn_Vna_SetKeyPhrase, int32_t, int32_t, int32_t)            // 0x530 -> 0x5AB0
SSN_VNA_CTX("sceVnaSetKeyPhraseCategoryEnable", ssn_Vna_SetKeyPhraseCategoryEnable, int32_t, int32_t)  // 0x540 -> 0x5BF0
SSN_VNA_CTX("sceVnaSetClientStatus", ssn_Vna_SetClientStatus, int32_t)                        // 0x4F0 -> 0x4F50
SSN_VNA_CTX("sceVnaGetSupportedLanguageInfo", ssn_Vna_GetSupportedLanguageInfo, void*)        // 0x330 -> 0x5070
SSN_VNA_CTX("sceVnaGetSupportedSpeakerType", ssn_Vna_GetSupportedSpeakerType, int32_t, void*) // 0x340 -> 0x52A0
SSN_VNA_CTX("sceVnaGetAgentSettings", ssn_Vna_GetAgentSettings, int32_t, void*)               // 0x550 -> 0x3790
SSN_VNA_CTX("sceVnaSetEnableAssistantSettings", ssn_Vna_SetEnableAssistantSettings, int32_t)  // 0x560 -> 0x38D0
SSN_VNA_CTX("sceVnaSetTtsSpeedSettings", ssn_Vna_SetTtsSpeedSettings, int32_t)                // 0x570 -> 0x39E0
SSN_VNA_CTX("sceVnaSetTtsVolumeSettings", ssn_Vna_SetTtsVolumeSettings, uint32_t)             // 0x580 -> 0x3B00
SSN_VNA_CTX("sceVnaSetSessionTimeoutSettings", ssn_Vna_SetSessionTimeoutSettings, uint32_t)   // 0x590 -> 0x3C20
SSN_VNA_CTX("sceVnaSetLanguageSettings", ssn_Vna_SetLanguageSettings, int32_t)                // 0x5A0 -> 0x3D40
SSN_VNA_CTX("sceVnaSetEnableWupSettings", ssn_Vna_SetEnableWupSettings, int32_t)              // 0x5B0 -> 0x3E70
SSN_VNA_CTX("sceVnaSetAsrExclusive", ssn_Vna_SetAsrExclusive, int32_t)                        // 0x390 -> 0x53F0
SSN_VNA_CTX("sceVnaDisableAgentGui", ssn_Vna_DisableAgentGui, int32_t)                        // 0x5D0 -> 0x4560
SSN_VNA_CTX("sceVnaSetAgentIntentSessionTelemetry", ssn_Vna_SetAgentIntentSessionTelemetry, void*)  // 0x350 -> 0x4670
SSN_VNA_CTX("sceVnaSetWakeUpPhraseTelemetry", ssn_Vna_SetWakeUpPhraseTelemetry, void*)        // 0x360 -> 0x4800
SSN_VNA_CTX("sceVnaGenerateTelemetrySessionId", ssn_Vna_GenerateTelemetrySessionId, void*, void*)  // 0x370 -> 0x4990
SSN_VNA_CTX("sceVnaPlaySound", ssn_Vna_PlaySound, void*)                                      // 0x380 -> 0x4AE0
SSN_VNA_CTX("sceVnaPlayTts", ssn_Vna_PlayTts, void*)                                          // 0x5E0 -> 0x4C20
SSN_VNA_CTX0("sceVnaGetVoiceDataCollectionSettingsAsync", ssn_Vna_GetVoiceDataCollectionSettingsAsync)  // 0x5F0 -> 0x3F80
SSN_VNA_CTX("sceVnaSetVoiceDataCollectionSettingsAsync", ssn_Vna_SetVoiceDataCollectionSettingsAsync, const char*)  // 0x600 -> 0x4070
SSN_VNA_CTX("sceVnaSetEnableAssistantLegalCheckStatus", ssn_Vna_SetEnableAssistantLegalCheckStatus, int32_t)  // 0x5C0 -> 0x41C0
SSN_VNA_CTX("sceVnaGetAgentAvailabilityInfo", ssn_Vna_GetAgentAvailabilityInfo, int32_t, void*)  // 0x610 -> 0x42E0
SSN_VNA_CTX("sceVnaGetVoiceOskOptInStatus", ssn_Vna_GetVoiceOskOptInStatus, void*)            // 0x630 -> 0x5D10
SSN_VNA_CTX("sceVnaSetVoiceOskOptInStatus", ssn_Vna_SetVoiceOskOptInStatus, int32_t)          // 0x640 -> 0x5E40
SSN_VNA_CTX("sceVnaSendAgentEvent", ssn_Vna_SendAgentEvent, void*)                            // 0x620 -> 0x4420
SSN_VNA_CTX("sceVnaSetLocation", ssn_Vna_SetLocation, const char*)                            // 0x650 -> 0x5F60
SSN_VNA_CTX("sceVnaGetCurrentAudioInTelemetryInfo", ssn_Vna_GetCurrentAudioInTelemetryInfo, void*)  // 0x660 -> 0x6100
SSN_VNA_CTX("sceVnaSetUmmStatus", ssn_Vna_SetUmmStatus, void*)                                // 0x670 -> 0x6240
SSN_VNA_CTX("sceVnaSetVadConfig", ssn_Vna_SetVadConfig, void*)                                // 0x900 -> 0x6370
SSN_VNA_CTX("sceVnaSetAgentInfo", ssn_Vna_SetAgentInfo, void*)                                // 0x910 -> 0x64A0

#undef SSN_VNA_CTX
#undef SSN_VNA_CTX0

// ---- sceVnaUtil* ------------------------------------------------------------------------

// T4BiNWsLHtQ (0x920 -> 0x6C90): the count is raised in any case; the first reference opens
// "SceVnaStatVOIN" and then "SceVnaVoLang" (the second only when the first opened).
SSN_PINVOKE("libSceVnaInternal", "sceVnaUtilInitialize", int32_t, ssn_VnaUtil_Initialize, ()) {
  note_absent();
  Util& u = util();
  std::lock_guard<std::mutex> l(u.mu);
  if (u.refs++ != 0) return 0;
  int32_t r = open_flag("SceVnaStatVOIN", &u.voin);
  if (r >= 0) r = open_flag("SceVnaVoLang", &u.lang);
  return r;
}

// C2Hf+ZWoLJA (0x930 -> 0x6D10).
SSN_PINVOKE("libSceVnaInternal", "sceVnaUtilTerminate", int32_t, ssn_VnaUtil_Terminate, ()) {
  Util& u = util();
  std::lock_guard<std::mutex> l(u.mu);
  if (u.refs == 0) return kErrNotInitialized;
  if (--u.refs == 0) {
    u.voin = nullptr;
    u.lang = nullptr;
  }
  return 0;
}

// Bj2oUqp84J0 (0x940 -> 0x6DA0): SceVnaStatVOIN holds the user id in the high word and the
// mic / net / sign-in bits in the low bits.
SSN_PINVOKE("libSceVnaInternal", "sceVnaUtilGetVoiceInputStatus", int32_t, ssn_VnaUtil_GetVoiceInputStatus,
            (int32_t user_id, uint8_t* available, int32_t* status)) {
  Util& u = util();
  std::lock_guard<std::mutex> l(u.mu);
  if (u.refs == 0) return kErrNotInitialized;
  if (!available || !status) return kErrInvalidArgument;
  *available = 0;
  std::memset(status, 0, 5 * sizeof(int32_t));
  uint64_t bits = 0;
  const int32_t r = poll_all(u.voin, &bits);
  if (r != kernel::kErrorEbusy && r < 0) {
    status[4] = r;
    return r;
  }
  if (static_cast<int32_t>(bits >> 32) == user_id) {
    if ((bits & 1) == 0) status[0] = static_cast<int32_t>(0x816F8043u);
    if ((bits & 2) == 0) status[2] = static_cast<int32_t>(0x816F8044u);
    if ((bits & 4) == 0) status[1] = static_cast<int32_t>(0x816F8045u);
  } else {
    status[4] = static_cast<int32_t>(0x816F8009u);  // SCE_VNA_ERROR_INVALID_USER_ID
  }
  *available = status[0] == 0 && status[1] == 0 && status[2] == 0 && status[3] == 0 && status[4] == 0;
  return 0;
}

// jJwavlmTirk (0x950 -> 0x6F10): SceVnaVoLang is a bit set of language ids.
SSN_PINVOKE("libSceVnaInternal", "sceVnaUtilIsLanguageSupported", int32_t, ssn_VnaUtil_IsLanguageSupported,
            (int32_t language, uint8_t* supported)) {
  Util& u = util();
  std::lock_guard<std::mutex> l(u.mu);
  if (u.refs == 0) return kErrNotInitialized;
  if (!supported) return kErrInvalidArgument;
  *supported = 0;
  uint64_t bits = 0;
  const int32_t r = poll_all(u.lang, &bits);
  if (r != kernel::kErrorEbusy && r < 0) return r;
  int32_t list[50];
  uint32_t n = 0;
  for (uint32_t b = 0; n < 50; ++b) {
    if ((bits >> b) & 1) list[n++] = static_cast<int32_t>(b);
    if (b > 62) break;
  }
  for (uint32_t i = 0; i < n; ++i)
    if (list[i] == language) {
      *supported = 1;
      break;
    }
  return 0;
}

// QFx6geRbV5Q (0x960 -> 0x7050).
SSN_PINVOKE("libSceVnaInternal", "sceVnaUtilGetSupportedLanguageList", int32_t, ssn_VnaUtil_GetSupportedLanguageList,
            (int32_t* list, uint32_t len)) {
  Util& u = util();
  std::lock_guard<std::mutex> l(u.mu);
  if (u.refs == 0) return kErrNotInitialized;
  if (!list || len < 50) return kErrInvalidArgument;
  uint64_t bits = 0;
  const int32_t r = poll_all(u.lang, &bits);
  if (r != kernel::kErrorEbusy && r < 0) return r;
  uint32_t n = 0;
  for (uint32_t b = 0; n < len; ++b) {
    if ((bits >> b) & 1) list[n++] = static_cast<int32_t>(b);
    if (b > 62) break;
  }
  return static_cast<int32_t>(n);
}

}  // namespace ssn::vna
