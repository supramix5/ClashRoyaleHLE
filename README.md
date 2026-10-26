# Clash Royale v1.0.0 Client
*Disclaimer: AI was used to create this repo. Mostly the reason why I'm making this open source*

The original Clash Royale v1.0.0 iOS game, running on Windows and Linux through High Level Emulation via Dynarmic
Native iOS API calls are translated to Windows/Linux equivelants at runtime. Runs functionally 1 to 1 with the 
original iOS client (please report any bugs you find in the issues tab).

> This is an unofficial, non-commercial preservation project, not affiliated with or endorsed by Supercell.

## Quick start

1. Start a server (May it be by Clash Royale v1.0.0 Server repo, or a different server)
2. Run the client:
   - **Windows:** `Run-Client.bat`
   - **Linux:** `./run_client.sh`

The client connects to the address in `server.txt` (default `127.0.0.1:9339`, the local computer). For a server on another
computer, put its address there, for example `192.168.1.20:9339`.

Click to touch. When a text field is active, type on the keyboard: Enter sends, Esc dismisses, Ctrl+V pastes.

## Players

Each player has a save folder in `players/`, holding its account login and settings.

| Windows | Linux | What it does |
| --- | --- | --- |
| `Run-Client.bat` | `./run_client.sh` | Plays the default player (`players/Default`) |
| `New-Player.bat` | `./new_player.sh` | Creates a saved player: asks for a name, makes `players/<name>` and a `Play-<name>` launcher |
| `Play-<name>.bat` | `./play_<name>.sh` | Plays that saved player |
| `Run-Fresh-0-0-Client.bat` | `./run_fresh_0_0_client.sh` | Plays a brand-new account every time; nothing is saved |

Deleting a save folder loses access to its account (the account stays on the server)

## Linux

The included `bin/ClashRoyaleHLE` was built on WSL Ubuntu 26.04 and needs the SDL2, OpenGL and FFmpeg 8 (libavformat 62)
libraries. `run_client.sh` tells you when it cannot start on your system; then build it for your computer:

```bash
./build_client.sh
```

This needs GCC 13 or newer, CMake, pkg-config and the SDL2, OpenGL and FFmpeg development packages. When something is
missing, it prints the install command for apt, dnf, pacman and zypper. On Debian or Ubuntu without root, it can
unpack the packages into your home folder instead: `./build_client.sh --local-deps`.

The log of the last run is `log.txt` in the save folder (`players/<name>/log.txt`).

## Building from source

| Windows | Linux | What it does |
| --- | --- | --- |
| `Build-Client.bat` | `./build_client.sh` | Recompiles the client from `source/` into `bin/` |
| `Build-Client-Linux.bat` | | Runs `build_client.sh` in WSL, from Windows |

Building on Windows needs Visual Studio 2019 or 2022 (or the Build Tools) with *Desktop development with C++*.
Extract the package to a short folder first: the Visual Studio build fails inside
deeply nested folders because of the Windows path length limit.

## Contents

| Folder | Contents |
| --- | --- |
| `bin/` | `ClashRoyaleHLE.exe` (Windows), `ClashRoyaleHLE` (Linux), and the `launch.bat` / `launch.sh` scripts the launchers use |
| `game/` | The original game: `library` (the iOS executable, called "Clash Royale" inside IPA) and `res/` (game assets) |
| `players/` | Player saves |
| `source/` | The client's source code: `src/` (the emulator: ARM64 CPU through dynarmic, Mach-O loader, Objective-C runtime, libc, Foundation, UIKit, OpenGL ES, audio; `src/platform/win32` and `src/platform/sdl` are the Windows and Linux parts), `external/` (dynarmic, Boost headers, stb) and `CMakeLists.txt` |
| `server.txt` | The server address (`host:port`) |
