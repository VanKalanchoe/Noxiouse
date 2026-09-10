@echo off
echo The following files will be deleted:
echo.

dir /s /b "*.nmesh"
dir /s /b "*.nsmesh"
dir /s /b "*.ntex"
dir /s /b "*.nmat"
dir /s /b "*.hash"

echo.
choice /C YN /M "Delete all these files?"
if errorlevel 2 goto :cancel

del /s /q "*.nmesh"
del /s /q "*.nsmesh"
del /s /q "*.ntex"
del /s /q "*.nmat"
del /s /q "*.hash"

echo.
echo Done.
pause
exit /b

:cancel
echo.
echo Cancelled. Nothing was deleted.
pause