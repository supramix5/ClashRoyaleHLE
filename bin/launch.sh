#!/usr/bin/env bash
# launch.sh <save folder>: starts the client with that save folder (its account login, settings and downloaded
# content updates). The server address comes from server.txt (host:port); the log of the last run goes to log.txt in
# the save folder.
cd "$(dirname "${BASH_SOURCE[0]}")/.."

deps="${XDG_DATA_HOME:-$HOME/.local/share}/clashroyale-hle/deps/root/usr/lib/x86_64-linux-gnu"
if [ -d "$deps" ]; then
    export LD_LIBRARY_PATH="$(find "$deps" -type d | tr '\n' ':')${LD_LIBRARY_PATH:-}"
fi
if [ ! -x "bin/ClashRoyaleHLE" ]; then
    echo "bin/ClashRoyaleHLE is missing: run ./build_client.sh first."
    exit 1
fi
missing="$(ldd "bin/ClashRoyaleHLE" 2>&1 | grep -E 'not found' || true)"
if [ -n "$missing" ]; then
    echo "bin/ClashRoyaleHLE was built on a newer Linux than this one and cannot start here:"
    echo "$missing"
    echo "Rebuild it for this computer with ./build_client.sh"
    exit 1
fi

save="$(mkdir -p "$1" && cd "$1" && pwd)"
server=127.0.0.1:9339
[ -f server.txt ] && server="$(head -n 1 server.txt | tr -d '\r[:space:]')"
echo "Clash Royale - $(basename "$save") ($server); log: $save/log.txt"
HLE_SANDBOX="$save" HLE_SERVER="$server" HLE_QUIET="${HLE_QUIET:-bridge,data,file,keychain,msg,touch}" \
    ./bin/ClashRoyaleHLE "$PWD/game/library" > "$save/log.txt" 2>&1
