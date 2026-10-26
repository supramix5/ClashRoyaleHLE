#!/usr/bin/env bash
# Creates a new saved player: an empty save folder in players/<name> (the first login is account 0,0, so the server
# creates a new account and the save keeps it) and a play_<name>.sh that always plays that save.
cd "$(dirname "${BASH_SOURCE[0]}")"
read -r -p "Name for the new player save: " name
safe="$(printf '%s' "$name" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e 's/[^A-Za-z0-9_-]/_/g')"
[ -n "$safe" ] || exit 0
if [ -e "players/$safe" ]; then
    echo "A player save named \"$safe\" already exists. Use ./play_$safe.sh to play it."
    exit 1
fi
mkdir -p "players/$safe"
cat > "play_$safe.sh" <<EOF
#!/usr/bin/env bash
# Plays the saved player "$safe" (save folder players/$safe).
here="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
exec "\$here/bin/launch.sh" "\$here/players/$safe"
EOF
chmod +x "play_$safe.sh"
echo "Created players/$safe and play_$safe.sh"
read -r -p "Start $safe now? [Y/n] " answer
case "$answer" in [nN]*) exit 0 ;; esac
exec "./play_$safe.sh"
