# pjournal-lvgl

Linux framebuffer/LVGL port of `pjournal-esp32` for the 1024x600 target device.

Implemented target changes:

- LVGL framebuffer UI (`/dev/fb0` by default), evdev keyboard input.
- TTF/OTF vector fonts through FreeType, scanned from `/root/.fonts`.
- Bluetooth management and voice recognition are intentionally absent.
- Typing sound is absent because the target exposes no usable `/dev/snd` playback device.
- WiFi is managed through `wpa_supplicant`/`wpa_cli` on `wlan0`.
- Markdown themes are configurable: dark is default, light is available.
- Font size is user-adjustable rather than fixed bitmap slots.
- Journal save location is configurable.
- Static ARM build target is provided.

Build for the target:

```sh
cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=toolchain-armv7.cmake -DPJOURNAL_TARGET_ARM=ON -DPJOURNAL_STATIC=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm -j
```

Deploy:

```sh
scp build-arm/pjournal-lvgl root@192.168.50.251:/root/pjournal-lvgl
ssh root@192.168.50.251 /root/pjournal-lvgl
```

Useful runtime environment variables:

- `PJOURNAL_FB=/dev/fb0`
- `PJOURNAL_INPUT=/dev/input/event0`
- `PJOURNAL_WLAN=wlan0`
