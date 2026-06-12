#pragma once

// Auto-sync on boot
// ------------------
// Opens the Wi-Fi file-transfer web server (ports 80/81) automatically during
// startup for a short, bounded window, then tears it down and lets the normal
// boot routing continue. A watching host on the LAN (e.g. crosspoint_sync.py
// --serve) detects port 80 opening and pushes any new books with zero on-device
// interaction.
//
// This is deliberately gated behind the `autoSyncOnBoot` setting (default off)
// and is skipped on recovery/panic/silent reboots by the caller. The window is
// interruptible: pressing any button finishes it early.
//
// The firmware upstream intentionally has no background networking; this is an
// opt-in local addition that reuses the existing transfer server verbatim.

class GfxRenderer;
class HalGPIO;

namespace AutoSyncOnBoot {

// Run the boot-time sync window (blocking). No-op when the setting is disabled
// or (when charger-only is set) the device is not on USB power. Always leaves
// Wi-Fi powered off on return.
void run(GfxRenderer& renderer, HalGPIO& gpio);

}  // namespace AutoSyncOnBoot
