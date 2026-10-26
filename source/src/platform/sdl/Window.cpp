// platform/Window.h on Linux: an SDL2 window with a desktop OpenGL (compatibility) context, and SDL input. The mouse is
// one finger; typed text arrives as SDL text input, with Return / Backspace / Escape from key presses (as WM_CHAR
// delivers them on Windows).
#include "platform/Window.h"
#include "platform/GL.h"
#include "platform/Platform.h"
#include "Log.h"
#include <SDL.h>
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace platform {
namespace {
SDL_Window *g_window = nullptr;
SDL_GLContext g_context = nullptr;
std::mutex g_shareLock;
std::vector<SDL_GLContext> g_sharedPool;
// Handlers are function-local statics: UIKit.cpp installs the touch handler from a static initializer, and a
// namespace-scope std::function here could be constructed after that (cross-file initialization order).
std::function<void(int, double, double)> &touchHandler() { static std::function<void(int, double, double)> f; return f; }
std::function<void(std::uint32_t, int)> &keyHandler() { static std::function<void(std::uint32_t, int)> f; return f; }
bool g_mouseDown = false;

// Window coordinates -> points (window size, not the drawable: they differ on high-DPI displays).
void toPoints(int wx, int wy, double &x, double &y) {
    int w = 1, h = 1;
    SDL_GetWindowSize(g_window, &w, &h);
    x = double(wx) * kPointsWidth / double(std::max(1, w));
    y = double(wy) * kPointsHeight / double(std::max(1, h));
}
// UTF-8 text from SDL_TEXTINPUT -> code points.
void typeText(const char *text) {
    const auto *p = reinterpret_cast<const unsigned char *>(text);
    while (*p) {
        std::uint32_t c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3Fu >> extra;
        for (; extra && (*p & 0xC0) == 0x80; --extra) c = (c << 6) | (*p++ & 0x3F);
        if (keyHandler()) keyHandler()(c, 0);
    }
}
// The run loop's wait: returns early on input without taking it off the queue.
void waitForInput(int ms) { SDL_WaitEventTimeout(nullptr, ms); }
} // namespace

void createWindow() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) fatal("SDL could not start its video subsystem: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    // Fit the 750x1334 screen into ~85% of the desktop height (and 90% of its width).
    SDL_Rect usable{0, 0, kPixelsWidth, kPixelsHeight};
    SDL_GetDisplayUsableBounds(0, &usable);
    const double fit = std::min({1.0, usable.h * 0.85 / kPixelsHeight, usable.w * 0.9 / kPixelsWidth});
    const int w = int(kPixelsWidth * fit), h = int(kPixelsHeight * fit);
    g_window = SDL_CreateWindow("Clash Royale (HLE)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!g_window) fatal("SDL could not create the window: %s", SDL_GetError());
    g_context = SDL_GL_CreateContext(g_window);
    if (!g_context) fatal("SDL could not create an OpenGL context: %s", SDL_GetError());
    // Contexts for other threads, sharing the main one's objects: made up front (each is created sharing with the
    // current context, so the main one is made current before each).
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    for (int i = 0; i < 8; ++i) {
        SDL_GL_MakeCurrent(g_window, g_context);
        if (SDL_GLContext shared = SDL_GL_CreateContext(g_window)) g_sharedPool.push_back(shared);
    }
    SDL_GL_MakeCurrent(g_window, g_context);
    SDL_GL_SetSwapInterval(1);
    SDL_StartTextInput();
    setEventWaiter(waitForInput);
    hlog("window", "%dx%d window (%s), GL %s on %s", w, h, SDL_GetCurrentVideoDriver(), reinterpret_cast<const char *>(glGetString(GL_VERSION)),
         reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
}
void makeContextCurrent() { SDL_GL_MakeCurrent(g_window, g_context); }
void makeSharedContextCurrent() {
    SDL_GLContext shared = nullptr;
    {
        std::lock_guard lock(g_shareLock);
        if (!g_sharedPool.empty()) { shared = g_sharedPool.back(); g_sharedPool.pop_back(); }
    }
    if (!shared) fatal("no shared GL context left for another thread");
    SDL_GL_MakeCurrent(g_window, shared);
}
void swapBuffers() { SDL_GL_SwapWindow(g_window); }
void clientSize(int &width, int &height) { SDL_GL_GetDrawableSize(g_window, &width, &height); }
void pumpEvents() {
    SDL_Event e;
    double x, y;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button != SDL_BUTTON_LEFT || g_mouseDown) break;
            g_mouseDown = true;
            SDL_CaptureMouse(SDL_TRUE);
            toPoints(e.button.x, e.button.y, x, y);
            if (touchHandler()) touchHandler()(0, x, y);
            break;
        case SDL_MOUSEMOTION:
            if (g_mouseDown) { toPoints(e.motion.x, e.motion.y, x, y); if (touchHandler()) touchHandler()(1, x, y); }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT && g_mouseDown) {
                SDL_CaptureMouse(SDL_FALSE);
                g_mouseDown = false;
                toPoints(e.button.x, e.button.y, x, y);
                if (touchHandler()) touchHandler()(3, x, y);
            }
            break;
        case SDL_KEYDOWN:
            if (keyHandler()) {
                if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) keyHandler()(13, 0);
                else if (e.key.keysym.sym == SDLK_BACKSPACE) keyHandler()(8, 0);
                else if (e.key.keysym.sym == SDLK_ESCAPE) keyHandler()(27, 0);
                else if (e.key.keysym.sym == SDLK_v && (e.key.keysym.mod & KMOD_CTRL)) keyHandler()(22, 0); // paste
            }
            break;
        case SDL_TEXTINPUT:
            typeText(e.text.text);
            break;
        case SDL_QUIT:
            hlog("window", "closed");
            std::_Exit(0);
        }
    }
}
void setTouchHandler(std::function<void(int, double, double)> handler) { touchHandler() = std::move(handler); }
void setKeyHandler(std::function<void(std::uint32_t, int)> handler) { keyHandler() = std::move(handler); }
void queueTypedText(const std::string &text) {
    for (char c : text) {
        SDL_Event e{};
        if (c == '|' || c == '<') {
            e.type = SDL_KEYDOWN;
            e.key.keysym.sym = c == '|' ? SDLK_RETURN : SDLK_BACKSPACE;
        } else {
            e.type = SDL_TEXTINPUT;
            e.text.text[0] = c;
        }
        SDL_PushEvent(&e);
    }
}
} // namespace platform
