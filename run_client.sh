#!/usr/bin/env bash
# Plays the default player save (players/Default). It is kept between runs.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$here/bin/launch.sh" "$here/players/Default"
