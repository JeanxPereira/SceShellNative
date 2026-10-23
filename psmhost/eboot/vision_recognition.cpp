// VisionRecognitionUiProxy (eboot.bin, FW 4.03, vsh/shell/shell_ui/src/recognition): the
// vision engine behind face login / camera set-up, as the icalls of
// Sce.Vsh.ShellUI.VisionRecognition.Critical.VisionRecognitionUiProxy reach it
// ({name, fn} table 0xcdd090..).
//
// Objects: the recognizer (qword_EE1CE8, 1240 bytes, ctor 0x35EAE0: +1016 the callback list,
// +1232 "initialised"), guarded by the reader/writer lock unk_EE1D00 (rcg_rw_lock.cpp); the
// resource-arbitrator core (qword_EE1CF8, 24 bytes, +16 the settings callback); the vision
// manager driver (qword_EE1D28, 736 bytes) over libSceVisionManager.
//
// libSceVisionManager (/system/priv/lib/libSceVisionManager.sprx: sceVisionManagerInitialize
// 9I4TdP3A9-g 0x14B00 starts its "SceVisionMgrCtrlCoreThr" and has it load the dictionaries
// of /app0/recognition/vision/dict/) is not reproduced on the host — and this dump's
// NPXS40087 has no recognition/ directory. The engine initialisation therefore takes the
// eboot's own failure branch of 0x35F590: the driver's Initialize (0x38E3B0) reports an
// error, the objects are released and the recognizer pointer is cleared, so the per-frame
// CheckCallback finds no recognizer.
#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include "host/boundary.h"

namespace ssn::eboot {
namespace {

struct Recognizer {
  bool initialized = false;  // +1232
  // +1016: vector of callback objects; CheckCallback calls vtable +16 of each (0x387910).
};

struct Vision {
  std::shared_mutex lock;              // unk_EE1D00
  Recognizer* recognizer = nullptr;    // qword_EE1CE8
  bool ra_core = false;                // qword_EE1CF8
  const void* ra_settings_callback = nullptr;  // core +16 (&loc_35E0C0)
};
Vision& vision() {
  static Vision v;
  return v;
}

// InitializeVisionEngine() — 0x35C500. The kiosk test (nQ5AHlyJghQ == 1) skips everything;
// the host is a retail unit. Under the write lock (0x35DE10): the recognizer is created when
// absent (0x35EAE0) and initialised (0x35F590): RcgAllocator (0x35F850), callback object
// qword_EE1D20 (vtable 0xCDDDD0) added to +1016, the resource-arbitrator core created and
// initialised (0x35E4E0: sceSysmoduleLoadModuleInternal(0x80000092),
// sceResourceArbitratorSetCallback), the vision manager driver created (0x38E010) and
// initialised (0x38E3B0 -> sceVisionManagerInitialize; failure -> 0x81078001). A non-zero result or a zero
// register-data size (0x38E970) releases the callback object, the recognizer (vtable +16,
// qword_EE1CE8 = 0), the RA core and the allocator; otherwise the job event object is set up
// (0x365B10) and +1232 = 1.
void InitializeVisionEngine() {
  Vision& v = vision();
  std::unique_lock<std::shared_mutex> l(v.lock);
  if (!v.recognizer) v.recognizer = new Recognizer();
  if (v.recognizer->initialized) return;
  host::note_unimplemented(
      "eboot vision engine: libSceVisionManager (sceVisionManagerInitialize) not reproduced; 0x35F590 failure branch taken "
      "(recognizer released)");
  delete v.recognizer;
  v.recognizer = nullptr;
  v.ra_core = false;
}
SSN_ICALL("Sce.Vsh.ShellUI.VisionRecognition.Critical.VisionRecognitionUiProxy::InitializeVisionEngine", InitializeVisionEngine);

// CheckCallback() — 0x35CB30: try-read-lock (0x35DD60 mode 1: scePthreadRwlockTryrdlock,
// EBUSY -> return); with a recognizer whose +1232 is set, every callback object of +1016 is
// called (0x362710 -> 0x387910).
void CheckCallback() {
  Vision& v = vision();
  std::shared_lock<std::shared_mutex> l(v.lock, std::try_to_lock);
  if (!l.owns_lock()) return;
  if (v.recognizer && v.recognizer->initialized)
    host::note_unimplemented("eboot vision engine callback objects (0x387910) not reproduced");
}
SSN_ICALL("Sce.Vsh.ShellUI.VisionRecognition.Critical.VisionRecognitionUiProxy::CheckCallback", CheckCallback);

// ResourceArbitratorRegisterCallback() — 0x35D920: kiosk -> 0; otherwise 0x35E0B0: the RA
// core (qword_EE1CF8, ctor 0x35E480) created when absent, then 0x35E690(core, &loc_35E0C0):
// a callback other than the one stored -> stored, 0; the same one -> 0x81078002
// ("parameter already registered").
int32_t ResourceArbitratorRegisterCallback() {
  Vision& v = vision();
  static const char kCallback = 0;  // stands for loc_35E0C0
  static std::mutex ra_mu;           // (the eboot takes no lock here)
  std::lock_guard<std::mutex> l(ra_mu);
  v.ra_core = true;
  if (v.ra_settings_callback == &kCallback) return static_cast<int32_t>(0x81078002u);
  v.ra_settings_callback = &kCallback;
  return 0;
}
SSN_ICALL("Sce.Vsh.ShellUI.VisionRecognition.Critical.VisionRecognitionUiProxy::ResourceArbitratorRegisterCallback",
          ResourceArbitratorRegisterCallback);

}  // namespace
}  // namespace ssn::eboot
