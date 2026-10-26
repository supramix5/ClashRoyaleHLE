#pragma once
// The emulated device screen: an iPhone 6 (iPhone7,2), 375x667 points at scale 2 (750x1334 pixels), shown in a
// window scaled to fit (Win32 on Windows, SDL2 on Linux), with a desktop OpenGL (compatibility profile) context for the app's OpenGL ES calls.
#include <cstdint>
#include <functional>
#include <string>

namespace platform {
constexpr int kPointsWidth = 375, kPointsHeight = 667;
constexpr double kScale = 2.0;
constexpr int kPixelsWidth = 750, kPixelsHeight = 1334;

void createWindow();
void makeContextCurrent();       // the main GL context on the calling thread
void makeSharedContextCurrent(); // a context sharing objects with the main one, for another thread
void swapBuffers();
void clientSize(int &width, int &height);
void pumpEvents();
// Touch input in points (mouse: one finger). phase: 0 began, 1 moved, 3 ended, 4 cancelled.
void setTouchHandler(std::function<void(int phase, double x, double y)> handler);
// Text keys while a keyboard is shown (UITextField): characters and backspace/return.
void setKeyHandler(std::function<void(std::uint32_t character, int key)> handler);
// Scripted tests: queues typed text as window input (Windows WM_CHAR messages, Linux SDL key / text events), so it
// goes through the window's own event handling like real typing. '|' is Return, '<' Backspace.
void queueTypedText(const std::string &text);
} // namespace platform
