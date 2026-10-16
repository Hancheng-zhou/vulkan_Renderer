@echo off
:: Switch to the directory of this script to ensure relative paths work correctly
cd /d "%~dp0"

echo ===============================================
echo  Generating Visual Studio 2022 project files...
echo ===============================================

:: Execute premake5
"vendor\bin\premake5\premake5.exe" vs2022

:: Check if the command was successful
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Premake execution failed! Please check if vendor\bin\premake5\premake5.exe exists.
) else (
    echo.
    echo [SUCCESS] Project files generated!
)

:: Pause to prevent the window from closing instantly when double-clicked
pause