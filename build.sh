#!/bin/sh
# Requirements: brew install jq tree
set -e
BUILD_SERVER="https://build-stage.defold.com"
CHANNEL="alpha"
VERSION_FILENAME="info.json"
DEFOLD_VERSION=`curl -s http://d.defold.com/$CHANNEL/$VERSION_FILENAME | jq -r '.sha1'`
echo "Default version: $DEFOLD_VERSION"
echo "Downloading BOB"
rm -f bob.jar
wget -q http://d.defold.com/archive/$CHANNEL/$DEFOLD_VERSION/bob/bob.jar
java -jar bob.jar --version
java -jar bob.jar resolve --email a@b.com --auth 123456
SERVER=$BUILD_SERVER DEFOLDSDK=$DEFOLD_VERSION BOB=./bob.jar ./utils/build_plugins.sh arm64-macos


