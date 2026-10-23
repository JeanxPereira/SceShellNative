#include "platform/window.h"

namespace ssn::platform {

Window& main_window() {
  static Window w;
  return w;
}

bool Window::create(const wchar_t* title, int client_width, int client_height) {
  instance_ = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &Window::proc;
  wc.hInstance = instance_;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"SceShellNative";
  RegisterClassExW(&wc);

  // Fit the requested client size on the primary monitor's work area, keeping aspect.
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const DWORD style = WS_OVERLAPPEDWINDOW;
  int w = client_width, h = client_height;
  for (;;) {
    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, style, FALSE);
    if ((r.right - r.left) <= (work.right - work.left) && (r.bottom - r.top) <= (work.bottom - work.top)) {
      hwnd_ = CreateWindowExW(0, wc.lpszClassName, title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                              r.bottom - r.top, nullptr, nullptr, instance_, this);
      break;
    }
    w = w * 3 / 4;
    h = h * 3 / 4;
  }
  if (!hwnd_) return false;
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ShowWindow(hwnd_, SW_SHOW);
  return true;
}

bool Window::pump() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return !closed_;
}

void Window::client_size(int& w, int& h) const {
  RECT r{};
  GetClientRect(hwnd_, &r);
  w = r.right - r.left;
  h = r.bottom - r.top;
}

void Window::step_aside() {
  if (hwnd_) ShowWindowAsync(hwnd_, SW_MINIMIZE);
}

void Window::come_to_front() {
  if (!hwnd_) return;
  ShowWindowAsync(hwnd_, SW_RESTORE);
  // Windows hands the foreground only to the process the user last worked in (here, the
  // application that just ended, or what it left in front). Joining the input of the
  // foreground window's thread lifts that lock for this call.
  const DWORD self = GetCurrentThreadId();
  HWND fg = GetForegroundWindow();
  const DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
  const bool joined = fg_thread && fg_thread != self && AttachThreadInput(self, fg_thread, TRUE);
  BringWindowToTop(hwnd_);
  SetForegroundWindow(hwnd_);
  if (joined) AttachThreadInput(self, fg_thread, FALSE);
}

LRESULT CALLBACK Window::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_CLOSE:
      if (self) self->closed_ = true;
      return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
      if (self && self->on_key_scan)
        self->on_key_scan(static_cast<uint32_t>(wp), static_cast<uint32_t>((lp >> 16) & 0xFF), ((lp >> 24) & 1) != 0,
                          msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
      else if (self && self->on_key)
        self->on_key(static_cast<uint32_t>(wp), msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace ssn::platform
