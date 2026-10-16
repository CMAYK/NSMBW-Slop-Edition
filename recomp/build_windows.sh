#!/bin/sh
# Build nsmbw.exe and deploy it to ../game. Same as build.py (which also runs on Windows:
# double-click build_windows.bat there). Needs: python3 -m pip install ziglang
cd "$(dirname "$0")" && exec python3 build.py "$@"
