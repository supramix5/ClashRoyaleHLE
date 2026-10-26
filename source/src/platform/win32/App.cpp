// platform/Platform.h on Windows: the client's services (OpenGL functions, fonts, alerts).
#include "platform/Platform.h"
#include <windows.h>
#include <commctrl.h>
#include <filesystem>
#include <string>
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace platform {
namespace {
std::wstring wide(const std::string &utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
    return out;
}
} // namespace

void *glProc(const char *name) {
    void *p = reinterpret_cast<void *>(wglGetProcAddress(name));
    // wglGetProcAddress only knows extensions; OpenGL 1.1 functions come from opengl32.dll (it may also answer 1-3, -1).
    if (!p || p == reinterpret_cast<void *>(1) || p == reinterpret_cast<void *>(2) || p == reinterpret_cast<void *>(3) ||
        p == reinterpret_cast<void *>(-1)) {
        static HMODULE gl = LoadLibraryA("opengl32.dll");
        p = reinterpret_cast<void *>(GetProcAddress(gl, name));
    }
    return p;
}

std::string findFont(const std::vector<std::string> &fileNames) {
    char windows[MAX_PATH];
    GetWindowsDirectoryA(windows, MAX_PATH);
    const auto fonts = std::filesystem::path(windows) / "Fonts";
    for (const auto &name : fileNames) {
        std::error_code ec;
        if (std::filesystem::exists(fonts / name, ec)) return (fonts / name).string();
    }
    return {};
}

std::string clipboardText() {
    std::string out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto *w = static_cast<const wchar_t *>(GlobalLock(h))) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                out.resize(std::size_t(n - 1));
                WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

// A task dialog (the title as its heading, the message, one button per label); a plain message box if task dialogs
// are unavailable.
int showAlert(const std::string &title, const std::string &message, const std::vector<std::string> &labels) {
    std::vector<std::wstring> texts;
    for (const auto &l : labels) texts.push_back(wide(l));
    if (texts.empty()) texts.push_back(L"OK");
    std::vector<TASKDIALOG_BUTTON> buttons;
    for (std::size_t i = 0; i < texts.size(); ++i) buttons.push_back({int(100 + i), texts[i].c_str()});
    const auto heading = wide(title), body = wide(message);
    TASKDIALOGCONFIG config{sizeof config};
    config.hwndParent = GetActiveWindow();
    config.pszWindowTitle = L"Clash Royale";
    config.pszMainInstruction = heading.c_str();
    config.pszContent = body.c_str();
    config.cButtons = UINT(buttons.size());
    config.pButtons = buttons.data();
    config.nDefaultButton = 100;
    int pressed = 100;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) {
        MessageBoxW(config.hwndParent, body.c_str(), heading.c_str(), MB_OK);
        pressed = 100;
    }
    return pressed - 100;
}
} // namespace platform
