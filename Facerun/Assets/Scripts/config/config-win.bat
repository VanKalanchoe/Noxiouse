@echo off
setlocal

for %%I in ("%~dp0..") do set "SCRIPTS_DIR=%%~fI"
echo Generating Facerun.sln...
dotnet new sln --name Facerun --output "%SCRIPTS_DIR%\build\win" --force >nul
if errorlevel 1 exit /b %errorlevel%

dotnet sln "%SCRIPTS_DIR%\build\win\Facerun.sln" add ^
    "%SCRIPTS_DIR%\Facerun.csproj" ^
    "%SCRIPTS_DIR%\..\..\..\NoxScriptCore\Nox.ScriptCore.csproj" >nul
if errorlevel 1 exit /b %errorlevel%

echo.
echo Generated: %SCRIPTS_DIR%\build\win\Facerun.sln
echo Build with: dotnet build "%SCRIPTS_DIR%\build\win\Facerun.sln" --configuration Debug
