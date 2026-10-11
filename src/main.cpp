#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <nvs_flash.h>
#include <Preferences.h>
#include <qrcode.h>
#include <string.h>
#include <time.h>
#include <Update.h>
#include <WebServer.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "rnv_root_ca.h"
#include "rnv_credentials.h"

WebServer server(80);
StaticJsonDocument<1024> config;
Arduino_DataBus *lcdBus = new Arduino_ESP32SPI(15, 14, 1, 2, GFX_NOT_DEFINED);
Arduino_GFX *lcd = new Arduino_ST7789(lcdBus, 22, 0, true, 172, 320, 34, 0, 34, 0);
bool lcdReady = false;
bool otaUploadOk = false;
bool filesystemOtaUploadOk = false;
bool filesystemOtaStarted = false;
bool apiConnected = false;
constexpr char firmwareVersion[] = "0.1.8";
String filesystemVersion = "Nicht verfügbar";
String apiAccessToken;
uint32_t apiTokenExpiresAt = 0;
uint32_t lastDisplayRefresh = 0;
uint32_t lastScreenTouch = 0;
bool screenDimmed = false;
bool setupClientDetected = false;
bool setupScreenDrawn = false;
bool departureScreenActive = false;
bool departureDataLoaded = false;
String displayDestinations[3];
String displayLines[3];
String displayTimes[3];
String displayStation;
String displayNote;
uint16_t destinationScrollOffsets[3] = {};
uint32_t lastDestinationScroll = 0;

uint16_t displayColor(uint16_t color) {
  const uint16_t brightness = (((color >> 11) & 0x1F) * 255 / 31 * 3 +
                               ((color >> 5) & 0x3F) * 255 / 63 * 6 +
                               (color & 0x1F) * 255 / 31) / 10;
  const bool dark = brightness < 128;
  const bool white = (config["screenInverted"] | false) ? dark : !dark;
  return white ? 0xFFFF : 0x0000;
}

void setScreenBrightness(bool dimmed = false) {
  int brightness = (config["screenBrightness"] | 100) / 2;
  if (dimmed && brightness > 30) brightness = 30;
  analogWrite(23, brightness * 255 / 100);
}

bool screenTouched() {
  if (digitalRead(21) != LOW) return false;
  Wire.beginTransmission(0x63);
  Wire.write(0x01);
  if (Wire.endTransmission() != 0 || Wire.requestFrom(0x63, 14) != 14) return false;
  uint8_t data[14];
  if (Wire.readBytes(data, sizeof(data)) != sizeof(data)) return false;
  return data[1] > 0;
}

String departureLabel(const String &isoTime) {
  int year, month, day, hour, minute, second;
  if (sscanf(isoTime.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
    const int timeStart = isoTime.indexOf('T');
    return timeStart >= 0 ? isoTime.substring(timeStart + 1, timeStart + 6) : isoTime;
  }
  struct tm departure = {};
  departure.tm_year = year - 1900;
  departure.tm_mon = month - 1;
  departure.tm_mday = day;
  departure.tm_hour = hour;
  departure.tm_min = minute;
  departure.tm_sec = second;
  time_t departureTime = mktime(&departure);
  const int timeStart = isoTime.indexOf('T');
  const char *zone = isoTime.c_str() + (timeStart >= 0 ? timeStart + 1 : isoTime.length());
  while (*zone && *zone != '+' && *zone != '-') ++zone;
  if (*zone) {
    int zoneHours = 0, zoneMinutes = 0;
    if (sscanf(zone + 1, "%d:%d", &zoneHours, &zoneMinutes) == 2) {
      const int zoneOffset = (zoneHours * 60 + zoneMinutes) * 60;
      departureTime -= *zone == '+' ? zoneOffset : -zoneOffset;
    }
  }
  const time_t now = time(nullptr);
  if (now < 1700000000) {
    return isoTime.substring(timeStart + 1, timeStart + 6);
  }
  const long secondsLeft = static_cast<long>(departureTime - now);
  const long minutesLeft = secondsLeft > 0 ? (secondsLeft + 59) / 60 : 0;
  return minutesLeft ? String(minutesLeft) + " min" : "Jetzt";
}

uint8_t utf8SequenceLength(uint8_t lead) {
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

size_t utf8Length(const String &value) {
  size_t count = 0;
  for (size_t i = 0; i < value.length(); ++count) i += utf8SequenceLength(static_cast<uint8_t>(value[i]));
  return count;
}

String utf8CharacterAt(const String &value, size_t characterIndex) {
  size_t start = 0;
  while (start < value.length() && characterIndex--) start += utf8SequenceLength(static_cast<uint8_t>(value[start]));
  if (start >= value.length()) return "";
  return value.substring(start, start + utf8SequenceLength(static_cast<uint8_t>(value[start])));
}

uint8_t displayGlyph(const String &value, size_t index, uint8_t sequenceLength) {
  const uint8_t lead = static_cast<uint8_t>(value[index]);
  if (sequenceLength == 1) return lead;
  if (sequenceLength == 2 && lead == 0xC3) {
    switch (static_cast<uint8_t>(value[index + 1])) {
      case 0x84: return 0x8E; // Ä
      case 0x96: return 0x99; // Ö
      case 0x9C: return 0x9A; // Ü
      case 0x9F: return 0xE1; // ß
      case 0xA4: return 0x84; // ä
      case 0xB6: return 0x94; // ö
      case 0xBC: return 0x81; // ü
    }
  }
  return '?';
}

void printUtf8(const String &value) {
  for (size_t i = 0; i < value.length();) {
    const uint8_t length = utf8SequenceLength(static_cast<uint8_t>(value[i]));
    lcd->write(displayGlyph(value, i, length));
    i += length;
  }
}

void renderDestinationCells(int width, bool advance) {
  constexpr int destinationX = 46;
  constexpr int timeColumnWidth = 88;
  constexpr int rowTop = 48;
  constexpr int rowHeight = 30;
  constexpr int glyphWidth = 12;
  const int destinationWidth = width - destinationX - timeColumnWidth - 4;
  const size_t maxChars = max(1, destinationWidth / glyphWidth);
  lcd->setTextSize(2);
  for (size_t row = 0; row < 3; ++row) {
    const String &destination = displayDestinations[row];
    const size_t characterCount = utf8Length(destination);
    if (advance && characterCount <= maxChars) continue;
    const int y = rowTop + row * rowHeight;
    lcd->setTextColor(displayColor(0x1168), displayColor(0xFFFF));
    lcd->setCursor(destinationX, y + 2);
    if (characterCount <= maxChars) {
      printUtf8(characterCount ? destination : "--");
      continue;
    }
    const size_t cycleLength = characterCount + 3;
    const size_t offset = destinationScrollOffsets[row] % cycleLength;
    String window;
    window.reserve(maxChars);
    for (size_t i = 0; i < maxChars; ++i) {
      const size_t index = (offset + i) % cycleLength;
      window += index < characterCount ? utf8CharacterAt(destination, index) : " ";
    }
    printUtf8(window);
    if (advance) destinationScrollOffsets[row] = (offset + 1) % cycleLength;
  }
}

void renderScreen(const char *station, const JsonArrayConst &departures, const char *message = nullptr) {
  if (!lcdReady) return;
  const bool fullRedraw = !departureScreenActive;
  departureScreenActive = true;
  constexpr uint16_t blue = 0x0129;
  constexpr uint16_t text = 0x1168;
  constexpr uint16_t muted = 0x63B0;
  constexpr uint16_t rowLine = 0xE75D;
  const int width = lcd->width();
  if (fullRedraw) {
    lcd->fillScreen(displayColor(0xFFFF));
    lcd->fillRect(0, 24, width, 22, displayColor(blue));
    lcd->setTextSize(2);
    lcd->setTextColor(displayColor(0xFFFF));
    lcd->setCursor(46, 27);
    lcd->print("Ziel");
    lcd->setCursor(width - 4 - 7 * 12, 27);
    lcd->print("Abfahrt");
    for (size_t i = 0; i < 3; ++i) lcd->drawFastHLine(0, 48 + i * 30 + 24, width, displayColor(rowLine));
  }
  lcd->setTextWrap(false);
  const String stationName = station && *station ? station : "Haltestelle nicht konfiguriert";
  if (fullRedraw || stationName != displayStation) {
    lcd->fillRect(0, 0, width, 24, displayColor(0xFFFF));
    const size_t stationLength = utf8Length(stationName);
    lcd->setTextSize(stationLength > 25 ? 1 : 2);
    lcd->setTextColor(displayColor(blue), displayColor(0xFFFF));
    lcd->setCursor(max(4, (width - static_cast<int>(stationLength) * (stationLength > 25 ? 6 : 12)) / 2), 4);
    printUtf8(stationName);
    displayStation = stationName;
  }

  for (size_t i = 0; i < 3; ++i) {
    const int y = 48 + i * 30;
    String line;
    String time;
    String destination;
    if (message || departures.isNull() || i >= departures.size()) {
      line = "--";
      time = "--";
    } else {
      JsonObjectConst departure = departures[i];
      line = departure["line"] | "--";
      time = departureLabel(departure["time"] | "");
      destination = departure["destination"] | "";
    }
    if (fullRedraw || line != displayLines[i]) {
      if (!fullRedraw) lcd->fillRect(0, y, 46, 24, displayColor(0xFFFF));
      lcd->setTextColor(displayColor(text), displayColor(0xFFFF));
      lcd->setTextSize(2);
      lcd->setCursor(4, y + 2);
      printUtf8(line);
      displayLines[i] = line;
    }
    if (fullRedraw || time != displayTimes[i]) {
      if (!fullRedraw) lcd->fillRect(width - 92, y, 88, 24, displayColor(0xFFFF));
      lcd->setTextColor(displayColor(text), displayColor(0xFFFF));
      lcd->setTextSize(2);
      lcd->setCursor(width - 4 - time.length() * 12, y + 2);
      printUtf8(time);
      displayTimes[i] = time;
    }
    if (destination != displayDestinations[i]) {
      if (!fullRedraw) {
        const int destinationWidth = width - 46 - 88 - 4;
        lcd->fillRect(46, y, destinationWidth, 24, displayColor(0xFFFF));
      }
      destinationScrollOffsets[i] = 0;
      displayDestinations[i] = destination;
    }
  }
  renderDestinationCells(width, false);

  String note;
  if (message) note = message;
  else if (departures.isNull() || departures.size() == 0) {
    note = station && *station ? "Keine Abfahrten verfuegbar" : "Warte auf Fahrplandaten";
  }
  else {
    struct tm now;
    if (getLocalTime(&now, 0)) {
      char updated[24];
      strftime(updated, sizeof(updated), "Aktualisiert %H:%M", &now);
      note = updated;
    } else note = "Fahrplandaten werden geladen";
  }
  if (fullRedraw || note != displayNote) {
    lcd->fillRect(0, 148, width, 20, displayColor(0xFFFF));
    const size_t noteLength = utf8Length(note);
    lcd->setTextSize(noteLength > 25 ? 1 : 2);
    lcd->setTextColor(displayColor(muted), displayColor(0xFFFF));
    lcd->setCursor(max(4, (width - static_cast<int>(noteLength) * (noteLength > 25 ? 6 : 12)) / 2), 150);
    printUtf8(note);
    displayNote = note;
  }
}

void showLoadingScreen() {
  if (!lcdReady) return;
  lcd->fillScreen(displayColor(0xFFFF));
  lcd->setTextColor(displayColor(0x0129));
  lcd->setTextSize(2);
  lcd->setCursor((lcd->width() - 14 * 12) / 2, lcd->height() / 2 - 24);
  lcd->print("Fahrplandaten");
  lcd->setCursor((lcd->width() - 14 * 12) / 2, lcd->height() / 2);
  lcd->print("werden geladen");
  departureScreenActive = false;
}

void showSetupScreen() {
  if (!lcdReady) return;
  departureScreenActive = false;
  if (WiFi.softAPgetStationNum() > 0 && !setupClientDetected) {
    setupClientDetected = true;
    setupScreenDrawn = false;
  }
  if (setupScreenDrawn) return;
  setupScreenDrawn = true;
  if (setupClientDetected) {
    const String ip = WiFi.softAPIP().toString();
    lcd->fillScreen(displayColor(0x0000));
    lcd->setTextColor(displayColor(0xFFFF));
    lcd->setTextSize(3);
    lcd->setCursor((lcd->width() - ip.length() * 18) / 2, (lcd->height() - 24) / 2);
    lcd->print(ip);
    return;
  }

  constexpr uint8_t qrVersion = 3;
  QRCode qr;
  uint8_t qrData[qrcode_getBufferSize(qrVersion)];
  if (qrcode_initText(&qr, qrData, qrVersion, ECC_LOW, "WIFI:T:nopass;S:RNV-Haltestelle;;") != 0) {
    renderScreen("WLAN Setup", JsonArrayConst(), "QR-Code konnte nicht erstellt werden");
    return;
  }
  constexpr int scale = 4;
  constexpr int quietZone = 4;
  constexpr int qrX = 8;
  const int qrSize = (qr.size + quietZone * 2) * scale;
  const int qrY = (lcd->height() - qrSize) / 2;
  lcd->fillScreen(displayColor(0x0000));
  for (uint8_t y = 0; y < qr.size; ++y) {
    for (uint8_t x = 0; x < qr.size; ++x) {
      if (qrcode_getModule(&qr, x, y)) {
        lcd->fillRect(qrX + (x + quietZone) * scale, qrY + (y + quietZone) * scale, scale, scale, displayColor(0xFFFF));
      }
    }
  }
  lcd->setTextColor(displayColor(0xFFFF));
  lcd->setTextSize(2);
  lcd->setCursor(220, 62);
  lcd->print("Zum");
  lcd->setCursor(178, 78);
  lcd->print("Einrichten");
  lcd->setCursor(196, 94);
  lcd->print("scannen");
}

void showDepartures(const String &body) {
  StaticJsonDocument<2048> result;
  if (deserializeJson(result, body)) {
    if (departureDataLoaded) renderScreen(nullptr, JsonArrayConst(), "Fahrplandaten nicht verfuegbar");
    return;
  }
  departureDataLoaded = true;
  renderScreen(result["station"] | "", result["departures"].as<JsonArrayConst>());
}

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
  String serialized;
  serializeJson(nextConfig, serialized);
  Preferences preferences;
  if (!preferences.begin("rnvconfig", false)) return false;
  const bool saved = preferences.putString("config", serialized) > 0 && preferences.getString("config", "") == serialized;
  preferences.end();
  return saved;
}

void loadConfig() {
  Preferences preferences;
  if (!preferences.begin("rnvconfig", true)) return;
  const String serialized = preferences.getString("config", "");
  preferences.end();
  if (!serialized.length()) return;
  if (deserializeJson(config, serialized)) Serial.println("Saved NVS config is invalid");
}

void startSetupAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("RNV-Haltestelle");
  Serial.printf("Setup AP: %s\n", WiFi.softAPIP().toString().c_str());
}

int fetchDepartures(uint8_t slot, String &resultBody) {
  JsonArrayConst slots = config["slots"].as<JsonArrayConst>();
  const String hafasId = slots.isNull() || slots.size() <= slot ? "" : String(slots[slot] | "");
  if (!hafasId.length()) {
    resultBody = "{\"station\":\"\",\"departures\":[]}";
    return 200;
  }
  if (hafasId.length() > 12 || strspn(hafasId.c_str(), "0123456789") != hafasId.length()) {
    resultBody = "{\"error\":\"Ungültige Haltestellen-ID\"}";
    return 400;
  }
  if ((WiFi.getMode() & WIFI_AP) || WiFi.status() != WL_CONNECTED || !hasRnvCredentials()) {
    apiConnected = false;
    resultBody = "{\"error\":\"Fahrplandaten nicht verfügbar\"}";
    return 503;
  }

  char startTime[25];
  if (!currentUtcIsoString(startTime, sizeof(startTime)) || !getRnvAccessToken()) {
    apiConnected = false;
    resultBody = "{\"error\":\"Fahrplandaten nicht verfügbar\"}";
    return 503;
  }
  String query;
  query.reserve(500);
  query += F("{ station(id: \"");
  query += hafasId;
  query += F("\") { longName journeys(startTime: \"");
  query += startTime;
  query += F("\", first: 3) { elements { ... on Journey { line { id } stops(onlyHafasID: \"");
  query += hafasId;
  query += F("\") { destinationLabel plannedDeparture { isoString } realtimeDeparture { isoString } } } } } } }");
  StaticJsonDocument<2048> request;
  request["query"] = query;
  String requestBody;
  serializeJson(request, requestBody);
  String apiUrl = RNV_API_URL;
  if (!apiUrl.endsWith("/")) apiUrl += '/';
  String responseBody;
  if (!postHttps(apiUrl, "application/json", requestBody, responseBody, "Bearer " + apiAccessToken)) {
    apiConnected = false;
    resultBody = "{\"error\":\"Fahrplandaten nicht verfügbar\"}";
    return 502;
  }

  DynamicJsonDocument response(12288);
  if (deserializeJson(response, responseBody) || !response["errors"].isNull()) {
    apiConnected = false;
    resultBody = "{\"error\":\"Fahrplandaten nicht verfügbar\"}";
    return 502;
  }
  JsonObjectConst station = response["data"]["station"].as<JsonObjectConst>();
  if (station.isNull()) {
    apiConnected = false;
    resultBody = "{\"error\":\"Fahrplandaten nicht verfügbar\"}";
    return 502;
  }

  StaticJsonDocument<2048> result;
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
    departure["destination"] = stop["destinationLabel"] | "";
    departure["time"] = departureTime;
  }
  apiConnected = true;
  serializeJson(result, resultBody);
  return 200;
}

void setupRoutes() {
  server.on("/", HTTP_GET, [] {
    File index = LittleFS.open("/index.html", "r");
    if (!index) {
      server.send(500, "text/plain", "Web UI is missing");
      return;
    }
    server.streamFile(index, "text/html; charset=utf-8");
  });
  server.on("/stops.js", HTTP_GET, [] {
    File stops = LittleFS.open("/stops.js", "r");
    if (!stops) {
      server.send(404, "text/plain", "Haltestellenkatalog fehlt");
      return;
    }
    server.streamFile(stops, "application/javascript; charset=utf-8");
  });
  server.on("/api/departures", HTTP_GET, [] {
    const String slotText = server.arg("slot");
    if (!server.hasArg("slot") || slotText.length() != 1 || slotText[0] < '0' || slotText[0] > '2') {
      server.send(400, "application/json", "{\"error\":\"Ungültiger Bildschirm\"}");
      return;
    }
    const uint8_t slot = static_cast<uint8_t>(slotText[0] - '0');
    String resultBody;
    const int status = fetchDepartures(slot, resultBody);
    if (slot == (config["defaultSlot"] | 0)) {
      if (status == 200) showDepartures(resultBody);
      else if (departureDataLoaded) renderScreen(nullptr, JsonArrayConst(), "Fahrplandaten nicht verfuegbar");
    }
    server.send(status, "application/json", resultBody);
  });
  server.on("/api/status", HTTP_GET, [] {
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    const bool setupApActive = (WiFi.getMode() & WIFI_AP) != 0;
    StaticJsonDocument<384> status;
    status["firmwareVersion"] = firmwareVersion;
    status["filesystemVersion"] = filesystemVersion;
    status["apiStatus"] = !hasRnvCredentials() ? "Nicht konfiguriert" : (apiConnected ? "Verbunden" : "Warte auf Abfrage");
    status["apiConnected"] = apiConnected;
    status["setupApActive"] = setupApActive;
    status["wifiConnected"] = wifiConnected;
    status["wifiSsid"] = wifiConnected ? WiFi.SSID() : (setupApActive ? "RNV-Haltestelle (Setup)" : "Nicht verbunden");
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
    safeConfig.remove("screenFlipped");
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
    const bool updateDevice = incoming.containsKey("hostname") || incoming.containsKey("screenBrightness") ||
                              incoming.containsKey("screenInverted");
    const int updateGroups = updateWifi + updateStations + updateDevice;
    const size_t expectedFields = updateDevice && incoming.containsKey("screenInverted") ? 3 : 2;
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
          (incoming.containsKey("screenInverted") && !incoming["screenInverted"].is<bool>())) {
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
      if (incoming.containsKey("screenInverted")) nextConfig["screenInverted"] = incoming["screenInverted"];
    }

    if (!saveConfig(nextConfig)) {
      server.send(500, "application/json", "{\"error\":\"Could not save settings\"}");
      return;
    }
    config.set(nextConfig);
    if (updateWifi) {
      server.send(200, "application/json", "{\"saved\":true}");
      delay(500);
      ESP.restart();
      return;
    }
    if (updateDevice) {
      WiFi.setHostname(config["hostname"]);
      setScreenBrightness();
      screenDimmed = false;
      lastScreenTouch = millis();
    }
    showLoadingScreen();
    setupScreenDrawn = false;
    lastDisplayRefresh = millis() - 20000UL;
    server.send(200, "application/json", "{\"saved\":true}");
  });
  server.on("/api/restart", HTTP_POST, [] {
    server.send(200, "application/json", "{\"restarting\":true}");
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
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      Update.abort();
      otaUploadOk = false;
    }
  });
  server.on("/api/ota/filesystem", HTTP_POST, [] {
    const bool restart = filesystemOtaStarted;
    server.send(filesystemOtaUploadOk ? 200 : 500, "application/json",
                filesystemOtaUploadOk ? "{\"updated\":true}" : "{\"error\":\"Dateisystem-Update fehlgeschlagen\"}");
    filesystemOtaUploadOk = false;
    filesystemOtaStarted = false;
    if (restart) {
      delay(500);
      ESP.restart();
    }
  }, [] {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      filesystemOtaStarted = false;
      LittleFS.end();
      filesystemOtaUploadOk = Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASHFS);
      filesystemOtaStarted = filesystemOtaUploadOk;
      if (!filesystemOtaUploadOk && !LittleFS.begin(false)) {
        filesystemOtaStarted = true;
      }
    } else if (upload.status == UPLOAD_FILE_WRITE && filesystemOtaUploadOk) {
      filesystemOtaUploadOk = Update.write(upload.buf, upload.currentSize) == upload.currentSize;
      if (!filesystemOtaUploadOk) Update.abort();
    } else if (upload.status == UPLOAD_FILE_END && filesystemOtaUploadOk) {
      filesystemOtaUploadOk = Update.end(true);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      if (filesystemOtaStarted) Update.abort();
      filesystemOtaUploadOk = false;
    }
  });
}

void setup() {
  Serial.begin(115200);
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }
  loadConfig();
  config.remove("screenFlipped");
  if (File file = LittleFS.open("/version.txt", "r")) {
    char version[32];
    const size_t length = file.readBytesUntil('\n', version, sizeof(version) - 1);
    version[length] = '\0';
    filesystemVersion = version;
    filesystemVersion.trim();
    if (!filesystemVersion.length()) filesystemVersion = "Nicht verfügbar";
  }
  if (!config.containsKey("hostname")) config["hostname"] = "rnv-table-station";
  if (!config.containsKey("screenBrightness")) config["screenBrightness"] = 100;
  if (!config.containsKey("screenInverted")) config["screenInverted"] = false;
  pinMode(23, OUTPUT);
  Wire.begin(18, 19);
  pinMode(21, INPUT_PULLUP);
  pinMode(20, OUTPUT);
  digitalWrite(20, LOW);
  delay(200);
  digitalWrite(20, HIGH);
  delay(300);
  if (lcd->begin()) {
    lcdReady = true;
    lcd->setRotation(5);
    lcd->invertDisplay(false);
    setScreenBrightness();
    showLoadingScreen();
  } else {
    Serial.println("LCD initialization failed");
  }

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
    showSetupScreen();
  }
  setupRoutes();
  server.begin();
  lastDisplayRefresh = millis() - 20000UL;
  lastScreenTouch = millis();
}

void loop() {
  server.handleClient();
  if (lcdReady && screenTouched()) {
    lastScreenTouch = millis();
    if (screenDimmed) {
      screenDimmed = false;
      setScreenBrightness();
    }
  } else if (lcdReady && !screenDimmed && millis() - lastScreenTouch >= 60000UL &&
             ((config["screenBrightness"] | 100) / 2) > 30) {
    screenDimmed = true;
    setScreenBrightness(true);
  }
  if (WiFi.getMode() & WIFI_AP) showSetupScreen();
  else if (departureScreenActive && millis() - lastDestinationScroll >= 500UL) {
    lastDestinationScroll = millis();
    renderDestinationCells(lcd->width(), true);
  }
  if (millis() - lastDisplayRefresh >= 20000UL) {
    lastDisplayRefresh = millis();
    if (WiFi.status() == WL_CONNECTED) {
      String resultBody;
      const uint8_t slot = config["defaultSlot"] | 0;
      if (fetchDepartures(slot, resultBody) == 200) showDepartures(resultBody);
      else if (departureDataLoaded) renderScreen(nullptr, JsonArrayConst(), "Fahrplandaten nicht verfuegbar");
    }
  }
}
