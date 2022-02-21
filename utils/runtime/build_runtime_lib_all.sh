#!/usr/bin/env bash

set -e
SCRIPTDIR=$(dirname "$0")

echo "SCRIPTDIR" $SCRIPTDIR

SCRIPT=${SCRIPTDIR}/build_runtime_lib.sh

${SCRIPT} x86_64-osx
${SCRIPT} x86_64-linux
${SCRIPT} x86_64-win32
${SCRIPT} x86-win32
${SCRIPT} js-web
${SCRIPT} wasm-web
${SCRIPT} armv7-android
${SCRIPT} arm64-android
${SCRIPT} arm64-ios
#${SCRIPT} x86_64-ios
#${SCRIPT} arm64-nx64
