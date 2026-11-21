#!/bin/bash

unset SteamGameId

# We don't want any case of any HID device being ignored by Proton.
unset SDL_GAMECONTROLLER_IGNORE_DEVICES
unset SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT

# By default, Wine/Proton does not use hidraw.
# This is required for practically all HID devices like PSVR2 Sense controllers, WMR headsets, Rift, etc.
./proton run reg import ./wine_hidraw.reg

./proton run "$@"
