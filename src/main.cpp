#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <esp_sntp.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>

#include "RTClib.h"
#include "mfactoryfont.h"   // Custom font
#include "tz_lookup.h"      // Timezone lookup

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES   4

MD_Parola P = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
RTC_DS3231 rtc;
AsyncWebServer server(80);

// Settings
char ssids[10][32]     = {"","","","","","","","","",""};
char passwords[10][64] = {"","","","","","","","","",""};
char timeZone[64]      = "";
int  brightness        = 10;
bool flipDisplay       = false;
char ntpServer1[128]   = "pool.ntp.org";
char ntpServer2[128]   = "time.nist.gov";
char mdns[64]          = "";

// Globals
bool          isAPMode                   = false;
int           displayMode                = 0;  // 0: Clock, 3: Countupdown
bool          ntpSyncSuccessful          = false;
unsigned long lastColonBlink             = 0;
time_t        countupdownTargetTimestamp = 0;  // Unix timestamp

// State management
WiFiClient client;
DNSServer dnsServer;
const byte DNS_PORT = 53;

// NTP Synchronization State Machine
enum NtpState {
  NTP_IDLE,
  NTP_SYNCING,
  NTP_SUCCESS,
  NTP_FAILED
};
NtpState            ntpState               = NTP_IDLE;
unsigned long       ntpStartTime           = 0;
const int           ntpTimeout             = 10000;  // 10 seconds
const int           maxNtpRetries          = 3;
int                 ntpRetryCount          = 0;

// --- Safe WiFi credential getters ---
const char *getSafeSsid(int ix) {
  return isAPMode ? "" : ssids[ix];
}

const char *getSafePassword(int ix) {
  if (strlen(passwords[ix]) == 0) {  // No password set yet — return empty string for fresh install
    return "";
  } else {  // Password exists — mask it in the web UI
    return "********";
  }
}

// -----------------------------------------------------------------------------
// Configuration Load & Save
// -----------------------------------------------------------------------------
void loadConfig() {
  Serial.println(F("[CONFIG] Loading configuration..."));

  // Check if config.json exists, if not, create default
  if (!LittleFS.exists("/config.json")) {
    Serial.println(F("[CONFIG] config.json not found, creating with defaults..."));
    JsonDocument doc;
    doc[F("timeZone")] = timeZone;
    doc[F("brightness")] = brightness;
    doc[F("flipDisplay")] = flipDisplay;
    doc[F("mdns")] = mdns;

    JsonArray ssidArray = doc["ssids"].to<JsonArray>();
    JsonArray pwdArray = doc["passwords"].to<JsonArray>();
    for (int i=0;i<10;i++) {
      ssidArray.add("");
      pwdArray.add("");
    }

    // Add countupdown defaults when creating a new config.json
    JsonObject countupdownObj = doc["countupdown"].to<JsonObject>();
    countupdownObj["targetTimestamp"] = 0;

    File f = LittleFS.open("/config.json", "w");
    if (f) {
      serializeJsonPretty(doc, f);
      f.close();
      Serial.println(F("[CONFIG] Default config.json created."));
    } else {
      Serial.println(F("[ERROR] Failed to create default config.json"));
    }
  }

  Serial.println(F("[CONFIG] Attempting to open config.json for reading."));
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println(F("[ERROR] Failed to open config.json for reading. Cannot load config."));
    return;
  }

  JsonDocument doc;  // Size based on ArduinoJson Assistant + buffer
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (error) {
    Serial.print(F("[ERROR] JSON parse failed during load: "));
    Serial.println(error.f_str());
    return;
  }

  JsonArray ssidArray = doc["ssids"];
  JsonArray pwdArray = doc["passwords"];
  for (int i=0; i<10; i++) {
    strlcpy(ssids[i], ssidArray[i] | "", sizeof(ssids[i]));
    strlcpy(passwords[i], pwdArray[i] | "", sizeof(passwords[i]));
  }
  strlcpy(timeZone, doc["timeZone"] | "Etc/UTC", sizeof(timeZone));

  brightness = doc["brightness"] | 7;
  flipDisplay = doc["flipDisplay"] | false;
  strlcpy(ntpServer1, doc["ntpServer1"] | "pool.ntp.org", sizeof(ntpServer1));
  strlcpy(ntpServer2, doc["ntpServer2"] | "time.nist.gov", sizeof(ntpServer2));
  strlcpy(mdns, doc["mdns"] | "chronoclock", sizeof(mdns));

  // --- COUNTUPDOWN CONFIG LOADING ---
  if (doc["countupdown"].is<JsonObject>()) {
    JsonObject countupdownObj = doc["countupdown"];
    countupdownTargetTimestamp = countupdownObj["targetTimestamp"] | 0;
  } else {
    countupdownTargetTimestamp = 0;
    Serial.println(F("[CONFIG] Countupdown object not found, defaulting to disabled."));
  }
  Serial.println(F("[CONFIG] Configuration loaded."));
}

bool saveCountupdownConfig(time_t targetTimestamp) {
  JsonDocument doc;

  File configFile = LittleFS.open("/config.json", "r");
  if (configFile) {
    DeserializationError err = deserializeJson(doc, configFile);
    configFile.close();
    if (err) {
      Serial.print(F("[saveCountupdownConfig] Error parsing config.json: "));
      Serial.println(err.f_str());
      return false;
    }
  }

  JsonObject countupdownObj = doc["countupdown"].is<JsonObject>() ? doc["countupdown"].as<JsonObject>() : doc["countupdown"].to<JsonObject>();
  countupdownObj["targetTimestamp"] = targetTimestamp;

  if (LittleFS.exists("/config.json")) {
    LittleFS.rename("/config.json", "/config.bak");
  }

  File f = LittleFS.open("/config.json", "w");
  if (!f) {
    Serial.println(F("[saveCountupdownConfig] ERROR: Cannot write to /config.json"));
    return false;
  }

  size_t bytesWritten = serializeJson(doc, f);
  f.close();

  Serial.printf("[saveCountupdownConfig] Config updated. %u bytes written.\n", bytesWritten);
  return true;
}

bool saveBrightnessConfig(int brightness) {
  JsonDocument doc;

  File configFile = LittleFS.open("/config.json", "r");
  if (configFile) {
    DeserializationError err = deserializeJson(doc, configFile);
    configFile.close();
    if (err) {
      Serial.print(F("[saveBrightness] Error parsing config.json: "));
      Serial.println(err.f_str());
      return false;
    }
  }
  
  doc[F("brightness")] = brightness;

  if (LittleFS.exists("/config.json")) {
    LittleFS.rename("/config.json", "/config.bak");
  }

  File f = LittleFS.open("/config.json", "w");
  if (!f) {
    Serial.println(F("[saveBrightness] ERROR: Cannot write to /config.json"));
    return false;
  }

  size_t bytesWritten = serializeJson(doc, f);
  f.close();

  Serial.printf("[saveBrightness] Config updated. %u bytes written.\n", bytesWritten);
  return true;
}

bool saveFlipDisplay() {
  JsonDocument doc;

  File configFile = LittleFS.open("/config.json", "r");
  if (configFile) {
    DeserializationError err = deserializeJson(doc, configFile);
    configFile.close();
    if (err) {
      Serial.print(F("[saveFlipDisplay] Error parsing config.json: "));
      Serial.println(err.f_str());
      return false;
    }
  }
  
  doc[F("flipDisplay")] = flipDisplay;

  if (LittleFS.exists("/config.json")) {
    LittleFS.rename("/config.json", "/config.bak");
  }

  File f = LittleFS.open("/config.json", "w");
  if (!f) {
    Serial.println(F("[saveFlipDisplay] ERROR: Cannot write to /config.json"));
    return false;
  }

  size_t bytesWritten = serializeJson(doc, f);
  f.close();

  Serial.printf("[saveFlipDisplay] Config updated. %u bytes written.\n", bytesWritten);
  return true;
}

// -----------------------------------------------------------------------------
// WiFi Setup
// -----------------------------------------------------------------------------
const char *DEFAULT_AP_PASSWORD = "chrono157";
const char *AP_SSID = "ChronoClock";

void connectWiFi() {
  Serial.println(F("[WIFI] Connecting to WiFi..."));

  bool credentialsExist = false;

  for (int i=0; i<10; i++) {
    credentialsExist = credentialsExist || (strlen(ssids[i]) > 0);
  }

  if (!credentialsExist) {
    Serial.println(F("[WIFI] No saved credentials. Starting AP mode directly."));
    WiFi.mode(WIFI_AP);
    WiFi.disconnect(true);
    delay(100);

    if (strlen(DEFAULT_AP_PASSWORD) < 8) {
      WiFi.softAP(AP_SSID);
      Serial.println(F("[WIFI] AP Mode started (no password, too short)."));
    } else {
      WiFi.softAP(AP_SSID, DEFAULT_AP_PASSWORD);
      Serial.println(F("[WIFI] AP Mode started."));
    }

    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    Serial.print(F("[WIFI] AP IP address: "));
    Serial.println(WiFi.softAPIP());
    isAPMode = true;

    WiFiMode_t mode = WiFi.getMode();
    Serial.printf("[WIFI] WiFi mode after setting AP: %s\n",
                  mode == WIFI_OFF    ? "OFF"
                : mode == WIFI_STA    ? "STA ONLY"
                : mode == WIFI_AP     ? "AP ONLY"
                : mode == WIFI_AP_STA ? "AP + STA (Error!)"
                                      : "UNKNOWN");

    Serial.println(F("[WIFI] AP Mode Started"));
    return;
  }

  // If credentials exist, attempt STA connection
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  for (int i=0;i<10;i++) {
    if (strlen(ssids[i]) > 0) {
      WiFi.begin(ssids[i], passwords[i]);
      unsigned long startAttemptTime = millis();
    
      const unsigned long timeout = 10000; // 10 second timeout
      while (true) {
        unsigned long now = millis();
        // Connection successful
        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("[WiFi] Connected: " + WiFi.localIP().toString());
          isAPMode = false;
          WiFiMode_t mode = WiFi.getMode();
          Serial.printf("[WIFI] WiFi mode after STA connection: %s\n",
                        mode == WIFI_OFF ? "OFF"
                      : mode == WIFI_STA    ? "STA ONLY"
                      : mode == WIFI_AP     ? "AP ONLY"
                      : mode == WIFI_AP_STA ? "AP + STA (Error!)"
                                            : "UNKNOWN");
          break; // Exit the connection loop
        // Connection timed out...
        } else if (now - startAttemptTime >= timeout) {
          break; // Exit the connection loop
        }
        delay(1);
      }
    } // End animation
    // Break loop when connected to a network.
    if (WiFi.status() == WL_CONNECTED) {
      break;
    }
  } // End loop for connecting to Wifi
  // Didn't connect to any network.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[WiFi] Failed. Starting AP mode..."));
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, DEFAULT_AP_PASSWORD);
    Serial.print(F("[WiFi] AP IP address: "));
    Serial.println(WiFi.softAPIP());
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    isAPMode = true;
    WiFiMode_t mode = WiFi.getMode();
    Serial.printf("[WIFI] WiFi mode after STA failure and setting AP: %s\n",
                  mode == WIFI_OFF ? "OFF"
                : mode == WIFI_STA    ? "STA ONLY"
                : mode == WIFI_AP     ? "AP ONLY"
                : mode == WIFI_AP_STA ? "AP + STA (Error!)"
                                      : "UNKNOWN");
    Serial.println(F("[WIFI] AP Mode Started"));
  }
  // Wifi connected, start mdns
  if (!MDNS.begin(mdns)) {
    Serial.println(F("[WIFI] Error setting up mDNS responder."));
    while (1) {
      delay(1000);
    }
  }
  Serial.println(F("[WIFI] mDNS responder started."));
}

void clearWiFiCredentialsInConfig() {
  JsonDocument doc;

  // Open existing config, if present
  File configFile = LittleFS.open("/config.json", "r");
  if (configFile) {
    DeserializationError err = deserializeJson(doc, configFile);
    configFile.close();
    if (err) {
      Serial.print(F("[SECURITY] Error parsing config.json: "));
      Serial.println(err.f_str());
      return;
    }
  }

  JsonArray ssidArray = doc["ssids"].to<JsonArray>();
  JsonArray pwdArray = doc["passwords"].to<JsonArray>();
  for (int i=0;i<10;i++) {
    ssidArray.add("");
    pwdArray.add("");
  }

  // Optionally backup previous config
  if (LittleFS.exists("/config.json")) {
    LittleFS.rename("/config.json", "/config.bak");
  }

  File f = LittleFS.open("/config.json", "w");
  if (!f) {
    Serial.println(F("[SECURITY] ERROR: Cannot write to /config.json to clear credentials!"));
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println(F("[SECURITY] Cleared WiFi credentials in config.json."));
}

// -----------------------------------------------------------------------------
// Time Functions
// -----------------------------------------------------------------------------
void startNTPSync(bool resetTime, int retryCount = 0) {
  if (isAPMode) {
    return;
  }
  Serial.println(F("[TIME] Starting NTP sync"));
  bool serverOk = false;
  IPAddress resolvedIP;

  // Try first server if it's not empty
  if (strlen(ntpServer1) > 0 && WiFi.hostByName(ntpServer1, resolvedIP) == 1) {
    serverOk = true;
  }
  // Try second server if first failed
  else if (strlen(ntpServer2) > 0 && WiFi.hostByName(ntpServer2, resolvedIP) == 1) {
    serverOk = true;
  }

  if (resetTime) {
    struct timeval now = { .tv_sec = 0 };
    settimeofday(&now, NULL);
  }
  ntpState = NTP_SYNCING;
  ntpStartTime = millis();
  ntpSyncSuccessful = false;
  if (serverOk) {
    configTime(0, 0, ntpServer1, ntpServer2);
    setenv("TZ", ianaToPosix(timeZone), 1);
    tzset();
    ntpRetryCount = retryCount + 1;
  }
}

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------
void printConfigToSerial() {
  Serial.println(F("========= Loaded Configuration ========="));
  for (int i=0;i<10;i++) {
    Serial.print(F("Wifi Network "));
    Serial.print(i);
    Serial.print(F(": "));
    Serial.print(ssids[i]);
    Serial.print(F(" - "));
    Serial.println(passwords[i]);
  }
  Serial.print(F("TimeZone (IANA): "));
  Serial.println(timeZone);
  Serial.print(F("Brightness: "));
  Serial.println(brightness);
  Serial.print(F("Flip Display: "));
  Serial.println(flipDisplay ? "Yes" : "No");
  Serial.print(F("NTP Server 1: "));
  Serial.println(ntpServer1);
  Serial.print(F("NTP Server 2: "));
  Serial.println(ntpServer2);
  Serial.print(F("mDNS: "));
  Serial.println(mdns);
  Serial.print(F("Countupdown Target Timestamp: "));
  Serial.println(countupdownTargetTimestamp);
  Serial.println(F("========================================"));
  Serial.println();
}

// -----------------------------------------------------------------------------
// Web Server and Captive Portal
// -----------------------------------------------------------------------------
void setupWebServer() {
  Serial.println(F("[WEBSERVER] Setting up web server..."));

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[WEBSERVER] Request: /"));
    request->send(LittleFS, "/index.html", "text/html");
  });

  server.on("/config.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println(F("[WEBSERVER] Request: /config.json"));
    File f = LittleFS.open("/config.json", "r");
    if (!f) {
      Serial.println(F("[WEBSERVER] Error opening /config.json"));
      request->send(500, "application/json", "{\"error\":\"Failed to open config.json\"}");
      return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      Serial.print(F("[WEBSERVER] Error parsing /config.json: "));
      Serial.println(err.f_str());
      request->send(500, "application/json", "{\"error\":\"Failed to parse config.json\"}");
      return;
    }

    // Always sanitize before sending to browser
    JsonArray ssidArray = doc["ssids"];
    JsonArray pwdArray = doc["passwords"];
    for (int i=0;i<10;i++) {
      ssidArray[i] = getSafeSsid(i);
      pwdArray[i] = getSafePassword(i);
    }
    doc[F("mode")] = isAPMode ? "ap" : "sta";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println(F("[WEBSERVER] Request: /save"));
    bool restartWifi = false;
    JsonDocument doc;
    File configFile = LittleFS.open("/config.json", "r");
    if (configFile) {
      Serial.println(F("[WEBSERVER] Existing config.json found, loading for update..."));
      DeserializationError err = deserializeJson(doc, configFile);
      configFile.close();
      if (err) {
        Serial.print(F("[WEBSERVER] Error parsing existing config.json: "));
        Serial.println(err.f_str());
      }
    } else {
      Serial.println(F("[WEBSERVER] config.json not found, starting with empty doc for save."));
    }

    JsonArray ssidArray = doc["ssids"];
    JsonArray pwdArray = doc["passwords"];

    for (int i = 0; i < request->params(); i++) {
      const AsyncWebParameter *p = request->getParam(i);
      String n = p->name();
      String v = p->value();

      if (n == "brightness") {
        doc[n] = v.toInt();
      } else if (n == "flipDisplay") {
        doc[n] = (v == "true" || v == "on" || v == "1");
      } else if (n == "password0"
              || n == "password1"
              || n == "password2"
              || n == "password3"
              || n == "password4"
              || n == "password5"
              || n == "password6"
              || n == "password7"
              || n == "password8"
              || n == "password9") {
        if (v != "********" && v.length() > 0) {
          int num = n.substring(8,9).toInt();
          pwdArray[num] = v; // user entered a new password
          Serial.print(F("[SAVE] Password change: "));
          Serial.println(num+1);
          if (String(passwords[num]) != v) {
            restartWifi = true;
          }
          strlcpy(passwords[num], v.c_str(), sizeof(passwords[num]));
        } else {
          Serial.println(F("[SAVE] Password unchanged."));
          // do nothing, keep the one already in doc
        }
      } else if (n == "ssid0"
              || n == "ssid1"
              || n == "ssid2"
              || n == "ssid3"
              || n == "ssid4"
              || n == "ssid5"
              || n == "ssid6"
              || n == "ssid7"
              || n == "ssid8"
              || n == "ssid9") {
        int num = n.substring(4,5).toInt();
        ssidArray[num] = v;
        Serial.print(F("[SAVE] SSID change: "));
        Serial.print(v);
        Serial.print(F(" - "));
        Serial.println(num);
        if (String(ssids[num]) != v) {
          restartWifi = true;
        }
        strlcpy(ssids[num], v.c_str(), sizeof(ssids[num]));
      } else if (n != "countupdownDate" && n != "countupdownTime" && n != "mode") {
        doc[n] = v;
      }
    }

    String countupdownDateStr = request->hasParam("countupdownDate", true) ? request->getParam("countupdownDate", true)->value() : "";
    String countupdownTimeStr = request->hasParam("countupdownTime", true) ? request->getParam("countupdownTime", true)->value() : "";

    time_t newTargetTimestamp = 0;
    if (countupdownDateStr.length() > 0 && countupdownTimeStr.length() > 0) {
      int year = countupdownDateStr.substring(0, 4).toInt();
      int month = countupdownDateStr.substring(5, 7).toInt();
      int day = countupdownDateStr.substring(8, 10).toInt();
      int hour = countupdownTimeStr.substring(0, 2).toInt();
      int minute = countupdownTimeStr.substring(3, 5).toInt();
      int second = countupdownTimeStr.substring(6, 8).toInt();

      struct tm tm;
      tm.tm_year = year - 1900;
      tm.tm_mon = month - 1;
      tm.tm_mday = day;
      tm.tm_hour = hour;
      tm.tm_min = minute;
      tm.tm_sec = second;
      tm.tm_isdst = -1;

      newTargetTimestamp = mktime(&tm);
      if (newTargetTimestamp == (time_t)-1) {
        Serial.println("[SAVE] Error converting countupdown date/time to timestamp.");
        newTargetTimestamp = 0;
      } else {
        Serial.printf("[SAVE] Converted countupdown target: %s %s -> %lu\n", countupdownDateStr.c_str(), countupdownTimeStr.c_str(), newTargetTimestamp);
      }
      countupdownTargetTimestamp = newTargetTimestamp;
    } else {
      countupdownTargetTimestamp = 0;
    }

    JsonObject countupdownObj = doc["countupdown"].to<JsonObject>();
    countupdownObj["targetTimestamp"] = countupdownTargetTimestamp;

    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    Serial.printf("[SAVE] LittleFS total bytes: %llu, used bytes: %llu\n", LittleFS.totalBytes(), LittleFS.usedBytes());

    if (LittleFS.exists("/config.json")) {
      Serial.println(F("[SAVE] Renaming /config.json to /config.bak"));
      LittleFS.rename("/config.json", "/config.bak");
    }
    File f = LittleFS.open("/config.json", "w");
    if (!f) {
      Serial.println(F("[SAVE] ERROR: Failed to open /config.json for writing!"));
      JsonDocument errorDoc;
      errorDoc[F("error")] = "Failed to write config file.";
      String response;
      serializeJson(errorDoc, response);
      request->send(500, "application/json", response);
      return;
    }

    size_t bytesWritten = serializeJson(doc, f);
    Serial.printf("[SAVE] Bytes written to /config.json: %u\n", bytesWritten);
    f.close();
    Serial.println(F("[SAVE] /config.json file closed."));

    File verify = LittleFS.open("/config.json", "r");
    if (!verify) {
      Serial.println(F("[SAVE] ERROR: Failed to open /config.json for reading during verification!"));
      JsonDocument errorDoc;
      errorDoc[F("error")] = "Verification failed: Could not re-open config file.";
      String response;
      serializeJson(errorDoc, response);
      request->send(500, "application/json", response);
      return;
    }

    while (verify.available()) {
      verify.read();
    }
    verify.seek(0);

    JsonDocument test;
    DeserializationError err = deserializeJson(test, verify);
    verify.close();

    if (err) {
      Serial.print(F("[SAVE] Config corrupted after save: "));
      Serial.println(err.f_str());
      JsonDocument errorDoc;
      errorDoc[F("error")] = String("Config corrupted. Reboot cancelled. Error: ") + err.f_str();
      String response;
      serializeJson(errorDoc, response);
      request->send(500, "application/json", response);
      return;
    }

    Serial.println(F("[SAVE] Config verification successful."));
    JsonDocument okDoc;
    okDoc[F("message")] = "Saved successfully. Rebooting...";
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
    Serial.println(F("[WEBSERVER] Sending success response and scheduling reboot..."));

    if (restartWifi) {
      connectWiFi();
    }
  });

  server.on("/restore", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println(F("[WEBSERVER] Request: /restore"));
    if (LittleFS.exists("/config.bak")) {
      File src = LittleFS.open("/config.bak", "r");
      if (!src) {
        Serial.println(F("[WEBSERVER] Failed to open /config.bak"));
        JsonDocument errorDoc;
        errorDoc[F("error")] = "Failed to open backup file.";
        String response;
        serializeJson(errorDoc, response);
        request->send(500, "application/json", response);
        return;
      }
      File dst = LittleFS.open("/config.json", "w");
      if (!dst) {
        src.close();
        Serial.println(F("[WEBSERVER] Failed to open /config.json for writing"));
        JsonDocument errorDoc;
        errorDoc[F("error")] = "Failed to open config for writing.";
        String response;
        serializeJson(errorDoc, response);
        request->send(500, "application/json", response);
        return;
      }

      while (src.available()) {
        dst.write(src.read());
      }
      src.close();
      dst.close();

      JsonDocument okDoc;
      okDoc[F("message")] = "✅ Backup restored! Device will now reboot.";
      String response;
      serializeJson(okDoc, response);
      request->send(200, "application/json", response);
      request->onDisconnect([]() {
        Serial.println(F("[WEBSERVER] Rebooting after restore..."));
        ESP.restart();
      });

    } else {
      Serial.println(F("[WEBSERVER] No backup found"));
      JsonDocument errorDoc;
      errorDoc[F("error")] = "No backup found.";
      String response;
      serializeJson(errorDoc, response);
      request->send(404, "application/json", response);
    }
  });

  server.on("/clear_wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println(F("[WEBSERVER] Request: /clear_wifi"));
    clearWiFiCredentialsInConfig();

    JsonDocument okDoc;
    okDoc[F("message")] = "✅ WiFi credentials cleared! Rebooting...";
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);

    request->onDisconnect([]() {
      Serial.println(F("[WEBSERVER] Rebooting after clearing WiFi..."));
      ESP.restart();
    });
  });
  
  server.on("/ap_status", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.print(F("[WEBSERVER] Request: /ap_status. isAPMode = "));
    Serial.println(isAPMode);
    String json = "{\"isAP\": ";
    json += (isAPMode) ? "true" : "false";
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/set_brightness", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("value", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
      return;
    }
    int newBrightness = request->getParam("value", true)->value().toInt();

    // Clamp brightness to valid range
    if (newBrightness < 1) newBrightness = 1;
    if (newBrightness > 15) newBrightness = 15;
    
    Serial.printf("[WEBSERVER] Setting brightness to %d from %d\n", newBrightness, brightness);
    brightness = newBrightness;
    P.setIntensity(brightness);
    saveBrightnessConfig(brightness);
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/set_flip", HTTP_POST, [](AsyncWebServerRequest *request) {
    bool flip = false;
    if (request->hasParam("value", true)) {
      String v = request->getParam("value", true)->value();
      flip = (v == "1" || v == "true" || v == "on");
    }
    flipDisplay = flip;
    P.setZoneEffect(0, flipDisplay, PA_FLIP_UD);
    P.setZoneEffect(0, flipDisplay, PA_FLIP_LR);
    Serial.printf("[WEBSERVER] Set flipDisplay to %d\n", flipDisplay);
    saveFlipDisplay();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true}");
    ESP.restart();
  });

  server.on("/set_countupdown", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("DateTime", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
    }

    String DateTimeStr = request->getParam("DateTime", true)->value();

    time_t newTargetTimestamp = 0;
    if (DateTimeStr.length() == 19) {
      int year = DateTimeStr.substring(0, 4).toInt();
      int month = DateTimeStr.substring(5, 7).toInt();
      int day = DateTimeStr.substring(8, 10).toInt();
      int hour = DateTimeStr.substring(11, 13).toInt();
      int minute = DateTimeStr.substring(14, 16).toInt();
      int second = DateTimeStr.substring(17, 19).toInt();

      struct tm tm;
      tm.tm_year = year - 1900;
      tm.tm_mon = month - 1;
      tm.tm_mday = day;
      tm.tm_hour = hour;
      tm.tm_min = minute;
      tm.tm_sec = second;
      tm.tm_isdst = -1;

      newTargetTimestamp = mktime(&tm);
      if (newTargetTimestamp == (time_t)-1) {
        Serial.println("[SAVE] Error converting countupdown date/time to timestamp.");
        newTargetTimestamp = 0;
      } else {
        Serial.printf("[SAVE] Converted countupdown target: %s -> %lu\n", DateTimeStr.c_str(), newTargetTimestamp);
      }
      countupdownTargetTimestamp = newTargetTimestamp;
      saveCountupdownConfig(countupdownTargetTimestamp);
    }
  });

  server.on("/start_countup", HTTP_GET, [](AsyncWebServerRequest *request) {
    DateTime dtNow = rtc.now();
    countupdownTargetTimestamp = dtNow.unixtime();
    saveCountupdownConfig(countupdownTargetTimestamp);
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/set_time", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("DateTime", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
    }
    String DateTimeStr = request->getParam("DateTime", true)->value();
    if (DateTimeStr.length() == 19) {
      int year = DateTimeStr.substring(0, 4).toInt();
      int month = DateTimeStr.substring(5, 7).toInt();
      int day = DateTimeStr.substring(8, 10).toInt();
      int hour = DateTimeStr.substring(11, 13).toInt();
      int minute = DateTimeStr.substring(14, 16).toInt();
      int second = DateTimeStr.substring(17, 19).toInt();

      struct tm tm;
      tm.tm_year = year - 1900;
      tm.tm_mon = month - 1;
      tm.tm_mday = day;
      tm.tm_hour = hour;
      tm.tm_min = minute;
      tm.tm_sec = second;
      tm.tm_isdst = -1;

      time_t newTime = mktime(&tm);
      if (newTime != (time_t)-1) {
        struct timeval now = {.tv_sec = newTime };
        settimeofday(&now, NULL);
        rtc.adjust(DateTime(time(nullptr)));
      }
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/ntp_sync", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (ntpState == NTP_IDLE) {
      startNTPSync(true, 0);
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.begin();
  Serial.println(F("[WEBSERVER] Web server started"));
}

// -----------------------------------------------------------------------------
// Main setup() and loop()
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println(F("[SETUP] Starting setup..."));

  // Check if RTC was found.
  if (!rtc.begin()) {
    Serial.println(F("[SETUP] Unable to find RTC."));
    return;
  }

  // Set time if new device or after a power loss.
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  if (!LittleFS.begin(true)) {
    Serial.println(F("[ERROR] LittleFS mount failed in setup! Halting."));
    while (true) {
      delay(1000);
      yield();
    }
  }
  Serial.println(F("[SETUP] LittleFS file system mounted successfully."));

  P.begin();  // Initialize Parola library

  P.setCharSpacing(0);
  P.setFont(mFactory);
  loadConfig();  // This function now has internal yields and prints

  P.setIntensity(brightness);
  P.setZoneEffect(0, flipDisplay, PA_FLIP_UD);
  P.setZoneEffect(0, flipDisplay, PA_FLIP_LR);

  Serial.println(F("[SETUP] Parola (LED Matrix) initialized"));

  connectWiFi();

  if (isAPMode) {
    Serial.println(F("[SETUP] WiFi connection failed. Device is in AP Mode."));
  } else if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("[SETUP] WiFi connected successfully to local network."));
  } else {
    Serial.println(F("[SETUP] WiFi state is uncertain after connection attempt."));
  }

  setupWebServer();
  Serial.println(F("[SETUP] Webserver setup complete"));
  Serial.println(F("[SETUP] Setup complete"));
  Serial.println();
  printConfigToSerial();
  lastColonBlink = millis();
}

void loop() {
  if (isAPMode) {
    dnsServer.processNextRequest();
  }

  static bool colonVisible = true;
  const unsigned long colonBlinkInterval = 800;
  if (millis() - lastColonBlink > colonBlinkInterval) {
    colonVisible = !colonVisible;
    lastColonBlink = millis();
  }

  // --- NTP State Machine ---
  switch (ntpState) {
    case NTP_IDLE: break;
    case NTP_SYNCING:
      {
        time_t lNow = time(nullptr);
        if (lNow > 1000) {  // NTP sync successful
          Serial.println(F("[TIME] NTP sync successful."));
          ntpSyncSuccessful = true;
          ntpState = NTP_SUCCESS;
        } else if (millis() - ntpStartTime > ntpTimeout && ntpRetryCount < maxNtpRetries) {
          Serial.println(F("[TIME] NTP sync failed."));
          ntpSyncSuccessful = false;
          ntpState = NTP_FAILED;
        } else if (ntpRetryCount >= maxNtpRetries) {
          ntpState = NTP_IDLE;
        }
        break;
      }
    case NTP_SUCCESS:
      {
        Serial.println(F("[TIME] Setting timezone then adjusting RTC clock."));
        setenv("TZ", ianaToPosix(timeZone), 1);
        tzset();
        ntpState = NTP_IDLE;
        time_t now_time = time(nullptr);
        struct tm timeinfo;
        localtime_r(&now_time, &timeinfo);
        rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
        break;
      }
    case NTP_FAILED:
      {
        startNTPSync(false, ntpRetryCount);
        Serial.println(F("[TIME] Retrying NTP sync..."));
        break;
      }
  }

  DateTime dtNow = rtc.now();
  if (countupdownTargetTimestamp > 0) { // --- COUNTUPDOWN Display Mode ---
    static int countupdownSegment = 0;
    static unsigned long segmentStartTime = 0;
    const unsigned long SEGMENT_DISPLAY_DURATION = 1500;  // 1.5 seconds for each static segment

    long timeSeconds = countupdownTargetTimestamp - dtNow.unixtime();
    if (timeSeconds < 0) {
      timeSeconds = timeSeconds * -1;
    }

    long hours = timeSeconds / 3600;
    long minutes = (timeSeconds % 3600) / 60;
    long seconds = timeSeconds % 60;

    // Format the full string
    char timeWithSeconds[12];
    sprintf(timeWithSeconds, "%d:%02d:%02d", hours, minutes, seconds);

    // keep spacing logic the same ---
    char timeSpacedStr[24];
    int j = 0;
    for (int i = 0; timeWithSeconds[i] != '\0'; i++) {
      timeSpacedStr[j++] = timeWithSeconds[i];
      if (timeWithSeconds[i + 1] != '\0') {
        timeSpacedStr[j++] = ' ';
      }
    }
    timeSpacedStr[j] = '\0';
    // build final string ---
    String formattedTime = String(timeSpacedStr);
    P.setCharSpacing(0);
    // --- DISPLAY COUNTUPDOWN ---
    String timeString = formattedTime;
    if (!colonVisible) {
      timeString.replace(":", " ");
    }
    P.setTextAlignment(PA_CENTER);
    P.print(timeString);
  }  // End COUNTUPDOWN Display Mode
  else { // --- CLOCK Display Mode ---
    // build base HH:MM:SS first ---
    char timeWithSeconds[12];
    sprintf(timeWithSeconds, "%02d:%02d:%02d", dtNow.hour(), dtNow.minute(), dtNow.second());
  
    // keep spacing logic the same ---
    char timeSpacedStr[24];
    int j = 0;
    for (int i = 0; timeWithSeconds[i] != '\0'; i++) {
      timeSpacedStr[j++] = timeWithSeconds[i];
      if (timeWithSeconds[i + 1] != '\0') {
        timeSpacedStr[j++] = ' ';
      }
    }
    timeSpacedStr[j] = '\0';
  
    // build final string ---
    String formattedTime = String(timeSpacedStr);
    P.setCharSpacing(0);

    // --- DISPLAY CLOCK ---
    P.setTextAlignment(PA_CENTER);
    P.print(formattedTime);
  } // End CLOCK Display Mode
  yield();
}