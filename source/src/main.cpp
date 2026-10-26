#include "Cpu.h"
#include "Hle.h"
#include "Loader.h"
#include "Log.h"
#include "Memory.h"
#include "Runtime.h"
#include "libc/Files.h"
#include "patches/StateDump.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

// ClashRoyaleHLE <library> [res directory]
int main(int argc, char **argv) {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // no modal abort() dialog; the log has the cause
#endif
    namespace fs = std::filesystem;
    const fs::path exeDir = fs::path(argv[0]).parent_path();
    std::string library = argc > 1 ? argv[1] : "";
    std::string resources = argc > 2 ? argv[2] : "";
    if (library.empty()) {
        // Default: the project's inputs/original copy.
        for (fs::path dir = fs::absolute(exeDir); !dir.empty() && dir != dir.root_path(); dir = dir.parent_path())
            if (fs::exists(dir / "inputs/original/library")) {
                library = (dir / "inputs/original/library").string();
                resources = (dir / "inputs/original/res").string();
                break;
            }
    }
    if (library.empty()) fatal("usage: ClashRoyaleHLE <library> [res]");
    if (resources.empty()) resources = (fs::path(library).parent_path() / "res").string();
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(); };

    mem::init();
    cpu::initProcess();
    hle::createSpecialStubs();
    // The .app bundle holds res/ (and the executable), so it maps to the folder containing res.
    runtime::setBundlePath(fs::path(resources).parent_path().string());
    // HLE_SANDBOX: another app container (a second device: its own keychain account and settings).
    const char *sandbox = std::getenv("HLE_SANDBOX");
    files::setSandboxRoot(sandbox ? std::string(sandbox) : (fs::absolute(exeDir) / "sandbox").string());
    loader::load(library);
    statedump::installClientHook();
    hlog("startup", "Mach-O loading and binding elapsed_ms=%lld", elapsed());

    cpu::attachThread(mem::allocStack(8 * 1024 * 1024));
    runtime::initialize(); // Objective-C class registration, +load, C++ static initializers
    hlog("startup", "initializers done elapsed_ms=%lld", elapsed());

    // main(argc, argv, envp, apple)
    const auto exePath = mem::heapString("/var/containers/Bundle/Application/ClashRoyale.app/Clash Royale");
    const auto argvBlock = mem::calloc(32);
    mem::w64(argvBlock, exePath);
    const auto envp = mem::calloc(16);
    const auto apple = mem::calloc(16);
    mem::w64(apple, mem::heapString("executable_path=/var/containers/Bundle/Application/ClashRoyale.app/Clash Royale"));
    const auto result = cpu::runThread(loader::entryPoint(), {1, argvBlock, envp, apple});
    hlog("startup", "main returned %lld", (long long)result);
    return int(result);
}
