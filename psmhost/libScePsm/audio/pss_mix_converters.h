// The voice -> output converters of libScePsm's PSS audio layer (FW 4.03) for 32-bit float
// sources mixed into the 2-channel 32-bit float main output: entries 8..11 of the converter
// table at 0x4FBED0 (filled by sub_1741E0; sub_190330 overrides none of them on this
// firmware). The voice picks them in sub_15F670:
//   1 ch: rate ratio == 1 -> [9] sub_16DE80 (generic [8] sub_16C6A0), else [8] / [8]
//   2 ch: rate ratio == 1 -> [11] sub_16D970 (generic [10] sub_16C9A0), else [10] / [10]
// and switches to the generic one when the pan leaves the centre (sub_1646A0) or the rate
// leaves [0.999, 1.0001] (sub_1644A0).
//
// Common signature: (src frames, dst float stereo, dst frame offset, in/out source frames,
// in/out destination frames, 20-byte resampler state (voice +208), volume, volume step,
// pan [0..1], pan step, source step = ratio * rate). The output is accumulated (+=).
// Pan law (all four): pan 0.5 = unchanged; mono: L *= 2 - 2p, R *= 2p; stereo below 0.5:
// L = vL + (1 - 2p) vR, R = 2p vR; stereo from 0.5: L = (2 - 2p) vL, R = vR + (2p - 1) vL.
#pragma once
#include <cstdint>

namespace ssn::psm::audio {

using PssConverter = uint32_t (*)(const float* src, float* dst, uint32_t dst_off, uint32_t* src_frames,
                                  uint32_t* dst_frames, float* state, float vol, float dvol, float pan, float dpan,
                                  float step);

uint32_t pss_conv_mono_resample(const float*, float*, uint32_t, uint32_t*, uint32_t*, float*, float, float, float,
                                float, float);  // [8]  sub_16C6A0
uint32_t pss_conv_mono_unity(const float*, float*, uint32_t, uint32_t*, uint32_t*, float*, float, float, float, float,
                             float);  // [9]  sub_16DE80
uint32_t pss_conv_stereo_resample(const float*, float*, uint32_t, uint32_t*, uint32_t*, float*, float, float, float,
                                  float, float);  // [10] sub_16C9A0
uint32_t pss_conv_stereo_unity(const float*, float*, uint32_t, uint32_t*, uint32_t*, float*, float, float, float,
                               float, float);  // [11] sub_16D970

// sub_1665C0: a 6-channel 16-bit source mixed into an 8-channel float port (the BGM port;
// sub_15F360 sets it as both converters). `src` holds int16 samples.
uint32_t pss_conv_6ch16_8ch(const float*, float*, uint32_t, uint32_t*, uint32_t*, float*, float, float, float, float,
                            float);

// sub_174570(index): table entry, nullptr for entries this host does not reproduce
// (0..7: 8- and 16-bit sources, which no ShellUI resource produces).
PssConverter pss_converter(int index);

}  // namespace ssn::psm::audio
