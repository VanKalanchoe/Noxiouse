@echo off
echo Configuring Noxiouse with Visual Studio 2026 + ClangCL...
echo Installing dependencies

:: Add local vcpkg folder to PATH temporarily
set "VCPKG_ROOT=%~dp0..\NoxCore\vendors\vcpkg"
set "PATH=%VCPKG_ROOT%;%PATH%"

:: Define vendors directory anchored to this script's directory
set "VENDORS_DIR=%~dp0..\NoxCore\vendors"

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
    call bootstrap-vcpkg.bat
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
vcpkg install sdl3[vulkan] glm entt spdlog xxhash yaml-cpp box2d freetype skia stb ktx[vulkan] tinyobjloader --triplet=x64-windows

:: slang
set SLANG_VERSION=2026.14.1
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

 :: Streamline (NVIDIA DLSS SDK)
    set STREAMLINE_VERSION=2.14.1
    set STREAMLINE_URL=https://github.com/NVIDIA-RTX/Streamline/releases/download/v%STREAMLINE_VERSION%/streamline-sdk-v%STREAMLINE_VERSION%.zip
    set "STREAMLINE_DIR=%VENDORS_DIR%\Streamline"

    if not exist "%STREAMLINE_DIR%" (
        echo Downloading NVIDIA Streamline %STREAMLINE_VERSION%...
        curl -L "%STREAMLINE_URL%" -o "%TEMP%\streamline.zip" || exit /b 1

        echo Creating Streamline destination folder...
        mkdir "%STREAMLINE_DIR%"

        echo Extracting Streamline...
        tar -xf "%TEMP%\streamline.zip" -C "%STREAMLINE_DIR%"
        del "%TEMP%\streamline.zip"

        echo Streamline installed in "%STREAMLINE_DIR%".
    ) else (
        echo Streamline already installed in "%STREAMLINE_DIR%", skipping.
    )

    :: NRD (NVIDIA Real-Time Denoisers)
    set NRD_VERSION=v4.17.3
    set "NRD_DIR=%VENDORS_DIR%\NRD"
    set "NRD_BUILD_DIR=%TEMP%\NRD_build"

    if not exist "%NRD_DIR%" (
        echo Setting up NVIDIA Real-Time Denoisers %NRD_VERSION%...
        if exist "%NRD_BUILD_DIR%" rmdir /S /Q "%NRD_BUILD_DIR%"

        git clone --depth 1 --branch %NRD_VERSION% --recursive https://github.com/NVIDIA-RTX/NRD.git "%NRD_BUILD_DIR%" || exit /b 1
        pushd "%NRD_BUILD_DIR%"

        call 1-Deploy.bat
        if errorlevel 1 (
            echo Failed to deploy NRD.
            popd
            exit /b 1
        )

        call 2-Build.bat
        if errorlevel 1 (
            echo Failed to build NRD.
            popd
            exit /b 1
        )

        call 3-PrepareSDK.bat
        if errorlevel 1 (
            echo Failed to prepare NRD SDK.
            popd
            exit /b 1
        )
        popd

        echo Copying NRD SDK into "%NRD_DIR%"...
        mkdir "%NRD_DIR%"
        xcopy /E /I /Y "%NRD_BUILD_DIR%\_NRD_SDK\*" "%NRD_DIR%"

        rem Also copy NRI SDK needed by NRDIntegration
        if exist "%NRD_BUILD_DIR%\_NRI_SDK" (
            echo Copying NRI SDK into "%NRD_DIR%"...
            xcopy /E /I /Y "%NRD_BUILD_DIR%\_NRI_SDK\*" "%NRD_DIR%"
        )
        if exist "%NRD_BUILD_DIR%\_Build\_deps\nri-src\_NRI_SDK" (
            echo Copying NRI SDK from _deps into "%NRD_DIR%"...
            xcopy /E /I /Y "%NRD_BUILD_DIR%\_Build\_deps\nri-src\_NRI_SDK\*" "%NRD_DIR%"
        )

        rmdir /S /Q "%NRD_BUILD_DIR%"
        echo NRD %NRD_VERSION% installed successfully in "%NRD_DIR%".
    ) else (
        echo NRD already installed in "%NRD_DIR%", skipping.
    )

    rem RTXDI (NVIDIA RTX Direct Illumination SDK - Library Only)
    rem Note: RTXDI-Library repository does not have git tags/releases; main branch HEAD is the official v3.1.0 library SDK.
    set "RTXDI_DIR=%VENDORS_DIR%\RTXDI"

    if not exist "%RTXDI_DIR%" (
        echo Setting up NVIDIA RTXDI Library [v3.1.0 SDK]...
        git clone --depth 1 https://github.com/NVIDIA-RTX/RTXDI-Library.git "%RTXDI_DIR%"
        if errorlevel 1 (
            echo Failed to clone RTXDI Library.
            pause
            exit /b 1
        )
        echo RTXDI Library installed successfully in "%RTXDI_DIR%".
    ) else (
        echo RTXDI already installed in "%RTXDI_DIR%", skipping.
    )

:: FileWatch
set FILEWATCH_DIR=../NoxCore/vendors/filewatch

if not exist "%FILEWATCH_DIR%" (
    echo Downloading FileWatch...

    git clone https://github.com/ThomasMonkman/filewatch.git "%FILEWATCH_DIR%"

    if errorlevel 1 (
        echo Failed to clone FileWatch.
        pause
        exit /b 1
    )

    echo FileWatch installed.
) else (
    echo FileWatch already installed, skipping.
)

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