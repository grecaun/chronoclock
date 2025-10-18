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
#define DEBUG         true

const char    *DEFAULT_AP_SSID      = "chronoclock";
const char    *DEFAULT_AP_PASSWORD  = "chrono157";
const char    *PASSWORD_MASK        = "********";
const uint32_t COLON_BLINK_INTERVAL = 800;

/*
 * Function definitions
*/
// --- Config Load / Save / Safe Getters ---
void loadConfig();
String saveConfig();
const char *getSafeSsid(int ix);
const char *getSafePassword(int ix);
// -- Network ---
void connectWiFi();
void wifiGotIP();
void wifiDisconnected();
void startAPMode();
void startMDNS();
void startElegantOTA();
#if ESPVERS == 32
void WiFiStationGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info);
#endif
#if ESPVERS == 8266
WiFiEventHandler WiFiStationGotIP, WiFiStationDisconnected;
#endif
// --- Time ---
void startNTPSync(bool resetTime, int retryCount);
void setTimeZone(const char *local_TZ);
// --- Utility ---
void printConfigToSerial();
// --- Web Server ---
void setupWebServer();

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


// Settings
char mdns[64]          = "";
char apSsid[32]        = "";
char apPassword[64]    = "";
char ssids[10][32]     = {"","","","","","","","","",""};
char passwords[10][64] = {"","","","","","","","","",""};
char timeZone[64]      = "";
int  brightness        = 10;
bool flipDisplay       = false;
bool twelveHour        = false;
bool lockCountUpDown   = false;
char ntpServer1[128]   = "pool.ntp.org";
char ntpServer2[128]   = "time.nist.gov";

// Globals
bool          rtcEnabled           = false;
bool          colonVisible         = true;
unsigned long lastColonBlink       = 0;
time_t        countupdownTimestamp = 0;  // Unix timestamp

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

/*
 * Configuration Load & Save
 */
void loadConfig() {
#if DEBUG==true
  Serial.println(F("[CONFIG] Loading configuration..."));
#endif

  // Check if config.json exists, if not, create default
  if (!LittleFS.exists("/config.json")) {
#if DEBUG==true
    Serial.println(F("[CONFIG] config.json not found, creating with defaults..."));
#endif
    JsonDocument doc;
    doc[F("mdns")] = mdns;
    doc[F("apSsid")] = apSsid;
    doc[F("apPassword")] = apPassword;
    doc[F("timeZone")] = timeZone;
    doc[F("brightness")] = brightness;
    doc[F("flipDisplay")] = flipDisplay;
    doc[F("twelveHour")] = twelveHour;
    doc[F("lockCountUpDown")] = lockCountUpDown;
    doc[F("ntpServer1")] = ntpServer1;
    doc[F("ntpServer2")] = ntpServer2;
    doc[F("countupdownTimestamp")] = 0;

    JsonArray ssidArray = doc[F("ssids")].to<JsonArray>();
    JsonArray pwdArray = doc[F("passwords")].to<JsonArray>();
    for (int i=0;i<10;i++) {
      ssidArray[i] = ssids[i];
      pwdArray[i] = passwords[i];
    }

    File f = LittleFS.open("/config.json", "w");
    if (f) {
      serializeJsonPretty(doc, f);
      f.close();
#if DEBUG==true
      Serial.println(F("[CONFIG] Default config.json created."));
#endif
    }
#if DEBUG==true
    else {
      Serial.println(F("[CONFIG] Failed to create default config.json"));
    }
#endif
  }

#if DEBUG==true
  Serial.println(F("[CONFIG] Attempting to open config.json for reading."));
#endif
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
#if DEBUG==true
    Serial.println(F("[CONFIG] Failed to open config.json for reading. Cannot load config."));
#endif
    return;
  }

  JsonDocument doc;  // Size based on ArduinoJson Assistant + buffer
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (error) {
#if DEBUG==true
    Serial.print(F("[CONFIG] JSON parse failed during load: "));
    Serial.println(error.f_str());
#endif
    return;
  }

  JsonArray ssidArray = doc[F("ssids")];
  JsonArray pwdArray = doc[F("passwords")];
  for (int i=0; i<10; i++) {
    strlcpy(ssids[i], ssidArray[i] | "", sizeof(ssids[i]));
    strlcpy(passwords[i], pwdArray[i] | "", sizeof(passwords[i]));
  }
  strlcpy(mdns, doc[F("mdns")] | "chronoclock", sizeof(mdns));
  strlcpy(apSsid, doc[F("apSsid")] | "", sizeof(apSsid));
  strlcpy(apPassword, doc[F("apPassword")] | "", sizeof(apPassword));
  strlcpy(timeZone, doc[F("timeZone")] | "Etc/UTC", sizeof(timeZone));
  brightness = doc[F("brightness")] | 7;
  flipDisplay = doc[F("flipDisplay")] | false;
  twelveHour = doc[F("twelveHour")] | false;
  lockCountUpDown = doc[F("lockCountUpDown")] | false;
  strlcpy(ntpServer1, doc[F("ntpServer1")] | "pool.ntp.org", sizeof(ntpServer1));
  strlcpy(ntpServer2, doc[F("ntpServer2")] | "time.nist.gov", sizeof(ntpServer2));
  countupdownTimestamp = doc[F("countupdownTimestamp")] | 0;
#if DEBUG==true
  Serial.println(F("[CONFIG] Configuration loaded."));
#endif
}

String saveConfig() {
    JsonDocument doc;
    doc[F("mdns")] = mdns;
    doc[F("apSsid")] = apSsid;
    doc[F("apPassword")] = apPassword;
    doc[F("timeZone")] = timeZone;
    doc[F("brightness")] = brightness;
    doc[F("flipDisplay")] = flipDisplay;
    doc[F("twelveHour")] = twelveHour;
    doc[F("lockCountUpDown")] = lockCountUpDown;
    doc[F("ntpServer1")] = ntpServer1;
    doc[F("ntpServer2")] = ntpServer2;
    doc[F("countupdownTimestamp")] = countupdownTimestamp;

    JsonArray ssidArray = doc[F("ssids")].to<JsonArray>();
    JsonArray pwdArray = doc[F("passwords")].to<JsonArray>();
    for (int i=0;i<10;i++) {
      ssidArray[i] = ssids[i];
      pwdArray[i] = passwords[i];
    }

    if (LittleFS.exists("/config.json")) {
#if DEBUG==true
      Serial.println(F("[SAVE] Renaming /config.json to /config.bak"));
#endif
      LittleFS.rename("/config.json", "/config.bak");
    }
    File f = LittleFS.open("/config.json", "w");
    if (!f) {
#if DEBUG==true
      Serial.println(F("[SAVE] ERROR: Failed to open /config.json for writing!"));
#endif
      LittleFS.rename("/config.bak", "/config.json");
      JsonDocument errorDoc;
      errorDoc[F("error")] = "Failed to write config file.";
      String response;
      serializeJson(errorDoc, response);
      return response;
    }

#if DEBUG==true
    size_t bytesWritten = serializeJson(doc, f);
    Serial.print(F("[SAVE] Bytes written to /config.json: "));
    Serial.println(bytesWritten);
#endif
    f.close();
#if DEBUG==true
    Serial.println(F("[SAVE] /config.json file closed."));
#endif

    File verify = LittleFS.open("/config.json", "r");
    if (!verify) {
#if DEBUG==true
      Serial.println(F("[SAVE] ERROR: Failed to open /config.json for reading during verification!"));
#endif
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
#if DEBUG==true
      Serial.print(F("[SAVE] Config corrupted after save: "));
      Serial.println(err.f_str());
#endif
      LittleFS.rename("/config.bak", "/config.json");
      JsonDocument errorDoc;
      errorDoc[F("error")] = String("Config corrupted. Error: ") + err.f_str();
      String response;
      serializeJson(errorDoc, response);
      return response;
    }

    return "";
}

// --- Safe WiFi credential getters ---
const char *getSafeSsid(int ix) {
  return ssids[ix];
}

const char *getSafePassword(int ix) {
  // If no SSID set, or the password is empty, return empty, otherwise mask.
  if (strlen(ssids[ix]) == 0 || strlen(passwords[ix]) == 0) {
    return "";
  } else {
    return PASSWORD_MASK;
  }
}

/*
 * Network Code
*/
void connectWiFi() {
#if DEBUG==true
  Serial.println(F("[WIFI] Connecting to WiFi..."));
#endif
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
    wifiGotIP();
  });

  WiFiStationDisconnected = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event) {
    wifiDisconnected();
  });
#endif

  for (int i=0;i<10;i++) {
    wifiNetNum = i;
    if (strlen(ssids[i]) > 0) {
#if DEBUG==true
      Serial.print(F("[WIFI] Attempting to connect to: "));
      Serial.println(ssids[i]);
#endif
      WiFi.begin(ssids[i],passwords[i]);
      wifiLastTime = millis();
      return;
    }
  }
  wifiNetNum++;
}

void wifiGotIP() {
  wifiState = WIFI_CONNECTED;
#if DEBUG==true
  WiFiMode_t mode = WiFi.getMode();
  Serial.println(F("[WIFI] Connection successful."));
  Serial.print(F("[WIFI] WiFi mode after STA connection: "));
  Serial.println( mode == WIFI_OFF    ? F("OFF")
                : mode == WIFI_STA    ? F("STA")
                : mode == WIFI_AP     ? F("AP")
                : mode == WIFI_AP_STA ? F("AP + STA")
                                      : F("UNKNOWN"));
  Serial.print(F("[WIFI] IP: "));
  Serial.println(WiFi.localIP().toString());
#endif
}

void wifiDisconnected() {
#if DEBUG==true
  Serial.println(F("[WIFI] Network has disconnected."));
#endif
  wifiLastTime = millis();
  wifiState = WIFI_DISCONNECTED;
}

void startAPMode() {
#if DEBUG==true
  Serial.println(F("[WIFI] Unable to connect to a WiFi network. Starting AP mode..."));
#endif
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
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
#if DEBUG==true
  WiFiMode_t mode = WiFi.getMode();
  Serial.println(F("[WIFI] AP Mode Started"));
  Serial.print(F("[WIFI] WiFi mode after STA failure and setting AP:"));
  Serial.println( mode == WIFI_OFF    ? F("OFF")
                : mode == WIFI_STA    ? F("STA")
                : mode == WIFI_AP     ? F("AP")
                : mode == WIFI_AP_STA ? F("AP + STA")
                : F("UNKNOWN"));
  Serial.print(F("[WIFI] AP IP address: "));
  Serial.println(WiFi.softAPIP());
#endif
  wifiLastTime = millis();
  wifiState = WIFI_APMODE;
}

void startMDNS() {
#if DEBUG==true
  Serial.println(F("[WIFI] Starting mDNS responder."));
#endif
  MDNS.end();
  MDNS.begin(mdns);
#if DEBUG==true
  Serial.println(F("[WIFI] mDNS responder started."));
#endif
}

void startElegantOTA() {
#if DEBUG==true
  Serial.println(F("[SETUP] Starting ElegantOTA."));
#endif
  ElegantOTA.setAuth(ELEGANT_USER, ELEGANT_PASS);
  ElegantOTA.begin(&server);
}

#if ESPVERS == 32
void WiFiStationGotIP(WiFiEvent_t event, WiFiEventInfo_t info) {
  wifiGotIP();
}

void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  wifiDisconnected();
}
#endif

/*
 * Time Functions
 */
void startNTPSync(bool resetTime, int retryCount = 0) {
  if (wifiState == WIFI_APMODE) {
    return;
  }
#if DEBUG==true
  Serial.println(F("[TIME] Starting NTP sync"));
#endif

  setTimeZone(""); // ConfigTime is going to sync assuming the server is set to UTC0, so set it to UTC0.
  if (resetTime) {
#if DEBUG==true
    Serial.println(F("[TIME] Resetting time."));
#endif
    struct timeval now = { 0, 0 };
    settimeofday(&now, NULL);
  }
  ntpState = NTP_SYNCING;
  ntpLastTime = millis();
  configTime(0, 0, ntpServer1, ntpServer2);
  ntpRetryCount = retryCount + 1;
}

void setTimeZone(const char *localTZ) {
#if DEBUG==true
  Serial.printf("[TIME] Setting Time Zone to: %s (%s)\n", localTZ, ianaToPosix(localTZ));
#endif
  setenv("TZ", ianaToPosix(localTZ), 1);
  tzset();
}

/*
 * Utility
 */
void printConfigToSerial() {
#if DEBUG==true
  Serial.println(F("========= Loaded Configuration ========="));
  for (int i=0;i<10;i++) {
    Serial.print(F("Wifi Network "));
    Serial.print(i);
    Serial.print(F(": "));
    Serial.print(ssids[i]);
    Serial.print(F(" - "));
    Serial.println(passwords[i]);
  }
  Serial.print(F("mDNS: "));
  Serial.println(mdns);
  Serial.print(F("Brightness: "));
  Serial.println(brightness);
  Serial.print(F("Flip Display: "));
  Serial.println(flipDisplay ? "Yes" : "No");
  Serial.print(F("12 Hour: "));
  Serial.println(twelveHour ? "Yes" : "No");
  Serial.print(F("CountUpDown Locked: "));
  Serial.println(lockCountUpDown ? "Yes" : "No");
  Serial.print(F("Countupdown Target Timestamp: "));
  Serial.println(countupdownTimestamp);
  Serial.print(F("NTP Server 1: "));
  Serial.println(ntpServer1);
  Serial.print(F("NTP Server 2: "));
  Serial.println(ntpServer2);
  Serial.print(F("TimeZone (IANA): "));
  Serial.println(timeZone);
  Serial.println(F("========================================"));
  Serial.println();
#endif
}

/*
 * Web Server
 */
void setupWebServer() {
#if DEBUG==true
  Serial.println(F("[WEBSERVER] Setting up web server..."));
#endif

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /"));
#endif
    request->send(LittleFS, "/index.html", "text/html");
  });

  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /"));
#endif
    request->send(LittleFS, "/favicon.ico", "image/x-icon");
  });

  server.on("/config.json", HTTP_GET, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /config.json"));
#endif
    File f = LittleFS.open("/config.json", "r");
    if (!f) {
#if DEBUG==true
      Serial.println(F("[WEBSERVER] Error opening /config.json"));
#endif
      request->send(500, "application/json", "{\"error\":\"Failed to open config.json\"}");
      return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
#if DEBUG==true
      Serial.print(F("[WEBSERVER] Error parsing /config.json: "));
      Serial.println(err.f_str());
#endif
      request->send(500, "application/json", "{\"error\":\"Failed to parse config.json\"}");
      return;
    }

    // Always sanitize before sending to browser
    JsonArray ssidArray = doc[F("ssids")];
    JsonArray pwdArray = doc[F("passwords")];
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
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /save"));
#endif
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
      } else if (n == "twelveHour") {
        twelveHour = (v == "true" || v == "on" || v == "1");
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
        if (strcmp(v.c_str(), PASSWORD_MASK) != 0 && v.length() > 0) {
          int num = n.substring(8).toInt();
#if DEBUG==true
          Serial.print(F("[WEBSERVER] Password changed for network "));
          Serial.println(num+1);
#endif
          if (strcmp(passwords[num], v.c_str()) != 0) {
#if DEBUG==true
            Serial.println(F("[WEBSERVER] RestartWiFi set to true."));
#endif
            restartWifi = true;
          }
          strlcpy(passwords[num], v.c_str(), sizeof(passwords[num])); // user entered a new password
        }
#if DEBUG==true
        else { // do nothing, keep previous
          Serial.println(F("[WEBSERVER] Password unchanged."));
        }
#endif
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
#if DEBUG==true
        Serial.print(F("[WEBSERVER] SSID "));
        Serial.printf("%d: '%s' (%s)\n", num+1, v.c_str(), ssids[num]);
#endif
        if (v.length() == 0) {
          if (strlen(ssids[num]) > 0) {
            restartWifi = true;
          }
          strlcpy(ssids[num], "", sizeof(ssids[num]));
          strlcpy(passwords[num], "", sizeof(passwords[num]));
        } else {
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
        setTimeZone(timeZone);
      } else if (n == "mdns") {
        if (strcmp(mdns, v.c_str()) != 0) {
          strlcpy(mdns, v.c_str(), sizeof(mdns));
          startMDNS();
        } else {
          strlcpy(mdns, v.c_str(), sizeof(mdns));
        }
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
      countupdownTimestamp = mktime(&tm);
      if (countupdownTimestamp == (time_t)-1) {
#if DEBUG==true
        Serial.println(F("[WEBSERVER] Error converting countupdown date/time to timestamp."));
#endif
        countupdownTimestamp = 0;
      }
#if DEBUG==true
      else {
        Serial.print(F("[WEBSERVER] Converted countupdown target: "));
        Serial.printf("%s %s -> %lld\n", countupdownDateStr.c_str(), countupdownTimeStr.c_str(), countupdownTimestamp);
      }
#endif
    }

    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
    } else {
#if DEBUG==true
      Serial.println(F("[WEBSERVER] Config saved successful."));
#endif
      JsonDocument okDoc;
      okDoc[F("message")] = "Saved successfully.";
      String response;
      serializeJson(okDoc, response);
      request->send(200, "application/json", response);
    }

    request->onDisconnect([restartWifi]() {
      if (restartWifi) {
#if DEBUG==true
        Serial.println(F("[WEBSERVER] WiFi information changed, restarting WiFi."));
#endif
        connectWiFi();
      }
    });
  });

  server.on("/restore", HTTP_POST, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /restore"));
#endif
    if (LittleFS.exists("/config.bak")) {
      File src = LittleFS.open("/config.bak", "r");
      if (!src) {
#if DEBUG==true
        Serial.println(F("[WEBSERVER] Failed to open /config.bak"));
#endif
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
#if DEBUG==true
        Serial.println(F("[WEBSERVER] Failed to open /config.json for writing"));
#endif
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
#if DEBUG==true
        Serial.println(F("[WEBSERVER] Rebooting after restore..."));
#endif
        ESP.restart();
      });
    } else {
#if DEBUG==true
      Serial.println(F("[WEBSERVER] No backup found"));
#endif
      JsonDocument errorDoc;
      errorDoc[F("error")] = "No backup found.";
      String response;
      serializeJson(errorDoc, response);
      request->send(404, "application/json", response);
    }
  });

  server.on("/clear_wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /clear_wifi"));
#endif
    for (int i=0;i<10;i++) {
      strlcpy(ssids[i], "", sizeof(ssids[i]));
      strlcpy(passwords[i], "", sizeof(passwords[i]));
    }
    String msg = saveConfig();
    if (msg.length() > 0) {
#if DEBUG==true
      Serial.println(F("[CLEARWIFI] Error saving cleared WiFi credentials."));
#endif
      request->send(500, "application/json", msg);
      return;
    }
#if DEBUG==true
    Serial.println(F("[CLEARWIFI] WiFi credentials cleared."));
#endif
    JsonDocument okDoc;
    okDoc[F("message")] = "✅ WiFi credentials cleared! Restarting WiFi...";
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
    request->onDisconnect([]() {
#if DEBUG==true
      Serial.println(F("[WEBSERVER] Restarting WiFi connection..."));
#endif
      connectWiFi();
    });
  });
  
  server.on("/ap_status", HTTP_GET, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.print(F("[WEBSERVER] Request: /ap_status. isAPMode = "));
    Serial.println(wifiState == WIFI_APMODE);
#endif
    String json = "{\"isAP\": ";
    json += (wifiState == WIFI_APMODE) ? "true" : "false";
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/set_brightness", HTTP_POST, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /set_brightness"));
#endif
    if (!request->hasParam("value", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
      return;
    }
    int newBrightness = request->getParam("value", true)->value().toInt();

    // Clamp brightness to valid range
    if (newBrightness < 1) { newBrightness = 1; }
    if (newBrightness > 15) { newBrightness = 15; }
#if DEBUG==true
    Serial.print(F("[WEBSERVER] Setting brightness to "));
    Serial.println(newBrightness);
#endif
    brightness = newBrightness;
    P.setIntensity(brightness);
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    JsonDocument okDoc;
    okDoc[F("brightness")] = brightness;
    okDoc[F("flipDisplay")] = flipDisplay;
    okDoc[F("twelveHour")] = twelveHour;
    okDoc[F("lockCountUpDown")] = lockCountUpDown;
    okDoc[F("countupdownTimestamp")] = countupdownTimestamp;
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
  });

  server.on("/set_flip", HTTP_POST, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /set_flip"));
#endif
    bool flip = false;
    if (request->hasParam("value", true)) {
      String v = request->getParam("value", true)->value();
      flip = (v == "1" || v == "true" || v == "on");
    }
    flipDisplay = flip;
    P.setZoneEffect(0, flipDisplay, PA_FLIP_UD);
    P.setZoneEffect(0, flipDisplay, PA_FLIP_LR);
#if DEBUG==true
    Serial.print(F("[WEBSERVER] Set flipDisplay to "));
    Serial.println(flipDisplay);
#endif
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    JsonDocument okDoc;
    okDoc[F("brightness")] = brightness;
    okDoc[F("flipDisplay")] = flipDisplay;
    okDoc[F("twelveHour")] = twelveHour;
    okDoc[F("lockCountUpDown")] = lockCountUpDown;
    okDoc[F("countupdownTimestamp")] = countupdownTimestamp;
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
  });

  server.on("/set_twelvehour", HTTP_POST, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /set_twelvehour"));
#endif
    bool twelve = false;
    if (request->hasParam("value", true)) {
      String v = request->getParam("value", true)->value();
      twelve = (v == "1" || v == "true" || v == "on");
    }
    twelveHour = twelve;
#if DEBUG==true
    Serial.print(F("[WEBSERVER] Set twelveHour to "));
    Serial.println(twelveHour);
#endif
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    JsonDocument okDoc;
    okDoc[F("brightness")] = brightness;
    okDoc[F("flipDisplay")] = flipDisplay;
    okDoc[F("twelveHour")] = twelveHour;
    okDoc[F("lockCountUpDown")] = lockCountUpDown;
    okDoc[F("countupdownTimestamp")] = countupdownTimestamp;
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
  });

  server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /restart"));
#endif
    request->send(200, "application/json", "{\"ok\":true}");
    ESP.restart();
  });

  server.on("/set_lock", HTTP_POST, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /set_lock"));
#endif
    bool lock = false;
    if (request->hasParam("value", true)) {
      String v = request->getParam("value", true)->value();
      lock = (v == "1" || v == "true" || v == "on");
    }
    lockCountUpDown = lock;
#if DEBUG==true
    Serial.print(F("[WEBSERVER] Set lockCountUpDown to "));
    Serial.println(lockCountUpDown);
#endif
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    JsonDocument okDoc;
    okDoc[F("brightness")] = brightness;
    okDoc[F("flipDisplay")] = flipDisplay;
    okDoc[F("twelveHour")] = twelveHour;
    okDoc[F("lockCountUpDown")] = lockCountUpDown;
    okDoc[F("countupdownTimestamp")] = countupdownTimestamp;
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
  });

  server.on("/set_countupdown", HTTP_POST, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /set_countupdown"));
#endif
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
      countupdownTimestamp = mktime(&tm);
      if (countupdownTimestamp == (time_t)-1) {
#if DEBUG==true
        Serial.println("[WEBSERVER] Error converting countupdown date/time to timestamp.");
#endif
        countupdownTimestamp = 0;
      }
#if DEBUG==true
      else {
        Serial.print(F("[WEBSERVER] Converted countupdown target: "));
        Serial.printf("%s -> %lld\n", DateTimeStr.c_str(), countupdownTimestamp);
      }
#endif
      String msg = saveConfig();
      if (msg.length() > 0) {
        request->send(500, "application/json", msg);
        return;
      }
      JsonDocument okDoc;
      okDoc[F("flipDisplay")] = flipDisplay;
      okDoc[F("twelveHour")] = twelveHour;
      okDoc[F("lockCountUpDown")] = lockCountUpDown;
      okDoc[F("countupdownTimestamp")] = countupdownTimestamp;
      String response;
      serializeJson(okDoc, response);
      request->send(200, "application/json", response);
    } else {
      request->send(400, "application/json", "{\"error\":\"Invalid datetime\"}");
    }
  });

  server.on("/start", HTTP_GET, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /start"));
#endif
    if (lockCountUpDown) {
      request->send(409, "application/json", "{\"error\":\"CountUpDown locked.\"}"); // Conflict
      return;
    }
    if (countupdownTimestamp > 0) {
      request->send(409, "application/json", "{\"error\":\"CountUpDown already running.\"}"); // Conflict
      return;
    }
    if (rtcEnabled) {
      countupdownTimestamp = rtc.now().unixtime();
    } else {
      countupdownTimestamp = time(nullptr);
    }
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    JsonDocument okDoc;
    okDoc[F("brightness")] = brightness;
    okDoc[F("flipDisplay")] = flipDisplay;
    okDoc[F("twelveHour")] = twelveHour;
    okDoc[F("lockCountUpDown")] = lockCountUpDown;
    okDoc[F("countupdownTimestamp")] = countupdownTimestamp;
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
  });

  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /stop"));
#endif
    if (lockCountUpDown) {
      request->send(409, "application/json", "{\"error\":\"CountUpDown locked.\"}"); // Conflict
      return;
    }
    if (countupdownTimestamp < 1) {
      request->send(409, "application/json", "{\"error\":\"CountUpDown not running.\"}"); // Conflict
      return;
    }
    countupdownTimestamp = 0;
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    JsonDocument okDoc;
    okDoc[F("brightness")] = brightness;
    okDoc[F("flipDisplay")] = flipDisplay;
    okDoc[F("twelveHour")] = twelveHour;
    okDoc[F("lockCountUpDown")] = lockCountUpDown;
    okDoc[F("countupdownTimestamp")] = countupdownTimestamp;
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
  });

  server.on("/add_seconds", HTTP_POST, [](AsyncWebServerRequest *request){
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /add_seconds"));
#endif
    if (lockCountUpDown) {
      request->send(409, "application/json", "{\"error\":\"CountUpDown locked.\"}"); // Conflict
      return;
    }
    if (countupdownTimestamp < 1) {
      request->send(409, "application/json", "{\"error\":\"CountUpDown not set, unable to adjust.\"}"); // Conflict
      return;
    }
    if (!request->hasParam("seconds", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
      return;
    }
    DateTime dtNow;
    if (rtcEnabled) {
      dtNow = rtc.now();
    } else {
      dtNow = DateTime(time(nullptr));
    }
    int seconds = request->getParam("seconds", true)->value().toInt();
    if (countupdownTimestamp < dtNow.unixtime()) { // Count Up!
      seconds = seconds * -1; // needs to be further in the past if in countup mode
    }
    countupdownTimestamp += seconds;
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    JsonDocument okDoc;
    okDoc[F("brightness")] = brightness;
    okDoc[F("flipDisplay")] = flipDisplay;
    okDoc[F("twelveHour")] = twelveHour;
    okDoc[F("lockCountUpDown")] = lockCountUpDown;
    okDoc[F("countupdownTimestamp")] = countupdownTimestamp;
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
  });

  server.on("/remove_seconds", HTTP_POST, [](AsyncWebServerRequest *request){
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /remove_seconds"));
#endif
    if (lockCountUpDown) {
      request->send(409, "application/json", "{\"error\":\"CountUpDown locked.\"}"); // Conflict
      return;
    }
    if (countupdownTimestamp < 1) {
      request->send(409, "application/json", "{\"error\":\"CountUpDown not set, unable to adjust.\"}"); // Conflict
      return;
    }
    if (!request->hasParam("seconds", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
      return;
    }
    DateTime dtNow;
    if (rtcEnabled) {
      dtNow = rtc.now();
    } else {
      dtNow = DateTime(time(nullptr));
    }
    int seconds = request->getParam("seconds", true)->value().toInt();
    if (countupdownTimestamp < dtNow.unixtime()) { // Count Up!
      seconds = seconds * -1; // needs to be closer to today if in countup
    }
    countupdownTimestamp -= seconds;
    String msg = saveConfig();
    if (msg.length() > 0) {
      request->send(500, "application/json", msg);
      return;
    }
    JsonDocument okDoc;
    okDoc[F("brightness")] = brightness;
    okDoc[F("flipDisplay")] = flipDisplay;
    okDoc[F("twelveHour")] = twelveHour;
    okDoc[F("lockCountUpDown")] = lockCountUpDown;
    okDoc[F("countupdownTimestamp")] = countupdownTimestamp;
    String response;
    serializeJson(okDoc, response);
    request->send(200, "application/json", response);
  });

  server.on("/get_time", HTTP_GET, [](AsyncWebServerRequest *request){
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /get_time"));
#endif
    DateTime dtNow;
    if (rtcEnabled) {
      dtNow = rtc.now();
    } else {
      dtNow = DateTime(time(nullptr));
    }
    // Convert from UTC to Local
    time_t nowTime = dtNow.unixtime();
    struct tm timeInfo;
    localtime_r(&nowTime, &timeInfo);
    char dateTimeJson[48];
    snprintf(dateTimeJson, sizeof(dateTimeJson), "{\"time\":\"%04u-%02u-%02u %02u:%02u:%02u\"}", timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
    request->send(200, "application/json", dateTimeJson);
  });

  server.on("/set_time", HTTP_POST, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /set_time"));
#endif
    Serial.println(request->params());
    if (!request->hasParam("DateTime", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing value\"}");
      return;
    }
    String DateTimeStr = request->getParam("DateTime", true)->value();
    if (DateTimeStr.length() >= 19) {
      int year = DateTimeStr.substring(0, 4).toInt();
      int month = DateTimeStr.substring(5, 7).toInt();
      int day = DateTimeStr.substring(8, 10).toInt();
      int hour = DateTimeStr.substring(11, 13).toInt();
      int minute = DateTimeStr.substring(14, 16).toInt();
      int second = DateTimeStr.substring(17, 19).toInt();
      int millisec = 0;
      if (DateTimeStr.length() >= 23) {
        millisec = DateTimeStr.substring(20,23).toInt();
      }

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
        struct timeval newNow = {.tv_sec = newTime, .tv_usec = millisec*1000};
        settimeofday(&newNow, NULL);
        if (rtcEnabled) {
          time_t nowTime = time(nullptr);
          struct tm timeInfo;
          gmtime_r(&nowTime, &timeInfo);
          rtc.adjust(DateTime(timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec));
        }
      }
    }
    DateTime dtNow;
    if (rtcEnabled) {
      dtNow = rtc.now();
    } else {
      dtNow = DateTime(time(nullptr));
    }
    // Convert from UTC to Local
    time_t nowTime = dtNow.unixtime();
    struct tm timeInfo;
    localtime_r(&nowTime, &timeInfo);
    char dateTimeJson[48];
    snprintf(dateTimeJson, sizeof(dateTimeJson), "{\"time\":\"%04u-%02u-%02u %02u:%02u:%02u\"}", timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
    request->send(200, "application/json", dateTimeJson);
  });

  server.on("/ntp_sync", HTTP_GET, [](AsyncWebServerRequest *request) {
#if DEBUG==true
    Serial.println(F("[WEBSERVER] Request: /ntp_sync"));
#endif
    if (ntpState == NTP_IDLE || ntpState == NTP_SUCCESS) {
      startNTPSync(true);
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.begin();
#if DEBUG==true
  Serial.println(F("[WEBSERVER] Web server started"));
#endif
}

/*
 * Main setup() and loop()
 */
void setup() {
  Serial.begin(115200);
  delay(500);
#if DEBUG==true
  Serial.println(F("[SETUP] Starting setup..."));
#endif

  // Check if RTC was found.
  if (!rtc.begin()) {
#if DEBUG==true
    Serial.println(F("[SETUP] Unable to find RTC."));
#endif
    rtcEnabled = false;
  } else {
#if DEBUG==true
    Serial.println(F("[SETUP] RTC found."));
#endif
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
#if DEBUG==true
  Serial.println(F("[ERROR] LittleFS mount failed in setup! Halting."));
#endif
    while (true) {
      delay(100);
      yield();
    }
  }
#if DEBUG==true
  Serial.println(F("[SETUP] LittleFS file system mounted successfully."));
#endif

  P.begin();  // Initialize Parola library
  P.setCharSpacing(1);
  P.setFont(mFactory);
  loadConfig();  // This function now has internal yields and prints

  P.setIntensity(brightness);
  P.setZoneEffect(0, flipDisplay, PA_FLIP_UD);
  P.setZoneEffect(0, flipDisplay, PA_FLIP_LR);
#if DEBUG==true
  Serial.println(F("[SETUP] Parola (LED Matrix) initialized"));
#endif

  connectWiFi();
  setupWebServer();
  startMDNS();
  startElegantOTA();
  setTimeZone(timeZone);
#if DEBUG==true
  Serial.println(F("[SETUP] Setup complete"));
#endif
  printConfigToSerial();
  lastColonBlink = millis();
}

void loop() {
  uint32_t curMillis = millis();
  // --- WiFi Connection State Machine ---
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
#if DEBUG==true
          Serial.print(F("[WIFI] Attempting to connect to "));
          Serial.println(ssids[i]);
#endif
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
#if DEBUG==true
        Serial.println(F("[WIFI] WiFi disconnect timeout reached."));
#endif
        connectWiFi();
      }
      break;
    case WIFI_APMODE:
      dnsServer.processNextRequest();
      // Try to reconnect to a network every apTimeout when in ap mode
      if (curMillis >= wifiLastTime + apTimeout) {
#if DEBUG==true
        Serial.println(F("[WIFI] AP Mode timeout reached."));
#endif
        connectWiFi();
      }
    default:
      // --- ElegantOTA ---
      // Only try to run if we're connected or in AP mode.
      ElegantOTA.loop();
      break;
  }

  // --- NTP State Machine ---
  if (wifiState == WIFI_CONNECTED) {
    switch (ntpState) {
      case NTP_SUCCESS:
      case NTP_IDLE: {
          // If RTC doesn't work, attempt to refresh NTP sync every hour.
          if (!rtcEnabled  && (ntpLastTime == 0 || curMillis > ntpLastTime + ntpRefreshTime)) {
            startNTPSync(false);
          }
        }
        break;
      case NTP_SYNCING: {
          time_t lNow = time(nullptr);
          if (lNow > 1000) {  // NTP sync successful
  #if DEBUG==true
            Serial.println(F("[TIME] NTP sync successful."));
  #endif
            ntpState = NTP_SUCCESS;
            setTimeZone(timeZone);
            if (rtcEnabled) {
              #if DEBUG==true
              Serial.println(F("[TIME] Adjusting RTC clock."));
              #endif
              time_t nowTime = rtc.now().unixtime();
              struct tm timeInfo;
              gmtime_r(&nowTime, &timeInfo);
              rtc.adjust(DateTime(timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec));
            }
          } else if (curMillis - ntpLastTime > ntpTimeout && ntpRetryCount < maxNtpRetries) {
            #if DEBUG==true
            Serial.println(F("[TIME] NTP sync failed."));
            #endif
            ntpState = NTP_FAILED;
          } else if (ntpRetryCount >= maxNtpRetries) {
            ntpState = NTP_IDLE;
          }
        }
        break;
      case NTP_FAILED: {
  #if DEBUG==true
          Serial.println(F("[TIME] Retrying NTP sync..."));
  #endif
          startNTPSync(false, ntpRetryCount);
        }
        break;
    }
  }

  // Colon is visible for 800 ms then not for 800 ms
  if (curMillis - lastColonBlink > COLON_BLINK_INTERVAL) {
    colonVisible = !colonVisible;
    lastColonBlink = millis();
  }

  // Create DateTime object and set it via RTC or via (maybe) ntp synced clock.
  DateTime dtNow;
  if (rtcEnabled) { // RTC stores UTC time.
    dtNow = rtc.now();
  } else {
    dtNow = DateTime(time(nullptr));
  }
  char timeWithSeconds[24];
  // --- COUNTUPDOWN Display Mode ---
  if (countupdownTimestamp > 0) {
    long timeSeconds = countupdownTimestamp - dtNow.unixtime();
    if (timeSeconds < 0) {
      timeSeconds = timeSeconds * -1;
    }

    if (timeSeconds < 1296000) { // less than 15 days
      // Format the full string
      if (colonVisible) {
        snprintf(timeWithSeconds, sizeof(timeWithSeconds), "%ld:%02ld:%02ld", timeSeconds / 3600, (timeSeconds % 3600) / 60, timeSeconds % 60);
      } else {
        snprintf(timeWithSeconds, sizeof(timeWithSeconds), "%ld %02ld %02ld", timeSeconds / 3600, (timeSeconds % 3600) / 60, timeSeconds % 60);
      }
    } else if (timeSeconds < 31557600) { // less than 365.25 days / 1 year-ish
      // display days-hours:minutes
      if (colonVisible) {
        snprintf(timeWithSeconds, sizeof(timeWithSeconds), "%ld+%02ld:%02ld", timeSeconds / 86400, (timeSeconds % 86400) / 3600, (timeSeconds % 3600) / 60);
      } else {
        snprintf(timeWithSeconds, sizeof(timeWithSeconds), "%ld^%02ld %02ld", timeSeconds / 86400, (timeSeconds % 86400) / 3600, (timeSeconds % 3600) / 60);
      }
    } else {
      if (colonVisible) {
        snprintf(timeWithSeconds, sizeof(timeWithSeconds), "%ld+%ld", timeSeconds / 31557600, (timeSeconds % 31557600) / 86400);
      } else {
        snprintf(timeWithSeconds, sizeof(timeWithSeconds), "%ld^%ld", timeSeconds / 31557600, (timeSeconds % 31557600) / 86400);
      }
    }
  }  // End COUNTUPDOWN Display Mode
  // --- CLOCK Display Mode ---
  else {
    // Convert UTC to local.
    struct tm tm;
    time_t utcStamp = dtNow.unixtime();
    localtime_r(&utcStamp, &tm);
    if (twelveHour) {
      uint8_t hour = tm.tm_hour % 12;
      snprintf(timeWithSeconds, sizeof(timeWithSeconds), "%d:%02d:%02d", hour == 0 ? 12 : hour, tm.tm_min, tm.tm_sec);
    } else {
      snprintf(timeWithSeconds, sizeof(timeWithSeconds), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
  } // End CLOCK Display Mode
  P.setTextAlignment(PA_CENTER);
  P.print(timeWithSeconds);
  yield();
}