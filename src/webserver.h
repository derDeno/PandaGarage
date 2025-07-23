#include <Update.h>
#include <nvs_flash.h>
#include <base64.h>
#include "mbedtls/sha256.h"

extern AsyncEventSource events;
extern Preferences pref;
extern AppConfig appConfig;

bool updateInProgress = false;
volatile int lastReportedPct = 0;
volatile int currentPct = 0;


// Auth Middleware
bool isAuthorized(AsyncWebServerRequest* request) {
    if(!appConfig.useAuth) {
        return true;
    }

    if (!request->hasHeader("Authorization")) {
        request->send(403, "application/json", "{\"error\":\"missing auth\"}");
        return false;
    }

    String hash = request->getHeader("Authorization")->value();
    if (!verifyPasswordHash(hash)) {
        request->send(403, "application/json", "{\"error\":\"invalid auth\"}");
        return false;
    }
    return true;
}

String processorInfo(const String &var) {
    if (var == "TEMPLATE_DEVICE_NAME") {
        return appConfig.name;

    } else if (var == "TEMPLATE_WIFI_ICON") {
        if (WiFi.RSSI() > -50) {
            return String("assets/img/wifi-3.svg");
        } else if (WiFi.RSSI() > -65) {
            return String("assets/img/wifi-2.svg");
        } else if (WiFi.RSSI() > -80) {
            return String("assets/img/wifi-1.svg");
        } else {
            return String("assets/img/wifi-0.svg");
        }

    } else if (var == "TEMPLATE_WIFI_SIGNAL") {
        return String(WiFi.RSSI());

    } else if (var == "TEMPLATE_IP") {
        return WiFi.localIP().toString();

    } else if (var == "TEMPLATE_MAC") {
        return WiFi.macAddress();

    } else if (var == "TEMPLATE_UPTIME") {
        unsigned long uptimeMillis = millis();

        unsigned long seconds = uptimeMillis / 1000;
        unsigned long minutes = seconds / 60;
        unsigned long hours = minutes / 60;
        unsigned long days = hours / 24;

        seconds = seconds % 60;
        minutes = minutes % 60;
        hours = hours % 24;

        String uptime = String(days) + " days " + String(hours) + "h " + (minutes < 10 ? "0" : "") + String(minutes) + "min " + (seconds < 10 ? "0" : "") + String(seconds) + "s";
        return uptime;

    } else if (var == "TEMPLATE_LOCAL_TIME") {
        getLocalTime(&timeinfo);

        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);

        return String(timeStr) + " UTC";

    } else if (var == "TEMPLATE_VERSION_FW") {
        return VERSION;

    } else if (var == "TEMPLATE_VERSION_FS") {
        return F(appConfig.versionFs);

    } else if (var == "TEMPLATE_VERSION_HW") {
        return appConfig.hwRev;

    } else if (var == "TEMPLATE_DEVICE_SERIAL") {
        return appConfig.serialNumber;

    } else if (var == "TEMPLATE_RESTART_REASON") {
        return restartReasonString(esp_reset_reason());
    }

    return String();
}

String processorLogs(const String &var) {
    if (var == "LOG_ACCESS_TEMPLATE") {
        // check if logging is even active
        bool logging = appConfig.logAccess;

        if (!logging) {
            return "Access logging is disabled!";
        }

        File logFile = LittleFS.open("/log-access.txt", "r");
        String logContent = "";
        if (logFile) {
            while (logFile.available()) {
                String temp = logFile.readStringUntil('\n');

                // check if string begins with E: or W: and colorize it
                if (temp.indexOf("E: ") != -1) {
                    logContent += "<span class='text-danger'>" + temp + "</span><br>";
                } else if (temp.indexOf("W:") != -1) {
                    logContent += "<span class='text-warning'>" + temp + "</span><br>";
                } else if (temp.indexOf("MQTT:") != -1) {
                    logContent += "<span class='text-info'>" + temp + "</span><br>";
                } else {
                    logContent += temp + "<br>";
                }
            }
            logFile.close();
        } else {
            logContent = "empty";
        }
        return logContent;
    }

    // Return an empty string if the placeholder is unknown
    return String();
}

void handleOtaFw(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
    static size_t totalSize = 0;

    if (index == 0) {
        vTaskSuspend(modBusTask);
        logger("OTA Firmware update start: " + filename, "Device", LOG_INFO);

        if (!Update.begin(request->contentLength())) {
            Update.printError(Serial);
            vTaskResume(modBusTask);
            return;
        }

        totalSize = 0;
    }

    if (!Update.hasError() && len > 0) {
        if (Update.write(data, len) != len) {
            String errMsg;
            StringPrinter sp(errMsg);

            Update.printError(sp);
            logger(errMsg, "Device", LOG_ERROR);
            events.send(errMsg, "ota-progress", millis());

            vTaskResume(modBusTask);

        } else {
            totalSize += len;
            int progress = (totalSize * 100) / request->contentLength();
            Serial.println("OTA Firmware progress: " + String(progress) + "%");
            events.send(String(progress).c_str(), "ota-progress", millis());

            // if mqtt is connected, publish progress
            updateInProgress = true;
            currentPct = progress;
        }
    }

    if (final) {
        if (Update.end(true)) {
            String msg = "Firmware update success: " + String(index + len) + "bytes written";
            logger(msg, "Device", LOG_INFO);
            events.send("100", "ota-progress", millis());

        } else {
            String errMsg;
            StringPrinter sp(errMsg);
            
            Update.printError(sp);
            logger(errMsg, "Device", LOG_ERROR);
            events.send(errMsg, "ota-progress", millis());
        }
        
        vTaskResume(modBusTask);
    }
}

void handleOtaFs(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
    static size_t totalSize = 0;

    if (index == 0) {
        vTaskSuspend(modBusTask);
        logger("OTA Filesystem update start: " + filename, "Device", LOG_INFO);

        // fixed size for SPIFFS
        if (!Update.begin(0x200000, U_SPIFFS)) {
            Update.printError(Serial);
            vTaskResume(modBusTask);
            return;
        }

        totalSize = 0;
    }

    if (!Update.hasError() && len > 0) {
        if (Update.write(data, len) != len) {
            Update.printError(Serial);
            vTaskResume(modBusTask);

        } else {
            totalSize += len;
            int progress = (totalSize * 100) / request->contentLength();
            Serial.println("OTA Filesystem progress: " + String(progress) + "%");
            events.send(String(progress).c_str(), "ota-progress", millis());
        }
    }

    if (final) {
        if (Update.end(true)) {
            String msg = "Filesystem update success: " + String(index + len) + "bytes written";
            logger(msg, "Device", LOG_INFO);
            events.send("100", "ota-progress", millis());

        } else {
            Update.printError(Serial);
            events.send("error", "ota-progress", millis());
        }

        vTaskResume(modBusTask);
    }
}

void setupSettingsRoutes(AsyncWebServer &server) {
    server.on("/api/settings/device", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        AsyncResponseStream *response = request->beginResponseStream("application/json");
        JsonDocument doc;
        doc["name"] = appConfig.name;
        doc["lang"] = appConfig.lang;
        doc["tempUnit"] = appConfig.tempUnit;
        doc["sensorUpdateInterval"] = appConfig.sensorUpdateInterval;
        doc["thresholdTemp"] = appConfig.tempThreshold;
        doc["thresholdHumidity"] = appConfig.humThreshold;
        doc["thresholdPressure"] = appConfig.presThreshold;
        doc["thresholdLight"] = appConfig.luxThreshold;
        doc["externalSensorSet"] = appConfig.externalSensorSet;
        doc["externalSensor"] = appConfig.externalSensor;
        doc["combineSensors"] = appConfig.combineSensors;
        doc["buzzerSet"] = appConfig.buzzerSet;
        doc["buzzerTune"] = appConfig.buzzerTune;
        doc["buzzerOpening"] = appConfig.buzzerOpening;
        doc["buzzerClosing"] = appConfig.buzzerClosing;
        doc["logLevel"] = appConfig.logLevel;
        doc["logAccess"] = appConfig.logAccess;

        serializeJson(doc, *response);
        request->send(response);
    });

    server.on("/api/settings/device", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        pref.begin("deviceSettings");

        if (request->hasParam("name", true)) {
            const String name = request->getParam("name", true)->value();
            pref.putString("name", name);
        }

        if (request->hasParam("lang", true)) {
            const String lang = request->getParam("lang", true)->value();
            pref.putString("lang", lang);
        }

        if (request->hasParam("tempUnit", true)) {
            const int tempUnit = request->getParam("tempUnit", true)->value().toInt();
            pref.putInt("tempUnit", tempUnit);
        }

        if (request->hasParam("sensorUpdateInterval", true)) {
            const int sensorUpdateInterval = request->getParam("sensorUpdateInterval", true)->value().toInt();
            pref.putInt("sensorInterval", sensorUpdateInterval);
        }

        if (request->hasParam("thresholdTemp", true)) {
            const float thresholdTemp = request->getParam("thresholdTemp", true)->value().toFloat();
            pref.putFloat("thresholdTemp", thresholdTemp);
        }

        if (request->hasParam("thresholdHumidity", true)) {
            const int thresholdHum = request->getParam("thresholdHumidity", true)->value().toInt();
            pref.putInt("thresholdHum", thresholdHum);
        }

        if (request->hasParam("thresholdPressure", true)) {
            const int thresholdPres = request->getParam("thresholdPressure", true)->value().toInt();
            pref.putInt("thresholdPres", thresholdPres);
        }

        if (request->hasParam("thresholdLight", true)) {
            const int thresholdLight = request->getParam("thresholdLight", true)->value().toInt();
            pref.putInt("thresholdLux", thresholdLight);
        }

        if (request->hasParam("externalSensorSet", true)) {
            const String val = request->getParam("externalSensorSet", true)->value();

            bool externalSensorSet = false;
            if (val == "true" || val == "1") {
                externalSensorSet = true;
            }

            pref.putBool("extSensorSet", externalSensorSet);
        }

        if (request->hasParam("externalSensor", true)) {
            const int externalSensor = request->getParam("externalSensor", true)->value().toInt();
            pref.putInt("extSensor", externalSensor);
        }

        if (request->hasParam("combineSensors", true)) {
            const String val = request->getParam("combineSensors", true)->value();

            bool combineSensors = false;
            if (val == "true" || val == "1") {
                combineSensors = true;
            }

            pref.putBool("combineSensors", combineSensors);
        }

        if (request->hasParam("buzzerSet", true)) {
            const String val = request->getParam("buzzerSet", true)->value();

            bool buzzerSet = false;
            if (val == "true" || val == "1") {
                buzzerSet = true;
            }

            pref.putBool("buzzerSet", buzzerSet);
        }

        if (request->hasParam("buzzerTune", true)) {
            const int buzzerTune = request->getParam("buzzerTune", true)->value().toInt();
            pref.putInt("buzzerTune", buzzerTune);
        }

        if (request->hasParam("buzzerOpening", true)) {
            const String val = request->getParam("buzzerOpening", true)->value();

            bool buzzerOpening = false;
            if (val == "true" || val == "1") {
                buzzerOpening = true;
            }

            pref.putBool("buzzerOpening", buzzerOpening);
        }

        if (request->hasParam("buzzerClosing", true)) {
            const String val = request->getParam("buzzerClosing", true)->value();

            bool buzzerClosing = false;
            if (val == "true" || val == "1") {
                buzzerClosing = true;
            }

            pref.putBool("buzzerClosing", buzzerClosing);
        }

        if (request->hasParam("logLevel", true)) {
            const uint8_t val = request->getParam("logLevel", true)->value().toInt();

            pref.putInt("logLevel", val);
        }

        if (request->hasParam("logAccess", true)) {
            const String val = request->getParam("logAccess", true)->value();

            bool logAccess = false;
            if (val == "true" || val == "1") {
                logAccess = true;
            }

            pref.putBool("logAccess", logAccess);
            Serial.println("Access logging: " + String(logAccess));

            // if loggging is set to false delete the existing file
            if (!logAccess) {
                deleteLogFile("/log-access.txt");
            }
        }

        pref.end();

        request->send(200, "application/json", "{\"status\":\"saved\"}");
        request->onDisconnect([]() {
            delay(100);
            ESP.restart();
        });
    });

    server.on("/api/settings/ha", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        AsyncResponseStream *response = request->beginResponseStream("application/json");
        JsonDocument doc;
        doc["activate"] = appConfig.haSet;
        doc["ip"] = appConfig.haIp;
        doc["port"] = appConfig.haPort;
        doc["user"] = appConfig.haUser;
        doc["pwd"] = appConfig.haPwd;

        serializeJson(doc, *response);
        request->send(response);
    });

    server.on("/api/settings/ha", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        pref.begin("haSettings");

        if (request->hasParam("activate", true)) {
            const String val = request->getParam("activate", true)->value();

            bool activate = false;
            if (val == "true" || val == "1") {
                activate = true;
            }
            pref.putBool("activate", activate);
        }

        if (request->hasParam("ip", true)) {
            const String haIp = request->getParam("ip", true)->value();
            pref.putString("ip", haIp);
        }

        if (request->hasParam("port", true)) {
            const int port = request->getParam("port", true)->value().toInt();
            pref.putInt("port", port);
        }

        if (request->hasParam("user", true)) {
            const String user = request->getParam("user", true)->value();
            pref.putString("user", user);
        }

        if (request->hasParam("pwd", true)) {
            const String pwd = request->getParam("pwd", true)->value();
            pref.putString("pwd", pwd);
        }

        pref.end();

        request->send(200, "application/json", "{\"status\":\"saved\"}");
        request->onDisconnect([]() {
            delay(100);
            ESP.restart();
        });
    });

    server.on("/api/settings/security", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        AsyncResponseStream *response = request->beginResponseStream("application/json");
        JsonDocument doc;
        doc["useAuth"] = appConfig.useAuth;

        serializeJson(doc, *response);
        request->send(response);
    });

    server.on("/api/settings/security", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        pref.begin("security");

        if (request->hasParam("pwd", true)) {
            const String pwd = request->getParam("pwd", true)->value();

            // check if password is set
            if (pwd.length() > 0) {
                String output = sha256(pwd);
                pref.putString("adminPwd", output);

                logger("Admin password updated", "Device", LOG_WARNING);
            }
        }

        if (request->hasParam("useAuth", true)) {
            const String val = request->getParam("useAuth", true)->value();

            bool useAuth = false;
            if (val == "true" || val == "1") {
                useAuth = true;
            }
            pref.putBool("useAuth", useAuth);
        }

        pref.end();

        request->send(200, "application/json", "{\"status\":\"saved\"}");
        request->onDisconnect([]() {
            delay(100);
            ESP.restart();
        });
    });

    server.on("/api/settings/wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        pref.begin("wifi");

        if (request->hasParam("ssid", true)) {
            const String ssid = request->getParam("ssid", true)->value();

            pref.putString("ssid", ssid);
        }

        if (request->hasParam("pwd", true)) {
            const String pwd = request->getParam("pwd", true)->value();
            pref.putString("pwd", pwd);
        }

        pref.end();

        request->send(200, "application/json", "{\"status\":\"saved\"}");
        request->onDisconnect([]() {
            delay(100);
            ESP.restart();
        });
    });

    server.on("/api/settings/ap", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        pref.begin("wifi");

        pref.clear();
        pref.end();

        request->send(200, "application/json", "{\"status\":\"saved\"}");
        request->onDisconnect([]() {
            delay(100);
            ESP.restart();
        });
    });
}

void setupFileRoutes(AsyncWebServer &server) {
    server.on("/api/ota/fw", HTTP_POST, [](AsyncWebServerRequest *request) { 
        if (!isAuthorized(request)) return;

        if(Update.hasError()) {
            request->send(500, "text/plain", "OTA Firmware update failed! Check Logs for details.");

        }else {

            logger("OTA Firmware update complete, restarting...", "Device", LOG_INFO);

            AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "OTA Firmware update successful! Rebooting...");
            response->addHeader("Connection", "close");
            request->send(response);

            request->onDisconnect([]() {
            delay(2000);
            ESP.restart();
            });
        } 
    }, handleOtaFw);

    server.on("/api/ota/fs", HTTP_POST, [](AsyncWebServerRequest *request) { 
        if (!isAuthorized(request)) return;

        if(Update.hasError()) {
            request->send(500, "text/plain", "OTA Filesystem update failed! Check Logs for details.");

        }else {

            logger("OTA Filesystem update complete, restarting...", "Device", LOG_INFO);

            AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "OTA Filesystem update successful! Rebooting...");
            response->addHeader("Connection", "close");
            request->send(response);

            request->onDisconnect([]() {
            delay(2000);
            ESP.restart();
            });
        } 
    }, handleOtaFs);
}

void setupApiRoutes(AsyncWebServer &server) {

    // simple token check endpoint
    server.on("/api/auth", HTTP_POST, [](AsyncWebServerRequest *request) {

        if (request->hasParam("pwd", true)) {
            const String pwd = request->getParam("pwd", true)->value();
            String output = sha256(pwd);

            if (!verifyPasswordHash(output)) {
                request->send(401, "application/json", "{\"error\":\"invalid auth\"}");
            } else {
                request->send(200, "application/json", "{\"token\":\"" + output + "\"}");
            }
        } else {
            if (!isAuthorized(request)) return;

            String stored = appConfig.adminPwd;
            request->send(200, "application/json", "{\"token\":\"" + stored + "\"}");
        }
    });
    
    // info endpoint returns information about the board
    server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncResponseStream *response = request->beginResponseStream("application/json");

        JsonDocument doc;
        doc["mac"] = WiFi.macAddress();
        doc["ip"] = WiFi.localIP().toString();
        doc["hostname"] = WiFi.getHostname();
        doc["rssi"] = WiFi.RSSI();
        doc["version_fw"] = VERSION;
        doc["version_fs"] = appConfig.versionFs;
        doc["version_hw"] = appConfig.hwRev;
        doc["sn"] = appConfig.serialNumber;
        doc["uptime"] = millis();

        serializeJson(doc, *response);
        request->send(response);
    });

    // diagnose endpoint for diagnose export
    server.on("/api/diagnose", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncResponseStream *response = request->beginResponseStream("application/json");

        JsonDocument hw;
        hw["mac"] = WiFi.macAddress();
        hw["ip"] = WiFi.localIP().toString();
        hw["hostname"] = WiFi.getHostname();
        hw["rssi"] = WiFi.RSSI();
        hw["version_fw"] = VERSION;
        hw["version_fs"] = appConfig.versionFs;
        hw["version_hw"] = appConfig.hwRev;
        hw["sn"] = appConfig.serialNumber;
        hw["uptime"] = millis();

        JsonDocument door;
        door["position_current"] = (int)(hoermannEngine->state->currentPosition * 100);
        door["position_target"] = (int)(hoermannEngine->state->targetPosition * 100);
        door["state"] = hoermannEngine->state->translatedState;
        door["moving"] = (hoermannEngine->state->currentPosition != hoermannEngine->state->targetPosition);
        door["light"] = hoermannEngine->state->lightOn;

        JsonDocument sensor;
        sensor["temperature"] = appConfig.temperature;
        sensor["humidity"] = appConfig.humidity;
        sensor["pressure"] = appConfig.pressure;
        sensor["lux"] = appConfig.lux;
        sensor["thresholdTemp"] = appConfig.tempThreshold;
        sensor["thresholdHumidity"] = appConfig.humThreshold;
        sensor["thresholdPressure"] = appConfig.presThreshold;
        sensor["thresholdLight"] = appConfig.luxThreshold;
        sensor["sensorUpdateInterval"] = appConfig.sensorUpdateInterval;
        sensor["tempUnit"] = appConfig.tempUnit;
        
        // if external sensor is set, add it to the response
        if (appConfig.externalSensorSet) {
            sensor["externalSensor"] = appConfig.externalSensor;
            sensor["extSensorData"] = appConfig.extSensorData;
        } else {
            sensor["externalSensor"] = "none";
        }
        sensor["combineSensors"] = appConfig.combineSensors;

        JsonDocument settings;
        settings["name"] = appConfig.name;
        settings["lang"] = appConfig.lang;
        settings["buzzerSet"] = appConfig.buzzerSet;
        settings["buzzerTune"] = appConfig.buzzerTune;
        settings["buzzerOpening"] = appConfig.buzzerOpening;
        settings["buzzerClosing"] = appConfig.buzzerClosing;
        settings["logLevel"] = appConfig.logLevel;
        settings["logAccess"] = appConfig.logAccess;
        settings["useAuth"] = appConfig.useAuth;
        settings["setupDone"] = appConfig.setupDone;
        settings["wifiSet"] = appConfig.wifiSet;
        settings["wifiSsid"] = appConfig.wifiSsid;
        settings["wifiSsidTest"] = appConfig.wifiSsidTest;
        settings["haSet"] = appConfig.haSet;
        settings["haIp"] = appConfig.haIp;
        settings["haPort"] = appConfig.haPort;
        
        JsonDocument doc;
        doc["status"] = "ok";
        doc["hardware"] = hw;
        doc["door"] = door;
        doc["sensor"] = sensor;
        doc["settings"] = settings;


        serializeJson(doc, *response);
        request->send(response);
    });

    // status returns status of garage door and sensors
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        const int doorCurrentPosition = (int)(hoermannEngine->state->currentPosition * 100);
        const int doorTargetPosition = (int)(hoermannEngine->state->targetPosition * 100);
        const String doorState = hoermannEngine->state->translatedState;
        const bool light = hoermannEngine->state->lightOn;
        const bool doorMoving = doorCurrentPosition != doorTargetPosition;

        AsyncResponseStream *response = request->beginResponseStream("application/json");

        JsonDocument door;
        door["position_current"] = doorCurrentPosition;
        door["position_target"] = doorTargetPosition;
        door["state"] = doorState;
        door["moving"] = doorMoving;
        door["light"] = light;

        JsonDocument sensor;
        sensor["temperature"] = appConfig.temperature;
        sensor["humidity"] = appConfig.humidity;
        sensor["pressure"] = appConfig.pressure;
        sensor["lux"] = appConfig.lux;
        sensor["combineSensors"] = appConfig.combineSensors;

        // if exxternal sensor is set, add it to the response
        if (appConfig.externalSensorSet) {
            sensor["externalSensor"] = appConfig.externalSensor;
            sensor["extSensorData"] = appConfig.extSensorData;
        } else {
            sensor["externalSensor"] = "none";
        }

        JsonDocument doc;
        doc["status"] = "ok";
        doc["version_fw"] = VERSION;
        doc["door"] = door;
        doc["sensor"] = sensor;

        serializeJson(doc, *response);
        request->send(response);
    });

    // control door
    server.on("/api/control", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        // retrieve access header
        if (!request->hasHeader("X-Access-Source")) {
            request->send(401, "application/json", "{\"status\":\"unauthorized\"}");
            return;
        }

        String accessSource = request->getHeader("X-Access-Source")->value();

        if (accessSource != "api" && accessSource != "webui") {
            request->send(401, "application/json", "{\"status\":\"unauthorized\"}");
            return;
        }

        if (request->hasParam("action", true)) {
            const String action = request->getParam("action", true)->value();

            if (action == "open") {
                hoermannEngine->openDoor();
                loggerAccess("Door opened", accessSource);
                request->send(200, "application/json", "{\"status\":\"ok\"}");

            } else if (action == "close") {
                hoermannEngine->closeDoor();
                loggerAccess("Door closed", accessSource);
                request->send(200, "application/json", "{\"status\":\"ok\"}");

            } else if (action == "stop") {
                hoermannEngine->stopDoor();
                loggerAccess("Door movement stopped", accessSource);
                request->send(200, "application/json", "{\"status\":\"ok\"}");

            } else if (action == "half") {
                hoermannEngine->halfPositionDoor();
                loggerAccess("Door half opened", accessSource);
                request->send(200, "application/json", "{\"status\":\"ok\"}");

            } else if (action == "vent") {
                hoermannEngine->ventilationPositionDoor();
                loggerAccess("Door vent opened", accessSource);
                request->send(200, "application/json", "{\"status\":\"ok\"}");

            } else if (action == "light") {
                if (request->hasParam("state", true)) {
                    const String val = request->getParam("state", true)->value();

                    bool lightOn = false;
                    String lightState = "off";
                    if (val == "on" || val == "1") {
                        lightOn = true;
                        lightState = "on";
                    }

                    hoermannEngine->turnLight(lightOn);
                    loggerAccess("Light turned " + lightState, accessSource);
                    request->send(200, "application/json", "{\"status\":\"ok\"}");

                } else {
                    hoermannEngine->toggleLight();
                    loggerAccess("Light toggle", accessSource);
                    request->send(200, "application/json", "{\"status\":\"ok\"}");
                }

            } else if (action == "position") {
                if (request->hasParam("position", true)) {
                    const int position = request->getParam("position", true)->value().toInt();

                    if (position >= 0 && position <= 100) {
                        hoermannEngine->setPosition(position);
                        loggerAccess("Door moved to position " + String(position) + "%", accessSource);
                        request->send(200, "application/json", "{\"status\":\"ok\"}");

                    } else {
                        request->send(400, "application/json", "{\"status\":\"invalid\"}");
                        return;
                    }
                } else {
                    request->send(400, "application/json", "{\"status\":\"invalid\"}");
                    return;
                }

            } else {
                request->send(400, "application/json", "{\"status\":\"invalid\"}");
                return;
            }
        } else {
            request->send(400, "application/json", "{\"status\":\"invalid\"}");
            return;
        }
    });

    server.on("/api/log/debug", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (LittleFS.exists("/log.csv")) {
            request->send(LittleFS, "/log.csv", "text/csv", true);
        } else {
            request->send(404, "text/plain", "Debug log file not found!");
        }
    });

    server.on("/api/log/access", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (LittleFS.exists("/log-access.txt")) {
            request->send(LittleFS, "/log-access.txt", "text/plain", true);
        } else {
            request->send(404, "text/plain", "Access log file not found!");
        }
    });

    server.on("/api/log/debug", HTTP_DELETE, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        deleteLogFile("/log.csv");
        logger("Debug log file deleted by user", "Device", LOG_WARNING);
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    server.on("/api/log/access", HTTP_DELETE, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        deleteLogFile("/log-access.txt");
        logger("Access log file deleted by user", "Device", LOG_WARNING);
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    server.on("/api/buzzer/play", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        if (request->hasParam("tune", true)) {
            const int tune = request->getParam("tune", true)->value().toInt();
            playTune(tune);
        }

        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!isAuthorized(request)) return;

        logger("Factory reset by user requested", "Device", LOG_WARNING);

        nvs_flash_erase();
        nvs_flash_init();

        request->redirect("/");

        request->onDisconnect([]() {
            delay(100);
            ESP.restart();
        });
    });

    server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
        
        logger("Restart by user requested", "Device", LOG_WARNING);
        request->send(200, "application/json", "{\"status\":\"restarting\"}");

        request->onDisconnect([]() {
            delay(100);
            ESP.restart();
        });
    });
}

void setupSetupRoutes(AsyncWebServer &server) {
    // setup step 1 - device name
    server.on("/api/setup/1", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        JsonDocument doc;
        doc["name"] = appConfig.name;

        serializeJson(doc, *response);
        request->send(response);
    });

    server.on("/api/setup/1", HTTP_POST, [](AsyncWebServerRequest *request) {
        pref.begin("deviceSettings");

        if (request->hasParam("name", true)) {
            const String name = request->getParam("name", true)->value();
            pref.putString("name", name);
            strcpy(appConfig.name, name.c_str());
        }

        if (request->hasParam("lang", true)) {
            const String lang = request->getParam("lang", true)->value();
            pref.putString("lang", lang);
            strcpy(appConfig.lang, lang.c_str());
        }

        pref.end();
        request->send(200, "application/json", "{\"status\":\"saved\"}");
    });

    // setup step 2 - network settings
    server.on("/api/setup/2", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", "{\"status\":\"scanning\"}");
    });

    server.on("/api/setup/2", HTTP_POST, [](AsyncWebServerRequest *request) {
        // get network details
        if (request->hasParam("ssid", true)) {
            String testSsid = request->getParam("ssid", true)->value();
            strcpy(appConfig.wifiSsidTest, testSsid.c_str());
        }

        if (request->hasParam("pwd", true)) {
            String testPwd = request->getParam("pwd", true)->value();
            strcpy(appConfig.wifiPwdTest, testPwd.c_str());
        }

        if (request->hasParam("bssid", true)) {
            String bssid = request->getParam("bssid", true)->value();
        }

        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    // setup step 3 - device info
    server.on("/api/setup/3", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        JsonDocument doc;
        doc["ip"] = WiFi.localIP().toString();
        doc["ssid"] = appConfig.wifiSsid;
        doc["pwd"] = PREF_ADMIN_PASSWORD;

        serializeJson(doc, *response);
        request->send(response);
    });

    // setup step 4 - restart and finish setup
    server.on("/api/setup/4", HTTP_POST, [](AsyncWebServerRequest *request) {
        // save the setup done flag
        pref.begin("security");
        pref.putBool("setupDone", true);
        appConfig.setupDone = true;
        pref.end();

        request->send(200, "application/json", "{\"status\":\"restarting\"}");

        request->onDisconnect([]() {
            delay(100);
            ESP.restart();
        });
    });
}

// Routing here
void routing(AsyncWebServer &server) {
    setupSetupRoutes(server);

    // these routes require authentication
    setupSettingsRoutes(server);
    setupApiRoutes(server);
    setupFileRoutes(server);

    // map requests to static files
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html").setFilter(ON_STA_FILTER);
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    server.serveStatic("/settings", LittleFS, "/settings/").setDefaultFile("index.html");

    // captive portal mapping
    server.on("/captive", HTTP_GET, [](AsyncWebServerRequest *req) {
        AsyncWebServerResponse *response = req->beginResponse(LittleFS, "/captive.html", "text/html");
        response->addHeader("Cache-Control", "no-cache");
        response->addHeader("Pragma", "no-cache");
        response->addHeader("Expires", "-1");
        req->send(response);
    });


    // not found handler
    server.onNotFound([](AsyncWebServerRequest *request) {
        
        if (WiFi.getMode() & WIFI_AP && request->client()->remoteIP() != WiFi.softAPIP()) {
            Serial.println("Redirecting to captive portal");
            request->redirect("/captive");
            return;
        }

        String path = request->url();
        if (!path.endsWith(".html") && path.indexOf(".") == -1) {
            path += ".html";
        }

        if (LittleFS.exists(path)) {
            // check for info and log for processing content
            if (path == "/info.html") {
                request->send(LittleFS, path, String(), false, processorInfo);
                return;
            }

            if (path == "/logs.html") {
                request->send(LittleFS, path, String(), false, processorLogs);
                return;
            }

            request->send(LittleFS, path, String(), false);
        } else {
            request->redirect("/404");
        }
    });
}