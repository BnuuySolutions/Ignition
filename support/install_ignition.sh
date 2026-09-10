#!/bin/bash

# Script to install Ignition for a Windows SteamVR driver on Linux.

set -e

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 /path/to/your/steamvr/driver"
    exit 1
fi

DRIVER_DIR="$1"
DRIVER_MANIFEST="$DRIVER_DIR/driver.vrdrivermanifest"

if [ ! -f "$DRIVER_MANIFEST" ]; then
    echo "Error: driver.vrdrivermanifest not found in $DRIVER_DIR"
    exit 1
fi

echo "Found driver manifest at $DRIVER_MANIFEST"

# Check for jq
if ! command -v jq &> /dev/null
then
    echo "Error: jq is not installed. Please install it to continue."
    echo "On Debian/Ubuntu: sudo apt-get install jq"
    echo "On Arch Linux: sudo pacman -S jq"
    echo "On Fedora: sudo dnf install jq"
    exit 1
fi

# Get the directory where this script is located
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

echo "Reading driver name from manifest..."
DRIVER_NAME=$(jq -r '.name' "$DRIVER_MANIFEST")

if [ -z "$DRIVER_NAME" ]; then
    echo "Error: Could not read driver 'name' from $DRIVER_MANIFEST"
    exit 1
fi

echo "Driver name: $DRIVER_NAME"

LINUX_BIN_DIR="$DRIVER_DIR/bin/linux64"

echo "Creating directory: $LINUX_BIN_DIR"
mkdir -p "$LINUX_BIN_DIR"

IGNITION_CONFIG_PATH="$LINUX_BIN_DIR/ignition.json"
IGNITION_DRIVER_SO="$SCRIPT_DIR/libdriver_ignition.so"
TARGET_DRIVER_SO="$LINUX_BIN_DIR/driver_$DRIVER_NAME.so"

if [ ! -f "$IGNITION_DRIVER_SO" ]; then
    echo "Error: Ignition driver not found at $IGNITION_DRIVER_SO"
    echo "Please run this script from the Ignition installation directory."
    exit 1
fi

echo "Creating ignition.json at $IGNITION_CONFIG_PATH"

cat > "$IGNITION_CONFIG_PATH" << EOL
{
    "server_exe": "${SCRIPT_DIR}/ignition_server.exe",
    "driver_dll": "../win64/driver_${DRIVER_NAME}.dll",
    "wine_cmd": [
        "./launch_serverhelper.sh"
    ],
    "wait_for_debugger": false
}
EOL

echo "Copying support files..."
cp "$SCRIPT_DIR/proton" "$LINUX_BIN_DIR/"
cp "$SCRIPT_DIR/launch_serverhelper.sh" "$LINUX_BIN_DIR/"
cp "$SCRIPT_DIR/driver_install.sh" "$LINUX_BIN_DIR/"
cp "$SCRIPT_DIR/driver_uninstall.sh" "$LINUX_BIN_DIR/"
cp "$SCRIPT_DIR/wine_hidraw.reg" "$LINUX_BIN_DIR/"


echo "Creating symbolic link for the driver..."

# Remove existing link if it exists
rm -f "$TARGET_DRIVER_SO"
ln -s "$IGNITION_DRIVER_SO" "$TARGET_DRIVER_SO"

echo "Installation complete!"
echo
echo "You can add the driver to SteamVR by running driver_install.sh in $LINUX_BIN_DIR"