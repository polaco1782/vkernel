minimp3 player for vkernel

This userspace app vendors the upstream minimp3 decoder and wraps it with the
file-picker UI style introduced in clownmdemu.

Usage:
- Build the disk image with `make DEBUG=1 disk`
- Put one or more `.mp3` files in `userspace/minimp3/tracks/`
- The disk build stages those files into the ESP so the app can browse and play them

Browser controls:
- `Up` `Down`: move selection
- `Enter` or `Right`: open directory / play file
- `Backspace` or `Left`: parent directory
- `Tab`: refresh listing
- `Escape`: quit

Playback controls:
- `Backspace` or `Left`: stop playback and return to the browser
- `Tab`: restart the current track
- `Escape`: quit

Notes:
- The player currently accepts `.mp3` files only.
- The vendored decoder is CC0. See `vendor/LICENSE`.