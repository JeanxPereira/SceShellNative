// Compositor canvases (libSceComposite): the surfaces another process renders into and the
// shell samples by canvas handle (libScePsm CanvasTexture2D, pixel-buffer type 3).
//
// On the console the BE core process (becore.elf) owns a video source's canvas and the
// compositor hands libScePsm the texture descriptor of its current frame
// (libScePsm 0x4100 "SetNativeTextureDescriptor", patched in 0x8ac0). The host has no
// compositor process: this table is the host side of that boundary (HOST VALUE). A producer
// creates a canvas, publishes complete RGBA8 frames into it, and the renderer pulls the
// newest frame when a texture bound to the canvas is drawn.
#pragma once
#include <cstdint>
#include <vector>

namespace ssn::composite {

// A new canvas handle (never 0 nor 0xFFFFFFFF, the CANVAS_HANDLE_INVALID of the managed side).
uint32_t canvas_create();
void canvas_destroy(uint32_t handle);
bool canvas_exists(uint32_t handle);
// Replaces the canvas content with one RGBA8 frame (rows of w*4 bytes, top row first).
void canvas_publish(uint32_t handle, int32_t w, int32_t h, std::vector<uint8_t>&& rgba);
// Copies the newest frame when it is newer than `generation`; returns false (and leaves the
// outputs) when the canvas is unknown or nothing new was published.
bool canvas_take(uint32_t handle, uint64_t& generation, int32_t& w, int32_t& h, std::vector<uint8_t>& rgba);

}  // namespace ssn::composite
