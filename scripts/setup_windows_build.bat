@echo off
echo Configuring Noxiouse with Visual Studio 2026 + ClangCL...

:: Vendor submodules added after the first clone (cpp-httplib: the editor's MCP server). Only fetches what is missing; submodules
:: already there are left as they are.
git -C .. submodule update --init NoxCore/vendors/cpp-httplib
if %errorlevel% neq 0 (
    echo.
    echo Could not fetch the cpp-httplib submodule ^(git submodule update --init NoxCore/vendors/cpp-httplib^).
    pause
    exit /b %errorlevel%
)

cmake -S .. -B ../build -G "Visual Studio 18 2026" -T ClangCL -A x64 -DCMAKE_TOOLCHAIN_FILE=..\NoxCore\vendors\vcpkg\scripts\buildsystems\vcpkg.cmake
::cmake -S .. -B ../build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=..\NoxCore\vendors\vcpkg\scripts\buildsystems\vcpkg.cmake
if %errorlevel% neq 0 (
    echo.
    echo CMake configuration failed.
    pause
    exit /b %errorlevel%
)

echo.
echo CMake configuration successful.
pause