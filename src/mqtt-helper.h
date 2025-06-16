#ifndef MQTT_HA_H
#define MQTT_HA_H

/*
* Managig MQTT communication with Home Assistant
*/
#include <WiFiClient.h>

extern AppConfig appConfig;

WiFiClient wifiClientHa;
PubSubClient mqttClientHa;
bool configSent = false;
bool mqttInitState = false;


void mqttHaPublish(const char* topic, const char* payload, bool retain) {

    if(!appConfig.haSet || !mqttClientHa.connected()) {
        return;
    }
    
    const String mqttBase = String("pandagarage/") + appConfig.name;
    mqttClientHa.publish((mqttBase + topic).c_str(), payload, retain);
}


void mqttHaInitState() {
    mqttHaPublish("/status", "online", true);

    mqttHaPublish("/temp/state", String(appConfig.temperature).c_str(), true);
    mqttHaPublish("/humidity/state", String(appConfig.humidity).c_str(), true);
    mqttHaPublish("/lux/state", String(appConfig.lux).c_str(), true);
    mqttHaPublish("/light/state", "OFF", true);
    mqttHaPublish("/cover/state", "closed", true);
    mqttHaPublish("/cover/position", "0", true);

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
    tempSensor["name"] = "Garage Temperature";
    tempSensor["uniq_id"] = appConfig.name + String("_temp");
    tempSensor["stat_t"] = mqttBase + "/temp/state";
    tempSensor["avty_t"] = availability_topic;
    tempSensor["unit_of_meas"] = "°C";
    tempSensor["dev_cla"] = "temperature";
    tempSensor["dev"] = device;

    // humidity sensor
    // topic: homeassistant/sensor/pandagarage/humidity/config
    JsonDocument humiditySensor;
    humiditySensor["name"] = "Garage Humidity";
    humiditySensor["uniq_id"] = appConfig.name + String("_humidity");
    humiditySensor["stat_t"] = mqttBase + "/humidity/state";
    humiditySensor["avty_t"] = availability_topic;
    humiditySensor["unit_of_meas"] = "%";
    humiditySensor["dev_cla"] = "humidity";
    humiditySensor["dev"] = device;

    // lux sensor
    // topic: homeassistant/sensor/pandagarage/lux/config
    JsonDocument luxSensor;
    luxSensor["name"] = "Garage Lux";
    luxSensor["uniq_id"] = appConfig.name + String("_lux");
    luxSensor["stat_t"] = mqttBase + "/lux/state";
    luxSensor["avty_t"] = availability_topic;
    luxSensor["unit_of_meas"] = "lx";
    luxSensor["dev_cla"] = "illuminance";
    luxSensor["dev"] = device;

    // update sensor
    // topic: homeassistant/sensor/pandagarage/update/config
    JsonDocument updateSensor;
    luxSensor["name"] = "Update Available";
    luxSensor["uniq_id"] = appConfig.name + String("_update");
    luxSensor["stat_t"] = mqttBase + "/update/state";
    luxSensor["avty_t"] = availability_topic;
    luxSensor["dev_cla"] = "enum";
    luxSensor["dev"] = device;
    

    // light
    // topic: homeassistant/light/pandagarage/light/config
    JsonDocument light;
    light["name"] = "Garage Door Light";
    light["uniq_id"] = appConfig.name + String("_light");
    light["stat_t"] = mqttBase + "/light/state";
    light["cmd_t"] = mqttBase + "/light/switch";
    light["avty_t"] = availability_topic;
    light["payload_on"] = "ON";
    light["payload_off"] = "OFF";
    light["state_on"] = "ON";
    light["state_off"] = "OFF";    
    light["retain"] = false;
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
    cover["name"] = "Garage Door";
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

    
    // serialize
    String restartConfig, tempConfig, humidityConfig, luxConfig, lightConfig, ventConfig, halfConfig, toggleConfig, coverConfig;
    serializeJson(restart, restartConfig);
    serializeJson(tempSensor, tempConfig);
    serializeJson(humiditySensor, humidityConfig);
    serializeJson(luxSensor, luxConfig);
    serializeJson(light, lightConfig);
    serializeJson(vent, ventConfig);
    serializeJson(half, halfConfig);
    serializeJson(toggle, toggleConfig);
    serializeJson(cover, coverConfig);


    // publish
    mqttClientHa.publish((String("homeassistant/button/") + appConfig.name + String("/restart/config")).c_str(), restartConfig.c_str(), true);
    mqttClientHa.publish((String("homeassistant/sensor/") + appConfig.name + String("/temp/config")).c_str(), tempConfig.c_str(), true);
    mqttClientHa.publish((String("homeassistant/sensor/") + appConfig.name + String("/humidity/config")).c_str(), humidityConfig.c_str(), true);
    mqttClientHa.publish((String("homeassistant/sensor/") + appConfig.name + String("/lux/config")).c_str(), luxConfig.c_str(), true);
    mqttClientHa.publish((String("homeassistant/light/") + appConfig.name + String("/light/config")).c_str(), lightConfig.c_str(), true);
    mqttClientHa.publish((String("homeassistant/button/") + appConfig.name + String("/vent/config")).c_str(), ventConfig.c_str(), true);
    mqttClientHa.publish((String("homeassistant/button/") + appConfig.name + String("/half/config")).c_str(), halfConfig.c_str(), true);
    mqttClientHa.publish((String("homeassistant/button/") + appConfig.name + String("/toggle/config")).c_str(), toggleConfig.c_str(), true);
    mqttClientHa.publish((String("homeassistant/cover/") + appConfig.name + String("/cover/config")).c_str(), coverConfig.c_str(), true);

    mqttHaInitState();
}


void mqttHaListen(char* topic, byte* payload, unsigned int length) {
    const String mqttBase = String("pandagarage/") + String(appConfig.name);

    // restart
    if (strcmp(topic, (mqttBase + "/restart/set").c_str()) == 0) {
        logger("Restart triggered", "mqtt");
        delay(10);
        ESP.restart();
        return;
    }

    // light switch
    if (strcmp(topic, (mqttBase + "/light/switch").c_str()) == 0) {
        String payloadStr = String((char*)payload, length);
        
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
        String payloadStr = String((char*)payload, length);

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
        String payloadStr = String((char*)payload, length);

        int position = payloadStr.toInt();
        if (position >= 0 && position <= 100) {
            hoermannEngine->setPosition(position);
            loggerAccess("Door position set to " + String(position), "mqtt");

        } else {
            logger("Invalid cover position command: " + payloadStr, "E");
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


int mqttHaReconnect() {
    
    // check if wifi is connected
    if (WiFi.status() != WL_CONNECTED) {
        return 0;
    }

    // if mqtt connected, disconnect first
    if(mqttClientHa.connected()) {
        mqttClientHa.disconnect();
    }

    int retries = 0;
    while (!mqttClientHa.connected() && retries < 5) {

        if (mqttClientHa.connect(appConfig.name, appConfig.haUser, appConfig.haPwd, (String("pandagarage/") + appConfig.name + "/status").c_str(), 0, true, "offline")) {
            logger("connected to Home Assistant", "MQTT");

            // send config
            if(!configSent) {
                mqttHaConfig();
                configSent = true;
                Serial.println("Config sent");
            }

            // subscribe to topics
            const String topic = String("pandagarage/") + appConfig.name + String("/#");
            mqttClientHa.subscribe(topic.c_str());

            return 1;

        } else {
            logger("HA MQTT failed with state " + mqttClientHa.state(), "E");
            delay(2000);
            retries++;
        }
    }

    logger("Failed to connect to HA MQTT after 5 retries", "E");
    return 0;
}


bool mqttHaSetup() {

    if (!appConfig.haSet) {
        logger("Home Assistant not configured!", "E");
        return false;
    }

    mqttClientHa.setClient(wifiClientHa);
    mqttClientHa.setBufferSize(12000);
    mqttClientHa.setServer(appConfig.haIp, appConfig.haPort);
    mqttClientHa.setCallback(mqttHaListen);
    mqttClientHa.setSocketTimeout(20);

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
        mqttInitState = true;
    }

    mqttClientHa.loop();
}

#endif