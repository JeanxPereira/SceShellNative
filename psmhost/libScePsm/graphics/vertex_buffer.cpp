// Sce.PlayStation.Core.Graphics.PsmVertexBuffer (libScePsm.sprx, FW 4.03).
#include <algorithm>
#include <cstring>

#include "common/mono_array.h"
#include "host/boundary.h"
#include "libScePsm/error.h"
#include "libScePsm/graphics/core.h"
#include "libScePsm/graphics/objects.h"

namespace ssn::psm::gfx {
namespace {

constexpr uint32_t kComponentBytes[16] = {0, 4, 2, 1, 0, 0, 2, 2, 1, 1, 0, 0, 0, 0, 0, 0};  // 0x40fb40

// Create(vertexCount, indexCount, option, VertexFormat[] formats, out handle) — 0x1eb3c0:
//   null formats with a count -> 0x80b30002; more than 8 formats or counts >= 0x10000
//   -> 0x80b30003; option != 0 or an invalid format -> 0x80b30001. Each format gets
//   its own stream of vertexCount elements; the interleaved layout SetVertices reads
//   aligns each element to its component size (types 1,2,3,6,7,8,9) and the stride is
//   rounded up to the largest component size.
int32_t VertexBuffer_Create(int32_t vertex_count, int32_t index_count, int32_t option, MonoArray* formats, int32_t* out) {
  if (!Core::get().created()) return kErrorGraphicsSystem;
  ArrayView f = view(formats);
  if (f.length > 0 && !f.data) return kErrorArgumentNull;
  if (f.length > 8 || (uint32_t(vertex_count) | uint32_t(index_count)) >= 0x10000) return kErrorOutOfRange;
  if (option != 0) return kErrorInvalidParameter;
  auto vb = std::make_shared<VertexBuffer>();
  vb->vertex_count = vertex_count;
  vb->index_count = index_count;
  uint32_t offset = 0, max_align = 1;
  for (size_t i = 0; i < f.length; ++i) {
    const uint32_t fmt = reinterpret_cast<const uint32_t*>(f.data)[i];
    if ((fmt & 0xff) > 3) return kErrorInvalidParameter;
    if (fmt & 0xff00) {
      const uint32_t m = fmt & 0xff00;
      if (m >= 0x201 && (fmt & 0xef00) - 1536 >= 0x301) return kErrorInvalidParameter;
    } else if (fmt & 0xff) {
      return kErrorInvalidParameter;
    }
    vb->formats.push_back(fmt);
    if (fmt == 0) {
      vb->stream_offsets.push_back(offset);
      vb->streams.emplace_back();
      continue;
    }
    const uint32_t type = (fmt >> 8) & 0xf, comp = kComponentBytes[type];
    max_align = std::max(max_align, comp);
    if ((974u >> type) & 1) offset = (offset + comp - 1) & ~(comp - 1);
    vb->stream_offsets.push_back(offset);
    const uint32_t raw = comp * ((fmt & 3) + 1) * (((fmt >> 4) & 3) + 1);
    vb->streams.emplace_back(size_t(((raw == 3 || raw == 6) ? raw * 4 / 3 : raw)) * vertex_count, 0);
    offset += raw;
  }
  vb->vertex_stride = f.length ? (offset + max_align - 1) & ~(max_align - 1) : 0;
  vb->indices.assign(index_count, 0);
  *out = Objects::get().add_vertex_buffer(std::move(vb));
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmVertexBuffer::Create", VertexBuffer_Create);

int32_t VertexBuffer_Delete(int32_t h) {  // 0x1ebbc0
  auto& o = Objects::get();
  o.release(o.vertex_buffers(), h);
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmVertexBuffer::Delete", VertexBuffer_Delete);

int32_t VertexBuffer_AddRef(int32_t h) {  // 0x1ebc60
  auto& o = Objects::get();
  return o.add_ref(o.vertex_buffers(), h) ? kOk : kErrorObjectDisposed;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmVertexBuffer::AddRef", VertexBuffer_AddRef);

// SetVertices(handle, Array vertices, to, from, count) — 0x1ebd70: count < 0 means the
// whole buffer (the array must then hold exactly vertexCount*stride bytes); the range
// [to, to+count) must fit the buffer and [from, from+count) the array; each stream
// receives its element of every interleaved vertex.
int32_t VertexBuffer_SetVertices(int32_t h, MonoArray* vertices, int32_t to, int32_t from, int32_t count) {
  VertexBuffer* vb = Objects::get().vertex_buffer(h);
  if (!vb) return kErrorObjectDisposed;
  ArrayView a = view(vertices);
  if (count < 0) {
    count = vb->vertex_count;
    if (vertices && size_t(count) * vb->vertex_stride != a.bytes()) return kErrorInvalidOperation;
  }
  if (!a.data) return kErrorArgumentNull;
  if ((count | to | from) < 0 || int64_t(count) + to > vb->vertex_count) return kErrorOutOfRange;
  if (int64_t(vb->vertex_stride) * (from + int64_t(count)) > int64_t(a.bytes())) return kErrorInvalidOperation;
  if (count == 0) return kOk;
  vb->valid_first = to;
  vb->valid_end = to + count;
  const uint8_t* src = a.data + size_t(vb->vertex_stride) * from;
  for (size_t s = 0; s < vb->formats.size(); ++s) {
    const uint32_t fmt = vb->formats[s];
    if (!fmt) continue;
    const uint32_t raw = kComponentBytes[(fmt >> 8) & 0xf] * ((fmt & 3) + 1) * (((fmt >> 4) & 3) + 1);
    uint8_t* dst = vb->streams[s].data() + size_t(raw) * to;
    const uint8_t* p = src + vb->stream_offsets[s];
    for (int32_t i = 0; i < count; ++i, dst += raw, p += vb->vertex_stride) std::memcpy(dst, p, raw);
  }
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmVertexBuffer::SetVertices", VertexBuffer_SetVertices);

// SetVertices2(handle, stream, Array vertices, VertexFormat srcFormat, offset, stride, to,
// from, count) — 0x1ec210: fills one stream. count < 0 means the whole buffer (the
// array must then hold exactly vertexCount elements of the stream's format); stream out
// of range -> 0x80b30003; srcFormat None or equal to the stream's format copies as is
// (other source formats are converted by the native code — not reproduced); stride 0
// means the element size; the source must hold stride*(from+count-1)+offset+elem bytes.
int32_t VertexBuffer_SetVertices2(int32_t h, int32_t stream, MonoArray* vertices, uint32_t src_format, int32_t offset, int32_t stride,
                                  int32_t to, int32_t from, int32_t count) {
  VertexBuffer* vb = Objects::get().vertex_buffer(h);
  if (!vb) return kErrorObjectDisposed;
  ArrayView a = view(vertices);
  if (stream < 0 || stream >= int32_t(vb->formats.size())) return kErrorOutOfRange;
  const uint32_t fmt = vb->formats[stream];
  const uint32_t raw = fmt ? kComponentBytes[(fmt >> 8) & 0xf] * ((fmt & 3) + 1) * (((fmt >> 4) & 3) + 1) : 16;
  if (count < 0) {
    count = vb->vertex_count;
    if (vertices && size_t(count) * raw != a.bytes()) return kErrorInvalidOperation;
  }
  if (!a.data) return kErrorArgumentNull;
  if ((count | offset | to | from) < 0) return kErrorOutOfRange;
  if (src_format != 0 && src_format != fmt) {
    host::note_unimplemented("libScePsm PsmVertexBuffer::SetVertices2 format conversion");
    return kOk;
  }
  if (fmt == 0) return kOk;
  if (stride == 0) stride = int32_t(raw);
  if (int64_t(count) + to > vb->vertex_count || stride < int32_t(raw)) return kErrorOutOfRange;
  if (int64_t(stride) * (from + int64_t(count) - 1) + offset + raw > int64_t(a.bytes())) return kErrorInvalidOperation;
  if (count == 0) return kOk;
  uint8_t* dst = vb->streams[stream].data() + size_t(raw) * to;
  const uint8_t* src = a.data + offset + size_t(stride) * from;
  for (int32_t i = 0; i < count; ++i, dst += raw, src += stride) std::memcpy(dst, src, raw);
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmVertexBuffer::SetVertices2", VertexBuffer_SetVertices2);

// SetIndices(handle, ushort[] indices, to, from, count) — 0x1ec990 (range checks
// follow SetVertices; the exact native checks are not traced yet).
int32_t VertexBuffer_SetIndices(int32_t h, MonoArray* indices, int32_t to, int32_t from, int32_t count) {
  VertexBuffer* vb = Objects::get().vertex_buffer(h);
  if (!vb) return kErrorObjectDisposed;
  ArrayView a = view(indices);
  if (!a.data) return kErrorArgumentNull;
  if (count < 0) count = vb->index_count;
  if ((count | to | from) < 0 || int64_t(count) + to > vb->index_count || int64_t(count) + from > int64_t(a.length))
    return kErrorOutOfRange;
  std::memcpy(vb->indices.data() + to, a.data + size_t(from) * 2, size_t(count) * 2);
  return kOk;
}
SSN_ICALL("Sce.PlayStation.Core.Graphics.PsmVertexBuffer::SetIndices", VertexBuffer_SetIndices);

}  // namespace
}  // namespace ssn::psm::gfx
