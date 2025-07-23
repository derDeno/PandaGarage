/*
* App Config holds all relevant config values for the application.
*/

// System values
#define VERSION "0.3.0"
#define RS_TXD 17
#define RS_RXD 18
#define RS_EN 16
#define I2C_SDA 11
#define I2C_SCL 12
#define BUZZER_PIN 3
#define BTN_PIN 0
#define DEBUG true // set this to true if you want serial output. false to reduce load in production

// Default Pref values
#define PREF_TEMP_UNIT 0 // 0 = Celsius, 1 = Fahrenheit
#define PREF_EXTERNAL_SENSOR false
#define PREF_EXTERNAL_SENSOR_TYPE 0 // 0 = none, 1 = AHT10, 2 = SCD40, 3 = SCD41, 4 = CCS811, 5 = VL6180X
#define PREF_COMBINE_SENSORS false

#define PREF_THRESHOLD_TEMP 0.5
#define PREF_THRESHOLD_HUM 1.0
#define PREF_THRESHOLD_PRES 1.0
#define PREF_THRESHOLD_LUX 10.0

#define PREF_SENSOR_UPDATE_INTERVAL 10000 // 10 seconds

#define PREF_BUZZER_SET false 
#define PREF_BUZZER_TUNE 0 // 0 = none, 1 = beep, 2 = alarm, 3 = melody
#define PREF_BUZZER_OPENING false
#define PREF_BUZZER_CLOSING false

#define PREF_LOG_ACCESS false
#define PREF_LOG_LVL 0 // 0 = none, 1 = debug, 2 = info, 3 = warning, 4 = error

#define PREF_HA false
#define PREF_USE_AUTH true
#define PREF_ADMIN_PASSWORD "admin"


/**
 * Application Configuration
 */
struct AppConfig {

    // system config
    char versionFs[13];         // version of the filesystem
    String serialNumber;        // serial number
    String hwRev;               // hardware revision
    char latestFw[64];          // latest firmware version


    // device config
    char name[65];              // device name
    char lang[3];               // device language
    uint8_t tempUnit;           // temperature unit (0 = Celsius, 1 = Fahrenheit)
    bool externalSensorSet;     // is external sensor enabled
    uint8_t externalSensor;     // external sensor (0 = none, 1 = AHT10, 2 = SCD40, 3 = SCD41, 4 = CCS811)
    bool buzzerSet;             // is buzzer enabled
    uint8_t buzzerTune;         // buzzer tune (0 = none, 1 = beep, 2 = alarm, 3 = melody)
    bool buzzerOpening;         // is buzzer opening enabled
    bool buzzerClosing;         // is buzzer closing enabled
    bool logAccess;             // logging active
    uint8_t logLevel;           // logging level (0 = debug, 1 = info, 2 = warning, 3 = error)


    // security settings
    bool useAuth;               // is authentication enabled
    char adminPwd[65];          // admin password
    bool setupDone;             // is setup done


    // WiFi config
    bool wifiSet;               // Is WiFi config present
    char wifiSsid[33];          // WiFi ssid
    char wifiPwd[64];           // WiFi password
    char wifiSsidTest[33];      // WiFi ssid for testing
    char wifiPwdTest[64];       // WiFi password for testing


    // Home Assistant config
    bool haSet;                 // Is HA config present
    char haIp[16];              // HA mqtt ip
    uint16_t haPort;            // HA mqtt port
    char haUser[33];            // HA mqtt user
    char haPwd[64];             // HA mqtt password


    // sensor values
    uint32_t sensorUpdateInterval;  // sensor update interval in ms
    float temperature;              // temperature
    float humidity;                 // humidity
    float pressure;                 // pressure in hPa
    float lux;                      // lux
    String extSensorData;           // external sensor data (JSON string)
    float tempThreshold;            // temperature threshold for changes
    float humThreshold;             // humidity threshold for changes
    float presThreshold;            // pressure threshold for changes
    float luxThreshold;             // lux threshold for changes
    bool combineSensors;            // combine sensors of same type (e.g. AHT10 and BME280) to average values
};