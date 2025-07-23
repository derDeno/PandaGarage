/*
* Managig MQTT communication with Home Assistant
*/
#include <AsyncMqttClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define MQTT_TOPIC_LEN 128
#define MQTT_PAYLOAD_LEN 256

extern AppConfig appConfig;

AsyncMqttClient mqttClientHa;
bool configSent = false;
bool mqttInitState = false;

static const unsigned long GH_UPDATE_INTERVAL = 24UL * 60UL * 60UL * 1000UL;
static unsigned long lastGhUpdateCheck = 0;

struct MqttMessage {
    char topic[MQTT_TOPIC_LEN];
    char payload[MQTT_PAYLOAD_LEN];
    bool retain;
};

QueueHandle_t mqttQueue = NULL;
TaskHandle_t mqttTaskHandle = NULL;
void mqttTask(void *parameter);
void initMqttTask();
void onMqttConnect(bool sessionPresent);
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total);


void checkForFirmwareUpdate() {
    WiFiClientSecure client;
    client.setInsecure();

    String url = "https://api.github.com/repos/derDeno/PandaGarage/releases/latest";

    HTTPClient https;
    https.begin(client, url);
    https.addHeader("User-Agent", "PandaGarage");

    int httpCode = https.GET();
    if (httpCode == HTTP_CODE_OK) {
        String payload = https.getString();
        https.end();

        JsonDocument res;
        auto err = deserializeJson(res, payload);
        if (err) {
            Serial.printf("JSON parse failed: %s\n", err.c_str());
            return;
        }

        const char* latestTag = res["tag_name"];
        strcpy(appConfig.latestFw, latestTag);

        JsonDocument doc;
        doc["installed_version"] = VERSION;
        doc["latest_version"] = latestTag;
        doc["entity_picture"] = "https://raw.githubusercontent.com/derDeno/PandaGarage/refs/heads/gh-pages/img/logo.png";
        doc["release_url"] = "https://github.com/derDeno/PandaGarage/releases/latest";
        doc["update_percentage"] = nullptr;

        String state;
        serializeJson(doc, state);
        mqttHaPublish("/update/state", state.c_str(), true);

    } else {
        https.end();
        return;
    }
}


void mqttHaPublish(const char* topic, const char* payload, bool retain) {

    if(!appConfig.haSet) {
        return;
    }

    const String mqttBase = String("pandagarage/") + appConfig.name;
    String fullTopic = mqttBase + topic;

    // if called from MQTT task directly publish
    if (mqttTaskHandle != NULL && xTaskGetCurrentTaskHandle() == mqttTaskHandle) {
        if (mqttClientHa.connected()) {
            mqttClientHa.publish(fullTopic.c_str(), 0, retain, payload);
        }
        return;
    }

    if (mqttQueue != NULL) {
        MqttMessage msg;
        strncpy(msg.topic, fullTopic.c_str(), MQTT_TOPIC_LEN - 1);
        msg.topic[MQTT_TOPIC_LEN - 1] = '\0';
        strncpy(msg.payload, payload, MQTT_PAYLOAD_LEN - 1);
        msg.payload[MQTT_PAYLOAD_LEN - 1] = '\0';
        msg.retain = retain;
        xQueueSend(mqttQueue, &msg, 0);
    }
}


void mqttHaInitState() {
    mqttHaPublish("/status", "online", true);

    mqttHaPublish("/temp/state", String(appConfig.temperature).c_str(), true);
    mqttHaPublish("/humidity/state", String(appConfig.humidity).c_str(), true);
    mqttHaPublish("/pressure/state", String(appConfig.pressure).c_str(), true);
    mqttHaPublish("/lux/state", String(appConfig.lux).c_str(), true);
    mqttHaPublish("/light/state", "OFF", true);
    mqttHaPublish("/cover/state", "closed", true);
    mqttHaPublish("/cover/position", "0", true);

    JsonDocument doc;
    doc["installed_version"] = VERSION;
    doc["update_percentage"] = nullptr;

    String state;
    serializeJson(doc, state);
    mqttHaPublish("/update/state", state.c_str(), true);


    if (appConfig.externalSensorSet) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, appConfig.extSensorData);

        if (error) {
            logger("Failed to deserialize external sensor data: " + String(error.c_str()), "MQTT", LOG_ERROR);
            return;
        }

        if (appConfig.externalSensor == 1) { // AHT10
            //mqttHaPublish("/ext_sensor/state", appConfig.extSensorData.c_str(), true);

        } else if (appConfig.externalSensor == 2 || appConfig.externalSensor == 3) { // SCD40 or SCD41
            u_int16_t co2 = doc["co2"];
            String co2Str = String(co2);
            mqttHaPublish("/co2/state", co2Str.c_str(), true);

        } else if (appConfig.externalSensor == 4) { // CCS811
            u_int16_t eco2 = doc["eco2"];
            String eco2Str = String(eco2);
            mqttHaPublish("/eco2/state", eco2Str.c_str(), true);

            u_int16_t tvoc = doc["tvoc"];
            String tvocStr = String(tvoc);
            mqttHaPublish("/tvoc/state", tvocStr.c_str(), true);
        }
    }

    delay(250);
}


void mqttHaConfig() {
    const String mqttBase = String("pandagarage/") + String(appConfig.name);
    const String availability_topic = String("pandagarage/") + appConfig.name + String("/status");

    // device config
    JsonDocument device;
    device["name"] = appConfig.name;
    device["ids"][0] = appConfig.name;
    device["mdl"] = "PandaGarage Controller";
    device["mf"] = "DNO";
    device["sw"] = VERSION;
    device["cu"] = "http://" + WiFi.localIP().toString() + "/settings";
    device["sn"] = appConfig.serialNumber;
    device["hw"] = appConfig.hwRev;

    // minimal device config for less data transfer
    JsonDocument deviceMinimal;
    deviceMinimal["name"] = appConfig.name;
    deviceMinimal["ids"][0] = appConfig.name;


    // button restart
    // topic: homeassistant/button/pandagarage/reboot/config
    JsonDocument restart;
    restart["name"] = "Restart";
    restart["uniq_id"] = appConfig.name + String("_restart");
    restart["cmd_t"] = mqttBase + "/restart/set";
    restart["ent_cat"] = "config";
    restart["dev_cla"] = "restart";
    restart["dev"] = device;


    // temp sensor
    // topic: homeassistant/sensor/pandagarage/temp/config
    JsonDocument tempSensor;
    tempSensor["name"] = "Temperature";
    tempSensor["uniq_id"] = appConfig.name + String("_temp");
    tempSensor["stat_t"] = mqttBase + "/temp/state";
    tempSensor["avty_t"] = availability_topic;
    tempSensor["unit_of_meas"] = "°C";
    tempSensor["dev_cla"] = "temperature";
    tempSensor["dev"] = device;

    // humidity sensor
    // topic: homeassistant/sensor/pandagarage/humidity/config
    JsonDocument humiditySensor;
    humiditySensor["name"] = "Humidity";
    humiditySensor["uniq_id"] = appConfig.name + String("_humidity");
    humiditySensor["stat_t"] = mqttBase + "/humidity/state";
    humiditySensor["avty_t"] = availability_topic;
    humiditySensor["unit_of_meas"] = "%";
    humiditySensor["dev_cla"] = "humidity";
    humiditySensor["dev"] = device;

    // pressure sensor
    // topic: homeassistant/sensor/pandagarage/pressure/config
    JsonDocument pressureSensor;
    pressureSensor["name"] = "Pressure";
    pressureSensor["uniq_id"] = appConfig.name + String("_pressure");
    pressureSensor["stat_t"] = mqttBase + "/pressure/state";
    pressureSensor["avty_t"] = availability_topic;
    pressureSensor["unit_of_meas"] = "hPa";
    pressureSensor["dev_cla"] = "pressure";
    pressureSensor["dev"] = device;

    // lux sensor
    // topic: homeassistant/sensor/pandagarage/lux/config
    JsonDocument luxSensor;
    luxSensor["name"] = "Lux";
    luxSensor["uniq_id"] = appConfig.name + String("_lux");
    luxSensor["stat_t"] = mqttBase + "/lux/state";
    luxSensor["avty_t"] = availability_topic;
    luxSensor["unit_of_meas"] = "lx";
    luxSensor["icon"] = "mdi:brightness-5";
    luxSensor["dev_cla"] = "illuminance";
    luxSensor["dev"] = device;

    // update sensor
    // topic: homeassistant/update/pandagarage/config
    JsonDocument updateSensor;
    updateSensor["name"] = "Firmware Update";
    updateSensor["title"] = appConfig.name + String(" Firmware");
    updateSensor["platform"] = "update";
    updateSensor["release_url"] = "https://github.com/derDeno/PandaGarage/releases/latest";
    updateSensor["uniq_id"] = appConfig.name + String("_update");
    updateSensor["stat_t"] = mqttBase + "/update/state";
    updateSensor["avty_t"] = availability_topic;
    updateSensor["icon"] = "mdi:cloud-download";
    updateSensor["ent_cat"] = "config";
    updateSensor["dev_cla"] = "firmware";
    updateSensor["dev"] = device;
    

    // light
    // topic: homeassistant/light/pandagarage/light/config
    JsonDocument light;
    light["name"] = "Light";
    light["uniq_id"] = appConfig.name + String("_light");
    light["stat_t"] = mqttBase + "/light/state";
    light["cmd_t"] = mqttBase + "/light/switch";
    light["avty_t"] = availability_topic;
    light["payload_on"] = "ON";
    light["payload_off"] = "OFF";
    light["state_on"] = "ON";
    light["state_off"] = "OFF";
    light["brightness"] = false;
    light["dev_cla"] = "light";
    light["dev"] = device;


    // button vent
    // topic: homeassistant/button/pandagarage/vent/config
    JsonDocument vent;
    vent["name"] = "Vent Position";
    vent["uniq_id"] = appConfig.name + String("_vent");
    vent["avty_t"] = availability_topic;
    vent["cmd_t"] = mqttBase + "/vent/set";
    vent["dev"] = device;


    // button half
    // topic: homeassistant/button/pandagarage/half/config
    JsonDocument half;
    half["name"] = "Half Position";
    half["uniq_id"] = appConfig.name + String("_half");
    half["avty_t"] = availability_topic;
    half["cmd_t"] = mqttBase + "/half/set";
    half["dev"] = device;


    // button toggle
    // topic: homeassistant/button/pandagarage/toggle/config
    JsonDocument toggle;
    toggle["name"] = "Toggle Door";
    toggle["uniq_id"] = appConfig.name + String("_toggle");
    toggle["avty_t"] = availability_topic;
    toggle["cmd_t"] = mqttBase + "/toggle/set";
    toggle["dev"] = device;


    // cover
    // topic: homeassistant/cover/pandagarage/cover/config
    JsonDocument cover;
    cover["name"] = "Door";
    cover["uniq_id"] = appConfig.name + String("_cover");
    cover["avty_t"] = availability_topic;
    cover["stat_t"] = mqttBase + "/cover/state";
    cover["cmd_t"] = mqttBase + "/cover/set";

    cover["position_topic"] = mqttBase + "/cover/position";
    cover["set_position_topic"] = mqttBase + "/cover/position/set";
    cover["position_open"] = 100;
    cover["position_closed"] = 0;

    cover["payload_open"] = "open";
    cover["payload_close"] = "close";
    cover["payload_stop"] = "stop";

    cover["state_open"] = "open";
    cover["state_opening"] = "opening";
    cover["state_close"] = "closed";
    cover["state_closing"] = "closing";
    cover["state_stopped"] = "stopped";

    cover["dev_cla"] = "garage";
    cover["dev"] = device;


    // external sensor setup
    if (appConfig.externalSensorSet) {
        const uint8_t externalSensorType = appConfig.externalSensor;
        if (externalSensorType == 1) {
            // ToDO: AHT10
            
        } else if (externalSensorType == 2 || externalSensorType == 3) {
            JsonDocument co2Sensor;
            co2Sensor["name"] = "CO2";
            co2Sensor["uniq_id"] = appConfig.name + String("_co2");
            co2Sensor["avty_t"] = availability_topic;            
            co2Sensor["stat_t"] = mqttBase + "/co2/state";
            co2Sensor["dev"] = device;
            co2Sensor["dev_cla"] = "carbon_dioxide";
            co2Sensor["unit_of_meas"] = "ppm";

            String co2Config;
            serializeJson(co2Sensor, co2Config);
            mqttClientHa.publish((String("homeassistant/sensor/") + appConfig.name + String("/co2/config")).c_str(), 0, true, co2Config.c_str());

        } else if (externalSensorType == 4) {
            JsonDocument eco2Sensor;
            eco2Sensor["name"] = "eCO2";
            eco2Sensor["uniq_id"] = appConfig.name + String("_eco2");
            eco2Sensor["avty_t"] = availability_topic;            
            eco2Sensor["stat_t"] = mqttBase + "/eco2/state";
            eco2Sensor["dev"] = device;
            eco2Sensor["dev_cla"] = "volatile_organic_compounds";
            eco2Sensor["unit_of_meas"] = "ppm";

            JsonDocument tvocSensor;
            tvocSensor["name"] = "TVOC";
            tvocSensor["uniq_id"] = appConfig.name + String("_tvoc");
            tvocSensor["avty_t"] = availability_topic;            
            tvocSensor["stat_t"] = mqttBase + "/tvoc/state";
            tvocSensor["dev"] = device;
            tvocSensor["dev_cla"] = "volatile_organic_compounds";
            tvocSensor["unit_of_meas"] = "ppb";

            String eco2Config, tvocConfig;
            serializeJson(eco2Sensor, eco2Config);
            serializeJson(tvocSensor, tvocConfig);
            mqttClientHa.publish((String("homeassistant/sensor/") + appConfig.name + String("/eco2/config")).c_str(), 0, true, eco2Config.c_str());
            mqttClientHa.publish((String("homeassistant/sensor/") + appConfig.name + String("/tvoc/config")).c_str(), 0, true, tvocConfig.c_str());

        } else {
            logger("Unknown external sensor type", "MQTT", LOG_ERROR);
        }
    }

    
    // serialize
    String restartConfig, tempConfig, humidityConfig, pressureConfig, luxConfig, updateConfig, lightConfig, ventConfig, halfConfig, toggleConfig, coverConfig;
    serializeJson(restart, restartConfig);
    serializeJson(tempSensor, tempConfig);
    serializeJson(humiditySensor, humidityConfig);
    serializeJson(pressureSensor, pressureConfig);
    serializeJson(luxSensor, luxConfig);
    serializeJson(updateSensor, updateConfig);
    serializeJson(light, lightConfig);
    serializeJson(vent, ventConfig);
    serializeJson(half, halfConfig);
    serializeJson(toggle, toggleConfig);
    serializeJson(cover, coverConfig);


    // publish
    mqttClientHa.publish((String("homeassistant/button/") + appConfig.name + String("/restart/config")).c_str(), 0, true, restartConfig.c_str());
    mqttClientHa.publish((String("homeassistant/sensor/") + appConfig.name + String("/temp/config")).c_str(), 0, true, tempConfig.c_str());
    mqttClientHa.publish((String("homeassistant/sensor/") + appConfig.name + String("/humidity/config")).c_str(), 0, true, humidityConfig.c_str());
    mqttClientHa.publish((String("homeassistant/sensor/") + appConfig.name + String("/pressure/config")).c_str(), 0, true, pressureConfig.c_str());
    mqttClientHa.publish((String("homeassistant/sensor/") + appConfig.name + String("/lux/config")).c_str(), 0, true, luxConfig.c_str());
    mqttClientHa.publish((String("homeassistant/update/") + appConfig.name + String("/config")).c_str(), 0, true, updateConfig.c_str());
    mqttClientHa.publish((String("homeassistant/light/") + appConfig.name + String("/light/config")).c_str(), 0, true, lightConfig.c_str());
    mqttClientHa.publish((String("homeassistant/button/") + appConfig.name + String("/vent/config")).c_str(), 0, true, ventConfig.c_str());
    mqttClientHa.publish((String("homeassistant/button/") + appConfig.name + String("/half/config")).c_str(), 0, true, halfConfig.c_str());
    mqttClientHa.publish((String("homeassistant/button/") + appConfig.name + String("/toggle/config")).c_str(), 0, true, toggleConfig.c_str());
    mqttClientHa.publish((String("homeassistant/cover/") + appConfig.name + String("/cover/config")).c_str(), 0, true, coverConfig.c_str());

    mqttHaInitState();
}


void mqttHaListen(const char* topic, const char* payload, unsigned int length) {
    const String mqttBase = String("pandagarage/") + String(appConfig.name);

    // restart
    if (strcmp(topic, (mqttBase + "/restart/set").c_str()) == 0) {
        logger("Restart triggered", "MQTT", LOG_INFO);
        delay(10);
        ESP.restart();
        return;
    }

    // light switch
    if (strcmp(topic, (mqttBase + "/light/switch").c_str()) == 0) {
        String payloadStr = String(payload).substring(0, length);
        
        if (payloadStr == "ON") {
            hoermannEngine->turnLight(true);
            loggerAccess("Light turned on", "mqtt");

        } else if (payloadStr == "OFF") {
            hoermannEngine->turnLight(false);
            loggerAccess("Light turned off", "mqtt");
        }
        return;
    }

    // cover command
    if (strcmp(topic, (mqttBase + "/cover/set").c_str()) == 0) {
        String payloadStr = String(payload).substring(0, length);

        if (payloadStr == "open") {
            hoermannEngine->openDoor();
            loggerAccess("Door opened", "mqtt");

        } else if (payloadStr == "close") {
            hoermannEngine->closeDoor();
            loggerAccess("Door closed", "mqtt");

        } else if (payloadStr == "stop") {
            hoermannEngine->stopDoor();
            loggerAccess("Door stopped", "mqtt");
        }
        return;
    }

    // cover position
    if (strcmp(topic, (mqttBase + "/cover/position/set").c_str()) == 0) {
        String payloadStr = String(payload).substring(0, length);

        int position = payloadStr.toInt();
        if (position >= 0 && position <= 100) {
            hoermannEngine->setPosition(position);
            loggerAccess("Door position set to " + String(position), "mqtt");

        } else {
            logger("Invalid cover position command: " + payloadStr, "MQTT", LOG_ERROR);
        }
        return;
    }

    // vent command
    if (strcmp(topic, (mqttBase + "/vent/set").c_str()) == 0) {
        loggerAccess("Door set to vent position", "mqtt");
        hoermannEngine->ventilationPositionDoor();
        return;
    }

    // half command
    if (strcmp(topic, (mqttBase + "/half/set").c_str()) == 0) {
        loggerAccess("Door set to half position", "mqtt");
        hoermannEngine->halfPositionDoor();
        return;
    }

    // toggle command
    if (strcmp(topic, (mqttBase + "/toggle/set").c_str()) == 0) {
        loggerAccess("Door toggled", "mqtt");
        hoermannEngine->toggleDoor();
        return;
    }
}

void onMqttConnect(bool sessionPresent) {
    logger("Connected to Home Assistant", "MQTT", LOG_DEBUG);

    if(!configSent) {
        mqttHaConfig();
        configSent = true;
        logger("Config sent", "MQTT", LOG_DEBUG);
    }

    const String topic = String("pandagarage/") + appConfig.name + String("/#");
    mqttClientHa.subscribe(topic.c_str(), 0);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
    logger("Disconnected from Home Assistant", "MQTT", LOG_WARNING);
    mqttInitState = false;
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
    static char msg[MQTT_PAYLOAD_LEN];
    size_t copyLen = len < MQTT_PAYLOAD_LEN - 1 ? len : MQTT_PAYLOAD_LEN - 1;
    memcpy(msg, payload, copyLen);
    msg[copyLen] = '\0';
    mqttHaListen(topic, msg, copyLen);
}


int mqttHaReconnect() {
    
    // check if wifi is connected
    if (WiFi.status() != WL_CONNECTED) {
        return 0;
    }

    // if mqtt connected, disconnect first
    if(mqttClientHa.connected()) {
        mqttClientHa.disconnect();
    }

    mqttClientHa.connect();

    int waitCount = 0;
    while (!mqttClientHa.connected() && waitCount < 10) {
        delay(500);
        waitCount++;
    }

    if (mqttClientHa.connected()) {
        return 1;
    }

    logger("Failed to connect to HA MQTT", "MQTT", LOG_ERROR);
    return 0;
}

bool mqttHaSetup() {

    if (!appConfig.haSet) {
        logger("Home Assistant not configured!", "MQTT", LOG_WARNING);
        return false;
    }

    mqttClientHa.setServer(appConfig.haIp, appConfig.haPort);
    mqttClientHa.setCredentials(appConfig.haUser, appConfig.haPwd);
    mqttClientHa.onMessage(onMqttMessage);
    mqttClientHa.onConnect(onMqttConnect);
    mqttClientHa.onDisconnect(onMqttDisconnect);

    return true;
}

void mqttHaLoop() {

    if(!appConfig.haSet) {
        return;
    }

    if (!mqttClientHa.connected()) {
        mqttHaReconnect();
    }

    // send init mqtt state
    if (!mqttInitState) {
        mqttHaPublish("/status", "online", true);
        mqttHaPublish("/cover/position", String(hoermannEngine->state->currentPosition).c_str(), true);
        mqttHaPublish("/cover/state", String(hoermannEngine->state->translatedState).c_str(), true);
        mqttHaPublish("/light/state", (hoermannEngine->state->lightOn ? "ON" : "OFF"), true);

        checkForFirmwareUpdate();
        mqttInitState = true;
    }

    // check if firmware update is available - only run once a day
    if (millis() - lastGhUpdateCheck >= GH_UPDATE_INTERVAL) {
        lastGhUpdateCheck = millis();
        checkForFirmwareUpdate();
    }

}


// FreeRTOS task
void mqttTask(void *parameter) {
    MqttMessage msg;
    while (true) {
        mqttHaLoop();

        while (mqttQueue != NULL && xQueueReceive(mqttQueue, &msg, 0) == pdPASS) {
            if (mqttClientHa.connected()) {
                mqttClientHa.publish(msg.topic, 0, msg.retain, msg.payload);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void initMqttTask() {
    if (mqttQueue == NULL) {
        mqttQueue = xQueueCreate(20, sizeof(MqttMessage));
    }
    if (mqttTaskHandle == NULL) {
        xTaskCreatePinnedToCore(mqttTask, "MQTTTask", 8192, NULL, 1, &mqttTaskHandle, 0);
    }
}