#pragma once

// Hub pull-sync on launch
// -----------------------
// On boot, fetch a manifest from the CrossPoint hub, delta-download any new or
// changed items (books into their folders, the daily card to /sleep.bmp), then
// continue to the normal UI. Replaces the old push-window approach with a
// deterministic pull.
//
// Configuration lives on the SD card at /.crosspoint/hub.json (NOT in firmware,
// so the token stays off the repo):
//   { "url": "https://crosspoint-hub.vercel.app",
//     "token": "<device token>", "device": "x4", "enabled": true }
//
// Delta sync is driven by a local record (/.crosspoint/synced.json mapping
// path -> last-synced content hash); only items whose hash changed are fetched.

class GfxRenderer;
class HalGPIO;

namespace HubSync {

// Run the boot-time pull (blocking, with a status screen). No-op when there is
// no config or it's disabled. Always leaves Wi-Fi powered off on return.
void run(const GfxRenderer& renderer, HalGPIO& gpio);

}  // namespace HubSync
