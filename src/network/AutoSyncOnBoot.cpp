#include "AutoSyncOnBoot.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <memory>
#include <string>

#include "../CrossPointSettings.h"
#include "../WifiCredentialStore.h"
#include "../fontIds.h"
#include "CrossPointWebServer.h"

namespace {

constexpr const char* TAG = "AUTOSYNC";

// Draw a simple centered status screen. `line2` may be empty.
void drawStatus(GfxRenderer& renderer, const char* title, const std::string& line1, const std::string& line2) {
  const int h = renderer.getScreenHeight();
  renderer.clearScreen();
  renderer.drawCenteredText(NOTOSANS_16_FONT_ID, h / 2 - 70, title);
  if (!line1.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, h / 2 - 16, line1.c_str());
  }
  if (!line2.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, h / 2 + 14, line2.c_str());
  }
  renderer.drawCenteredText(UI_12_FONT_ID, h / 2 + 80, "Press any key to finish");
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// Power Wi-Fi fully down. Safe to call regardless of current state.
void wifiOff() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

// Ignore the button press that woke the device so it doesn't instantly skip.
void settleButtons(HalGPIO& gpio) {
  const unsigned long start = millis();
  while (millis() - start < 600) {
    gpio.update();
    delay(20);
  }
}

}  // namespace

namespace AutoSyncOnBoot {

void run(GfxRenderer& renderer, HalGPIO& gpio) {
  if (!SETTINGS.autoSyncOnBoot) {
    return;
  }

  gpio.update();
  if (SETTINGS.autoSyncChargerOnly && !gpio.isUsbConnected()) {
    LOG_INF(TAG, "Auto-sync skipped: charger-only is set and device is on battery");
    return;
  }

  // Need a saved network to connect to. The last successfully-connected SSID is
  // the natural choice (the same one transfer mode auto-selects).
  WIFI_STORE.loadFromFile();
  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  if (ssid.empty()) {
    LOG_INF(TAG, "Auto-sync skipped: no saved Wi-Fi network");
    drawStatus(renderer, "Auto-Sync", "No saved Wi-Fi network", "");
    delay(1200);
    return;
  }
  const WifiCredential* cred = WIFI_STORE.findCredential(ssid);

  LOG_INF(TAG, "Auto-sync starting; connecting to '%s'", ssid.c_str());
  drawStatus(renderer, "Auto-Sync", "Connecting to " + ssid, "");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("crosspoint");
  WiFi.setAutoReconnect(true);
  if (cred != nullptr && !cred->password.empty()) {
    WiFi.begin(ssid.c_str(), cred->password.c_str());
  } else {
    WiFi.begin(ssid.c_str());
  }

  settleButtons(gpio);

  // Wait (up to 20s) for the association, interruptible with any button.
  constexpr unsigned long CONNECT_TIMEOUT_MS = 20000;
  const unsigned long connectStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - connectStart > CONNECT_TIMEOUT_MS) {
      LOG_ERR(TAG, "Wi-Fi connect timed out");
      drawStatus(renderer, "Auto-Sync", "Wi-Fi connect failed", "");
      delay(1200);
      wifiOff();
      return;
    }
    gpio.update();
    if (gpio.wasAnyPressed()) {
      LOG_INF(TAG, "Auto-sync cancelled during connect");
      wifiOff();
      return;
    }
    esp_task_wdt_reset();
    delay(50);
  }

  // Start the existing file-transfer server (HTTP :80 + WebSocket :81).
  auto server = std::make_unique<CrossPointWebServer>();
  server->begin();
  if (!server->isRunning()) {
    LOG_ERR(TAG, "Failed to start web server");
    drawStatus(renderer, "Auto-Sync", "Server failed to start", "");
    delay(1200);
    wifiOff();
    return;
  }

  const std::string ip = WiFi.localIP().toString().c_str();
  uint8_t minutes = SETTINGS.autoSyncMinutes;
  if (minutes < CrossPointSettings::MIN_AUTO_SYNC_MINUTES) {
    minutes = CrossPointSettings::MIN_AUTO_SYNC_MINUTES;
  }
  const uint32_t windowMs = static_cast<uint32_t>(minutes) * 60000UL;
  LOG_INF(TAG, "Sync window open at %s for %u min", ip.c_str(), static_cast<unsigned>(minutes));

  const unsigned long windowStart = millis();
  int lastShownMinute = -1;
  bool finished = false;

  while (!finished && (millis() - windowStart) < windowMs) {
    // Service HTTP/WebSocket clients in a tight burst, resetting the watchdog
    // and polling for a cancel button periodically (mirrors the transfer
    // activity's loop so large uploads keep the device responsive).
    constexpr int MAX_ITERATIONS = 500;
    for (int i = 0; i < MAX_ITERATIONS && server->isRunning(); i++) {
      server->handleClient();
      if ((i & 0x1F) == 0x1F) {
        esp_task_wdt_reset();
      }
      if ((i & 0x3F) == 0x3F) {
        yield();
        gpio.update();
        if (gpio.wasAnyPressed()) {
          finished = true;
          break;
        }
      }
    }
    if (finished) {
      break;
    }

    // Repaint only when the whole-minute countdown changes (e-ink is slow, so
    // avoid per-second refreshes).
    const uint32_t elapsed = millis() - windowStart;
    const int remainingMin = static_cast<int>((windowMs - elapsed + 59999UL) / 60000UL);
    if (remainingMin != lastShownMinute) {
      lastShownMinute = remainingMin;
      esp_task_wdt_reset();
      drawStatus(renderer, "Auto-Sync", "Ready at " + ip, "Closing in ~" + std::to_string(remainingMin) + " min");
    }
  }

  LOG_INF(TAG, "Sync window closing");
  server->stop();
  server.reset();
  wifiOff();
  esp_task_wdt_reset();
}

}  // namespace AutoSyncOnBoot
