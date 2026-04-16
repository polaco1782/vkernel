@echo off
REM vkernel - UEFI Microkernel
REM Copyright (C) 2026 vkernel authors
REM
REM build.bat - Visual Studio C++26 build script

setlocal enabledelayedexpansion

REM Configuration
set KERNEL_NAME=vkernel
set BUILD_DIR=build_vs
set PLATFORM=x64
set CONFIGURATION=Release

REM Check for Debug configuration
if "%1"=="debug" set CONFIGURATION=Debug

echo Building vkernel C++26 for Visual Studio (%CONFIGURATION% %PLATFORM%)
echo.

REM Check for Visual Studio environment
if "%VCINSTALLDIR%"=="" (
    echo Error: Visual Studio environment not detected
    echo Please run this script from a Visual Studio Developer Command Prompt
    echo or call vcvarsall.bat first
    echo.
    echo Example:
    echo   "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
    exit /b 1
)

REM Check for clang-cl availability
where clang-cl >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: clang-cl not found in PATH
    echo Please install Clang from https://llvm.org/ or via Visual Studio Installer
    echo   - Select "C++ Clang Compiler for Windows" component
    exit /b 1
)

REM Create build directory
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
if not exist %BUILD_DIR%\obj mkdir %BUILD_DIR%\obj
if not exist %BUILD_DIR%\obj\boot mkdir %BUILD_DIR%\obj\boot
if not exist %BUILD_DIR%\obj\core mkdir %BUILD_DIR%\obj\core
if not exist %BUILD_DIR%\obj\arch mkdir %BUILD_DIR%\obj\arch
if not exist %BUILD_DIR%\obj\arch\x86_64 mkdir %BUILD_DIR%\obj\arch\x86_64

REM Compiler flags for UEFI x86_64 C++26
set CXXFLAGS=/nologo /W3 /WX-
set CXXFLAGS=!CXXFLAGS! /DVK_BUILD_FOR_EFI=1
set CXXFLAGS=!CXXFLAGS! /I%CD%\include
set CXXFLAGS=!CXXFLAGS! /std:c++latest
set CXXFLAGS=!CXXFLAGS! /EHs-c-  /GR-  /MD

if "%CONFIGURATION%"=="Debug" (
    set CXXFLAGS=!CXXFLAGS! /Od /Zi /DDEBUG
) else (
    set CXXFLAGS=!CXXFLAGS! /O2 /DNDEBUG
)

echo Using compiler: clang-cl
clang-cl --version
echo.

REM Find all C++ source files
set CPP_SOURCES=
for /r src %%f in (*.cpp) do (
    set CPP_SOURCES=!CPP_SOURCES! %%f
)

echo Source files found:
for %%f in (%CPP_SOURCES%) do (
    echo   %%f
)
echo.

REM Compile each source file
echo Compiling...
for %%f in (%CPP_SOURCES%) do (
    set SRC=%%f
    set OBJ=!SRC:src\=%BUILD_DIR%\obj\!
    set OBJ=!OBJ:.cpp=.obj!
    
    REM Create directory if needed
    for %%d in ("!OBJ!") do (
        if not exist "%%~dpd" mkdir "%%~dpd"
    )
    
    echo   CXX     %%f
    clang-cl !CXXFLAGS! /c /Fo"%%OBJ%%" "%%f"
    if !errorlevel! neq 0 (
        echo Error compiling %%f
        exit /b 1
    )
)

echo.

REM Link using lld-link to create PE/COFF EFI application
echo Linking...
set EFI_FILE=%BUILD_DIR%\%KERNEL_NAME%.efi

lld-link /nologo /subsystem:efi_application /machine:x64 ^
    /out:"%EFI_FILE%" ^
    /entry:efi_main ^
    /nodefaultlib ^
    %BUILD_DIR%\obj\boot\efi_main.obj ^
    %BUILD_DIR%\obj\core\console.obj ^
    %BUILD_DIR%\obj\core\uefi.obj ^
    %BUILD_DIR%\obj\core\memory.obj ^
    %BUILD_DIR%\obj\arch\x86_64\arch_init.obj ^

if %errorlevel% neq 0 (
    echo Error linking
    exit /b 1
)

echo.
echo Build complete: %EFI_FILE%
dir %EFI_FILE%

endlocal
