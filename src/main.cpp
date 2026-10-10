#include <Arduino.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <nvs_flash.h>
#include <string.h>
#include <time.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "rnv_root_ca.h"
#include "rnv_credentials.h"

WebServer server(80);
StaticJsonDocument<1024> config;
bool otaUploadOk = false;
bool apiConnected = false;
constexpr char firmwareVersion[] = "0.1.0";
String apiAccessToken;
uint32_t apiTokenExpiresAt = 0;

String encodeFormValue(const char *value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  String encoded;
  while (*value) {
    const unsigned char c = static_cast<unsigned char>(*value++);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

bool postHttps(const String &url, const String &contentType, const String &request, String &responseBody, const String &authorization = "") {
  WiFiClientSecure tls;
  tls.setCACert(RNV_ROOT_CA);
  HTTPClient http;
  http.setTimeout(7000);
  if (!http.begin(tls, url)) return false;
  http.addHeader("Content-Type", contentType);
  http.addHeader("Accept", "application/json");
  if (authorization.length()) http.addHeader("Authorization", authorization);
  const int status = http.POST(request);
  if (status > 0) responseBody = http.getString();
  http.end();
  return status >= 200 && status < 300;
}

bool hasRnvCredentials() {
  return *RNV_CLIENT_ID && *RNV_TENANT_ID && *RNV_CLIENT_SECRET && *RNV_RESOURCE && *RNV_API_URL;
}

bool getRnvAccessToken() {
  if (apiAccessToken.length() && static_cast<int32_t>(apiTokenExpiresAt - millis()) > 30000) return true;
  if (!hasRnvCredentials()) return false;

  const String tokenUrl = String("https://login.microsoftonline.com/") + RNV_TENANT_ID + "/oauth2/token";
  const String form = String("grant_type=client_credentials&client_id=") + encodeFormValue(RNV_CLIENT_ID) +
    "&client_secret=" + encodeFormValue(RNV_CLIENT_SECRET) + "&resource=" + encodeFormValue(RNV_RESOURCE);
  String responseBody;
  if (!postHttps(tokenUrl, "application/x-www-form-urlencoded", form, responseBody)) return false;

  DynamicJsonDocument response(4096);
  if (deserializeJson(response, responseBody)) return false;
  apiAccessToken = response["access_token"] | "";
  const uint32_t lifetime = response["expires_in"] | 3600UL;
  apiTokenExpiresAt = millis() + (lifetime > 30 ? lifetime - 30 : lifetime) * 1000UL;
  return apiAccessToken.length() > 0;
}

bool currentUtcIsoString(char *buffer, size_t size) {
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;
  struct tm utc;
  if (!gmtime_r(&now, &utc)) return false;
  return strftime(buffer, size, "%Y-%m-%dT%H:%M:%SZ", &utc) > 0;
}

bool saveConfig(const StaticJsonDocument<1024> &nextConfig) {
  LittleFS.remove("/config.tmp");
  File file = LittleFS.open("/config.tmp", "w");
  if (!file) return false;
  const size_t written = serializeJson(nextConfig, file);
  file.flush();
  file.close();
  if (written != measureJson(nextConfig)) return false;
  if (LittleFS.exists("/config.bak") && !LittleFS.remove("/config.bak")) return false;

  const bool hadConfig = LittleFS.exists("/config.json");
  if (hadConfig && !LittleFS.rename("/config.json", "/config.bak")) return false;
  if (!LittleFS.rename("/config.tmp", "/config.json")) {
    if (hadConfig) LittleFS.rename("/config.bak", "/config.json");
    return false;
  }
  LittleFS.remove("/config.bak");
  return true;
}

void startSetupAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("RNV-Table-Station");
  Serial.printf("Setup AP: %s\n", WiFi.softAPIP().toString().c_str());
}

void setupRoutes() {
  server.on("/", HTTP_GET, [] {
    File index = LittleFS.open("/index.html", "r");
    if (!index) {
      server.send(500, "text/plain", "Web UI is missing");
      return;
    }
    server.streamFile(index, "text/html");
  });
  server.on("/stops.js", HTTP_GET, [] {
    File stops = LittleFS.open("/stops.js", "r");
    if (!stops) {
      server.send(404, "text/plain", "Haltestellenkatalog fehlt");
      return;
    }
    server.streamFile(stops, "application/javascript");
  });
  server.on("/api/departures", HTTP_GET, [] {
    const String slotText = server.arg("slot");
    if (!server.hasArg("slot") || slotText.length() != 1 || slotText[0] < '0' || slotText[0] > '2') {
      server.send(400, "application/json", "{\"error\":\"Ungültiger Bildschirm\"}");
      return;
    }
    const uint8_t slot = static_cast<uint8_t>(slotText[0] - '0');
    JsonArrayConst slots = config["slots"].as<JsonArrayConst>();
    const String hafasId = slots.isNull() || slots.size() <= slot ? "" : String(slots[slot] | "");
    if (!hafasId.length()) {
      server.send(200, "application/json", "{\"station\":\"\",\"departures\":[]}");
      return;
    }
    if (hafasId.length() > 12 || strspn(hafasId.c_str(), "0123456789") != hafasId.length()) {
      server.send(400, "application/json", "{\"error\":\"Ungültige Haltestellen-ID\"}");
      return;
    }
    if (WiFi.status() != WL_CONNECTED || !hasRnvCredentials()) {
      apiConnected = false;
      server.send(503, "application/json", "{\"error\":\"Fahrplandaten nicht verfügbar\"}");
      return;
    }

    char startTime[25];
    if (!currentUtcIsoString(startTime, sizeof(startTime)) || !getRnvAccessToken()) {
      apiConnected = false;
      server.send(503, "application/json", "{\"error\":\"Fahrplandaten nicht verfügbar\"}");
      return;
    }
    String query;
    query.reserve(420);
    query += F("{ station(id: \"");
    query += hafasId;
    query += F("\") { longName journeys(startTime: \"");
    query += startTime;
    query += F("\", first: 2) { elements { ... on Journey { line { id } stops(onlyHafasID: \"");
    query += hafasId;
    query += F("\") { plannedDeparture { isoString } realtimeDeparture { isoString } } } } } } }");
    StaticJsonDocument<2048> request;
    request["query"] = query;
    String requestBody;
    serializeJson(request, requestBody);
    String apiUrl = RNV_API_URL;
    if (!apiUrl.endsWith("/")) apiUrl += '/';
    String responseBody;
    if (!postHttps(apiUrl, "application/json", requestBody, responseBody, "Bearer " + apiAccessToken)) {
      apiConnected = false;
      server.send(502, "application/json", "{\"error\":\"Fahrplandaten nicht verfügbar\"}");
      return;
    }

    DynamicJsonDocument response(12288);
    if (deserializeJson(response, responseBody) || !response["errors"].isNull()) {
      apiConnected = false;
      server.send(502, "application/json", "{\"error\":\"Fahrplandaten nicht verfügbar\"}");
      return;
    }
    JsonObjectConst station = response["data"]["station"].as<JsonObjectConst>();
    if (station.isNull()) {
      apiConnected = false;
      server.send(502, "application/json", "{\"error\":\"Fahrplandaten nicht verfügbar\"}");
      return;
    }

    StaticJsonDocument<1024> result;
    result["station"] = station["longName"] | "";
    JsonArray departures = result.createNestedArray("departures");
    JsonArrayConst journeys = station["journeys"]["elements"].as<JsonArrayConst>();
    for (JsonObjectConst journey : journeys) {
      JsonArrayConst stopsAtStation = journey["stops"].as<JsonArrayConst>();
      if (stopsAtStation.isNull() || stopsAtStation.size() == 0) continue;
      JsonObjectConst stop = stopsAtStation[0];
      String departureTime = stop["realtimeDeparture"]["isoString"] | "";
      if (!departureTime.length()) departureTime = stop["plannedDeparture"]["isoString"] | "";
      if (!departureTime.length()) continue;
      JsonObject departure = departures.createNestedObject();
      String line = journey["line"]["id"] | "";
      const int separator = line.indexOf('-');
      if (separator >= 0) line.remove(0, separator + 1);
      departure["line"] = line.length() ? line : "—";
      departure["time"] = departureTime;
    }
    apiConnected = true;
    String resultBody;
    serializeJson(result, resultBody);
    server.send(200, "application/json", resultBody);
  });
  server.on("/api/status", HTTP_GET, [] {
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    const bool setupApActive = (WiFi.getMode() & WIFI_AP) != 0;
    StaticJsonDocument<384> status;
    status["firmwareVersion"] = firmwareVersion;
    status["apiStatus"] = !hasRnvCredentials() ? "Nicht konfiguriert" : (apiConnected ? "Verbunden" : "Warte auf Abfrage");
    status["apiConnected"] = apiConnected;
    status["wifiConnected"] = wifiConnected;
    status["wifiSsid"] = wifiConnected ? WiFi.SSID() : (setupApActive ? "RNV-Table-Station (Setup)" : "Nicht verbunden");
    status["wifiIp"] = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    status["wifiRssi"] = wifiConnected ? WiFi.RSSI() : 0;
    status["macAddress"] = setupApActive ? WiFi.softAPmacAddress() : WiFi.macAddress();
    status["uptimeSeconds"] = millis() / 1000UL;
    String json;
    serializeJson(status, json);
    server.send(200, "application/json", json);
  });
  server.on("/api/config", HTTP_GET, [] {
    String json;
    StaticJsonDocument<1024> safeConfig;
    safeConfig.set(config);
    safeConfig["wifiPassword"] = "";
    serializeJson(safeConfig, json);
    server.send(200, "application/json", json);
  });
  server.on("/api/config", HTTP_POST, [] {
    StaticJsonDocument<1024> incoming;
    const String body = server.arg("plain");
    if (body.length() > 1024 || deserializeJson(incoming, body)) {
      server.send(400, "application/json", "{\"error\":\"Invalid configuration\"}");
      return;
    }
    const bool updateWifi = incoming.containsKey("wifiSsid") || incoming.containsKey("wifiPassword");
    const bool updateStations = incoming.containsKey("slots") || incoming.containsKey("defaultSlot");
    const bool updateDevice = incoming.containsKey("hostname") || incoming.containsKey("screenBrightness") || incoming.containsKey("screenFlipped");
    const int updateGroups = updateWifi + updateStations + updateDevice;
    const size_t expectedFields = updateWifi || updateStations ? 2 : 3;
    if (updateGroups != 1 || incoming.size() != expectedFields) {
      server.send(400, "application/json", "{\"error\":\"Send one settings group at a time\"}");
      return;
    }

    StaticJsonDocument<1024> nextConfig;
    nextConfig.set(config);
    if (updateWifi) {
      if (!incoming["wifiSsid"].is<const char *>() || !incoming["wifiPassword"].is<const char *>()) {
        server.send(400, "application/json", "{\"error\":\"Invalid Wi-Fi settings\"}");
        return;
      }
      const char *ssid = incoming["wifiSsid"];
      const char *password = incoming["wifiPassword"];
      if (!*ssid || strlen(ssid) > 32 || strlen(password) > 63) {
        server.send(400, "application/json", "{\"error\":\"Wi-Fi settings are too long\"}");
        return;
      }
      nextConfig["wifiSsid"] = ssid;
      nextConfig["wifiPassword"] = password;
    } else if (updateStations) {
      if (!incoming["defaultSlot"].is<int>() ||
          incoming["defaultSlot"].as<int>() < 0 || incoming["defaultSlot"].as<int>() > 2 ||
          !incoming["slots"].is<JsonArray>() || incoming["slots"].size() != 3) {
        server.send(400, "application/json", "{\"error\":\"Invalid station settings\"}");
        return;
      }
      for (JsonVariant station : incoming["slots"].as<JsonArray>()) {
        if (!station.is<const char *>()) {
          server.send(400, "application/json", "{\"error\":\"Station IDs must be text\"}");
          return;
        }
        const char *id = station;
        if (strlen(id) > 12 || strspn(id, "0123456789") != strlen(id)) {
          server.send(400, "application/json", "{\"error\":\"Station IDs must be numeric\"}");
          return;
        }
      }
      nextConfig["defaultSlot"] = incoming["defaultSlot"];
      nextConfig["slots"].set(incoming["slots"]);
    } else {
      if (!incoming["hostname"].is<const char *>() ||
          !incoming["screenBrightness"].is<int>() ||
          !incoming["screenFlipped"].is<bool>()) {
        server.send(400, "application/json", "{\"error\":\"Invalid device settings\"}");
        return;
      }
      const char *hostname = incoming["hostname"];
      const size_t hostnameLength = strlen(hostname);
      if (!hostnameLength || hostnameLength > 32 || hostname[0] == '-' || hostname[hostnameLength - 1] == '-') {
        server.send(400, "application/json", "{\"error\":\"Invalid hostname\"}");
        return;
      }
      for (size_t i = 0; i < hostnameLength; ++i) {
        if (!isalnum(static_cast<unsigned char>(hostname[i])) && hostname[i] != '-') {
          server.send(400, "application/json", "{\"error\":\"Invalid hostname\"}");
          return;
        }
      }
      const int brightness = incoming["screenBrightness"];
      if (brightness < 1 || brightness > 100) {
        server.send(400, "application/json", "{\"error\":\"Brightness must be between 1 and 100\"}");
        return;
      }
      nextConfig["hostname"] = hostname;
      nextConfig["screenBrightness"] = brightness;
      nextConfig["screenFlipped"] = incoming["screenFlipped"];
    }

    if (!saveConfig(nextConfig)) {
      server.send(500, "application/json", "{\"error\":\"Could not save settings\"}");
      return;
    }
    config.set(nextConfig);
    server.send(200, "application/json", "{\"saved\":true}");
    delay(500);
    ESP.restart();
  });
  server.on("/api/reset", HTTP_POST, [] {
    const char *settingsFiles[] = {"/config.json", "/config.bak", "/config.tmp"};
    for (const char *path : settingsFiles) {
      if (LittleFS.exists(path) && !LittleFS.remove(path)) {
        server.send(500, "application/json", "{\"error\":\"Could not remove saved settings\"}");
        return;
      }
    }
    if (nvs_flash_erase() != ESP_OK) {
      server.send(500, "application/json", "{\"error\":\"Could not erase NVS settings\"}");
      return;
    }
    server.send(200, "application/json", "{\"reset\":true}");
    delay(500);
    ESP.restart();
  });
  server.on("/api/ota", HTTP_POST, [] {
    server.send(otaUploadOk ? 200 : 500, "application/json",
                otaUploadOk ? "{\"updated\":true}" : "{\"error\":\"Update failed\"}");
    if (otaUploadOk) {
      delay(500);
      ESP.restart();
    }
  }, [] {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      otaUploadOk = Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (upload.status == UPLOAD_FILE_WRITE && otaUploadOk) {
      otaUploadOk = Update.write(upload.buf, upload.currentSize) == upload.currentSize;
    } else if (upload.status == UPLOAD_FILE_END && otaUploadOk) {
      otaUploadOk = Update.end(true);
    }
  });
}

void setup() {
  Serial.begin(115200);
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }
  if (!LittleFS.exists("/config.json") && LittleFS.exists("/config.bak")) {
    LittleFS.rename("/config.bak", "/config.json");
  }
  if (File file = LittleFS.open("/config.json", "r")) deserializeJson(config, file);
  if (!config.containsKey("hostname")) config["hostname"] = "rnv-table-station";
  if (!config.containsKey("screenBrightness")) config["screenBrightness"] = 100;
  if (!config.containsKey("screenFlipped")) config["screenFlipped"] = false;

  const char *ssid = config["wifiSsid"] | "";
  WiFi.setHostname(config["hostname"]);
  if (*ssid) {
    WiFi.mode(WIFI_STA);
    const char *password = config["wifiPassword"] | "";
    WiFi.begin(ssid, *password ? password : nullptr);
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; ++i) delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    startSetupAp();
  }
  setupRoutes();
  server.begin();
}

void loop() {
  server.handleClient();
}
