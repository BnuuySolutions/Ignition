#!/bin/bash

# Get the absolute path to the directory containing this driver
DRIVER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd ../.. && pwd)"

# Common paths for vrpathreg on Linux
POSSIBLE_PATHS=(
    "$HOME/.steam/steam/steamapps/common/SteamVR/bin/vrpathreg.sh"
    "$HOME/.local/share/Steam/steamapps/common/SteamVR/bin/vrpathreg.sh"
    "$HOME/.steam/root/steamapps/common/SteamVR/bin/vrpathreg.sh"
    "$HOME/.var/app/com.valvesoftware.Steam/.steam/steam/steamapps/common/SteamVR/bin/vrpathreg.sh"
    "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/SteamVR/bin/vrpathreg.sh"
    "$HOME/.var/app/com.valvesoftware.Steam/.steam/root/steamapps/common/SteamVR/bin/vrpathreg.sh"
)

VRPATHREG=""

# Find the executable
for path in "${POSSIBLE_PATHS[@]}"; do
    if [ -f "$path" ]; then
        VRPATHREG="$path"
        break
    fi
done

if [ -z "$VRPATHREG" ]; then
    echo "Error: Could not find 'vrpathreg.sh'. Please ensure SteamVR is installed."
    exit 1
fi

echo "Unregistering driver at: $DRIVER_DIR"
"$VRPATHREG" removedriver "$DRIVER_DIR"

if [ $? -eq 0 ]; then
    echo "Success!"
else
    echo "Failed to unregister driver."
fi
