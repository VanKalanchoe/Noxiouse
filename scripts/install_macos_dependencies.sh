#!/bin/bash

echo "Configuring Noxiouse with Visual Studio 2026 + ClangCL..."
echo "Installing dependencies"

# Add local vcpkg folder to PATH temporarily
VCPKG_ROOT="$(dirname "$0")/../NoxCore/vendors/vcpkg"
export PATH="$VCPKG_ROOT:$PATH"

# Check if vcpkg is installed
if ! command -v vcpkg >/dev/null 2>&1
then
    echo "vcpkg not found. Installing automatically..."

    # Clone vcpkg if the folder doesn't exist
    if [ ! -d "$VCPKG_ROOT" ]
    then
        echo "Cloning vcpkg into $VCPKG_ROOT..."
        git clone https://github.com/Microsoft/vcpkg.git "$VCPKG_ROOT"

        if [ $? -ne 0 ]
        then
            echo "Failed to clone vcpkg."
            exit 1
        fi
    else
        echo "vcpkg folder already exists at $VCPKG_ROOT"
    fi

    # Bootstrap vcpkg
    echo "Bootstrapping vcpkg..."
    pushd "$VCPKG_ROOT"

    ./bootstrap-vcpkg.sh

    if [ $? -ne 0 ]
    then
        echo "Failed to bootstrap vcpkg."
        popd
        exit 1
    fi

    popd

    # Add vcpkg to PATH for current session
    export PATH="$VCPKG_ROOT:$PATH"

    echo "vcpkg installed successfully!"
else
    echo "vcpkg is already installed."
fi

# Enable binary caching for vcpkg
echo "Enabling binary caching for vcpkg..."
export VCPKG_BINARY_SOURCES="clear;files,$TMPDIR/vcpkg-cache,readwrite"

# Create cache directory if it doesn't exist
if [ ! -d "$TMPDIR/vcpkg-cache" ]
then
    mkdir "$TMPDIR/vcpkg-cache"
fi

# Install all dependencies at once using vcpkg with parallel installation
echo "Installing all dependencies..."
vcpkg install sdl3 glm entt spdlog xxhash yaml-cpp box2d freetype skia stb zstd ktx tinyobjloader --triplet=arm64-osx

# ============================================================
# Metal-cpp
# ============================================================

if [ "$(uname -s)" = "Darwin" ]
then
    METAL_CPP_DIR="../NoxCore/vendors/metal-cpp"

    if [ ! -d "$METAL_CPP_DIR/.git" ]
    then
        echo "Downloading Apple metal-cpp..."

        rm -rf "$METAL_CPP_DIR"

        git clone \
            https://github.com/apple/metal-cpp.git \
            "$METAL_CPP_DIR"

        if [ $? -ne 0 ]
        then
            echo "Failed to clone metal-cpp."
            exit 1
        fi
    else
        echo "metal-cpp already installed."
    fi

    echo "Generating metal-cpp single header..."

    pushd "$METAL_CPP_DIR" > /dev/null

    python3 \
        ./SingleHeader/MakeSingleHeader.py \
        Foundation/Foundation.hpp \
        QuartzCore/QuartzCore.hpp \
        Metal/Metal.hpp \
        -o ./SingleHeader/Metal.hpp

    if [ $? -ne 0 ]
    then
        echo "Failed to generate metal-cpp single header."
        popd > /dev/null
        exit 1
    fi

    popd > /dev/null

    # Create include layout:
    #
    # vendors/metal-cpp/include/Metal/Metal.hpp
    #

    mkdir -p "$METAL_CPP_DIR/include/Metal"

    cp \
        "$METAL_CPP_DIR/SingleHeader/Metal.hpp" \
        "$METAL_CPP_DIR/include/Metal/Metal.hpp"

    if [ $? -ne 0 ]
    then
        echo "Failed to install generated Metal.hpp."
        exit 1
    fi

    echo "metal-cpp installed successfully."
    echo "Header: $METAL_CPP_DIR/include/Metal/Metal.hpp"
fi

# slang
SLANG_VERSION=2026.12.2
SLANG_URL="https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/slang-${SLANG_VERSION}-macos-aarch64.zip"
DEST_DIR="../NoxCore/vendors/slang"

# Only download and extract if DEST_DIR does NOT exist
if [ ! -d "$DEST_DIR" ]
then
    echo "Downloading Slang $SLANG_VERSION..."
    curl -L "$SLANG_URL" -o slang.zip || exit 1

    echo "Creating destination folder..."
    mkdir "$DEST_DIR"

    echo "Extracting zip AS-IS into $DEST_DIR..."
    unzip slang.zip -d "$DEST_DIR"

    rm slang.zip
    echo "Slang downloaded and extracted into $DEST_DIR."
else
    echo "Slang already installed in $DEST_DIR, skipping download and extraction."
fi

# -------------

# FileWatch
FILEWATCH_DIR="../NoxCore/vendors/filewatch"

if [ ! -d "$FILEWATCH_DIR" ]
then
    echo "Downloading FileWatch..."

    git clone https://github.com/ThomasMonkman/filewatch.git "$FILEWATCH_DIR"

    if [ $? -ne 0 ]
    then
        echo "Failed to clone FileWatch."
        exit 1
    fi

    echo "FileWatch installed."
else
    echo "FileWatch already installed, skipping."
fi

echo
echo "Don't forget to install the Vulkan SDK from https://vulkan.lunarg.com/"
echo

echo "All dependencies have been installed successfully!"
echo "You can now use CMake to build your Vulkan project."

# Same as Windows errorlevel check
if [ $? -ne 0 ]
then
    echo
    echo "CMake configuration failed."
    read -p "Press enter to exit"
    exit $?
fi

echo
echo "CMake configuration successful."
read -p "Press enter to exit"