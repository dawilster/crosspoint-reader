# Auto-Sync on Boot (custom feature)

A local, opt-in addition to CrossPoint that opens the Wi-Fi file-transfer server
**automatically at startup** for a short, bounded window, then shuts it down and
continues to the normal UI. A watching host on your LAN pushes new books during
that window with **zero interaction on the device** — you just turn the reader on.

> Upstream intentionally keeps networking manual (see `SCOPE.md`). This feature is
> a personal fork addition and reuses the existing transfer server verbatim; it is
> off by default.

## How it works

1. On a genuine boot (cold boot / power-button wake — not silent/panic/recovery
   reboots), if **Auto-Sync on Boot** is enabled, the device:
   - connects Wi-Fi (STA) to the last-used network from `WifiCredentialStore`,
   - starts the existing `CrossPointWebServer` (HTTP `:80`, WebSocket `:81`),
   - shows a "Auto-Sync — Ready at <ip> — Closing in ~N min" screen,
   - services uploads for the configured window, then stops the server and powers
     Wi-Fi off and proceeds to home/reader as usual.
2. On your computer, `crosspoint_sync.py --serve` (in the xteink toolkit) detects
   port 80 opening and pushes everything new from `~/CrossPointInbox`.

Net result: drop books on the computer anytime → turn the reader on in the
morning → books arrive before you start reading. Press **any button** to end the
window early.

## Settings (System category, also in the web settings UI)

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `autoSyncOnBoot` | toggle | off | Master switch for the feature |
| `autoSyncMinutes` | value (1–10) | 2 | How long to hold the window open |
| `autoSyncChargerOnly` | toggle | off | Only run when on USB/charger power |

Tip: set `autoSyncChargerOnly = on` if you want it to sync only while charging
(e.g. overnight) — zero battery impact during normal use.

## What changed (the diff)

- `src/network/AutoSyncOnBoot.{h,cpp}` — **new**. The boot-time sync window:
  charger gate, Wi-Fi connect (interruptible), start/stop the transfer server,
  countdown status screen, watchdog-safe service loop.
- `src/main.cpp` — include + one guarded call to `AutoSyncOnBoot::run(renderer, gpio)`
  inserted after the boot-presentation logic and before normal activity routing
  (so home/reader resume still works unchanged afterward).
- `src/CrossPointSettings.h` — three new `uint8_t` fields + min/max minute constants.
- `src/SettingsList.h` — three `SettingInfo` entries (auto-exposed to the web API).
- `lib/I18n/translations/english.yaml` — three display strings.

The web settings API enumerates `SettingsList.h` automatically, so the three
options appear in the on-device Settings and at `http://<ip>/settings` with no
extra wiring.

## Build & flash

PlatformIO, ESP32-C3, 16MB flash. From the repo root:

```bash
# build
pio run -e default

# flash over USB (device connected; ESP32-C3 USB-CDC)
pio run -e default -t upload

# serial logs (115200) to watch the AUTOSYNC tag
pio device monitor -b 115200
```

You can also use the web flasher / `esptool.py` as with any CrossPoint build.
Recovery: hold **UP + POWER** at boot for the SD firmware picker; the ESP32-C3
ROM bootloader + web flasher recover a bad flash.

## Enable + use

1. Flash this firmware.
2. Connect the device to Wi-Fi once (normal File Transfer flow) so a network is
   saved in `WifiCredentialStore`.
3. Settings → System → **Auto-Sync on Boot = On** (adjust window / charger-only).
4. On the computer, run the watcher so the push side is always ready:
   ```bash
   cp ~/workspace/xteink/com.crosspoint.sync.plist ~/Library/LaunchAgents/
   launchctl load ~/Library/LaunchAgents/com.crosspoint.sync.plist
   ```
5. Drop books in `~/CrossPointInbox`, turn the reader on, watch them land.

## Design notes / caveats

- **Boot latency:** the window adds time to startup *only when enabled*. The
  countdown screen makes it obvious, and any button ends it instantly.
- **Skip-on-wake-press:** the button that wakes the device is ignored for the
  first ~600 ms so it doesn't instantly cancel.
- **Skipped reboots:** silent (heap-defrag) restarts, panic reboots, and recovery
  mode never trigger the window — avoids loops and surprises.
- **Watchdog:** the service loop resets the task WDT like the transfer activity,
  so large uploads keep the device responsive.
- **Battery:** a 2-min window draws ~5 mAh (~0.3% of a charge) — negligible; use
  `autoSyncChargerOnly` if you want it truly free.
- **Pull alternative:** if you'd rather the device fetch from a cloud OPDS feed
  instead of a host pushing, that's a separate (larger) change — this feature is
  the minimal "push window" approach.
