#include "platform/Window.h"
#include "Log.h"
#include <windows.h>
#include <windowsx.h>
#include "platform/GL.h"
#include <algorithm>
#include <mutex>

namespace platform {
namespace {
HWND g_window = nullptr;
HDC g_dc = nullptr;
HGLRC g_context = nullptr;
std::mutex g_shareLock;
// Handlers are function-local statics: UIKit.cpp installs the touch handler from a static initializer, and a
// namespace-scope std::function here could be constructed after that (cross-file initialization order).
std::function<void(int, double, double)> &touchHandler() { static std::function<void(int, double, double)> f; return f; }
std::function<void(std::uint32_t, int)> &keyHandler() { static std::function<void(std::uint32_t, int)> f; return f; }
bool g_mouseDown = false;

void toPoints(LPARAM lp, double &x, double &y) {
    RECT r;
    GetClientRect(g_window, &r);
    x = double(GET_X_LPARAM(lp)) * kPointsWidth / double(std::max<LONG>(1, r.right));
    y = double(GET_Y_LPARAM(lp)) * kPointsHeight / double(std::max<LONG>(1, r.bottom));
}
LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    double x, y;
    switch (msg) {
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        g_mouseDown = true;
        toPoints(lp, x, y);
        if (touchHandler()) touchHandler()(0, x, y);
        return 0;
    case WM_MOUSEMOVE:
        if (g_mouseDown) { toPoints(lp, x, y); if (touchHandler()) touchHandler()(1, x, y); }
        return 0;
    case WM_LBUTTONUP:
        if (g_mouseDown) {
            ReleaseCapture();
            g_mouseDown = false;
            toPoints(lp, x, y);
            if (touchHandler()) touchHandler()(3, x, y);
        }
        return 0;
    case WM_CHAR:
        if (keyHandler()) keyHandler()(std::uint32_t(wp), 0);
        return 0;
    case WM_CLOSE:
        hlog("window", "closed");
        ExitProcess(0);
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

void createWindow() {
    SetProcessDPIAware();
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ClashRoyaleHLE";
    RegisterClassW(&wc);
    // Fit the 750x1334 screen into ~85% of the desktop height.
    const int desktopHeight = GetSystemMetrics(SM_CYSCREEN);
    double fit = std::min(1.0, desktopHeight * 0.85 / kPixelsHeight);
    const int w = int(kPixelsWidth * fit), h = int(kPixelsHeight * fit);
    RECT r{0, 0, w, h};
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_window = CreateWindowW(L"ClashRoyaleHLE", L"Clash Royale (HLE)", style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    g_dc = GetDC(g_window);
    PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    SetPixelFormat(g_dc, ChoosePixelFormat(g_dc, &pfd), &pfd);
    g_context = wglCreateContext(g_dc);
    wglMakeCurrent(g_dc, g_context);
    hlog("window", "%dx%d window, GL %s on %s", w, h, reinterpret_cast<const char *>(glGetString(GL_VERSION)),
         reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    // VSync on (wglSwapIntervalEXT).
    using SwapInterval = BOOL(WINAPI *)(int);
    if (auto swap = reinterpret_cast<SwapInterval>(wglGetProcAddress("wglSwapIntervalEXT"))) swap(1);
}
void makeContextCurrent() { wglMakeCurrent(g_dc, g_context); }
void makeSharedContextCurrent() {
    std::lock_guard lock(g_shareLock);
    HGLRC shared = wglCreateContext(g_dc);
    wglShareLists(g_context, shared);
    wglMakeCurrent(g_dc, shared);
}
void swapBuffers() { SwapBuffers(g_dc); }
void clientSize(int &width, int &height) {
    RECT r;
    GetClientRect(g_window, &r);
    width = r.right;
    height = r.bottom;
}
void pumpEvents() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
void setTouchHandler(std::function<void(int, double, double)> handler) { touchHandler() = std::move(handler); }
void setKeyHandler(std::function<void(std::uint32_t, int)> handler) { keyHandler() = std::move(handler); }
void queueTypedText(const std::string &text) {
    for (char c : text) {
        const WPARAM ch = c == '|' ? 13 : c == '<' ? 8 : WPARAM(static_cast<unsigned char>(c));
        PostMessageW(g_window, WM_CHAR, ch, 0);
    }
}
} // namespace platform
