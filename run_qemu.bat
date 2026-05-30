@echo off
REM vkernel - UEFI Microkernel
REM Copyright (C) 2026 vkernel authors
REM
REM run_qemu.bat - Run vkernel in QEMU on Windows
REM
REM Stages an EFI System Partition tree under build_vs\esp, creates a GPT
REM VHD with a FAT32 ESP, copies the staged files into it, then boots QEMU
REM with the disk attached as a virtio block device so the kernel can mount
REM it through the block + FAT32 stack.

setlocal EnableExtensions EnableDelayedExpansion

set "BUILD_DIR=build_vs"
set "EFI_FILE=%BUILD_DIR%\vkernel.efi"
set "ESP_ROOT=%BUILD_DIR%\esp"
set "ESP_BOOT=%ESP_ROOT%\EFI\BOOT"
set "ESP_BIN=%ESP_ROOT%\bin"
set "ESP_BOOTDATA=%ESP_ROOT%\boot"
set "ESP_DATA=%ESP_ROOT%\data"
set "ESP_DATA_SHELL=%ESP_DATA%\shell"
set "ESP_DATA_VKGUI=%ESP_DATA%\vkgui"
set "ESP_DATA_VKGUI_PLUGINS=%ESP_DATA_VKGUI%\plugins"
set "ESP_DATA_DOOM=%ESP_DATA%\doom"
set "ESP_DATA_QUAKE_ID1=%ESP_DATA%\quake\id1"
set "ESP_DATA_QUAKE_ZEUSBOT=%ESP_DATA%\quake\zeusbot"
set "ESP_DATA_MODPLAY=%ESP_DATA%\modplay"
set "ESP_DATA_ROTOZOOM=%ESP_DATA%\rotozoom"
set "ESP_DATA_CLOWNMDEMU=%ESP_DATA%\clownmdemu\roms"
set "ESP_DATA_VNES=%ESP_DATA%\vnes\roms"
set "ESP_DATA_SNES9X=%ESP_DATA%\snes9x\roms"
set "ESP_DATA_MINIMP3=%ESP_DATA%\minimp3\tracks"
set "BOOT_IMG=%BUILD_DIR%\vkernel_boot.vhd"
set "NVRAM_FILE=%BUILD_DIR%\ovmf_vars.fd"
set "BUILD_CONFIG=Debug"
set "DEBUG_QEMU=0"
set "DISK_MB=512"
set "VOLUME_LABEL=VKRN%RANDOM%"
set "QEMU_EXE=C:\Program Files\qemu\qemu-system-x86_64.exe"
set "QEMU_DIR=C:\Program Files\qemu"
set "OVMF_CODE=%QEMU_DIR%\share\edk2-x86_64-code.fd"
set "OVMF_VARS=%QEMU_DIR%\share\edk2-i386-vars.fd"
set "MANIFEST_FILE=%BUILD_DIR%\vkgui_apps.txt"
set "DISKPART_CREATE_SCRIPT=%TEMP%\vkernel_diskpart_create_%RANDOM%%RANDOM%.txt"
set "DISKPART_DETACH_SCRIPT=%TEMP%\vkernel_diskpart_detach_%RANDOM%%RANDOM%.txt"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="Release" set "BUILD_CONFIG=Release"
if /I "%~1"=="Debug" set "BUILD_CONFIG=Debug"
if /I "%~1"=="--debug" set "DEBUG_QEMU=1"
if /I "%~1"=="-d" set "DEBUG_QEMU=1"
shift
goto parse_args

:args_done

for %%I in ("%BOOT_IMG%") do set "BOOT_IMG_ABS=%%~fI"

if not exist "%EFI_FILE%" (
    echo Error: EFI file not found at %EFI_FILE%
    echo Build the kernel first.
    exit /b 1
)

if not exist "%QEMU_EXE%" (
    echo Error: QEMU not found at %QEMU_EXE%
    echo Install QEMU for Windows from https://www.qemu.org/download/
    exit /b 1
)

if not exist "%OVMF_CODE%" (
    echo Error: OVMF firmware not found at %OVMF_CODE%
    exit /b 1
)

if not exist "%OVMF_VARS%" (
    echo Error: OVMF NVRAM template not found at %OVMF_VARS%
    exit /b 1
)

copy /y "%OVMF_VARS%" "%NVRAM_FILE%" >nul
if errorlevel 1 (
    echo Error: failed to copy OVMF vars template to %NVRAM_FILE%
    exit /b 1
)

if exist "%ESP_ROOT%" rmdir /s /q "%ESP_ROOT%"
mkdir "%ESP_BOOT%" || exit /b 1
mkdir "%ESP_BIN%" || exit /b 1
mkdir "%ESP_BOOTDATA%" || exit /b 1
mkdir "%ESP_DATA_SHELL%" || exit /b 1
mkdir "%ESP_DATA_VKGUI%" || exit /b 1
mkdir "%ESP_DATA_VKGUI_PLUGINS%" || exit /b 1
mkdir "%ESP_DATA_DOOM%" || exit /b 1
mkdir "%ESP_DATA_QUAKE_ID1%" || exit /b 1
mkdir "%ESP_DATA_QUAKE_ZEUSBOT%" || exit /b 1
mkdir "%ESP_DATA_MODPLAY%" || exit /b 1
mkdir "%ESP_DATA_ROTOZOOM%" || exit /b 1
mkdir "%ESP_DATA_CLOWNMDEMU%" || exit /b 1
mkdir "%ESP_DATA_VNES%" || exit /b 1
mkdir "%ESP_DATA_SNES9X%" || exit /b 1
mkdir "%ESP_DATA_MINIMP3%" || exit /b 1

copy /y "%EFI_FILE%" "%ESP_BOOT%\bootx64.efi" >nul
if errorlevel 1 (
    echo Error: failed to stage %EFI_FILE%
    exit /b 1
)

if exist "%MANIFEST_FILE%" del /q "%MANIFEST_FILE%"
type nul > "%MANIFEST_FILE%"

powershell -NoProfile -Command ^
    "$root = (Get-Location).Path; $config = '%BUILD_CONFIG%'; $binDir = (Resolve-Path '%ESP_BIN%').Path; $manifest = [System.IO.Path]::GetFullPath('%MANIFEST_FILE%'); $manifestDest = [System.IO.Path]::GetFullPath('%ESP_DATA_VKGUI%\vkgui_apps.txt');" ^
    "function Collect-Vbins([string]$base, [int]$priority) { if (-not (Test-Path -LiteralPath $base)) { return @() }; Get-ChildItem -LiteralPath $base -Recurse -Filter *.vbin | Where-Object { $_.FullName -notlike '*\esp\*' -and $_.FullName -notlike '*\obj\*' -and $_.Directory.Name -ieq $config } | ForEach-Object { [pscustomobject]@{ Name = $_.Name; FullName = $_.FullName; Priority = $priority } } };" ^
    "$candidates = @();" ^
    "$candidates += Collect-Vbins (Join-Path $root 'build_clang') 0;" ^
    "$candidates += Collect-Vbins (Join-Path $root 'build_vs') 1;" ^
    "$userspaceRoot = Join-Path $root 'userspace';" ^
    "if (Test-Path -LiteralPath $userspaceRoot) { $candidates += Get-ChildItem -LiteralPath $userspaceRoot -Recurse -Filter *.vbin | Where-Object { $_.FullName -match '[\\/]userspace[\\/][^\\/]+[\\/][^\\/]+\.vbin$' } | ForEach-Object { [pscustomobject]@{ Name = $_.Name; FullName = $_.FullName; Priority = 2 } } };" ^
    "$vbins = $candidates | Sort-Object Priority, FullName | Group-Object Name | ForEach-Object { $_.Group | Select-Object -First 1 } | Sort-Object Name;" ^
    "foreach ($file in $vbins) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $binDir $file.Name) -Force; Write-Output ('Staged bin\' + $file.Name + ' from ' + $file.FullName) };" ^
    "$lines = $vbins | ForEach-Object { '/bin/' + $_.Name } | Sort-Object -Unique; Set-Content -LiteralPath $manifest -Value $lines; Set-Content -LiteralPath $manifestDest -Value $lines"
if errorlevel 1 (
    echo Error: failed to stage userspace .vbin files
    exit /b 1
)

call :copy_if_exists "userspace\doom\doom1.wad" "data\doom\doom1.wad"
if errorlevel 1 exit /b 1
call :copy_if_exists "userspace\doom\doom2.wad" "data\doom\doom2.wad"
if errorlevel 1 exit /b 1
call :copy_if_exists "userspace\shell\shell_exec.txt" "data\shell\shell.txt"
if errorlevel 1 exit /b 1
call :copy_if_exists "userspace\MODPlay\makemove.mod" "data\modplay\makemove.mod"
if errorlevel 1 exit /b 1
call :copy_if_exists "userspace\MODPlay\UNREALPM.S3M" "data\modplay\UNREALPM.S3M"
if errorlevel 1 exit /b 1
call :copy_if_exists "userspace\rotozoom\head.bmp" "data\rotozoom\head.bmp"
if errorlevel 1 exit /b 1
call :copy_if_exists "userspace\quake\pak0.pak" "data\quake\id1\pak0.pak"
if errorlevel 1 exit /b 1
call :copy_if_exists "userspace\quake\progs.dat" "data\quake\zeusbot\progs.dat"
if errorlevel 1 exit /b 1
call :copy_if_exists "userspace\quake\zeus_pak0.pak" "data\quake\zeusbot\pak0.pak"
if errorlevel 1 exit /b 1

for %%F in ("userspace\vkgui\runtime_plugins\*.vplg") do (
    if exist "%%~fF" call :copy_if_exists "%%~fF" "data\vkgui\plugins\%%~nxF"
    if errorlevel 1 exit /b 1
)

call :copy_if_exists "build\symbols\build\vkernel.elf.map" "boot\vkernel.elf.map"
if errorlevel 1 exit /b 1
call :copy_if_exists "build\symbols\build\vkernel.elf.lines" "boot\vkernel.elf.lines"
if errorlevel 1 exit /b 1
if exist "build\symbols\userspace" (
    for /r "build\symbols\userspace" %%F in (*.vbin.lines) do (
        call :copy_if_exists "%%~fF" "bin\%%~nxF"
        if errorlevel 1 exit /b 1
    )
)

for %%E in (bin gen smd 32x md) do (
    for %%F in ("userspace\clownmdemu\roms\*.%%E") do (
        if exist "%%~fF" call :copy_if_exists "%%~fF" "data\clownmdemu\roms\%%~nxF"
        if errorlevel 1 exit /b 1
    )
)

for %%F in ("userspace\vnes\roms\*.nes") do (
    if exist "%%~fF" call :copy_if_exists "%%~fF" "data\vnes\roms\%%~nxF"
    if errorlevel 1 exit /b 1
)

for %%E in (smc sfc) do (
    for %%F in ("userspace\snes9x\roms\*.%%E") do (
        if exist "%%~fF" call :copy_if_exists "%%~fF" "data\snes9x\roms\%%~nxF"
        if errorlevel 1 exit /b 1
    )
)

for %%F in ("userspace\minimp3\tracks\*.mp3") do (
    if exist "%%~fF" call :copy_if_exists "%%~fF" "data\minimp3\tracks\%%~nxF"
    if errorlevel 1 exit /b 1
)

powershell -NoProfile -Command ^
    "$src = Join-Path (Get-Location).Path 'userspace\quake\reaperfx'; if (-not (Test-Path -LiteralPath $src)) { exit 0 }; $dst = [System.IO.Path]::GetFullPath('%ESP_ROOT%\data\quake\reaperfx'); [System.IO.Directory]::CreateDirectory($dst) | Out-Null; Copy-Item -LiteralPath (Join-Path $src '*') -Destination $dst -Recurse -Force"
if errorlevel 1 (
    echo Error: failed to stage userspace\quake\reaperfx
    exit /b 1
)

powershell -NoProfile -Command "Get-Process | Where-Object { $_.ProcessName -like 'qemu-system-*' -or $_.ProcessName -like 'qemu*' } | Stop-Process -Force"
timeout /t 1 /nobreak >nul
if exist "%BOOT_IMG%" call :detach_vhd >nul 2>nul
if exist "%BOOT_IMG%" del /q "%BOOT_IMG%"
if exist "%BOOT_IMG%" (
    echo Error: unable to remove %BOOT_IMG%. It may still be in use.
    call :cleanup_temp_files
    exit /b 1
)

> "%DISKPART_CREATE_SCRIPT%" echo create vdisk file="%BOOT_IMG_ABS%" maximum=%DISK_MB% type=fixed
>> "%DISKPART_CREATE_SCRIPT%" echo select vdisk file="%BOOT_IMG_ABS%"
>> "%DISKPART_CREATE_SCRIPT%" echo attach vdisk
>> "%DISKPART_CREATE_SCRIPT%" echo convert gpt
>> "%DISKPART_CREATE_SCRIPT%" echo create partition primary
>> "%DISKPART_CREATE_SCRIPT%" echo format fs=fat32 quick label=%VOLUME_LABEL%
>> "%DISKPART_CREATE_SCRIPT%" echo exit

> "%DISKPART_DETACH_SCRIPT%" echo select vdisk file="%BOOT_IMG_ABS%"
>> "%DISKPART_DETACH_SCRIPT%" echo select partition 1
>> "%DISKPART_DETACH_SCRIPT%" echo set id=c12a7328-f81f-11d2-ba4b-00a0c93ec93b override
>> "%DISKPART_DETACH_SCRIPT%" echo detach vdisk
>> "%DISKPART_DETACH_SCRIPT%" echo exit

diskpart /s "%DISKPART_CREATE_SCRIPT%" >nul
if errorlevel 1 (
    echo Error: failed to create and format GPT disk image.
    echo This step may require an elevated terminal on Windows.
    goto :fail_cleanup
)

powershell -NoProfile -Command ^
    "$src = (Resolve-Path '%ESP_ROOT%').Path; $vol = Get-Volume | Where-Object { $_.FileSystem -eq 'FAT32' -and $_.FileSystemLabel -eq '%VOLUME_LABEL%' } | Select-Object -First 1; if (-not $vol) { throw 'Mounted FAT32 volume not found'; }; $dst = $vol.Path; Get-ChildItem -LiteralPath $src -Recurse -Force | ForEach-Object { $relative = $_.FullName.Substring($src.Length).TrimStart('\'); if ([string]::IsNullOrEmpty($relative)) { return }; $target = $dst + $relative; if ($_.PSIsContainer) { [System.IO.Directory]::CreateDirectory($target) | Out-Null } else { $dir = [System.IO.Path]::GetDirectoryName($target); if ($dir) { [System.IO.Directory]::CreateDirectory($dir) | Out-Null }; [System.IO.File]::Copy($_.FullName, $target, $true) } }"
if errorlevel 1 (
    echo Error: failed to copy staged ESP files into %BOOT_IMG%
    goto :fail_copy
)

call :detach_vhd
if errorlevel 1 (
    echo Error: failed to detach %BOOT_IMG%
    goto :fail_cleanup
)

call :cleanup_temp_files

set "DEBUG_ARGS="
if "%DEBUG_QEMU%"=="1" (
    set "DEBUG_ARGS=-s -S"
    echo GDB debug workflow:
    echo   gdb build_vs\vkernel.efi -ex "target remote localhost:1234"
)

echo.
echo Running vkernel in QEMU...
echo Press Ctrl+Alt+2 to switch to QEMU monitor
echo Press Ctrl+Alt+1 to switch back to VM
echo Type 'quit' in QEMU monitor to exit
echo.
echo Mouse: press Ctrl+Alt+G to grab/release the mouse inside the VM.
echo.

"%QEMU_EXE%" ^
    -machine q35 ^
    -vga virtio ^
    -smp 4 ^
    -drive if=pflash,format=raw,readonly=on,file="%OVMF_CODE%" ^
    -drive if=pflash,format=raw,file="%NVRAM_FILE%" ^
    -drive if=none,id=bootdisk,format=vpc,file="%BOOT_IMG%" ^
    -device virtio-blk-pci,drive=bootdisk,bootindex=0,disable-modern=off,disable-legacy=off ^
    -m 512M ^
    -net none ^
    -device AC97 ^
    -serial stdio ^
    -no-reboot ^
    -no-shutdown %DEBUG_ARGS%

endlocal
exit /b %ERRORLEVEL%

:copy_if_exists
if not exist "%~1" goto :eof
set "COPY_DEST=%ESP_ROOT%\%~2"
for %%I in ("%COPY_DEST%") do if not exist "%%~dpI" mkdir "%%~dpI" >nul 2>nul
copy /y "%~1" "%COPY_DEST%" >nul
if errorlevel 1 (
    echo Error: failed to stage %~1
    exit /b 1
)
echo Staged %~2
set "COPY_DEST="
goto :eof

:detach_vhd
if not exist "%DISKPART_DETACH_SCRIPT%" (
    > "%DISKPART_DETACH_SCRIPT%" echo select vdisk file="%BOOT_IMG_ABS%"
    >> "%DISKPART_DETACH_SCRIPT%" echo detach vdisk
    >> "%DISKPART_DETACH_SCRIPT%" echo exit
)
diskpart /s "%DISKPART_DETACH_SCRIPT%" >nul
goto :eof

:fail_copy
call :detach_vhd >nul 2>nul

:fail_cleanup
call :cleanup_temp_files
exit /b 1

:cleanup_temp_files
if exist "%DISKPART_CREATE_SCRIPT%" del /q "%DISKPART_CREATE_SCRIPT%"
if exist "%DISKPART_DETACH_SCRIPT%" del /q "%DISKPART_DETACH_SCRIPT%"
if exist "%MANIFEST_FILE%" del /q "%MANIFEST_FILE%"
goto :eof
