@echo off
echo Configuring Noxiouse with Visual Studio 2026 + ClangCL...
echo Installing dependencies

:: Add local vcpkg folder to PATH temporarily
set "VCPKG_ROOT=%~dp0..\NoxCore\vendors\vcpkg"
set "PATH=%VCPKG_ROOT%;%PATH%"

:: Check if vcpkg is installed
where vcpkg >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo vcpkg not found. Installing automatically...

    :: Clone vcpkg if the folder doesn't exist
    if not exist "%VCPKG_ROOT%" (
        echo Cloning vcpkg into "%VCPKG_ROOT%"...
        git clone https://github.com/Microsoft/vcpkg.git "%VCPKG_ROOT%"
        if errorlevel 1 (
            echo Failed to clone vcpkg.
            exit /b 1
        )
    ) else (
        echo vcpkg folder already exists at "%VCPKG_ROOT%"
    )

    :: Bootstrap vcpkg
    echo Bootstrapping vcpkg...
    pushd "%VCPKG_ROOT%"
    bootstrap-vcpkg.bat
    if errorlevel 1 (
        echo Failed to bootstrap vcpkg.
        popd
        exit /b 1
    )
    popd

    :: Add vcpkg to PATH for current session
    set "PATH=%VCPKG_ROOT%;%PATH%"

    echo vcpkg installed successfully!
) else (
    echo vcpkg is already installed.
)

:: Enable binary caching for vcpkg
echo Enabling binary caching for vcpkg...
set VCPKG_BINARY_SOURCES=clear;files,%TEMP%\vcpkg-cache,readwrite

:: Create cache directory if it doesn't exist
if not exist %TEMP%\vcpkg-cache mkdir %TEMP%\vcpkg-cache

:: Install all dependencies at once using vcpkg with parallel installation
echo Installing all dependencies...
vcpkg install sdl3[vulkan] glm entt xxhash yaml-cpp stb ktx[vulkan] tinyobjloader --triplet=x64-windows

:: slang
set SLANG_VERSION=2026.13
set SLANG_URL=https://github.com/shader-slang/slang/releases/download/v%SLANG_VERSION%/slang-%SLANG_VERSION%-windows-x86_64.zip
set DEST_DIR=../NoxCore/vendors/slang

:: Only download and extract if DEST_DIR does NOT exist
if not exist "%DEST_DIR%" (
    echo Downloading Slang %SLANG_VERSION%...
    curl -L "%SLANG_URL%" -o slang.zip || exit /b 1

    echo Creating destination folder...
    mkdir "%DEST_DIR%"

    echo Extracting zip AS-IS into %DEST_DIR%...
    tar -xf slang.zip -C "%DEST_DIR%"

    del slang.zip
    echo Slang downloaded and extracted into "%DEST_DIR%".
) else (
    echo Slang already installed in "%DEST_DIR%", skipping download and extraction.
)
:: -------------


:: Remind about Vulkan SDK
echo.
echo Don't forget to install the Vulkan SDK from https://vulkan.lunarg.com/
echo.

echo All dependencies have been installed successfully!
echo You can now use CMake to build your Vulkan project.

if %errorlevel% neq 0 (
    echo.
    echo CMake configuration failed.
    pause
    exit /b %errorlevel%
)

echo.
echo CMake configuration successful.
pause