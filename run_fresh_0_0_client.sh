#!/usr/bin/env bash
# Starts a client with no account (account id 0,0): the server creates a brand-new player every time.
# Nothing is kept on this computer: the temporary save folder is deleted when the client closes.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fresh="$(mktemp -d "${TMPDIR:-/tmp}/ClashRoyaleHLE-0-0-XXXXXX")"
"$here/bin/launch.sh" "$fresh"
rm -rf "$fresh"
