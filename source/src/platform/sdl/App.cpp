// platform/Platform.h on Linux: the client's services (OpenGL functions, fonts, alerts) through SDL2.
#include "platform/Platform.h"
#include <SDL.h>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>

namespace platform {
namespace {
std::string lower(std::string s) {
    for (auto &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

void *glProc(const char *name) { return SDL_GL_GetProcAddress(name); }

// Font files by name in the usual font folders (indexed once, recursively, case-insensitively).
std::string findFont(const std::vector<std::string> &fileNames) {
    static std::once_flag once;
    static std::map<std::string, std::string> index; // lower-case file name -> path
    std::call_once(once, [] {
        std::vector<std::filesystem::path> roots = {"/usr/share/fonts", "/usr/local/share/fonts"};
        if (const char *home = std::getenv("HOME")) {
            roots.push_back(std::filesystem::path(home) / ".fonts");
            roots.push_back(std::filesystem::path(home) / ".local/share/fonts");
        }
        for (const auto &root : roots) {
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec)) index.emplace(lower(it->path().filename().string()), it->path().string());
        }
    });
    for (const auto &name : fileNames)
        if (auto found = index.find(lower(name)); found != index.end()) return found->second;
    return {};
}

std::string clipboardText() {
    std::string out;
    if (char *text = SDL_GetClipboardText()) {
        out = text;
        SDL_free(text);
    }
    return out;
}

int showAlert(const std::string &title, const std::string &message, const std::vector<std::string> &labels) {
    std::vector<std::string> texts = labels;
    if (texts.empty()) texts.push_back("OK");
    std::vector<SDL_MessageBoxButtonData> buttons;
    for (std::size_t i = 0; i < texts.size(); ++i) buttons.push_back({i == 0 ? Uint32(SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) : 0u, int(i), texts[i].c_str()});
    const std::string heading = title.empty() ? "Clash Royale" : title;
    SDL_MessageBoxData data{};
    data.flags = SDL_MESSAGEBOX_INFORMATION;
    data.window = SDL_GL_GetCurrentWindow();
    data.title = heading.c_str();
    data.message = message.c_str();
    data.numbuttons = int(buttons.size());
    data.buttons = buttons.data();
    int pressed = 0;
    if (SDL_ShowMessageBox(&data, &pressed) != 0 || pressed < 0) pressed = 0;
    return pressed;
}
} // namespace platform
