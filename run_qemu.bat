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

REM Copy userspace .vbin files to ESP
set DOOM_VBIN=%BUILD_DIR%\doom\%BUILD_CONFIG%\doom.vbin
if not exist "%DOOM_VBIN%" set DOOM_VBIN=userspace\rp2040-doom\build_vs\doom\%BUILD_CONFIG%\doom.vbin
set HELLO_VBIN=%BUILD_DIR%\hello\%BUILD_CONFIG%\hello.vbin
set FRAMEBUFFER_VBIN=%BUILD_DIR%\framebuffer\%BUILD_CONFIG%\framebuffer.vbin
set FRAMEBUFFER_TEXT_VBIN=%BUILD_DIR%\framebuffer_text\%BUILD_CONFIG%\framebuffer_text.vbin
set RAYTRACER_VBIN=%BUILD_DIR%\raytracer\%BUILD_CONFIG%\raytracer.vbin
set RAMFS_READER_VBIN=%BUILD_DIR%\ramfs_reader\%BUILD_CONFIG%\ramfs_reader.vbin
set SHELL_VBIN=%BUILD_DIR%\shell\%BUILD_CONFIG%\shell.vbin
set ESP_VKERNEL=%ESP_ROOT%\EFI\vkernel

if exist "%DOOM_VBIN%" (
    mkdir "%ESP_VKERNEL%"
    copy /y "%DOOM_VBIN%" "%ESP_VKERNEL%\doom.vbin" >nul
    echo Copied %DOOM_VBIN% to ESP
) else (
    echo Warning: doom.vbin not found at %DOOM_VBIN%
)

if exist "%HELLO_VBIN%" (
    mkdir "%ESP_VKERNEL%"
    copy /y "%HELLO_VBIN%" "%ESP_VKERNEL%\hello.vbin" >nul
    echo Copied %HELLO_VBIN% to ESP
) else (
    echo Warning: hello.vbin not found at %HELLO_VBIN%
)

if exist "%FRAMEBUFFER_VBIN%" (
    if not exist "%ESP_VKERNEL%" mkdir "%ESP_VKERNEL%"
    copy /y "%FRAMEBUFFER_VBIN%" "%ESP_VKERNEL%\framebuffer.vbin" >nul
    echo Copied %FRAMEBUFFER_VBIN% to ESP
) else (
    echo Warning: framebuffer.vbin not found at %FRAMEBUFFER_VBIN%
)

if exist "%FRAMEBUFFER_TEXT_VBIN%" (
    if not exist "%ESP_VKERNEL%" mkdir "%ESP_VKERNEL%"
    copy /y "%FRAMEBUFFER_TEXT_VBIN%" "%ESP_VKERNEL%\framebuffer_text.vbin" >nul
    echo Copied %FRAMEBUFFER_TEXT_VBIN% to ESP
) else (
    echo Warning: framebuffer_text.vbin not found at %FRAMEBUFFER_TEXT_VBIN%
)

if exist "%RAYTRACER_VBIN%" (
    if not exist "%ESP_VKERNEL%" mkdir "%ESP_VKERNEL%"
    copy /y "%RAYTRACER_VBIN%" "%ESP_VKERNEL%\raytracer.vbin" >nul
    echo Copied %RAYTRACER_VBIN% to ESP
) else (
    echo Warning: raytracer.vbin not found at %RAYTRACER_VBIN%
)

if exist "%RAMFS_READER_VBIN%" (
    if not exist "%ESP_VKERNEL%" mkdir "%ESP_VKERNEL%"
    copy /y "%RAMFS_READER_VBIN%" "%ESP_VKERNEL%\ramfs_reader.vbin" >nul
    echo Copied %RAMFS_READER_VBIN% to ESP
) else (
    echo Warning: ramfs_reader.vbin not found at %RAMFS_READER_VBIN%
)

if exist "%SHELL_VBIN%" (
    if not exist "%ESP_VKERNEL%" mkdir "%ESP_VKERNEL%"
    copy /y "%SHELL_VBIN%" "%ESP_VKERNEL%\shell.vbin" >nul
    echo Copied %SHELL_VBIN% to ESP
) else (
    echo Warning: shell.vbin not found at %SHELL_VBIN%
)

REM Copy DOOM WAD file (check multiple search locations)
set DOOM_WAD=
if exist "userspace\rp2040-doom\doom2.wad" set DOOM_WAD=userspace\rp2040-doom\doom2.wad
if exist "doom2.wad" set DOOM_WAD=doom2.wad
if not "%DOOM_WAD%"=="" (
    if not exist "%ESP_VKERNEL%" mkdir "%ESP_VKERNEL%"
    copy /y "%DOOM_WAD%" "%ESP_VKERNEL%\doom2.wad" >nul
    echo Copied %DOOM_WAD% to ESP
) else (
    echo Warning: doom2.wad not found - DOOM will not be able to find its IWAD
    echo   Place doom2.wad in the repo root or userspace\rp2040-doom\ to fix this
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
    -device sb16 ^
    -serial stdio ^
    -no-reboot ^
    -no-shutdown %DEBUG_ARGS%

endlocal
