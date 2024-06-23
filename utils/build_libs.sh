#! /usr/bin/env bash

set -e

LIBRARY_NAME=spinec
PROJECT_DIR=defold-spine
PROJECT_LIB_DIR=${PROJECT_DIR}/lib

PLATFORMS=$1
if [ "" == "${PLATFORMS}" ]; then
    PLATFORMS="x86_64-macos arm64-macos x86_64-linux x86_64-win32 x86-win32 arm64-ios x86_64-ios arm64-android armv7-android js-web wasm-web"
fi

DEFAULT_SERVER_NAME=build.defold.com
if [ "" == "${DM_EXTENDER_USERNAME}" ] && [ "" == "${DM_EXTENDER_PASSWORD}" ]; then
    DEFAULT_SERVER=https://${DEFAULT_SERVER_NAME}
else
    DEFAULT_SERVER=https://${DM_EXTENDER_USERNAME}:${DM_EXTENDER_PASSWORD}@${DEFAULT_SERVER_NAME}
fi

if [ "" == "${BOB}" ]; then
    BOB=${DYNAMO_HOME}/share/java/bob.jar
fi

echo "Using BOB=${BOB}"

if [ "" == "${DEFOLDSDK}" ]; then
    DEFOLDSDK=$(java -jar $BOB --version | awk '{print $5}')
fi

echo "Using DEFOLDSDK=${DEFOLDSDK}"

if [ "" == "${SERVER}" ]; then
    SERVER=${DEFAULT_SERVER}
fi

echo "Using SERVER=${SERVER}"

if [ "" == "${VARIANT}" ]; then
    VARIANT=headless
fi

echo "Using VARIANT=${VARIANT}"

function copyfile() {
    local path=$1
    local folder=$2
    if [ -f "$path" ]; then
        if [ ! -d "$folder" ]; then
            mkdir -v -p $folder
        fi
        cp -v $path $folder
    fi
}

function copy_results() {
    local platform=$1
    local platform_ne=$2
    local target_dir=$3

    for path in ./build/$platform_ne/*.a; do
        copyfile $path $target_dir
    done
    for path in ./build/$platform_ne/*.lib; do
        copyfile $path $target_dir
    done
}

for platform in $PLATFORMS; do

    echo "Building platform ${platform}"

    platform_ne=$platform

    case ${platform} in
        x86_64-macos)
            platform_ne="x86_64-osx";;
        arm64-macos)
            platform_ne="arm64-osx";;
    esac

    TARGET_LIB_DIR=${PROJECT_LIB_DIR}/${platform_ne}
    rm -rf ${TARGET_LIB_DIR}
    mkdir -p ${TARGET_LIB_DIR}

    # make sure it doesn't pick up the project's app manifest
    EXT_SETTINGS=./build/ext.settings
    echo "[native_extension]" > ${EXT_SETTINGS}
    echo "app_manifest =" >> ${EXT_SETTINGS}
    java -jar $BOB --platform=$platform --architectures=$platform --settings=${EXT_SETTINGS} build --build-artifacts=library --variant $VARIANT --build-server=$SERVER --defoldsdk=$DEFOLDSDK --debug-ne-upload true --ne-output-name=${LIBRARY_NAME} --ne-build-dir ${PROJECT_DIR}/src --ne-build-dir ${PROJECT_DIR}/commonsrc --ne-build-dir ${PROJECT_DIR}/include
    rm ${EXT_SETTINGS}

    copy_results $platform $platform_ne ${TARGET_LIB_DIR}
done
