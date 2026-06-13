#include "HubSync.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <string>
#include <vector>

#include "../WifiCredentialStore.h"
#include "../fontIds.h"
#include "HttpDownloader.h"

namespace {

constexpr const char* TAG = "HUBSYNC";
constexpr const char* CONFIG_PATH = "/.crosspoint/hub.json";
constexpr const char* SYNCED_PATH = "/.crosspoint/synced.json";

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
  // Trim a trailing slash on the base URL.
  while (!cfg.url.empty() && cfg.url.back() == '/') cfg.url.pop_back();
  return !cfg.url.empty() && !cfg.token.empty();
}

bool connectWifi(HalGPIO& gpio) {
  WIFI_STORE.loadFromFile();
  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  if (ssid.empty()) return false;
  const WifiCredential* cred = WIFI_STORE.findCredential(ssid);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("crosspoint");
  if (cred != nullptr && !cred->password.empty()) {
    WiFi.begin(ssid.c_str(), cred->password.c_str());
  } else {
    WiFi.begin(ssid.c_str());
  }

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 20000) return false;
    gpio.update();
    if (gpio.wasAnyPressed()) return false;  // user skip
    esp_task_wdt_reset();
    delay(50);
  }
  return true;
}

// path -> local target on the SD card
std::string targetPath(const Entry& e) {
  if (e.type == "sleep") return "/sleep.bmp";
  std::string p = e.path;
  if (p.empty() || p.front() != '/') p = "/" + p;
  return p;
}

}  // namespace

namespace HubSync {

void run(const GfxRenderer& renderer, HalGPIO& gpio) {
  HubConfig cfg;
  if (!loadConfig(cfg) || !cfg.enabled) {
    return;  // no config => feature off
  }

  LOG_INF(TAG, "Hub sync starting (%s)", cfg.url.c_str());
  drawStatus(renderer, "Hub Sync", "Connecting to Wi-Fi", "");
  if (!connectWifi(gpio)) {
    drawStatus(renderer, "Hub Sync", "Wi-Fi unavailable", "");
    delay(1200);
    wifiOff();
    return;
  }

  drawStatus(renderer, "Hub Sync", "Checking for updates", "");
  const std::string manifestUrl = cfg.url + "/api/device/manifest?token=" + cfg.token + "&device=" + cfg.device;
  std::string json;
  if (!HttpDownloader::fetchUrl(manifestUrl, json)) {
    LOG_ERR(TAG, "manifest fetch failed");
    drawStatus(renderer, "Hub Sync", "Hub unreachable", "");
    delay(1200);
    wifiOff();
    return;
  }

  JsonDocument manifest;
  if (deserializeJson(manifest, json)) {
    LOG_ERR(TAG, "manifest parse failed");
    drawStatus(renderer, "Hub Sync", "Bad manifest", "");
    delay(1200);
    wifiOff();
    return;
  }

  std::vector<Entry> entries;
  for (JsonObject e : manifest["entries"].as<JsonArray>()) {
    entries.push_back({std::string(e["path"] | ""), std::string(e["hash"] | ""), std::string(e["type"] | "file")});
  }

  // Local sync record: path -> last-synced hash.
  std::string syncedRaw;
  JsonDocument synced;
  if (readFile(SYNCED_PATH, syncedRaw)) deserializeJson(synced, syncedRaw);

  int downloaded = 0;
  int index = 0;
  for (const Entry& e : entries) {
    index++;
    if (e.hash.empty()) continue;
    const char* have = synced[e.path];
    if (have != nullptr && e.hash == have) continue;  // already have this version

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

  // Persist the updated sync record.
  {
    HalFile f;
    if (Storage.openFileForWrite(TAG, SYNCED_PATH, f)) {
      serializeJson(synced, f);
      f.close();
    }
  }

  drawStatus(renderer, "Hub Sync", downloaded > 0 ? "Synced " + std::to_string(downloaded) + " item(s)" : "Up to date",
             "");
  delay(700);
  wifiOff();
  esp_task_wdt_reset();
}

}  // namespace HubSync
