#include <Adafruit_Sensor.h>
#include <BH1750.h>
#include "Wire.h"

// external sensors
#include <Adafruit_AHTX0.h>
#include <SensirionI2cScd4x.h>
#include "Adafruit_CCS811.h"
#include <Adafruit_VL6180X.h>

// hardware specific includes
#ifdef HW1
    #include "ClosedCube_HDC1080.h"
    ClosedCube_HDC1080 hdc1080;
#endif

#ifdef HW2
    #include <Adafruit_BME280.h>
    Adafruit_BME280 bme;
#endif

#define BH1750_I2CADDR 0x23
#define BUZZER_CHANNEL 0  // LEDC channel (0–15)
#define BUZZER_FREQ 4000  // Frequency in Hz (2kHz tone)
#define BUZZER_RES 8      // Resolution in bits (8 bits = 0–255)
#define DUTY_CYCLE 128    // 50% duty cycle (128/255)

extern AsyncEventSource events;

BH1750 lightMeter;
Adafruit_AHTX0 aht;
Adafruit_CCS811 ccs;
Adafruit_VL6180X vl;
SensirionI2cScd4x scd4x;

unsigned long lastBuzzerTime = 0;
TaskHandle_t sensorTaskHandle = NULL;
bool baseSensorAvailable = true;
bool externalSensorAvailable = false;
void sensorTask(void *parameter);
void initSensorTask();
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
        logger("Playing beep tune", "Device", LOG_INFO);

        int melody[] = {880, 880};
        int duration[] = {250, 250};
        playback(melody, duration, 2);

    } else if (tune == 2) {
        // alarm
        logger("Playing alarm tune", "Device", LOG_INFO);

        // A5, F5, C5
        int melody[] = {880, 698, 523};
        int duration[] = {150, 150, 300};
        playback(melody, duration, 3);

    } else if (tune == 3) {
        // melody
        logger("Playing melody tune", "Device", LOG_INFO);

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
        logger("No tune to play", "Device", LOG_INFO);
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

    baseSensorAvailable = true;
    externalSensorAvailable = !appConfig.externalSensorSet;

    #ifdef HW1
        hdc1080.begin(0x40);
    #endif

    #ifdef HW2
        if (!bme.begin(0x76, &Wire)) {
            logger("Could not find a valid BME280 sensor, check wiring!", "Sensor", LOG_ERROR);
            baseSensorAvailable = false;
        }
    #endif

    lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, BH1750_I2CADDR, &Wire);

    if (appConfig.externalSensorSet) {
        if (appConfig.externalSensor == 1) { // AHT10
            externalSensorAvailable = aht.begin();
            if (!externalSensorAvailable) {
                logger("Failed to start AHT10 sensor, continuing without it.", "Sensor", LOG_ERROR);
            }

        } else if (appConfig.externalSensor == 2) { // SCD40
            scd4x.begin(Wire, SCD40_I2C_ADDR_62);
            scd4x.wakeUp();
            scd4x.stopPeriodicMeasurement();
            scd4x.reinit();
            scd4x.startPeriodicMeasurement();
            externalSensorAvailable = true;

        } else if (appConfig.externalSensor == 3) { // SCD41
            scd4x.begin(Wire, SCD41_I2C_ADDR_62);
            scd4x.wakeUp();
            scd4x.stopPeriodicMeasurement();
            scd4x.reinit();
            scd4x.startPeriodicMeasurement();
            externalSensorAvailable = true;

        } else if (appConfig.externalSensor == 4) { // CCS811
            if(!ccs.begin()){
                logger("Failed to start CCS811 sensor, continuing without it.", "Sensor", LOG_ERROR);
            } else {
                externalSensorAvailable = true;
                unsigned long startedAt = millis();
                while (!ccs.available() && millis() - startedAt < 2000) {
                    delay(10);
                }

                if (ccs.available()) {
                    float temp = ccs.calculateTemperature();
                    ccs.setTempOffset(temp - 25.0);
                } else {
                    logger("CCS811 did not become ready in time, skipping calibration.", "Sensor", LOG_WARNING);
                }
            }

        } else if (appConfig.externalSensor == 5) { // VL6180X
            vl = Adafruit_VL6180X();

            if (!vl.begin()) {
                logger("Failed to boot VL6180X, continuing without it.", "Sensor", LOG_ERROR);
            } else {
                externalSensorAvailable = true;
                vl.setAddress(0x30);
                delay(10);
            }
        } else {
            logger("Unknown external sensor type configured, continuing without it.", "Sensor", LOG_WARNING);
        }
    }

    if (!externalSensorAvailable) {
        setExtSensorData("{}");
    }
}

void sensorLoop() {
    float temp = appConfig.temperature;
    float humid = appConfig.humidity;
    float pressure = appConfig.pressure;

    #ifdef HW1
        if (baseSensorAvailable) {
            temp = hdc1080.readTemperature();
            humid = hdc1080.readHumidity();
            pressure = 0;
        }
    #endif

    #ifdef HW2
        if (baseSensorAvailable) {
            temp = bme.readTemperature();
            humid = bme.readHumidity();
            pressure = bme.readPressure() / 100.0F; // convert to hPa
        }
    #endif

    // convert to fahrenheit if set so
    bool isCelsius = (appConfig.tempUnit == 0) ? true : false;
    if (!isCelsius) {
        temp = (temp * 9.0 / 5.0) + 32.0;
    }

    float lux = lightMeter.readLightLevel();


    // external sensors
    if (appConfig.externalSensorSet && externalSensorAvailable) {
        if (appConfig.externalSensor == 1) {  // AHT10
            
            sensors_event_t humidity, temperature;
            aht.getEvent(&humidity, &temperature);

            if (!isCelsius) {
                temperature.temperature = (temperature.temperature * 9.0 / 5.0) + 32.0;
            }

            if (appConfig.combineSensors) {
                temp = (temp + temperature.temperature) / 2.0;
                humid = (humid + humidity.relative_humidity) / 2.0;
            }

            // convert sensor data to JSON string
            JsonDocument doc;
            doc["temperature"] = temperature.temperature;
            doc["humidity"] = humidity.relative_humidity;
            String extSensorJson;
            serializeJson(doc, extSensorJson);
            setExtSensorData(extSensorJson);

        } else if (appConfig.externalSensor == 2 || appConfig.externalSensor == 3) { // SCD40 or SCD41
            
            uint16_t co2;
            float temperature, humidity;
            scd4x.readMeasurement(co2, temperature, humidity);

            if (co2 > 8000) {
                // uncalibrated reading, therefore invalid
                return;
            }

            if (!isCelsius) {
                temperature = (temperature * 9.0 / 5.0) + 32.0;
            }

            if (appConfig.combineSensors) {
                temp = (temp + temperature) / 2.0;
                humid = (humid + humidity) / 2.0;
            }

            // convert sensor data to JSON string
            JsonDocument doc;
            doc["co2"] = co2;
            doc["temperature"] = temperature;
            doc["humidity"] = humidity;
            String extSensorJson;
            serializeJson(doc, extSensorJson);
            setExtSensorData(extSensorJson);

            // mqtt update
            String payload = String(co2);
            mqttHaPublish("/co2/state", payload.c_str(), true);

        } else if (appConfig.externalSensor == 4) { // CCS811

            if (ccs.available()) {
                float temperature = ccs.calculateTemperature();

                if (appConfig.combineSensors) {
                    temp = (temp + temperature) / 2.0;
                }

                if (!ccs.readData()) {
                    float eCO2 = ccs.geteCO2();
                    float TVOC = ccs.getTVOC();

                    // convert sensor data to JSON string
                    JsonDocument doc;
                    doc["eco2"] = eCO2;
                    doc["tvoc"] = TVOC;
                    doc["temperature"] = temperature;
                    String extSensorJson;
                    serializeJson(doc, extSensorJson);
                    setExtSensorData(extSensorJson);

                    // mqtt update
                    String payload = String(eCO2);
                    mqttHaPublish("/eco2/state", payload.c_str(), true);

                    String payload2 = String(TVOC);
                    mqttHaPublish("/tvoc/state", payload2.c_str(), true);
                }
            }
        } else if (appConfig.externalSensor == 5) { // VL6180X

            float vlLux = vl.readLux(VL6180X_ALS_GAIN_5);
            uint8_t range = vl.readRange();
            uint8_t status = vl.readRangeStatus();

            if (appConfig.combineSensors) {
                lux = (lux + vlLux) / 2.0;
            }

            JsonDocument doc;
            doc["lux"] = vlLux;
            doc["range"] = range;
            String extSensorJson;
            serializeJson(doc, extSensorJson);
            setExtSensorData(extSensorJson);

            // mqtt update
            //String payload = String(range);
            //mqttHaPublish("/vl_range/state", payload.c_str(), true);
        }
    }


    // check if value change is above threshold
    bool tempChanged = fabs(temp - appConfig.temperature) >= appConfig.tempThreshold;
    bool humidityChanged = fabs(humid - appConfig.humidity) >= appConfig.humThreshold;
    bool pressureChanged = fabs(pressure - appConfig.pressure) >= appConfig.presThreshold;
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

    if (pressureChanged) {
        appConfig.pressure = pressure;

        // push mqtt update
        String payload = String(pressure, 2);
        mqttHaPublish("/pressure/state", payload.c_str(), true);

        // push SSE update
        JsonDocument sensor;
        sensor["pressure"] = payload;

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
    
}

void sensorTask(void *parameter) {
    while (true) {
        sensorLoop();
        vTaskDelay(pdMS_TO_TICKS(appConfig.sensorUpdateInterval));
    }
}

void initSensorTask() {
    if (sensorTaskHandle == NULL) {
        xTaskCreatePinnedToCore(
            sensorTask,
            "SensorTask",
            10000,
            NULL,
            configMAX_PRIORITIES,
            &sensorTaskHandle,
            0
        );
    }
}

// parse restart reason to string
String restartReasonString(int reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "Power on reset";
        case ESP_RST_EXT:
            return "EXternal reset";
        case ESP_RST_SW:
            return "Software reset";
        case ESP_RST_PANIC:
            return "Panic reset";
        case ESP_RST_INT_WDT:
            return "Interrupt Watchdog reset";
        case ESP_RST_TASK_WDT:
            return "Task watchdog timeout";
        case ESP_RST_WDT:
            return "Other watchdog reset";
        case ESP_RST_BROWNOUT:
            return "Brownout (voltage drop)";
        default:
            return "Unknown reason";
    }
}
