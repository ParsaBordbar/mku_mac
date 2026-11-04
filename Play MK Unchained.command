#!/bin/sh
# Double-click on a Mac: asks for your ISO the first time, then just starts the game.
cd "$(dirname "$0")" && exec ./mku-play "$@"
