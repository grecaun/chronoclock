#include <Arduino.h>
#if ESPVERS == 32
#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <esp_sntp.h>
#endif
#if ESPVERS == 8266
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncTCP.h>
#include <sntp.h>
#endif
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <time.h>
#include <ElegantOTA.h>
#include "RTClib.h"
#include "mfactoryfont.h"   // Custom font
#include "tz_lookup.h"      // Timezone lookup
#include "auth.h"           // Auth information

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES   4

const char *DEFAULT_AP_SSID = "chronoclock";
const char *DEFAULT_AP_PASSWORD = "chrono157";

MD_Parola P = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
RTC_DS3231 rtc;
AsyncWebServer server(80);

const uint32_t wifiTimeout = 10000;  // 10 second timeout
const uint32_t apTimeout   = 300000; // 5 minutes
enum ChronoWiFiState {
  WIFI_CONNECTED,
  WIFI_WORKING,
  WIFI_DISCONNECTED,
  WIFI_APMODE
};
ChronoWiFiState wifiState    = WIFI_DISCONNECTED;
uint8_t         wifiNetNum   = 0;
uint32_t        wifiLastTime = 0;

#if ESPVERS == 32
void WiFiStationGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println(F("[WIFI] Connection successful."));
  wifiState = WIFI_CONNECTED;
  WiFiMode_t mode = WiFi.getMode();
  Serial.printf("[WIFI] WiFi mode after STA connection: %s\n",
                mode == WIFI_OFF    ? "OFF"
              : mode == WIFI_STA    ? "STA ONLY"
              : mode == WIFI_AP     ? "AP ONLY"
              : mode == WIFI_AP_STA ? "AP + STA (Error!)"
                                    : "UNKNOWN");
    Serial.println("[WIFI] IP: " + WiFi.localIP().toString());
}

void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println("[WIFI] Network has disconnected.");
  wifiLastTime = millis();
  wifiState = WIFI_DISCONNECTED;
}
#endif
#if ESPVERS == 8266
WiFiEventHandler WiFiStationGotIP, WiFiStationDisconnected;
#endif

// Settings
char mdns[64]          = "";
char apSsid[32]        = "";
char apPassword[64]    = "";
char ssids[10][32]     = {"","","","","","","","","",""};
char passwords[10][64] = {"","","","","","","","","",""};
char timeZone[64]      = "";
int  brightness        = 10;
bool flipDisplay       = false;
char ntpServer1[128]   = "pool.ntp.org";
char ntpServer2[128]   = "time.nist.gov";

// Globals
bool          rtcEnabled                 = false;
unsigned long lastColonBlink             = 0;
time_t        countupdownTargetTimestamp = 0;  // Unix timestamp

// State management
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
unsigned long       ntpLastTime            = 0;
const int           ntpTimeout             = 10000;   // 10 seconds
const int           ntpRefreshTime         = 3600000; // Auto refresh NTP sync every hour with no RTC
const int           maxNtpRetries          = 3;
int                 ntpRetryCount          = 0;

// --- Safe WiFi credential getters ---
const char *getSafeSsid(int ix) {
  return ssids[ix];
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
    doc[F("mdns")] = mdns;
    doc[F("apSsid")] = apSsid;
    doc[F("apPassword")] = apPassword;
    doc[F("timeZone")] = timeZone;
    doc[F("brightness")] = brightness;
    doc[F("flipDisplay")] = flipDisplay;
    doc[F("ntpServer1")] = ntpServer1;
    doc[F("ntpServer2")] = ntpServer2;

    JsonArray ssidArray = doc["ssids"].to<JsonArray>();
    JsonArray pwdArray = doc["passwords"].to<JsonArray>();
    for (int i=0;i<10;i++) {
      ssidArray[i] = ssids[i];
      pwdArray[i] = passwords[i];
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
      Serial.println(F("[CONFIG] Failed to create default config.json"));
    }
  }

  Serial.println(F("[CONFIG] Attempting to open config.json for reading."));
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println(F("[CONFIG] Failed to open config.json for reading. Cannot load config."));
    return;
  }

  JsonDocument doc;  // Size based on ArduinoJson Assistant + buffer
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (error) {
    Serial.print(F("[CONFIG] JSON parse failed during load: "));
    Serial.println(error.f_str());
    return;
  }

  JsonArray ssidArray = doc["ssids"];
  JsonArray pwdArray = doc["passwords"];
  for (int i=0; i<10; i++) {
    strlcpy(ssids[i], ssidArray[i] | "", sizeof(ssids[i]));
    strlcpy(passwords[i], pwdArray[i] | "", sizeof(passwords[i]));
  }
  strlcpy(mdns, doc["mdns"] | "chronoclock", sizeof(mdns));
  strlcpy(apSsid, doc["apSsid"] | "", sizeof(apSsid));
  strlcpy(apPassword, doc["apPassword"] | "", sizeof(apPassword));
  strlcpy(timeZone, doc["timeZone"] | "Etc/UTC", sizeof(timeZone));
  brightness = doc["brightness"] | 7;
  flipDisplay = doc["flipDisplay"] | false;
  strlcpy(ntpServer1, doc["ntpServer1"] | "pool.ntp.org", sizeof(ntpServer1));
  strlcpy(ntpServer2, doc["ntpServer2"] | "time.nist.gov", sizeof(ntpServer2));

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

String saveConfig() {
    JsonDocument doc;

    File configFile = LittleFS.open("/config.json", "r");
    if (configFile) {
      Serial.println(F("[SAVE] Existing config.json found, loading for update..."));
      DeserializationError err = deserializeJson(doc, configFile);
      configFile.close();
      if (err) {
        Serial.print(F("[SAVE] Error parsing existing config.json: "));
        Serial.println(err.f_str());
      }
    } else {
      Serial.println(F("[SAVE] config.json not found, starting with empty doc for save."));
    }
    
    doc[F("mdns")] = mdns;
    doc[F("apSsid")] = apSsid;
    doc[F("apPassword")] = apPassword;
    doc[F("timeZone")] = timeZone;
    doc[F("brightness")] = brightness;
    doc[F("flipDisplay")] = flipDisplay;
    doc[F("ntpServer1")] = ntpServer1;
    doc[F("ntpServer2")] = ntpServer2;

    JsonArray ssidArray = doc["ssids"].to<JsonArray>();
    JsonArray pwdArray = doc["passwords"].to<JsonArray>();
    for (int i=0;i<10;i++) {
      ssidArray[i] = ssids[i];
      pwdArray[i] = passwords[i];
    }

    JsonObject countupdownObj = doc["countupdown"].to<JsonObject>();
    countupdownObj["targetTimestamp"] = countupdownTargetTimestamp;

    if (LittleFS.exists("/config.json")) {
      Serial.println(F("[SAVE] Renaming /config.json to /config.bak"));
      LittleFS.rename("/config.json", "/config.bak");
    }
    File f = LittleFS.open("/config.json", "w");
    if (!f) {
      Serial.println(F("[SAVE] ERROR: Failed to open /config.json for writing!"));
      LittleFS.rename("/config.bak", "/config.json");
      JsonDocument errorDoc;
      errorDoc[F("error")] = "Failed to write config file.";
      String response;
      serializeJson(errorDoc, response);
      return response;
    }

    size_t bytesWritten = serializeJson(doc, f);
    Serial.printf("[SAVE] Bytes written to /config.json: %u\n", bytesWritten);
    f.close();
    Serial.println(F("[SAVE] /config.json file closed."));

    File verify = LittleFS.open("/config.json", "r");
    if (!verify) {
      Serial.println(F("[SAVE] ERROR: Failed to open /config.json for reading during verification!"));
      LittleFS.rename("/config.bak", "/config.json");
      JsonDocument errorDoc;
      errorDoc[F("error")] = "Verification failed: Could not re-open config file.";
      String response;
      serializeJson(errorDoc, response);
      return response;
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
      LittleFS.rename("/config.bak", "/config.json");
      Serial.println(err.f_str());
      JsonDocument errorDoc;
      errorDoc[F("error")] = String("Config corrupted. Error: ") + err.f_str();
      String response;
      serializeJson(errorDoc, response);
      return response;
    }

    return "";
}

// -----------------------------------------------------------------------------
// WiFi Setup
// -----------------------------------------------------------------------------
void connectWiFi() {
  Serial.println(F("[WIFI] Connecting to WiFi..."));
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  dnsServer.stop();
  wifiLastTime = millis();
  wifiState = WIFI_WORKING;
  delay(100);
  
#if ESPVERS == 32
  WiFi.onEvent(WiFiStationGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(WiFiStationDisconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#endif
#if ESPVERS == 8266
  WiFiStationGotIP = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& event) {
    Serial.println(F("[WIFI] Connection successful."));
    wifiState = WIFI_CONNECTED;
    WiFiMode_t mode = WiFi.getMode();
    Serial.printf("[WIFI] WiFi mode after STA connection: %s\n",
                  mode == WIFI_OFF    ? "OFF"
                : mode == WIFI_STA    ? "STA ONLY"
                : mode == WIFI_AP     ? "AP ONLY"
                : mode == WIFI_AP_STA ? "AP + STA (Error!)"
                                      : "UNKNOWN");
    Serial.println("[WIFI] IP: " + WiFi.localIP().toString());
  });

  WiFiStationDisconnected = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event) {
    Serial.println("[WIFI] Network has disconnected.");
    wifiLastTime = millis();
    wifiState = WIFI_DISCONNECTED;
  });
#endif

  for (int i=0;i<10;i++) {
    wifiNetNum = i;
    if (strlen(ssids[i]) > 0) {
      Serial.printf("[WIFI] Attempting to connect to: %s\n", ssids[i]);
      WiFi.begin(ssids[i],passwords[i]);
      wifiLastTime = millis();
      return;
    }
  }
  wifiNetNum++;

  // Start mdns
  if (!MDNS.begin(mdns)) {
    Serial.println(F("[WIFI] Error setting up mDNS responder."));
    while (1) {
      delay(1000);
    }
  }
  Serial.println(F("[WIFI] mDNS responder started."));
}

void startAPMode() {
  Serial.println(F("[WIFI] Unable to connect to a WiFi network. Starting AP mode..."));
  WiFi.mode(WIFI_AP);
  if (strlen(apSsid) > 0) {
    if (strlen(apPassword) >= 8) {
      WiFi.softAP(apSsid, apPassword);
    } else {
      WiFi.softAP(apSsid, NULL);
    }
  } else if (strlen(DEFAULT_AP_PASSWORD) >= 8) {
    WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASSWORD);
  } else {
    WiFi.softAP(DEFAULT_AP_SSID, NULL);
  }
  Serial.print(F("[WIFI] AP IP address: "));
  Serial.println(WiFi.softAPIP());
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  WiFiMode_t mode = WiFi.getMode();
  Serial.println(F("[WIFI] AP Mode Started"));
  Serial.printf("[WIFI] WiFi mode after STA failure and setting AP: %s\n",
                mode == WIFI_OFF    ? "OFF"
              : mode == WIFI_STA    ? "STA ONLY"
              : mode == WIFI_AP     ? "AP ONLY"
              : mode == WIFI_AP_STA ? "AP + STA (Error!)"
                                    : "UNKNOWN");
  wifiLastTime = millis();
  wifiState = WIFI_APMODE;
}

// -----------------------------------------------------------------------------
// Time Functions
// -----------------------------------------------------------------------------
void startNTPSync(bool resetTime, int retryCount = 0) {
  if (wifiState == WIFI_APMODE) {
    return;
  }
  Serial.println(F("[TIME] Starting NTP sync"));

  if (resetTime) {
    Serial.println(F("Resetting time."));
    struct timeval now = { .tv_sec = 0 };
    settimeofday(&now, NULL);
  }
  ntpState = NTP_SYNCING;
  ntpLastTime = millis();
  configTime(0, 0, ntpServer1, ntpServer2);
  setenv("TZ", ianaToPosix(timeZone), 1);
  tzset();
  ntpRetryCount = retryCount + 1;
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
    doc[F("mode")] = wifiState == WIFI_APMODE ? "ap" : "sta";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println(F("[WEBSERVER] Request: /save"));
    bool restartWifi = false;

    String countupdownDateStr = "";
    String countupdownTimeStr = "";
    for (u_int i = 0; i < request->params(); i++) {
      const AsyncWebParameter *p = request->getParam(i);
      String n = p->name();
      String v = p->value();

      if (n == "brightness") {
        brightness = v.toInt();
      } else if (n == "flipDisplay") {
        flipDisplay = (v == "true" || v == "on" || v == "1");
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
          int num = n.substring(8).toInt();
          Serial.printf("[WEBSERVER] Password change: %d\n", num+1);
          if (strcmp(passwords[num], v.c_str()) != 0) {
            Serial.println(F("[WEBSERVER] RestartWiFi set to true."));
            restartWifi = true;
          }
          strlcpy(passwords[num], v.c_str(), sizeof(passwords[num])); // user entered a new password
        } else {
          Serial.println(F("[WEBSERVER] Password unchanged."));
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
        int num = n.substring(4).toInt();
        if (v.length() == 0) {
          Serial.printf("[WEBSERVER] SSID #%d blank. Old: '%s'\n", num+1, ssids[num]);
          if (strlen(ssids[num]) > 0) {
            restartWifi = true;
          }
          strlcpy(ssids[num], "", sizeof(ssids[num]));
          strlcpy(passwords[num], "", sizeof(passwords[num]));
        } else {
          Serial.printf("[WEBSERVER] SSID #%d specified: '%s' (%s)\n", num+1, v.c_str(), ssids[num]);
          if (strcmp(ssids[num], v.c_str()) != 0) {
            restartWifi = true;
          }
          strlcpy(ssids[num], v.c_str(), sizeof(ssids[num]));
        }
      } else if (n == "ntpServer1") {
        strlcpy(ntpServer1, v.c_str(), sizeof(ntpServer1));
      } else if (n == "ntpServer2") {
        strlcpy(ntpServer2, v.c_str(), sizeof(ntpServer2));
      } else if (n == "timeZone") {
        strlcpy(timeZone, v.c_str(), sizeof(timeZone));
      } else if (n == "mdns") {
        strlcpy(mdns, v.c_str(), sizeof(mdns));
      } else if (n == "countupdownDate") {
        countupdownDateStr = v;
      } else if (n == "countupdownTime") {
        countupdownTimeStr = v;
      } else if (n == "apSsid") {
        strlcpy(apSsid, v.c_str(), sizeof(apSsid));
      } else if (n == "apPassword") {
        strlcpy(apPassword, v.c_str(), sizeof(apPassword));
      }
    }

    if (countupdownDateStr.length() > 0 && countupdownTimeStr.length() > 0) {
      struct tm tm;
      tm.tm_year = countupdownDateStr.substring(0, 4).toInt() - 1900;
      tm.tm_mon = countupdownDateStr.substring(5, 7).toInt() - 1;
      tm.tm_mday = countupdownDateStr.substring(8, 10).toInt();
      tm.tm_hour = countupdownTimeStr.substring(0, 2).toInt();
      tm.tm_min = countupdownTimeStr.substring(3, 5).toInt();
      tm.tm_sec = countupdownTimeStr.substring(6, 8).toInt();
      tm.tm_isdst = -1;

      countupdownTargetTimestamp = mktime(&tm);
      if (countupdownTargetTimestamp == (time_t)-1) {
        Serial.println("[WEBSERVER] Error converting countupdown date/time to timestamp.");
        countupdownTargetTimestamp = 0;
      } else {
        Serial.printf("[WEBSERVER] Converted countupdown target: %s %s -> %lld\n", countupdownDateStr.c_str(), countupdownTimeStr.c_str(), countupdownTargetTimestamp);
      }
    }

    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
    } else {
      Serial.println(F("[WEBSERVER] Config saved successful."));
      JsonDocument okDoc;
      okDoc[F("message")] = "Saved successfully.";
      String response;
      serializeJson(okDoc, response);
      request->send(200, "application/json", response);
    }

    request->onDisconnect([restartWifi]() {
      if (restartWifi) {
        Serial.println(F("[WEBSERVER] WiFi information changed, restarting WiFi."));
        connectWiFi();
      }
    });
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

    for (int i=0;i<10;i++) {
      strlcpy(ssids[i], "", sizeof(ssids[i]));
      strlcpy(passwords[i], "", sizeof(passwords[i]));
    }
    String msg = saveConfig();
    if (msg.length() > 0) {
      Serial.println(F("[CLEARWIFI] Error saving cleared WiFi credentials."));
      request->send(500, "application/json", msg);
      return;
    }
    Serial.println(F("[CLEARWIFI] WiFi credentials cleared."));

    JsonDocument okDoc;
    okDoc[F("message")] = "✅ WiFi credentials cleared! Restarting WiFi...";
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);

    request->onDisconnect([]() {
      Serial.println(F("[WEBSERVER] Restarting WiFi connection..."));
      connectWiFi();
    });
  });
  
  server.on("/ap_status", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.print(F("[WEBSERVER] Request: /ap_status. isAPMode = "));
    Serial.println(wifiState == WIFI_APMODE);
    String json = "{\"isAP\": ";
    json += (wifiState == WIFI_APMODE) ? "true" : "false";
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
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
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
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true}");
    ESP.restart();
  });

  server.on("/set_countupdown", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("DateTime", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
      return;
    }

    String DateTimeStr = request->getParam("DateTime", true)->value();

    if (DateTimeStr.length() == 19) {
      struct tm tm;
      tm.tm_year = DateTimeStr.substring(0, 4).toInt() - 1900;
      tm.tm_mon = DateTimeStr.substring(5, 7).toInt() - 1;
      tm.tm_mday = DateTimeStr.substring(8, 10).toInt();
      tm.tm_hour = DateTimeStr.substring(11, 13).toInt();
      tm.tm_min = DateTimeStr.substring(14, 16).toInt();
      tm.tm_sec = DateTimeStr.substring(17, 19).toInt();
      tm.tm_isdst = -1;

      countupdownTargetTimestamp = mktime(&tm);
      if (countupdownTargetTimestamp == (time_t)-1) {
        Serial.println("[WEBSERVER] Error converting countupdown date/time to timestamp.");
        countupdownTargetTimestamp = 0;
      } else {
        Serial.printf("[WEBSERVER] Converted countupdown target: %s -> %lld\n", DateTimeStr.c_str(), countupdownTargetTimestamp);
      }
      String msg = saveConfig();
      if (msg.length() > 0) {
        request->send(500, "application/json", msg);
        return;
      }
      request->send(200, "application/json", "{\"ok\":true}");
    } else {
      request->send(400, "application/json", "{\"error\":\"Invalid datetime\"}");
    }
  });

  server.on("/start", HTTP_GET, [](AsyncWebServerRequest *request) {
    DateTime dtNow = rtc.now();
    countupdownTargetTimestamp = dtNow.unixtime();
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *request) {
    countupdownTargetTimestamp = 0;
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/add_seconds", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->hasParam("seconds", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
      return;
    }
    countupdownTargetTimestamp += request->getParam("seconds", true)->value().toInt();
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/remove_seconds", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->hasParam("seconds", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
      return;
    }
    countupdownTargetTimestamp -= request->getParam("seconds", true)->value().toInt();
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/get_time", HTTP_GET, [](AsyncWebServerRequest *request){
    DateTime dtNow = rtc.now();
    char dateTimeJson[48];
    snprintf(dateTimeJson, sizeof(dateTimeJson), "{\"time\":\"%04d-%02d-%02d %02d:%02d:%02d\"}", dtNow.year(), dtNow.month(), dtNow.day(), dtNow.hour(), dtNow.minute(), dtNow.second());
    request->send(200, "application/json", dateTimeJson);
  });

  server.on("/set_time", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("DateTime", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
      return;
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
        struct timeval newNow = {.tv_sec = newTime };
        setenv("TZ", ianaToPosix(timeZone), 1);
        tzset();
        settimeofday(&newNow, NULL);
        time_t nowTime = time(nullptr);
        struct tm timeInfo;
        localtime_r(&nowTime, &timeInfo);
        rtc.adjust(DateTime(timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec));
      }
    }
    DateTime dtNow = rtc.now();
    char dateTimeJson[48];
    snprintf(dateTimeJson, sizeof(dateTimeJson), "{\"time\":\"%04u-%02u-%02u %02u:%02u:%02u\"}", dtNow.year(), dtNow.month(), dtNow.day(), dtNow.hour(), dtNow.minute(), dtNow.second());
    request->send(200, "application/json", dateTimeJson);
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
  delay(10);
  Serial.println(F("[SETUP] Starting setup..."));

  // Check if RTC was found.
  if (!rtc.begin()) {
    Serial.println(F("[SETUP] Unable to find RTC."));
    rtcEnabled = false;
  } else {
    rtcEnabled = true;
    if (rtc.lostPower()) {
      // Set time if new device or after a power loss.
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

#if ESPVERS == 8266
  if (!LittleFS.begin()) {
#else
  if (!LittleFS.begin(true)) {
#endif
    Serial.println(F("[ERROR] LittleFS mount failed in setup! Halting."));
    while (true) {
      delay(100);
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
  setupWebServer();
  Serial.println(F("[SETUP] Setup complete"));
  printConfigToSerial();
  lastColonBlink = millis();

  Serial.println(F("[SETUP] Starting ElegantOTA."));
  ElegantOTA.setAuth(ELEGANT_USER, ELEGANT_PASS);
  ElegantOTA.begin(&server);
}

void loop() {
  uint32_t curMillis = millis();
  switch (wifiState) {
    // Attempting to connect still.
    case WIFI_WORKING:
      // Check timeout
      if (curMillis >= wifiLastTime + wifiTimeout) {
        // try to connect to next -- wifiNetNum is always the index of the SSID/Password for
        // the network that timed out
        for (int i=wifiNetNum+1; i<10; i++) {
          wifiNetNum = i;
          if (strlen(ssids[i]) > 0) {
            Serial.printf("[WIFI] Attempting to connect to : %s\n", ssids[i]);
            WiFi.begin(ssids[i],passwords[i]);
            wifiLastTime = millis();
            break;
          }
        }
        // wifiNetNum could be 9 (loop executed at least once) or 
        // 10 (9 was the network that timed out) before the increment
        wifiNetNum++;
      }
      // Only reachable if no networks specified or they all timed out
      if (wifiNetNum >= 10) {
        startAPMode();
      }
      break;
    case WIFI_DISCONNECTED:
      // If disconnected and it doesn't reconnect within the timeout
      // then start the connection process again
      // potentially there is a network available that precedes
      // the disconnected network in the array
      if (curMillis >= wifiLastTime + wifiTimeout) {
        Serial.println(F("[WIFI] WiFi disconnect timeout reached."));
        connectWiFi();
      }
      break;
    case WIFI_APMODE:
      dnsServer.processNextRequest();
      // Try to reconnect to a network every apTimeout when in ap mode
      if (curMillis >= wifiLastTime + apTimeout) {
        Serial.println(F("[WIFI] AP Mode timeout reached."));
        connectWiFi();
      }
      break;
    default:
      break;
  }

  ElegantOTA.loop();

  static bool colonVisible = true;
  const unsigned long colonBlinkInterval = 800;
  if (millis() - lastColonBlink > colonBlinkInterval) {
    colonVisible = !colonVisible;
    lastColonBlink = millis();
  }

  // --- NTP State Machine ---
  switch (ntpState) {
    case NTP_IDLE:
    case NTP_SUCCESS: {
        // If RTC doesn't work, attempt to refresh NTP sync every hour.
        if (!rtcEnabled  && (ntpLastTime == 0 || curMillis > ntpLastTime + ntpRefreshTime)) {
          startNTPSync(false);
        }
      }
      break;
    case NTP_SYNCING: {
        time_t lNow = time(nullptr);
        if (lNow > 1000) {  // NTP sync successful
          Serial.println(F("[TIME] NTP sync successful."));
          ntpState = NTP_SUCCESS;
          Serial.println(F("[TIME] Setting timezone then adjusting RTC clock."));
          setenv("TZ", ianaToPosix(timeZone), 1);
          tzset();
          if (rtcEnabled) {
            time_t nowTime = time(nullptr);
            struct tm timeInfo;
            localtime_r(&nowTime, &timeInfo);
            rtc.adjust(DateTime(timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec));
          }
        } else if (curMillis - ntpLastTime > ntpTimeout && ntpRetryCount < maxNtpRetries) {
          Serial.println(F("[TIME] NTP sync failed."));
          ntpState = NTP_FAILED;
        } else if (ntpRetryCount >= maxNtpRetries) {
          ntpState = NTP_IDLE;
        }
      }
      break;
    case NTP_FAILED: {
        startNTPSync(false, ntpRetryCount);
        Serial.println(F("[TIME] Retrying NTP sync..."));
      }
      break;
  }

  DateTime dtNow;
  if (rtcEnabled) {
    dtNow = rtc.now();
  } else {
    time_t nowTime = time(nullptr);
    struct tm timeInfo;
    localtime_r(&nowTime, &timeInfo);
    dtNow = DateTime(timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
  }
  char timeWithSeconds[24];
  if (countupdownTargetTimestamp > 0) { // --- COUNTUPDOWN Display Mode ---
    long timeSeconds = countupdownTargetTimestamp - dtNow.unixtime();
    if (timeSeconds < 0) {
      timeSeconds = timeSeconds * -1;
    }
    // Format the full string
    if (colonVisible) {
      snprintf(timeWithSeconds, sizeof(timeWithSeconds), "%ld:%02ld:%02ld", timeSeconds / 3600, (timeSeconds % 3600) / 60, timeSeconds % 60);
    } else {
      snprintf(timeWithSeconds, sizeof(timeWithSeconds), "%ld %02ld %02ld", timeSeconds / 3600, (timeSeconds % 3600) / 60, timeSeconds % 60);
    }
  }  // End COUNTUPDOWN Display Mode
  else { // --- CLOCK Display Mode ---
    snprintf(timeWithSeconds, sizeof(timeWithSeconds), "%02d:%02d:%02d", dtNow.hour(), dtNow.minute(), dtNow.second());
  } // End CLOCK Display Mode
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
  // --- DISPLAY COUNTUPDOWN ---
  P.setCharSpacing(0);
  P.setTextAlignment(PA_CENTER);
  P.print(formattedTime);
  yield();
}