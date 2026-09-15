@echo off
echo Enabling Tracy profiler support in the Noxiouse build...
echo.

:: Re-configures the existing build folder (keeps Visual Studio 2026 + ClangCL + vcpkg from setup_windows_build.bat).
cmake -S "%~dp0.." -B "%~dp0..\build" -DNOX_ENABLE_TRACY=ON
if %errorlevel% neq 0 (
    echo.
    echo CMake configuration failed. Run setup_windows_build.bat first if the build folder does not exist yet.
    pause
    exit /b %errorlevel%
)

echo.
echo Tracy is ENABLED. Next steps:
echo   1. Open build\Noxiouse.slnx, select RelWithDebInfo, build and start NoxEditor (Ctrl+F5).
echo   2. Run open_tracy_viewer.bat and click "Connect" next to NoxEditor.
echo To turn Tracy off again, run disable_tracy.bat.
pause
