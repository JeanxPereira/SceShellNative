// libSceNgs2.native — the sampler voice's Player module (id 1, descriptor 0x696E0, process 0x30CF0)
// with its decoder class 0 (vtable 0x6E978: setup 0x25AC0, process 0x25D80) for waveform type 64
// (ATRAC9), the block queue and reader (0x23DF0..0x24330, queue 0xAF30 / 0xBD00 / 0xBF10), the
// resampler 0x8C90, and sceNgs2ParseWaveformData (0xCED0 -> 0xCF50) / CalcWaveformBlock (0xE060)
// for RIFF ATRAC9. The class-1 decoder (AJM, used only for racks with flag 0x10000000 from sampler
// rack option +0xCC) and the tone generators are not on this path.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "core/log.h"
#include "host/boundary.h"
#include "libSceNgs2/ngs2_at9.h"
#include "libSceNgs2/ngs2_image.h"
#include "libSceNgs2/ngs2_internal.h"

namespace ssn::ngs2 {
namespace {

void note_once(const char* what) { host::note_unimplemented(std::string("ngs2 player: ") + what); }

// Entry of the block queue built by sceNgs2VoiceControl 0x10000001 (absolute data address).
struct BlockEntry {
  const uint8_t* data;
  uint64_t size;
  uint32_t num_repeats, num_skip_samples, num_samples, zero;
  uint64_t user_data;
};
static_assert(sizeof(BlockEntry) == 40);

// The block queue (relative-offset ring with a writer and a reader set, params +0x18); the writer
// set becomes the reader set when the parameters are committed (sub_B9F0), which here happens
// before the Player reads them, so one ring serves both.
struct BlockQueue {
  std::vector<BlockEntry> ring;
  uint32_t head = 0, count = 0;
  bool last = false;  // set +9: end of data follows the queued blocks
  // sub_AF30(queue, entries, n, flags): (flags & 4) replaces, (flags & 1) == 0 marks the last block.
  void append(const BlockEntry* e, uint32_t n, uint8_t flags) {
    if (flags & 4) {
      ring.clear();
      head = count = 0;
      last = false;
    }
    if (flags & 8) note_once("waveform blocks flag 8 (keep current only) not reproduced");
    for (uint32_t i = 0; i < n; ++i) {
      if (ring.size() < head + count + 1) ring.resize(head + count + 1);
      ring[head + count] = e[i];
      ++count;
    }
    if ((flags & 1) == 0) last = true;
  }
  const BlockEntry* peek(uint32_t k) const { return k < count ? &ring[head + k] : nullptr; }  // sub_BF10
  void pop() {  // sub_BD00
    if (count) {
      ++head;
      --count;
    }
  }
  bool ended() const { return last && count == 0; }  // sub_23E50
  void rewind() {}  // sub_BE20: the reader set restarts at its head (it has not been read yet here)
};

// Block reader (slot +48), consumer mode (reader +16 = 1).
struct Reader {
  BlockQueue* q = nullptr;
  uint32_t start = 0, end = 0, cur = 0, byte_off = 0, repeat = 0;
  // sub_23EF0 / sub_23F80: when idle, load the head block's skip / end (blocks with neither are popped).
  void refresh() {
    if (start != 0 || end != 0) return;
    while (const BlockEntry* b = q->peek(0)) {
      if (b->num_skip_samples || b->num_samples) {
        start = b->num_skip_samples;
        end = b->num_skip_samples + b->num_samples;
        cur = 0;
        byte_off = 0;
        repeat = 0;
        return;
      }
      q->pop();
    }
  }
  // sub_24330(bytes, samples): samples first (the block's end repeats it or pops it), then bytes.
  void advance(uint32_t bytes, uint32_t samples) {
    const BlockEntry* b = q->peek(0);
    uint32_t done = 0, consumed_bytes = 0;
    while (b && done < samples) {
      const uint32_t step = std::min(samples - done, end - cur);
      cur += step;
      done += step;
      if (end > cur) break;
      consumed_bytes += static_cast<uint32_t>(b->size - byte_off);
      cur = 0;
      byte_off = 0;
      ++repeat;
      if (b->num_repeats == 0xFFFFFFFFu || repeat < b->num_repeats) {
        note_once("waveform block repeats (exitLoopCount, loop callbacks) not reproduced");
        break;
      }
      repeat = 0;
      q->pop();
      start = end = 0;
      refresh();
      b = q->peek(0);
    }
    const uint32_t left = bytes > consumed_bytes ? bytes - consumed_bytes : 0;
    if ((b = q->peek(0)) != nullptr && left) byte_off += static_cast<uint32_t>(std::min<uint64_t>(left, b->size - byte_off));
  }
};

// Per-voice decoder slot (class 0; 9400 bytes on the console).
struct DecoderSlot {
  uint32_t channels = 0;
  uint8_t config[4]{};
  uint32_t frame_samples = 0, sf_bytes = 0;
  uint8_t frame_index = 0, frames_per_sf = 0;
  uint32_t sf_offset = 0;
  uint16_t step = 0, frac = 0, hist = 0;
  std::vector<std::vector<float>> history;  // 260 floats per channel (1040-byte area), samples end at [260]
  Reader reader;
  bool report_ended = false, report_starved = false;
  At9FrameDecoder dec;
};

// setup 0x25AC0 for type 64.
void decoder_setup(DecoderSlot& s, const uint32_t fmt[6], BlockQueue* q) {
  s.channels = fmt[1];
  s.reader = Reader{};
  s.reader.q = q;
  s.step = 0;
  s.frac = 0;
  s.hist = 4;
  s.history.assign(s.channels, std::vector<float>(260, 0.0f));  // the 4 floats before the end zeroed (sub_4F80)
  const uint32_t c = fmt[3];
  s.config[0] = static_cast<uint8_t>(c >> 24);
  s.config[1] = static_cast<uint8_t>(c >> 16);
  s.config[2] = static_cast<uint8_t>(c >> 8);
  s.config[3] = static_cast<uint8_t>(c);
  s.frame_index = 0;
  s.sf_offset = 0;
  s.frames_per_sf = static_cast<uint8_t>(1u << (((c & 0xFF) >> 3) & 3));
  const auto& fs = frame_samples_table();  // word_4A520[(cfg >> 20) & 15]
  s.frame_samples = fs.empty() ? 0 : fs[(c >> 20) & 15];
  s.sf_bytes = static_cast<uint16_t>((((c & 0xFFFF) >> 5) + 1) << (((c & 0xFF) >> 3) & 3));
  s.report_ended = s.report_starved = false;
}

// sub_8C90: resample temp -> out (n samples per channel); 4-tap, 512-phase FIR at 0x6C930 except at
// step 0x4000 with no fraction, where it is the copy sub_5CE0(1, temp, 0, out).
void resample(DecoderSlot& s, float* const* in, float* const* out, uint32_t n, uint32_t ch) {
  if (s.step == 0x4000 && s.frac == 0) {
    for (uint32_t c = 0; c < ch; ++c) std::memmove(out[c], in[c] + 1, sizeof(float) * n);
    return;
  }
  note_once("resampling at a ratio other than 1 (FIR 0x6C930; the group-of-4 order is not verified)");
  const auto& coef = resampler_fir();
  if (coef.empty()) return;
  uint32_t pos = s.frac;
  for (uint32_t c = 0; c < ch; ++c) {
    pos = s.frac;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t base = pos >> 14;
      const float* k = coef.data() + 4 * ((pos >> 5) & 0x1FF);
      const float* x = in[c] + base;
      out[c][i] = (x[0] * k[0] + x[1] * k[1]) + (x[2] * k[2] + x[3] * k[3]);
      pos += s.step;
    }
  }
  s.frac = static_cast<uint16_t>(pos & 0x3FFF);
}

// process 0x25D80 for type 64 (n = grain).
int decoder_process(DecoderSlot& s, uint32_t n, float ratio, float* const* temp, float* const* out) {
  s.report_starved = false;
  {
    float r = std::min(4.0f, ratio) * 16384.0f;
    if (ratio < 0.0f) r = 0.0f;
    const int64_t v = static_cast<int64_t>(r);
    s.step = static_cast<uint16_t>(static_cast<uint32_t>(v) < 0xFFFF ? v : 0xFFFF);
  }
  const uint32_t consumed = (static_cast<uint32_t>(s.frac) + n * s.step) >> 14;
  const uint32_t need = consumed + 4;
  const uint32_t ch = s.channels;
  const uint32_t hist0 = s.hist;
  uint32_t total = 0;
  bool skip_decode = false;
  if (hist0) {  // a) history to the front of the temporaries
    const uint32_t cp = need > hist0 ? hist0 : need;
    for (uint32_t c = 0; c < ch; ++c) std::memcpy(temp[c], s.history[c].data() + 260 - hist0, sizeof(float) * cp);
    s.hist = static_cast<uint16_t>(hist0 - cp);
    total = need;
    if (need <= hist0) skip_decode = true;
  }
  if (!skip_decode) {  // b) decode frames after the history
    uint32_t decoded = 0;
    const uint32_t want = need - hist0;
    while (decoded < want) {
      Reader& rd = s.reader;
      rd.refresh();
      if (rd.start == 0 && rd.end == 0) break;
      const BlockEntry* b = rd.q->peek(0);
      if (!b) break;
      uint32_t skip = rd.start > rd.cur ? rd.start - rd.cur : 0;
      if (skip >= s.frame_samples) skip = s.frame_samples;
      const uint32_t cnt = std::min(rd.end - rd.cur, s.frame_samples - skip);
      if (rd.cur == 0 && skip != 0 && rd.byte_off == 0) {  // 0x36070 at the start of a block with skip
        if (s.dec.init(s.config, ch) < 0) return -1;
        s.frame_index = 0;
        s.sf_offset = 0;
      }
      const uint32_t avail = s.sf_bytes - s.sf_offset;
      if (b->size - rd.byte_off < avail) {
        note_once("superframes spanning waveform blocks (sub_272F0) not reproduced");
        break;
      }
      float* dst[32];
      for (uint32_t c = 0; c < ch && c < 32; ++c) dst[c] = temp[c] + hist0 + decoded;
      uint32_t used = 0;
      const bool last = static_cast<uint32_t>(s.frame_index) + 1 >= s.frames_per_sf;
      if (s.dec.decode_frame(b->data + rd.byte_off, avail, last, dst, skip, cnt, &used) < 0) return -1;
      if (++s.frame_index < s.frames_per_sf) {
        s.sf_offset += used;
      } else {
        s.frame_index = 0;
        s.sf_offset = 0;
      }
      rd.advance(used, cnt + skip);
      decoded += cnt;
    }
    total = hist0 + decoded;
    if (need > total) {  // c) LABEL_75: zero the rest; ended or starved
      for (uint32_t c = 0; c < ch; ++c) std::memset(temp[c] + total, 0, sizeof(float) * (need - total));
      if (s.reader.q->ended()) s.report_ended = true;
      else s.report_starved = true;
    }
  }
  resample(s, temp, out, n, ch);  // d)
  if (s.hist != 0) {               // e) history for the next grain
    s.hist = static_cast<uint16_t>(s.hist + 4);
  } else if (total > consumed) {
    const uint32_t rest = total - consumed;
    s.hist = static_cast<uint16_t>(rest);
    for (uint32_t c = 0; c < ch; ++c)
      std::memcpy(s.history[c].data() + 260 - rest, temp[c] + consumed, sizeof(float) * rest);
  }
  return 0;
}

}  // namespace

// Player parameters (descriptor 0x696E0 tables 0x69440 / 0x69470 / 0x695C0).
struct PlayerParams {
  uint32_t format[6] = {};             // +0x00 waveformFormat (0x18 bytes, param type 0x42)
  std::shared_ptr<BlockQueue> queue;   // +0x18 waveformBlocks (param type 0x81)
  uint64_t waveform_data = 0;          // +0x20
  float pitch_ratio = 1.0f;            // +0x28 (default 1.0, range [0, 4])
  uint32_t exit_loop_count = 0;        // +0x2C
};
// Player state (ctx +0x18, 128 bytes) and its decoder slot.
struct PlayerState {
  int32_t code = 0;  // +0x18: 0 / -421 / -378 idle, -525 decoding, -578 error, -588 ended
  DecoderSlot slot;
};
void PlayerDeleter::operator()(PlayerParams* p) const { delete p; }
void PlayerDeleter::operator()(PlayerState* p) const { delete p; }
PlayerParams* player_new_params() { return new PlayerParams; }
PlayerState* player_new_state() { return new PlayerState; }
void player_copy_params(PlayerParams& dst, const PlayerParams& src) { dst = src; }

void player_reset_params(PlayerParams& p) {
  p.format[0] = p.format[1] = p.format[2] = p.format[3] = p.format[4] = p.format[5] = 0;
  if (!p.queue) p.queue = std::make_shared<BlockQueue>();
  p.waveform_data = 0;
  p.pitch_ratio = 1.0f;
  p.exit_loop_count = 0;
}

// sub_A5A0 for the Player's parameters 0 (waveformFormat, type 0x42) and 1 (waveformBlocks, 0x81).
int32_t player_set_param(PlayerParams& p, const VoiceCommand& cmd) {
  switch (cmd.id & 0x3FFF) {
    case 0:
      std::memcpy(p.format, cmd.value.ptr, sizeof p.format);
      return 0;
    case 1: {
      if (!p.queue) p.queue = std::make_shared<BlockQueue>();
      p.queue->append(static_cast<const BlockEntry*>(cmd.value.ptr), cmd.count, cmd.flags);
      return 0;
    }
    default:
      note_once("parameters other than waveformFormat / waveformBlocks not reproduced");
      return 0;
  }
}

// Player process — 0x30CF0. Returns the module result bits (1 playing, |4 ended, |8 starved).
int32_t player_process(ModuleCtx& c) {
  Voice& v = *c.voice;
  if (!v.player_params || !v.player_state) return 0;
  PlayerParams& prm = *v.player_params;
  PlayerState& st = *v.player_state;
  const uint32_t n = c.num_samples;
  auto zero = [&] {
    for (uint32_t ch = 0; ch < c.num_channels; ++ch) clear_samples(c.out[ch], n);  // sub_5C10
  };
  if (c.flags & 0x100) {  // reset: sub_BE20(params+24), memset(state, 0, 128)
    if (prm.queue) prm.queue->rewind();
    st.code = 0;
  }
  if (v.flags.load() & 0x2000000) {  // sub_15CE0: killed
    zero();
    return 0;
  }
  if (st.code == -588) return 0;
  if (st.code == 0 || st.code == -421 || st.code == -378) {
    if (!(c.flags & 0x200)) {
      zero();
      st.code = -378;
      return 0;
    }
    st.code = -421;
    if (prm.format[0] != 64) {
      note_once("waveform types other than ATRAC9 (64) not reproduced");
      return 0;
    }
    if (!prm.queue || prm.queue->count == 0) return 0;  // sub_BEC0: keep waiting
    decoder_setup(st.slot, prm.format, prm.queue.get());
    if (st.slot.frame_samples == 0) {
      log::error("ngs2", "libSceNgs2.native frame-size table (0x4A520) not readable");
      return 0;
    }
    st.code = -525;
  }
  // sub_15600: one temporary per channel of 4*n + 264 bytes from the system pool.
  thread_local std::vector<std::vector<float>> tmp;
  thread_local std::vector<float*> tp;
  if (tmp.size() < st.slot.channels) tmp.resize(st.slot.channels);
  tp.assign(st.slot.channels, nullptr);
  for (uint32_t ch = 0; ch < st.slot.channels; ++ch) {
    tmp[ch].assign(size_t(n) + 66, 0.0f);
    tp[ch] = tmp[ch].data();
  }
  const float ratio = static_cast<float>(prm.format[2]) / static_cast<float>(c.sample_rate) * prm.pitch_ratio;
  if (decoder_process(st.slot, n, ratio, tp.data(), c.out) < 0) {
    log::error("ngs2", "Failed to decode codec data (voice {})", v.index);
    zero();
    st.code = -578;
    return 1;
  }
  int32_t ret = 1 | (st.slot.report_starved ? 8 : 0);
  if (st.slot.report_ended) {
    ret |= 4;
    st.code = -588;
  } else {
    st.code = -525;
  }
  return ret;
}

// --- sceNgs2ParseWaveformData (0xCED0 -> 0xCF50), in-memory RIFF / WAVE, ATRAC9 ---------------------

namespace {

// sceNgs2CalcWaveformBlock 0xE060 for type 64.
void calc_waveform_block_at9(const WaveformInfo& w, uint32_t start, uint32_t count, WaveformBlock* b) {
  std::memset(b, 0, sizeof *b);
  const auto& fst = frame_samples_table();
  const uint32_t cfg = w.config_data;
  const uint32_t sfi = ((cfg & 0xFF) >> 3) & 3;
  const uint64_t sf_bytes = (static_cast<uint64_t>((cfg & 0xFFFF) >> 5) + 1) << sfi;
  const uint32_t fs = fst.empty() ? 0 : fst[(cfg >> 20) & 0xF];
  const uint32_t sf_samples = fs << sfi;
  if (sf_samples == 0) return;
  const uint32_t v19 = start + fs;
  const uint64_t off = sf_bytes * (start / sf_samples);
  b->data_offset = off;
  b->data_size = static_cast<uint64_t>((sf_samples + count + v19 - 1) / sf_samples) * static_cast<uint32_t>(sf_bytes) - off;
  b->num_skip_samples = v19 - start / sf_samples * sf_samples;
  b->num_samples = count;
}

}  // namespace

int32_t ParseWaveformData(const void* data, size_t size, WaveformInfo* out) {
  constexpr int32_t kBad = static_cast<int32_t>(0x804A8430u);  // -2142600144: invalid waveform data
  if (!data || !out) return kBad;
  const auto* d = static_cast<const uint8_t*>(data);
  std::memset(out, 0, sizeof *out);
  auto u32 = [&](size_t o) {
    uint32_t v;
    std::memcpy(&v, d + o, 4);
    return v;
  };
  if (size < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) {
    note_once("ParseWaveformData: sources other than RIFF/WAVE (VAG, raw) not reproduced");
    return kBad;
  }
  // sub_E560: chunks from offset 12 (word aligned) within the RIFF size.
  auto find = [&](const char tag[4], uint32_t* off, uint32_t* sz) -> bool {
    const uint64_t riff_end = static_cast<uint64_t>(u32(4)) - 4;
    uint64_t p = 12;
    while (p + 8 <= size) {
      const uint32_t s = u32(p + 4);
      if (std::memcmp(d + p, tag, 4) == 0) {
        *off = static_cast<uint32_t>(p) + 8;
        *sz = s;
        return true;
      }
      if (s == 0) return false;
      p = p + 8 + ((s + 1) & ~1u);
      if (!(riff_end > p)) return false;
    }
    return false;
  };
  uint32_t fo, fs, ao, as;
  if (!find("fmt ", &fo, &fs) || fs < 0x28) return kBad;
  const uint8_t* f = d + fo;
  if (static_cast<uint16_t>(f[0] | f[1] << 8) != 0xFFFE) {
    note_once("ParseWaveformData: PCM / ADPCM / other RIFF formats not reproduced");
    return kBad;
  }
  // 0xD37E: SubFormat (fmt +0x18) against the GUIDs in the module image: 0x6C820 (another
  // extensible format), 0x6C830 (ATRAC9; also taken when the fmt chunk is 52 bytes), 0x6C840.
  const auto g820 = module_bytes(0x6C820, 16), g830 = module_bytes(0x6C830, 16);
  if (g820.size() != 16 || g830.size() != 16) return kBad;
  if (std::memcmp(f + 0x18, g820.data(), 16) == 0 ||
      (std::memcmp(f + 0x18, g830.data(), 16) != 0 && fs != 52)) {
    note_once("ParseWaveformData: extensible formats other than ATRAC9 not reproduced");
    return kBad;
  }
  const auto& fst = frame_samples_table();
  if (fst.empty()) return kBad;
  out->waveform_type = 64;
  out->num_channels = static_cast<uint16_t>(f[2] | f[3] << 8);
  out->sample_rate = u32(fo + 4);
  out->config_data = uint32_t(f[44]) << 24 | uint32_t(f[45]) << 16 | uint32_t(f[46]) << 8 | f[47];
  const uint32_t frame_samples = fst[f[45] >> 4];
  const uint32_t frame_bytes = ((uint32_t(f[46]) << 8 | f[47]) >> 5) + 1;
  const uint32_t fpsf = 1u << ((f[47] >> 3) & 3);
  uint32_t num_samples = 0, xo, xs;
  if (find("fact", &xo, &xs) && xs >= 4) num_samples = u32(xo);
  uint32_t so, ss;
  if (find("smpl", &so, &ss)) note_once("ParseWaveformData: 'smpl' loops not reproduced");
  if (!find("data", &ao, &as)) return kBad;
  out->data_offset = ao;
  out->data_size = as;
  out->num_samples = num_samples;
  out->frame_bytes = frame_bytes;
  out->frame_samples = frame_samples;
  out->frames_per_sf = fpsf;
  out->sf_bytes = fpsf * frame_bytes;
  out->sf_samples = frame_samples * fpsf;
  out->delay = frame_samples;
  calc_waveform_block_at9(*out, 0, num_samples, &out->blocks[0]);
  out->num_blocks = 1;
  out->blocks[0].data_offset += ao;  // LABEL_126: blocks are relative to the data chunk
  return 0;
}

}  // namespace ssn::ngs2
