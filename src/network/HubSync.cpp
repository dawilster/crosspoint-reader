#include "HubSync.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <ctime>
#include <string>
#include <vector>

#include "../CrossPointSettings.h"
#include "../WifiCredentialStore.h"
#include "../fontIds.h"
#include "HttpDownloader.h"

namespace {

constexpr const char* TAG = "HUBSYNC";
constexpr const char* CONFIG_PATH = "/.crosspoint/hub.json";
constexpr const char* SYNCED_PATH = "/.crosspoint/synced.json";
constexpr const char* LAST_SYNC_KEY = "__lastSync";

struct HubConfig {
  std::string url;
  std::string token;
  std::string device = "x4";
  bool enabled = true;
};

struct Entry {
  std::string path;
  std::string hash;
  std::string type;
};

// ---- small SD helpers ------------------------------------------------------

bool readFile(const char* path, std::string& out) {
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return false;
  char buf[512];
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
  f.close();
  return true;
}

void ensureParentDir(const std::string& filePath) {
  const auto pos = filePath.find_last_of('/');
  if (pos == std::string::npos || pos == 0) return;
  const std::string dir = filePath.substr(0, pos);
  if (!Storage.exists(dir.c_str())) Storage.mkdir(dir.c_str(), true);
}

// ---- status screen ---------------------------------------------------------

void drawStatus(const GfxRenderer& renderer, const char* title, const std::string& line1, const std::string& line2) {
  const int h = renderer.getScreenHeight();
  renderer.clearScreen();
  renderer.drawCenteredText(NOTOSANS_16_FONT_ID, h / 2 - 50, title);
  if (!line1.empty()) renderer.drawCenteredText(UI_12_FONT_ID, h / 2 + 4, line1.c_str());
  if (!line2.empty()) renderer.drawCenteredText(UI_12_FONT_ID, h / 2 + 34, line2.c_str());
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void wifiOff() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

// ---- config / wifi ---------------------------------------------------------

bool loadConfig(HubConfig& cfg) {
  std::string raw;
  if (!readFile(CONFIG_PATH, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;
  cfg.url = (doc["url"] | "");
  cfg.token = (doc["token"] | "");
  cfg.device = (doc["device"] | "x4");
  cfg.enabled = (doc["enabled"] | true);
  while (!cfg.url.empty() && cfg.url.back() == '/') cfg.url.pop_back();
  return !cfg.url.empty() && !cfg.token.empty();
}

bool tryConnect(const std::string& ssid, const std::string& pass, HalGPIO& gpio, unsigned long timeoutMs) {
  WiFi.disconnect(true, true);
  delay(50);
  if (!pass.empty()) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin(ssid.c_str());
  }
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeoutMs) return false;
    gpio.update();
    if (gpio.wasAnyPressed()) return false;  // user skip
    esp_task_wdt_reset();
    delay(50);
  }
  return true;
}

// Try the last-used network first, then fall back to every other saved network.
bool connectWifi(const GfxRenderer& renderer, HalGPIO& gpio) {
  WIFI_STORE.loadFromFile();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("crosspoint");

  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (!last.empty()) {
    const WifiCredential* c = WIFI_STORE.findCredential(last);
    drawStatus(renderer, "Hub Sync", "Connecting: " + last, "");
    if (tryConnect(last, c != nullptr ? c->password : std::string(), gpio, 12000)) return true;
  }
  for (const WifiCredential& c : WIFI_STORE.getCredentials()) {
    if (c.ssid == last) continue;  // already tried
    drawStatus(renderer, "Hub Sync", "Connecting: " + c.ssid, "");
    if (tryConnect(c.ssid, c.password, gpio, 12000)) return true;
  }
  return false;
}

std::string targetPath(const Entry& e) {
  if (e.type == "sleep") return "/sleep.bmp";
  std::string p = e.path;
  if (p.empty() || p.front() != '/') p = "/" + p;
  return p;
}

// Core sync: fetch the manifest and delta-download. Returns downloaded count, or
// -1 on failure. Assumes Wi-Fi is already connected. Updates the local record.
int doSync(const HubConfig& cfg, const GfxRenderer& renderer, HalGPIO& gpio) {
  drawStatus(renderer, "Hub Sync", "Checking for updates", "");
  const std::string manifestUrl = cfg.url + "/api/device/manifest?token=" + cfg.token + "&device=" + cfg.device;
  std::string json;
  if (!HttpDownloader::fetchUrl(manifestUrl, json)) {
    LOG_ERR(TAG, "manifest fetch failed");
    drawStatus(renderer, "Hub Sync", "Hub unreachable", "");
    delay(1200);
    return -1;
  }

  JsonDocument manifest;
  if (deserializeJson(manifest, json)) {
    LOG_ERR(TAG, "manifest parse failed");
    drawStatus(renderer, "Hub Sync", "Bad manifest", "");
    delay(1200);
    return -1;
  }

  std::vector<Entry> entries;
  for (JsonObject e : manifest["entries"].as<JsonArray>()) {
    entries.push_back({std::string(e["path"] | ""), std::string(e["hash"] | ""), std::string(e["type"] | "file")});
  }

  std::string syncedRaw;
  JsonDocument synced;
  if (readFile(SYNCED_PATH, syncedRaw)) deserializeJson(synced, syncedRaw);

  int downloaded = 0;
  int index = 0;
  for (const Entry& e : entries) {
    index++;
    if (e.hash.empty()) continue;
    const char* have = synced[e.path];
    if (have != nullptr && e.hash == have) continue;

    const std::string dest = targetPath(e);
    drawStatus(renderer, "Hub Sync", "Downloading " + std::to_string(index) + "/" + std::to_string(entries.size()),
               dest);
    ensureParentDir(dest);

    const std::string blobUrl = cfg.url + "/api/device/blob?token=" + cfg.token + "&id=" + e.hash;
    const auto err = HttpDownloader::downloadToFile(blobUrl, dest, [](size_t, size_t) { esp_task_wdt_reset(); });
    if (err == HttpDownloader::OK) {
      synced[e.path] = e.hash;
      downloaded++;
      LOG_INF(TAG, "downloaded %s", dest.c_str());
    } else {
      LOG_ERR(TAG, "download failed: %s (err %d)", dest.c_str(), static_cast<int>(err));
    }

    gpio.update();
    if (gpio.wasAnyPressed()) break;  // user skip
  }

  // Stamp last-sync time (monotonic across deep sleep) for the throttle.
  synced[LAST_SYNC_KEY] = static_cast<long>(time(nullptr));
  HalFile f;
  if (Storage.openFileForWrite(TAG, SYNCED_PATH, f)) {
    serializeJson(synced, f);
    f.close();
  }
  return downloaded;
}

}  // namespace

namespace HubSync {

void run(const GfxRenderer& renderer, HalGPIO& gpio) {
  HubConfig cfg;
  if (!loadConfig(cfg) || !cfg.enabled) {
    return;  // no config => feature off
  }

  // Throttle: skip silently if we synced within the configured interval. The
  // timestamp persists across deep sleep, so repeated open/close doesn't re-sync.
  const uint16_t intervalMin = SETTINGS.hubSyncIntervalMinutes;
  if (intervalMin > 0) {
    std::string syncedRaw;
    JsonDocument synced;
    if (readFile(SYNCED_PATH, syncedRaw)) deserializeJson(synced, syncedRaw);
    const time_t now = time(nullptr);
    const time_t last = static_cast<time_t>(synced[LAST_SYNC_KEY] | 0L);
    if (last > 0 && now >= last && (now - last) < static_cast<time_t>(intervalMin) * 60) {
      LOG_INF(TAG, "Hub sync throttled (%ld s since last, interval %u min)", static_cast<long>(now - last),
              static_cast<unsigned>(intervalMin));
      return;  // fast, silent boot
    }
  }

  LOG_INF(TAG, "Hub sync starting (%s)", cfg.url.c_str());
  if (!connectWifi(renderer, gpio)) {
    drawStatus(renderer, "Hub Sync", "Wi-Fi unavailable", "");
    delay(1200);
    wifiOff();
    return;
  }

  const int downloaded = doSync(cfg, renderer, gpio);
  if (downloaded >= 0) {
    drawStatus(renderer, "Hub Sync",
               downloaded > 0 ? "Synced " + std::to_string(downloaded) + " item(s)" : "Up to date", "");
    delay(700);
  }
  wifiOff();
  esp_task_wdt_reset();
}

void syncNow(const GfxRenderer& renderer, HalGPIO& gpio) {
  HubConfig cfg;
  if (!loadConfig(cfg) || !cfg.enabled) {
    drawStatus(renderer, "Hub Sync", "No hub configured", "(/.crosspoint/hub.json)");
    delay(1500);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    drawStatus(renderer, "Hub Sync", "Connect to Wi-Fi first", "");
    delay(1500);
    return;
  }
  const int downloaded = doSync(cfg, renderer, gpio);
  if (downloaded >= 0) {
    drawStatus(renderer, "Hub Sync",
               downloaded > 0 ? "Synced " + std::to_string(downloaded) + " item(s)" : "Up to date", "");
    delay(900);
  }
}

}  // namespace HubSync
