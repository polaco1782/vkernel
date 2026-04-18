@echo off
REM vkernel - UEFI Microkernel
REM Copyright (C) 2026 vkernel authors
REM
REM run_qemu.bat - Run vkernel in QEMU on Windows

setlocal

set BUILD_DIR=build_vs
set EFI_FILE=%BUILD_DIR%\vkernel.efi
set ESP_ROOT=%BUILD_DIR%\esp
set ESP_BOOT=%ESP_ROOT%\EFI\BOOT
set NVRAM_FILE=%BUILD_DIR%\ovmf_vars.fd
set BUILD_CONFIG=Debug
set DEBUG_QEMU=0

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="Release" set BUILD_CONFIG=Release
if /I "%~1"=="Debug" set BUILD_CONFIG=Debug
if /I "%~1"=="--debug" set DEBUG_QEMU=1
if /I "%~1"=="-d" set DEBUG_QEMU=1
shift
goto parse_args

:args_done

REM Check if EFI file exists
if not exist "%EFI_FILE%" (
    echo Error: EFI file not found at %EFI_FILE%
    echo Please build the kernel first using build.bat
    exit /b 1
)

set QEMU_EXE=C:\Program Files\qemu\qemu-system-x86_64.exe
set QEMU_DIR=C:\Program Files\qemu\

if not exist "%QEMU_EXE%" (
    echo Error: QEMU not found
    echo Please install QEMU from https://www.qemu.org/download/
    exit /b 1
)

REM Find OVMF firmware
set OVMF_CODE=%QEMU_DIR%share\edk2-x86_64-code.fd
set OVMF_VARS=%QEMU_DIR%share\edk2-i386-vars.fd

if not exist "%OVMF_CODE%" (
    echo Error: OVMF firmware not found at %OVMF_CODE%
    echo Expected the QEMU Windows package to include share\edk2-x86_64-code.fd
    exit /b 1
)

if not exist "%OVMF_VARS%" (
    echo Error: OVMF NVRAM template not found at %OVMF_VARS%
    echo Expected the QEMU Windows package to include share\edk2-i386-vars.fd
    exit /b 1
)

copy /y "%OVMF_VARS%" "%NVRAM_FILE%" >nul

REM Stage the ESP as a host directory and expose it to QEMU as a virtual FAT disk.
if exist "%ESP_ROOT%" rmdir /s /q "%ESP_ROOT%"
mkdir "%ESP_BOOT%"
copy /y "%EFI_FILE%" "%ESP_BOOT%\bootx64.efi" >nul

REM Copy userspace .exe files to ESP
set DOOM_EXE=userspace\rp2040-doom\build_vs\doom\%BUILD_CONFIG%\doom.exe
if not exist "%DOOM_EXE%" set DOOM_EXE=%BUILD_DIR%\doom\%BUILD_CONFIG%\doom.exe
set HELLO_EXE=%BUILD_DIR%\hello\%BUILD_CONFIG%\hello.exe
set FRAMEBUFFER_EXE=%BUILD_DIR%\framebuffer\%BUILD_CONFIG%\framebuffer.exe
set FRAMEBUFFER_TEXT_EXE=%BUILD_DIR%\framebuffer_text\%BUILD_CONFIG%\framebuffer_text.exe
set RAYTRACER_EXE=%BUILD_DIR%\raytracer\%BUILD_CONFIG%\raytracer.exe
set RAMFS_READER_EXE=%BUILD_DIR%\ramfs_reader\%BUILD_CONFIG%\ramfs_reader.exe
set SHELL_EXE=%BUILD_DIR%\shell\%BUILD_CONFIG%\shell.exe
set ESP_VKERNEL=%ESP_ROOT%\EFI\vkernel

if exist "%DOOM_EXE%" (
    mkdir "%ESP_VKERNEL%"
    copy /y "%DOOM_EXE%" "%ESP_VKERNEL%\doom.exe" >nul
    echo Copied %DOOM_EXE% to ESP
) else (
    echo Warning: doom.exe not found at %DOOM_EXE%
)

if exist "%HELLO_EXE%" (
    mkdir "%ESP_VKERNEL%"
    copy /y "%HELLO_EXE%" "%ESP_VKERNEL%\hello.exe" >nul
    echo Copied %HELLO_EXE% to ESP
) else (
    echo Warning: hello.exe not found at %HELLO_EXE%
)

if exist "%FRAMEBUFFER_EXE%" (
    if not exist "%ESP_VKERNEL%" mkdir "%ESP_VKERNEL%"
    copy /y "%FRAMEBUFFER_EXE%" "%ESP_VKERNEL%\framebuffer.exe" >nul
    echo Copied %FRAMEBUFFER_EXE% to ESP
) else (
    echo Warning: framebuffer.exe not found at %FRAMEBUFFER_EXE%
)

if exist "%FRAMEBUFFER_TEXT_EXE%" (
    if not exist "%ESP_VKERNEL%" mkdir "%ESP_VKERNEL%"
    copy /y "%FRAMEBUFFER_TEXT_EXE%" "%ESP_VKERNEL%\framebuffer_text.exe" >nul
    echo Copied %FRAMEBUFFER_TEXT_EXE% to ESP
) else (
    echo Warning: framebuffer_text.exe not found at %FRAMEBUFFER_TEXT_EXE%
)

if exist "%RAYTRACER_EXE%" (
    if not exist "%ESP_VKERNEL%" mkdir "%ESP_VKERNEL%"
    copy /y "%RAYTRACER_EXE%" "%ESP_VKERNEL%\raytracer.exe" >nul
    echo Copied %RAYTRACER_EXE% to ESP
) else (
    echo Warning: raytracer.exe not found at %RAYTRACER_EXE%
)

if exist "%RAMFS_READER_EXE%" (
    if not exist "%ESP_VKERNEL%" mkdir "%ESP_VKERNEL%"
    copy /y "%RAMFS_READER_EXE%" "%ESP_VKERNEL%\ramfs_reader.exe" >nul
    echo Copied %RAMFS_READER_EXE% to ESP
) else (
    echo Warning: ramfs_reader.exe not found at %RAMFS_READER_EXE%
)

if exist "%SHELL_EXE%" (
    if not exist "%ESP_VKERNEL%" mkdir "%ESP_VKERNEL%"
    copy /y "%SHELL_EXE%" "%ESP_VKERNEL%\shell.exe" >nul
    echo Copied %SHELL_EXE% to ESP
) else (
    echo Warning: shell.exe not found at %SHELL_EXE%
)

REM Copy DOOM WAD file (check multiple search locations)
set DOOM_WAD=
if exist "userspace\rp2040-doom\doom1.wad" set DOOM_WAD=userspace\rp2040-doom\doom1.wad
if exist "doom1.wad" set DOOM_WAD=doom1.wad
if not "%DOOM_WAD%"=="" (
    if not exist "%ESP_VKERNEL%" mkdir "%ESP_VKERNEL%"
    copy /y "%DOOM_WAD%" "%ESP_VKERNEL%\doom1.wad" >nul
    echo Copied %DOOM_WAD% to ESP
) else (
    echo Warning: doom1.wad not found - DOOM will not be able to find its IWAD
    echo   Place doom1.wad in the repo root or userspace\rp2040-doom\ to fix this
)

REM Run QEMU
echo.
echo Running vkernel in QEMU...
echo Press Ctrl+Alt+2 to switch to QEMU monitor
echo Press Ctrl+Alt+1 to switch back to VM
echo Type 'quit' in QEMU monitor to exit
echo.

set "DEBUG_ARGS="
if "%DEBUG_QEMU%"=="1" (
    set "DEBUG_ARGS=-s -S"
    echo GDB: gdb build_vs\vkernel.efi -ex "target remote localhost:1234"
)

"%QEMU_EXE%" ^
    -machine q35 ^
    -drive if=pflash,format=raw,readonly=on,file="%OVMF_CODE%" ^
    -drive if=pflash,format=raw,file="%NVRAM_FILE%" ^
    -drive if=ide,index=0,media=disk,format=raw,file="fat:rw:%ESP_ROOT%" ^
    -m 256M ^
    -net none ^
    -serial stdio ^
    -no-reboot ^
    -no-shutdown %DEBUG_ARGS%

endlocal
