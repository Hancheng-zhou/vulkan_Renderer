@echo off
setlocal

cd /d "%~dp0"

if "%VULKAN_SDK%"=="" (
    echo [ERROR] VULKAN_SDK is not set.
    exit /b 1
)

set GLSLANG=%VULKAN_SDK%\Bin\glslangValidator.exe
if not exist "%GLSLANG%" (
    echo [ERROR] glslangValidator not found: %GLSLANG%
    exit /b 1
)

set SHADER_DIR=assets\shaders

echo ===============================================
echo  Compiling GLSL to SPIR-V...
echo ===============================================

for %%f in ("%SHADER_DIR%\*.vert" "%SHADER_DIR%\*.frag") do (
    echo   %%~nxf
    "%GLSLANG%" -V "%%f" -o "%%f.spv"
    if errorlevel 1 (
        echo [ERROR] failed: %%~nxf
        exit /b 1
    )
)

echo.
echo [OK] SPIR-V written next to sources.
pause