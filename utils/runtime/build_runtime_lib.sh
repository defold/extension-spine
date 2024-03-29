#!/usr/bin/env bash

set -e

VERSION=4.1
#URL=https://github.com/EsotericSoftware/spine-runtimes/archive/refs/tags/${VERSION}.zip
URL=https://github.com/EsotericSoftware/spine-runtimes/archive/refs/heads/${VERSION}.zip
UNPACK_FOLDER="spine-runtimes-${VERSION}"

PLATFORM=$1
if [ ! -z "${PLATFORM}" ]; then
    shift
else
    echo "You must specify a target platform!"
    exit 1
fi

CXX_NAME=clang
BUILD_DIR=./build/${PLATFORM}
SOURCE_DIR="${UNPACK_FOLDER}/spine-c/spine-c/src/spine"
SOURCE_PATTERN="*.c"
TARGET_INCLUDE_DIR="../../defold-spine/include/spine"
TARGET_LIBRARY_DIR="../../defold-spine/lib/${PLATFORM}"
TARGET_NAME=libspinec
TARGET_NAME_SUFFIX=.a
OPT="-O2"
CXXFLAGS="${CXXFLAGS} -g -Werror=format -I${TARGET_INCLUDE_DIR}/.."


SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

function download_package
{
    local url=$1
    local basename=$(basename $url)

    if [ ! -e ${basename} ]; then
        wget $URL
    else
        echo "Found ${basename}, skipping."
    fi
}

function unpack_package
{
    local package=$1
    if [ ! -e ${package} ]; then
        echo "Cannot unpack! Package ${package} was not found!" && exit 1
    fi

    if [ -e "${UNPACK_FOLDER}" ]; then
        echo "Folder already exists ${UNPACK_FOLDER}"
    else
        unzip -q ${package}
    fi
}

function copy_headers
{
    local target_dir=$1
    echo "Copying header files to ${target_dir}"
    cp -r ${UNPACK_FOLDER}/spine-c/spine-c/include/spine/ ${target_dir}
}

function copy_library
{
    local library=$1
    local target_dir=$2

    if [ ! -d "${target_dir}" ]; then
        mkdir -p ${target_dir}
    fi
    echo "Copying library to ${target_dir}"
    cp -v ${library} ${target_dir}
}

function run_cmd
{
    local args=$1
    echo "${args}"
    ${args}
}

function compile_file
{
    local src=$1
    local tgt=$2

    run_cmd "${CXX} ${OPT} ${SYSROOT} ${CXXFLAGS} -c ${src} -o ${tgt}"
}

function link_library
{
    local out=$1
    local object_files=$2
    run_cmd "${AR} -rcs ${out} ${object_files}"

    if [ ! -z "${RANLIB}" ]; then
        run_cmd "${RANLIB} ${out}"
    fi
}

function build_library
{
    local platform=$1
    local library_target=$2
    local source_dir=$3
    local files=$(find ${source_dir} -iname ${SOURCE_PATTERN})

    if [ "" == "${files}" ]; then
        echo "Found no source files in ${source_dir}"
        exit 1
    fi

    echo "Building library for platform ${platform}"

    if [ ! -d "${BUILD_DIR}" ]; then
        mkdir -p ${BUILD_DIR}
    fi

    object_files=""

    for f in ${files}
    do
        local tgt=${BUILD_DIR}/$(basename ${f}).o

        object_files="$object_files $tgt"

        compile_file ${f} ${tgt}
    done

    link_library ${library_target} "${object_files}"

    echo ""
    echo "Wrote ${library_target}"
}

function find_latest_sdk
{
    local pattern=$1

    ls -1 -td $DYNAMO_HOME/ext/SDKs/${pattern} | head -1
}

if [ ! -z "${DYNAMO_HOME}" ]; then
    echo "Found DYNAMO_HOME=${DYNAMO_HOME}, setting up SDK paths:"
    DARWIN_TOOLCHAIN_ROOT=$(find_latest_sdk XcodeDefault*.xctoolchain)
    OSX_SDK_ROOT=$(find_latest_sdk MacOSX*.sdk)
    IOS_SDK_ROOT=$(find_latest_sdk iPhoneOS*.sdk)
    ANDROID_NDK_ROOT=$(find_latest_sdk android-ndk-*)
    EMSCRIPTEN=$(find_latest_sdk emsdk-*)/upstream/emscripten
    WIN32_MSVC_INCLUDE_DIR=$(find_latest_sdk Win32/MicrosoftVisualStudio14.0/VC/Tools/MSVC/14.*)/include
    WIN32_SDK_INCLUDE_DIR=$(find_latest_sdk Win32/WindowsKits/10/Include/10.0.*)/ucrt

    echo DARWIN_TOOLCHAIN_ROOT=${DARWIN_TOOLCHAIN_ROOT}
    echo OSX_SDK_ROOT=${OSX_SDK_ROOT}
    echo IOS_SDK_ROOT=${IOS_SDK_ROOT}
    echo ANDROID_NDK_ROOT=${ANDROID_NDK_ROOT}
    echo EMSCRIPTEN=${EMSCRIPTEN}
    echo ""
fi


#CXXFLAGS="${CXXFLAGS} -I./${UNPACK_FOLDER}/include"

case $PLATFORM in
    arm64-ios)
        [ ! -e "${DARWIN_TOOLCHAIN_ROOT}" ] && echo "No SDK found at DARWIN_TOOLCHAIN_ROOT=${DARWIN_TOOLCHAIN_ROOT}" && exit 1
        [ ! -e "${IOS_SDK_ROOT}" ] && echo "No SDK found at IOS_SDK_ROOT=${IOS_SDK_ROOT}" && exit 1
        export SDKROOT="${IOS_SDK_ROOT}"

        export PATH=$DARWIN_TOOLCHAIN_ROOT/usr/bin:$PATH
        export CXXFLAGS="${CXXFLAGS} -stdlib=libc++ -arch arm64 "

        if [ -z "${IOS_MIN_SDK_VERSION}" ]; then
            IOS_MIN_SDK_VERSION="8.0"
        fi

        if [ ! -z "${IOS_MIN_SDK_VERSION}" ]; then
            export CXXFLAGS="${CXXFLAGS} -miphoneos-version-min=${IOS_MIN_SDK_VERSION} "
            export LDFLAGS="${LDFLAGS} -miphoneos-version-min=${IOS_MIN_SDK_VERSION}"
        fi

        export CXX=$DARWIN_TOOLCHAIN_ROOT/usr/bin/${CXX_NAME}
        export AR=$DARWIN_TOOLCHAIN_ROOT/usr/bin/ar
        export RANLIB=$DARWIN_TOOLCHAIN_ROOT/usr/bin/ranlib

        ;;


    x86_64-osx)
        [ ! -e "${DARWIN_TOOLCHAIN_ROOT}" ] && echo "No SDK found at DARWIN_TOOLCHAIN_ROOT=${DARWIN_TOOLCHAIN_ROOT}" && exit 1
        [ ! -e "${OSX_SDK_ROOT}" ] && echo "No SDK found at OSX_SDK_ROOT=${OSX_SDK_ROOT}" && exit 1
        export SDKROOT="${OSX_SDK_ROOT}"
        export CXX=$DARWIN_TOOLCHAIN_ROOT/usr/bin/${CXX_NAME}
        export AR=$DARWIN_TOOLCHAIN_ROOT/usr/bin/ar
        export RANLIB=$DARWIN_TOOLCHAIN_ROOT/usr/bin/ranlib

        export CXXFLAGS="${CXXFLAGS} -stdlib=libc++ "

        if [ -z "${OSX_MIN_SDK_VERSION}" ]; then
            OSX_MIN_SDK_VERSION="10.7"
        fi

        if [ ! -z "${OSX_MIN_SDK_VERSION}" ]; then
            export MACOSX_DEPLOYMENT_TARGET=${OSX_MIN_SDK_VERSION}
            export CXXFLAGS="${CXXFLAGS} -mmacosx-version-min=${OSX_MIN_SDK_VERSION} "
            export LDFLAGS="${LDFLAGS} -mmacosx-version-min=${OSX_MIN_SDK_VERSION}"
        fi

        ;;

    x86_64-linux)
        export CXXFLAGS="${CXXFLAGS} -fPIC"
        if [ -z "${CXX}" ]; then
            export CXX=${CXX_NAME}
        fi
        if [ -z "${AR}" ]; then
            export AR=ar
        fi
        if [ -z "${RANLIB}" ]; then
            export RANLIB=ranlib
        fi
        ;;

    x86_64-win32)
        [ ! -e "${WIN32_MSVC_INCLUDE_DIR}" ] && echo "No SDK found at WIN32_MSVC_INCLUDE_DIR=${WIN32_MSVC_INCLUDE_DIR}" && exit 1
        [ ! -e "${WIN32_SDK_INCLUDE_DIR}" ] && echo "No SDK found at WIN32_SDK_INCLUDE_DIR=${WIN32_SDK_INCLUDE_DIR}" && exit 1

        TARGET_NAME_SUFFIX=.lib

        export host_platform=`uname | awk '{print tolower($0)}'`
        if [ "darwin" == "${host_platform}" ] || [ "linux" == "${host_platform}" ]; then
            export CXXFLAGS="${CXXFLAGS} -target x86_64-pc-windows-msvc -m64 -D_CRT_SECURE_NO_WARNINGS -D__STDC_LIMIT_MACROS -DWINVER=0x0600 -DNOMINMAX -gcodeview"
            export CXXFLAGS="${CXXFLAGS} -nostdinc++ -isystem ${WIN32_MSVC_INCLUDE_DIR} -isystem ${WIN32_SDK_INCLUDE_DIR}"
        fi
        ;;

    x86-win32)
        [ ! -e "${WIN32_MSVC_INCLUDE_DIR}" ] && echo "No SDK found at WIN32_MSVC_INCLUDE_DIR=${WIN32_MSVC_INCLUDE_DIR}" && exit 1
        [ ! -e "${WIN32_SDK_INCLUDE_DIR}" ] && echo "No SDK found at WIN32_SDK_INCLUDE_DIR=${WIN32_SDK_INCLUDE_DIR}" && exit 1

        TARGET_NAME_SUFFIX=.lib

        export host_platform=`uname | awk '{print tolower($0)}'`
        if [ "darwin" == "${host_platform}" ] || [ "linux" == "${host_platform}" ]; then
            export CXXFLAGS="${CXXFLAGS} -target i386-pc-win32-msvc -m32 -D_CRT_SECURE_NO_WARNINGS -D__STDC_LIMIT_MACROS -DWINVER=0x0600 -DNOMINMAX -gcodeview"
            export CXXFLAGS="${CXXFLAGS} -nostdinc++ -isystem ${WIN32_MSVC_INCLUDE_DIR} -isystem ${WIN32_SDK_INCLUDE_DIR}"
        fi
        ;;

    armv7-android)
        [ ! -e "${ANDROID_NDK_ROOT}" ] && echo "No SDK found at ANDROID_NDK_ROOT=${ANDROID_NDK_ROOT}" && exit 1
        export host_platform=`uname | awk '{print tolower($0)}'`
        export llvm="${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/${host_platform}-x86_64/bin"
        export SDKROOT="${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/${host_platform}-x86_64/sysroot"

        if [ -z "${OPT}" ]; then
            export OPT="-Os"
        fi
        export CXXFLAGS="${CXXFLAGS} -fpic -ffunction-sections -funwind-tables -D__ARM_ARCH_5__ -D__ARM_ARCH_5T__ -D__ARM_ARCH_5E__ -D__ARM_ARCH_5TE__ -march=armv7-a -mfloat-abi=softfp -mfpu=vfp -mthumb -fomit-frame-pointer -fno-strict-aliasing -DANDROID -Wno-c++11-narrowing"

        if [ -z "${ANDROID_VERSION}" ]; then
            ANDROID_VERSION=19
        fi

        export CXX="${llvm}/armv7a-linux-androideabi${ANDROID_VERSION}-${CXX_NAME}"
        export AR=${llvm}/llvm-ar
        export RANLIB=${llvm}/llvm-ranlib
        ;;

    arm64-android)
        [ ! -e "${ANDROID_NDK_ROOT}" ] && echo "No SDK found at ANDROID_NDK_ROOT=${ANDROID_NDK_ROOT}" && exit 1
        export host_platform=`uname | awk '{print tolower($0)}'`
        export llvm="${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/${host_platform}-x86_64/bin"
        export SDKROOT="${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/${host_platform}-x86_64/sysroot"

        if [ -z "${OPT}" ]; then
            export OPT="-Os"
        fi
        export CXXFLAGS="${CXXFLAGS} -fpic -ffunction-sections -funwind-tables -D__aarch64__  -march=armv8-a -fomit-frame-pointer -fno-strict-aliasing -DANDROID -Wno-c++11-narrowing"

        if [ -z "${ANDROID_VERSION}" ]; then
            ANDROID_VERSION=21 # Android 5.0
        fi

        export CXX="${llvm}/aarch64-linux-android${ANDROID_VERSION}-${CXX_NAME}"
        export AR=${llvm}/llvm-ar
        export RANLIB=${llvm}/llvm-ranlib
        ;;

    js-web)
        [ ! -e "${EMSCRIPTEN}" ] && echo "No SDK found at EMSCRIPTEN=${EMSCRIPTEN}" && exit 1
        export CXX=${EMSCRIPTEN}/em++
        export AR=${EMSCRIPTEN}/emar
        export RANLIB=${EMSCRIPTEN}/emranlib
        export CXXFLAGS="${CXXFLAGS} -fPIC -fno-exceptions"

        CXXFLAGS="${CXXFLAGS} -s PRECISE_F32=2 -s AGGRESSIVE_VARIABLE_ELIMINATION=1 -s DISABLE_EXCEPTION_CATCHING=1"
        CXXFLAGS="${CXXFLAGS} -s WASM=0 -s LEGACY_VM_SUPPORT=1"
        ;;

    wasm-web)
        [ ! -e "${EMSCRIPTEN}" ] && echo "No SDK found at EMSCRIPTEN=${EMSCRIPTEN}" && exit 1
        export CXX=${EMSCRIPTEN}/em++
        export AR=${EMSCRIPTEN}/emar
        export RANLIB=${EMSCRIPTEN}/emranlib
        export CXXFLAGS="${CXXFLAGS} -fPIC -fno-exceptions -fno-rtti"

        CXXFLAGS="${CXXFLAGS} -s PRECISE_F32=2 -s AGGRESSIVE_VARIABLE_ELIMINATION=1 -s DISABLE_EXCEPTION_CATCHING=1"
        CXXFLAGS="${CXXFLAGS} -s WASM=1 -s IMPORTED_MEMORY=1 -s ALLOW_MEMORY_GROWTH=1"

        ;;

    *)
        echo "Unknown platform: ${PLATFORM}, using host ${CXX_NAME}, ar and ranlib. Prefix with CROSS_TOOLS_PREFIX to use specific tools."
        ;;
esac

if [ ! -z "${SDKROOT}" ]; then
    export SYSROOT="-isysroot $SDKROOT"
fi

if [ -z "${CXX}" ]; then
    export CXX=${CROSS_TOOLS_PREFIX}${CXX_NAME}
fi
if [ -z "${AR}" ]; then
    export AR=${CROSS_TOOLS_PREFIX}ar
fi
if [ -z "${RANLIB}" ]; then
    export RANLIB=${CROSS_TOOLS_PREFIX}ranlib
fi

# *************************************************

pushd $SCRIPT_DIR

if [ ! -d "${BUILD_DIR}" ]; then
    mkdir -p ${BUILD_DIR}
fi

download_package $URL
unpack_package $(basename $URL)

copy_headers ${TARGET_INCLUDE_DIR}

TARGET_LIBRARY=${BUILD_DIR}/${TARGET_NAME}${TARGET_NAME_SUFFIX}
build_library ${PLATFORM} ${TARGET_LIBRARY} ${SOURCE_DIR}

copy_library ${TARGET_LIBRARY} ${TARGET_LIBRARY_DIR}

popd