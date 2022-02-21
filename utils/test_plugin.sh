#!/usr/bin/env bash

set -e

CLASS=com.dynamo.bob.pipeline.Spine
JAR=./defold-spine/plugins/share/pluginSpineExt.jar
BOB=~/work/defold/tmp/dynamo_home/share/java/bob.jar

java -cp $JAR:$BOB:./defold-spine/plugins/lib/x86_64-osx $CLASS $*
