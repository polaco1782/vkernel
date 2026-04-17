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

REM Copy userspace hello.exe to ESP
set HELLO_EXE=%BUILD_DIR%\hello\Debug\hello.exe
set ESP_VKERNEL=%ESP_ROOT%\EFI\vkernel

if exist "%HELLO_EXE%" (
    mkdir "%ESP_VKERNEL%"
    copy /y "%HELLO_EXE%" "%ESP_VKERNEL%\hello.exe" >nul
    echo Copied %HELLO_EXE% to ESP
) else (
    echo Warning: hello.exe not found at %HELLO_EXE%
)

REM Run QEMU
echo.
echo Running vkernel in QEMU...
echo Press Ctrl+Alt+2 to switch to QEMU monitor
echo Press Ctrl+Alt+1 to switch back to VM
echo Type 'quit' in QEMU monitor to exit
echo.

"%QEMU_EXE%" ^
    -machine q35 ^
    -drive if=pflash,format=raw,readonly=on,file="%OVMF_CODE%" ^
    -drive if=pflash,format=raw,file="%NVRAM_FILE%" ^
    -drive if=ide,index=0,media=disk,format=raw,file="fat:rw:%ESP_ROOT%" ^
    -m 256M ^
    -net none ^
    -serial stdio ^
    -no-reboot ^
    -no-shutdown

endlocal
