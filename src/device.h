#include <HDC1080JS.h>
#include <BH1750.h>
#include "Wire.h"

// external sensors
#include <Adafruit_AHTX0.h>
#include <SensirionI2cScd4x.h>
#include <ccs811.h>

#define BUZZER_CHANNEL 0  // LEDC channel (0–15)
#define BUZZER_FREQ 4000  // Frequency in Hz (2kHz tone)
#define BUZZER_RES 8      // Resolution in bits (8 bits = 0–255)
#define DUTY_CYCLE 128    // 50% duty cycle (128/255)

HDC1080JS hdc;
BH1750 lightMeter;

Adafruit_AHTX0 aht;
SensirionI2cScd4x scd4x;
CCS811 ccs811;

unsigned long lastSensorReadTime = 0;
unsigned long lastBuzzerTime = 0;
unsigned long tuneDuration(int tune) {
    if (tune == 1) {
        return 600;
    } else if (tune == 2) {
        return 750;
    } else if (tune == 3) {
        return 5100;
    }
    return 0;
}

// declare it here for the compiler
void mqttHaPublish(const char* topic, const char* payload, bool retain = true);

void playback(int* melody, int* duration, int length) {
    for (int i = 0; i < length; i++) {
        ledcWriteTone(BUZZER_CHANNEL, melody[i]);
        ledcWrite(BUZZER_CHANNEL, DUTY_CYCLE);
        delay(duration[i]);
        ledcWrite(BUZZER_CHANNEL, 0);
        delay(50);
    }
}

void playTune(int tune) {
    // 0 = none, 1 = beep, 2 = alarm, 3 = melody, 99 = startup
    if (tune == 1) {
        // beep
        logger("Playing beep tune", "Device");

        int melody[] = {880, 880};
        int duration[] = {250, 250};
        playback(melody, duration, 2);

    } else if (tune == 2) {
        // alarm
        logger("Playing alarm tune", "Device");

        // A5, F5, C5
        int melody[] = {880, 698, 523};
        int duration[] = {150, 150, 300};
        playback(melody, duration, 3);

    } else if (tune == 3) {
        // melody
        logger("Playing melody tune", "Device");

        int melody[] = {659, 698, 784, 0, 784, 880, 988, 0, 988, 1046, 1175, 0, 1318, 1175, 1046, 0, 988, 784, 880, 0, 659, 784, 523};
        int duration[] = {150, 150, 300, 100, 150, 150, 300, 100, 150, 150, 300, 100, 150, 150, 300, 100, 150, 150, 300, 100, 400, 200, 600};
        playback(melody, duration, 23);

    } else if (tune == 99) {
        // C5, E5, G5, C6
        int melody[] = {523, 659, 783, 1046};
        int duration[] = {200, 200, 200, 400};
        playback(melody, duration, 4);

    } else {
        // no tune
        logger("No tune to play", "Device");
    }
}

void setupBuzzer() {
    ledcSetup(BUZZER_CHANNEL, BUZZER_FREQ, BUZZER_RES);
    ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);

    // play startup tune
    playTune(99);
}

void buzzerLoop() {
    if (!appConfig.buzzerSet) {
        return;
    }

    unsigned long now = millis();
    bool play = false;

    if (appConfig.buzzerOpening && hoermannEngine->state->state == HoermannState::State::OPENING) {
        play = true;
    } else if (appConfig.buzzerClosing && hoermannEngine->state->state == HoermannState::State::CLOSING) {
        play = true;
    } else {
        lastBuzzerTime = 0;
    }

    if (play && (now - lastBuzzerTime >= tuneDuration(appConfig.buzzerTune))) {
        playTune(appConfig.buzzerTune);
        lastBuzzerTime = now;
    }
}

void setupSensors() {
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    delay(200);

    hdc = HDC1080JS();
    hdc.config();

    lightMeter.begin();

    if (appConfig.externalSensorSet) {
        if (appConfig.externalSensor == 1) {
            // AHT10
            aht.begin();

        } else if (appConfig.externalSensor == 2) {
            // SCD40
            scd4x.begin(Wire, SCD40_I2C_ADDR_62);
            scd4x.wakeUp();
            scd4x.stopPeriodicMeasurement();
            scd4x.reinit();
            scd4x.startPeriodicMeasurement();

        } else if (appConfig.externalSensor == 3) {
            // SCD41
            scd4x.begin(Wire, SCD41_I2C_ADDR_62);
            scd4x.wakeUp();
            scd4x.stopPeriodicMeasurement();
            scd4x.reinit();
            scd4x.startPeriodicMeasurement();

        } else if (appConfig.externalSensor == 4) {
            // CCS811
            ccs811.begin();
            ccs811.start(CCS811_MODE_1SEC);
        }
    }
}

void sensorLoop() {
    unsigned long currentTime = millis();

    if (currentTime - lastSensorReadTime >= appConfig.sensorUpdateInterval) {
        lastSensorReadTime = currentTime;

        // for temp get unit
        hdc.readTempHumid();
        bool isCelsius = (appConfig.tempUnit == 0) ? true : false;
        float temp = hdc.getTemp();
        float humid = hdc.getRelativeHumidity();

        // convert to fahrenheit if set so
        if (!isCelsius) {
            temp = (temp * 9.0 / 5.0) + 32.0;
        }

        float lux = 0;
        // float lux = lightMeter.readLightLevel();

        bool tempChanged = fabs(temp - appConfig.temperature) >= appConfig.tempThreshold;
        bool humidityChanged = fabs(humid - appConfig.humidity) >= appConfig.humThreshold;
        bool luxChanged = fabs(lux - appConfig.lux) >= appConfig.luxThreshold;

        // check if any sensor value changed
        if (tempChanged) {
            appConfig.temperature = temp;

            // push mqtt update
            String payload = String(temp, 2);
            mqttHaPublish("/temp/state", payload.c_str(), true);

            // push SSE update
            JsonDocument sensor;
            sensor["temperature"] = payload;

            JsonDocument doc;
            doc["sensor"] = sensor;

            String response;
            serializeJson(doc, response);
            events.send(response.c_str(), "door", millis());
        }

        if (humidityChanged) {
            appConfig.humidity = humid;

            // push mqtt update
            String payload = String(humid, 2);
            mqttHaPublish("/humidity/state", payload.c_str(), true);

            // push SSE update
            JsonDocument sensor;
            sensor["humidity"] = payload;

            JsonDocument doc;
            doc["sensor"] = sensor;

            String response;
            serializeJson(doc, response);
            events.send(response.c_str(), "door", millis());
        }

        if (luxChanged) {
            appConfig.lux = lux;

            // push mqtt update
            String payload = String(lux, 2);
            mqttHaPublish("/lux/state", payload.c_str(), true);

            // push SSE update
            JsonDocument sensor;
            sensor["lux"] = payload;

            JsonDocument doc;
            doc["sensor"] = sensor;

            String response;
            serializeJson(doc, response);
            events.send(response.c_str(), "door", millis());
        }

        // external sensors
        if (appConfig.externalSensorSet) {
            if (appConfig.externalSensor == 1) {
                // AHT10
                sensors_event_t humidity, temperature;
                aht.getEvent(&humidity, &temperature);

                if (!isCelsius) {
                    temperature.temperature = (temperature.temperature * 9.0 / 5.0) + 32.0;
                }

                // convert sensor data to JSON string
                appConfig.extSensorData = "{\"temperature\":" + String(temperature.temperature, 2) +
                                          ",\"humidity\":" + String(humidity.relative_humidity, 2) + "}";

            } else if (appConfig.externalSensor == 2 || appConfig.externalSensor == 3) {
                // SCD40 or SCD41
                uint16_t co2;
                float temperature, humidity;
                scd4x.readMeasurement(co2, temperature, humidity);

                if (!isCelsius) {
                    temperature = (temperature * 9.0 / 5.0) + 32.0;
                }

                // convert sensor data to JSON string
                appConfig.extSensorData = "{\"co2\":" + String(co2) +
                                          ",\"temperature\":" + String(temperature, 2) +
                                          ",\"humidity\":" + String(humidity, 2) + "}";

            } else if (appConfig.externalSensor == 4) {
                // CCS811
                uint16_t eco2, etvoc, errstat, raw;
                ccs811.read(&eco2, &etvoc, &errstat, &raw);

                // convert sensor data to JSON string
                appConfig.extSensorData = "{\"eco2\":" + String(eco2) +
                                          ",\"etvoc\":" + String(etvoc) +
                                          ",\"errstat\":" + String(errstat) +
                                          ",\"raw\":" + String(raw) + "}";
            }
        }
    }
}

