@echo off
:: Downloads the Tracy profiler GUI once (must match the client version in NoxCore/vendors/tracy) and starts it.
set "TRACY_VERSION=0.14.1"
set "TRACY_DIR=%~dp0..\build\tools\tracy-%TRACY_VERSION%"
set "TRACY_ZIP=%TRACY_DIR%\windows-%TRACY_VERSION%.zip"

if not exist "%TRACY_DIR%\tracy-profiler.exe" (
    echo Downloading Tracy profiler %TRACY_VERSION%...
    if not exist "%TRACY_DIR%" mkdir "%TRACY_DIR%"
    curl -L -o "%TRACY_ZIP%" "https://github.com/wolfpld/tracy/releases/download/v%TRACY_VERSION%/windows-%TRACY_VERSION%.zip"
    if errorlevel 1 (
        echo Download failed.
        pause
        exit /b 1
    )
    powershell -NoProfile -Command "Expand-Archive -Force -LiteralPath '%TRACY_ZIP%' -DestinationPath '%TRACY_DIR%'"
    if errorlevel 1 (
        echo Extracting failed.
        pause
        exit /b 1
    )
    del "%TRACY_ZIP%"

    rem The archive may contain a subfolder: copy the executable to this folder's root.
    if not exist "%TRACY_DIR%\tracy-profiler.exe" (
        for /r "%TRACY_DIR%" %%F in (tracy-profiler.exe) do if exist "%%F" copy /y "%%F" "%TRACY_DIR%\tracy-profiler.exe" >nul
    )
)

if not exist "%TRACY_DIR%\tracy-profiler.exe" (
    echo tracy-profiler.exe not found in "%TRACY_DIR%".
    pause
    exit /b 1
)

echo Starting Tracy profiler %TRACY_VERSION%. Start NoxEditor, then click "Connect" next to it.
start "" "%TRACY_DIR%\tracy-profiler.exe"
