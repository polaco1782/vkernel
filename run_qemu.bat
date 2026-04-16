@echo off
REM vkernel - UEFI Microkernel
REM Copyright (C) 2026 vkernel authors
REM
REM run_qemu.bat - Run vkernel in QEMU on Windows

setlocal

set BUILD_DIR=build_vs
set EFI_FILE=%BUILD_DIR%\vkernel.efi

REM Check if EFI file exists
if not exist "%EFI_FILE%" (
    echo Error: EFI file not found at %EFI_FILE%
    echo Please build the kernel first using build.bat
    exit /b 1
)

REM Find QEMU executable
where qemu-system-x86_64 >nul 2>nul
if %errorlevel% equ 0 (
    set QEMU=qemu-system-x86_64
) else if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" (
    set QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
) else if exist "C:\Program Files (x86)\qemu\qemu-system-x86_64.exe" (
    set QEMU="C:\Program Files (x86)\qemu\qemu-system-x86_64.exe"
) else (
    echo Error: QEMU not found
    echo Please install QEMU from https://www.qemu.org/download/
    exit /b 1
)

REM Find OVMF firmware
set OVMF_CODE=
set OVMF_VARS=

REM Check common OVMF locations
if exist "C:\Program Files\qemu\OVMF_CODE.fd" (
    set OVMF_CODE="C:\Program Files\qemu\OVMF_CODE.fd"
    set OVMF_VARS="C:\Program Files\qemu\OVMF_VARS.fd"
) else if exist "C:\Program Files (x86)\qemu\OVMF_CODE.fd" (
    set OVMF_CODE="C:\Program Files (x86)\qemu\OVMF_CODE.fd"
    set OVMF_VARS="C:\Program Files (x86)\qemu\OVMF_VARS.fd"
) else if exist "%CD%\OVMF\OVMF_CODE.fd" (
    set OVMF_CODE="%CD%\OVMF\OVMF_CODE.fd"
    set OVMF_VARS="%CD%\OVMF\OVMF_VARS.fd"
) else (
    echo Warning: OVMF firmware not found, attempting to download...
    echo Please download OVMF from:
    echo   https://github.com/tianocore/edk2/releases
    echo Or install via package manager
    echo.
    echo Continuing without OVMF...
)

REM Create FAT disk image with EFI file
set EFI_IMG=%BUILD_DIR%\efi_disk.img
echo Creating EFI disk image...

REM Create a 64MB FAT image
if exist "%EFI_IMG%" del "%EFI_IMG%"
qemu-img create -f raw "%EFI_IMG%" 64M >nul 2>nul

REM Format as FAT and copy EFI file
REM Note: This requires mtools on Windows
REM Alternative: Use a pre-made FAT image

REM Run QEMU
echo.
echo Running vkernel in QEMU...
echo Press Ctrl+Alt+2 to switch to QEMU monitor
echo Press Ctrl+Alt+1 to switch back to VM
echo Type 'quit' in QEMU monitor to exit
echo.

%QEMU% ^
    -drive if=pflash,format=raw,readonly=on,file=%OVMF_CODE% ^
    -drive if=pflash,format=raw,file=%OVMF_VARS% ^
    -hda "%EFI_IMG%" ^
    -m 256M ^
    -net none ^
    -serial stdio ^
    -no-reboot ^
    -no-shutdown

endlocal
