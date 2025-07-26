#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include <AsyncMqttClient.h>

#include "fs-helper.h"
#include "config.h"
#include "log.h"
#include "wifi-helper.h"
#include "hoermann.h"
#include "device.h"
#include "mqtt-helper.h"
#include "auth.h"
#include "webserver.h"


AppConfig appConfig;
Preferences pref;
AsyncWebServer server(80);
AsyncEventSource events("/api/events");

void initConfig() {
    
  // FS Version
  char versionBuffer[13];
  readFsVersion(versionBuffer, sizeof(versionBuffer));
  strcpy(appConfig.versionFs, versionBuffer);

  // efuse data
  char* serialNumber;
  char* hwRev;
  getEfuseData(serialNumber, hwRev);
  appConfig.serialNumber = serialNumber;
  appConfig.hwRev = hwRev;

  pref.begin("deviceSettings");

  // set the board name (aka hostname) using 2 mac bytes
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char boardName[20];
  snprintf(boardName, sizeof(boardName), "PandaGarage-%02X%02X", mac[4], mac[5]);

  if (pref.getString("name", "").length() == 0) {
      pref.putString("name", boardName);
  }

  strcpy(appConfig.name, pref.getString("name", boardName).c_str());
  strcpy(appConfig.lang, pref.getString("lang", "en").c_str());
  appConfig.sensorUpdateInterval = pref.getInt("sensorInterval", PREF_SENSOR_UPDATE_INTERVAL);
  appConfig.tempUnit = pref.getInt("tempUnit", PREF_TEMP_UNIT);
  appConfig.tempThreshold = pref.getFloat("thresholdTemp", PREF_THRESHOLD_TEMP);
  appConfig.humThreshold = pref.getInt("thresholdHum", PREF_THRESHOLD_HUM);
  appConfig.presThreshold = pref.getInt("thresholdPres", PREF_THRESHOLD_PRES);
  appConfig.luxThreshold = pref.getInt("thresholdLux", PREF_THRESHOLD_LUX);
  appConfig.externalSensorSet = pref.getBool("extSensorSet", PREF_EXTERNAL_SENSOR);
  appConfig.externalSensor = pref.getInt("extSensor", PREF_EXTERNAL_SENSOR_TYPE);
  appConfig.combineSensors = pref.getBool("combineSensors", PREF_COMBINE_SENSORS);
  appConfig.buzzerSet = pref.getBool("buzzerSet", PREF_BUZZER_SET);
  appConfig.buzzerTune = pref.getInt("buzzerTune", PREF_BUZZER_TUNE);
  appConfig.buzzerOpening = pref.getBool("buzzerOpening", PREF_BUZZER_OPENING);
  appConfig.buzzerClosing = pref.getBool("buzzerClosing", PREF_BUZZER_CLOSING);
  appConfig.logAccess = pref.getBool("logAccess", PREF_LOG_ACCESS);
  appConfig.logLevel = pref.getInt("logLevel", PREF_LOG_LVL);
  pref.end();


  pref.begin("haSettings", true);
  appConfig.haSet = pref.getBool("activate", false);
  strcpy(appConfig.haIp, pref.getString("ip", "").c_str());
  appConfig.haPort = pref.getInt("port", 1883);
  strcpy(appConfig.haUser, pref.getString("user", "").c_str());
  strcpy(appConfig.haPwd, pref.getString("pwd", "").c_str());
  pref.end();


  pref.begin("wifi", true);
  appConfig.wifiSet = pref.getBool("set", false);
  strcpy(appConfig.wifiSsid, pref.getString("ssid", "").c_str());
  strcpy(appConfig.wifiPwd, pref.getString("pwd", "").c_str());
  pref.end();


  pref.begin("security");
  appConfig.useAuth = pref.getBool("useAuth", PREF_USE_AUTH);
  appConfig.setupDone = pref.getBool("setupDone", false);

  // if the password is the default, we need to save hashed version
  String stored = pref.getString("adminPwd", PREF_ADMIN_PASSWORD);
  if (stored == PREF_ADMIN_PASSWORD) {
      stored = sha256(PREF_ADMIN_PASSWORD);
      pref.putString("adminPwd", stored);
  }
  strcpy(appConfig.adminPwd, stored.c_str());
  pref.end();

  // sensor init
  appConfig.temperature = 0;
  appConfig.humidity = 0;
  appConfig.pressure = 0;
  appConfig.lux = 0;
  appConfig.extSensorData = "{}"; // empty JSON object
}


void onDoorStateChanged(const HoermannState &s) {

  // publish to Home Assistant
  mqttHaPublish("/cover/position", String((s.currentPosition * 100)).c_str(), true);
  mqttHaPublish("/cover/state", String(s.coverState).c_str(), true);
  mqttHaPublish("/light/state", (s.lightOn ? "ON" : "OFF"), false);
  

  // publish to server sent events in same format as api status for compatibility
  JsonDocument door;
  door["position_current"] = (s.currentPosition * 100);
  door["position_target"] = (s.targetPosition * 100);
  door["state"] = s.translatedState;
  door["moving"] = s.currentPosition != s.targetPosition;
  door["light"] = s.lightOn;

  JsonDocument doc;
  doc["door"] = door;

  String response;
  serializeJson(doc, response);

  events.send(response.c_str(), "door", millis());
}


void setup() {
  Serial.begin(115200);
  delay(500);

  // Initialize LittleFS
  initFs();

  // start log task
  initLogger();

  // Initialize application config
  initConfig();

  // start garage door connection
  if (appConfig.setupDone) {
    pinMode(RS_EN, OUTPUT);
    digitalWrite(RS_EN, LOW);

    hoermannEngine->setup();
    logger("Garage Door: ok", "BOOT", LOG_INFO);
  }


  if (!appConfig.wifiSet) {
      logger("WiFi not setup yet, starting AP Mode", "BOOT", LOG_INFO);
      setupWifiAp();
  } else {
      setupWifi();
  }


  // Setup server sent events
  events.onConnect([](AsyncEventSourceClient* client) {
      client->send("Connected to PandaGarage SSE - Hello!", NULL, millis(), 1000);
      logger("Client connected", "SSE", LOG_INFO);
  });


  // Start server
  routing(server);
  server.addHandler(&events);
  server.begin();
  logger("HTTP Server: ok", "BOOT", LOG_INFO);
  

  // start mqtt for Home Assistant
  if (appConfig.haSet) {
      if (mqttHaSetup()) {
          mqttHaReconnect();
          initMqttTask();
      }
  }

  
  // setup sensors
  setupSensors();
  initSensorTask();
  logger("Sensors: ok", "BOOT", LOG_INFO);


  // setup buzzer
  if (appConfig.buzzerSet) {
      setupBuzzer();
      logger("Buzzer: ok", "BOOT", LOG_INFO);
  } else {
      logger("Buzzer: not set", "BOOT", LOG_INFO);
  }

  logger(String(appConfig.name) + " is ready!", "BOOT", LOG_INFO);
}


void loop() {

  // was setup done?
  if (appConfig.setupDone && !updateInProgress) {
    
    // check for garage door updates
    if (hoermannEngine->state->isValid() && hoermannEngine->state->changed) {
      onDoorStateChanged(*hoermannEngine->state);
      hoermannEngine->state->clearChanged();
    }

    // sensor and buzzer handled in dedicated tasks
    buzzerLoop();
  }

  // if update is in progress sent mqtt update
  if (updateInProgress && appConfig.haSet && lastReportedPct != currentPct) {

    JsonDocument doc;
    doc["installed_version"] = VERSION;
    doc["latest_version"] = appConfig.latestFw;
    doc["entity_picture"] = "https://raw.githubusercontent.com/derDeno/PandaGarage/refs/heads/gh-pages/img/logo.png";
    doc["release_url"] = "https://github.com/derDeno/PandaGarage/releases/latest";
    doc["update_percentage"] = currentPct;

    String state;
    serializeJson(doc, state);
    mqttHaPublish("/update/state", state.c_str(), true);

    lastReportedPct = currentPct;
  }

  wifiLoop();
  delay(10);
}
