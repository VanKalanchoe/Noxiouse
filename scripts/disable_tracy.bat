@echo off
echo Disabling Tracy profiler support in the Noxiouse build...
echo.

cmake -S "%~dp0.." -B "%~dp0..\build" -DNOX_ENABLE_TRACY=OFF
if %errorlevel% neq 0 (
    echo.
    echo CMake configuration failed.
    pause
    exit /b %errorlevel%
)

echo.
echo Tracy is DISABLED. Rebuild the solution for the change to take effect.
pause
