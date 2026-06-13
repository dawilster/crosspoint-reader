# Hub pull-sync on launch (custom feature)

On boot, the reader fetches a manifest from the **CrossPoint Hub**, delta-downloads
any new or changed items (books into their folders, the daily card to `/sleep.bmp`),
then continues to the UI. This replaces the push-window approach with a
deterministic pull — no host on your LAN, works from any network.

## Setup

Create `/.crosspoint/hub.json` on the SD card:

```json
{
  "url": "https://crosspoint-hub.vercel.app",
  "token": "<your device token>",
  "device": "x4",
  "enabled": true
}
```

- `url` — your hub base URL.
- `token` — the device sync token (the secret half of your "special link"). Kept
  on the SD card, **not** compiled into firmware.
- `device` — identifier used for per-device queue delivery (default `x4`).
- `enabled` — set `false` to turn the feature off without removing the file.

Set the device's **Sleep Screen = Custom** so the pulled `/sleep.bmp` daily card shows.

Without this file (or with `enabled:false`), boot is unchanged.

## How it works

1. Connect Wi-Fi to the last-used network (interruptible — any button skips).
2. `GET {url}/api/device/manifest?token=…&device=…` → list of `{path, hash, type, policy}`.
3. For each entry whose `hash` differs from the local record
   (`/.crosspoint/synced.json`, path → hash), download
   `GET {url}/api/device/blob?token=…&id=<hash>` to its target path
   (`sleep` type → `/sleep.bmp`, otherwise `/<path>`), creating folders as needed.
4. Update the local record and continue to home. A short status screen shows
   progress.

Delta sync means a boot with nothing new is a single small manifest request.

## Throttle, Wi-Fi fallback, manual sync

- **Throttle:** Settings → System → **Hub Sync Interval (min)** (default 60, 0 = every
  boot). Within the interval, boot skips the sync silently (the last-sync time
  persists across deep sleep), so repeatedly opening/closing the reader doesn't
  re-sync and add latency between reads.
- **Wi-Fi fallback:** connects to the last-used network first, then tries every
  other saved network until one works.
- **Manual "Sync now":** in File Transfer mode (STA), the **Select** button pulls
  from the hub immediately over the already-connected Wi-Fi (ignores the throttle).

## Notes / limits (v1)

- HTTPS is verified against the firmware's CA bundle (same path OTA uses) — no
  insecure mode.
- **Queue** vs **mirror** policy comes from the hub per folder; v1 honours new/
  changed downloads for both. Mirror *deletion* (removing local files no longer
  in the manifest) is not yet implemented — a follow-up.
- Server-side `ack` isn't sent in v1; the local hash record already prevents
  re-downloads, so queue items aren't re-fetched.
- The window blocks boot briefly (before any book opens), avoiding SD contention
  with the reader.

## Build & flash

Same as the rest of the firmware (PlatformIO, ESP32-C3). Local builds on this Mac
are blocked by a broken Homebrew Python; build via GitHub Actions (`ci.yml`
produces `firmware.bin`) and flash via the web flasher's **Custom .bin** option
or the on-device SD update. See `docs/AUTO_SYNC_ON_BOOT.md` for the flashing
walkthrough.
