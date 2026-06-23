@echo off
echo Configuring Noxiouse with Visual Studio 2026 + ClangCL...

cmake -S .. -B ../build -G "Visual Studio 18 2026" -T ClangCL -A x64 -DCMAKE_TOOLCHAIN_FILE=..\NoxCore\vendors\vcpkg\scripts\buildsystems\vcpkg.cmake

if %errorlevel% neq 0 (
    echo.
    echo CMake configuration failed.
    pause
    exit /b %errorlevel%
)

echo.
echo CMake configuration successful.
pause