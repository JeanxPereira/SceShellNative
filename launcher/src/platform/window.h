// The launcher's single top-level window (plain Win32: one window and keyboard input
// are all the shell needs, so no windowing library is pulled in).
#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>

namespace ssn::platform {

class Window {
 public:
  bool create(const wchar_t* title, int client_width, int client_height);
  // Processes pending window messages; returns false once the window was closed.
  bool pump();
  HWND hwnd() const { return hwnd_; }
  HINSTANCE instance() const { return instance_; }
  bool closed() const { return closed_; }
  void client_size(int& w, int& h) const;
  // While an application started by the shell runs (HOST POLICY, docs/host-values.md
  // "Application window"): the window minimizes for the application's own window, and comes
  // back to the front when it ends. Callable from any thread; no-ops without a window.
  void step_aside();
  void come_to_front();

  std::function<void(uint32_t vk, bool down)> on_key;
  // When set, receives key messages instead of on_key, with the set-1 scan code and the
  // extended-key flag of lParam (bits 16..23, 24).
  std::function<void(uint32_t vk, uint32_t scan, bool extended, bool down)> on_key_scan;

 private:
  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
  HWND hwnd_ = nullptr;
  HINSTANCE instance_ = nullptr;
  bool closed_ = false;
};

Window& main_window();

}  // namespace ssn::platform
