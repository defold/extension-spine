#!/bin/sh
set -e
export BOB=bob.jar && ./utils/build_libs.sh
export BOB=bob.jar && ./utils/build_plugins.sh


