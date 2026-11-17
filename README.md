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
- Template journal (`m` on the main screen): applies a Markdown file from `~/Templates`,
  expanding `{{date}}` / `{{weekday}}` on entry. When the selected template asks for
  `{{weather}}`, the current weather is fetched in the background on entry (if online)
  and can be refreshed with `Alt+W`. Templates are selected in Settings; the six
  built-in ones are seeded on first start.
- Clipboard history (20 entries, 64 KB each) with an `Alt+V` overlay; `w` in that menu
  serves a browser UI over HTTP for viewing/editing entries.
- Past-journal browser with a floating calendar date filter (`t`) and search (`/`):
  live filtering over date/title/first line, Enter runs an incremental full-text scan
  with progress and any-key cancellation.
- Left-Win tap in the editor opens a shortcut menu mirroring the keyboard shortcuts.
- AI chat (问 ai): `Ctrl+W` in the editor opens a floating window with the current
  selection or draft as context; `a` on the main screen opens it full-screen.
  The bottom input sends to DeepSeek on Enter; answers render Markdown, and questions
  vs answers use different colors; PageUp/PageDown scroll the conversation.
  Multiple sessions are kept in memory: `Ctrl+J` starts a new one, `Ctrl+H` toggles
  a left-side session list (`↑`/`↓` select, `Enter` open, `d` delete, `n` new,
  `Tab` switches focus), and `Ctrl+L` clears the current session.
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
