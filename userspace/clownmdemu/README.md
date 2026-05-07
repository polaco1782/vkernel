ClownMDEmu for vkernel

This userspace app vendors the upstream ClownMDEmu core and wraps it with a minimal vkernel frontend.

Controls:
- Arrow keys: D-pad
- `A` `S` `D`: A B C
- `Q` `W` `E`: X Y Z
- `Enter`: Start
- `Backspace`: Mode
- `Tab`: hard reset
- `Escape`: quit

ROM staging:
- Put one or more ROMs in `userspace/clownmdemu/`
- Supported picker extensions: `.bin`, `.md`, `.gen`, `.smd`, `.32x`
- `run_qemu.sh` stages every matching ROM into the ESP root alongside the userspace binaries
- The app opens with a simple ROM picker backed by `kobj fs_list`

Picker controls:
- `Up` `Down`: move selection
- `Enter` or `Right`: open directory / load ROM
- `Backspace` or `Left`: parent directory
- `Tab`: refresh listing
- `Escape`: quit

Notes:
- The vendored core is AGPL-3.0-or-later. See `vendor/clownmdemu-core/LICENCE.txt`.
- vkernel's ramfs is currently read-only, so SRAM/save-file writes are not persisted.