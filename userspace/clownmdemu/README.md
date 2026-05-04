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
- Put the ROM at `userspace/clownmdemu/sonic1.bin`
- `run_qemu.sh` copies it into the ESP as `sonic1.bin`
- The app always loads `sonic1.bin`

Notes:
- The vendored core is AGPL-3.0-or-later. See `vendor/clownmdemu-core/LICENCE.txt`.
- vkernel's ramfs is currently read-only, so SRAM/save-file writes are not persisted.