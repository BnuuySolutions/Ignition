#!/bin/bash

unset SteamGameId

# By default, Wine/Proton does not use hidraw.
# This is required for practically all HID devices like PSVR2 Sense controllers, WMR headsets, Rift, etc.
./proton run reg import ./wine_hidraw.reg

./proton run "$@"
