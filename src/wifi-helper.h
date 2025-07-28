#ifndef WIFI_HELPER_H
#define WIFI_HELPER_H

/*
* Managing and handling of WiFi and Network related tasks
*/

#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <DNSServer.h>


extern AppConfig appConfig;
extern AsyncWebServer server;
extern Preferences pref;

AsyncEventSource sseScan("/api/network-scan");
AsyncEventSource sseTest("/api/network-test");
DNSServer dnsServer;
IPAddress apIP(192, 1, 1, 1);

static unsigned long lastAttemptTime = 0;
const unsigned long reconnectInterval = 5000;
unsigned long reconnectDelay = 1000;
const unsigned long maxReconnectDelay = 60000;
uint8_t connectionAttempts = 0;
const uint8_t MAX_CONNECTION_ATTEMPTS = 5;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 7000;

bool scanInProgress = false;
bool scanRequested  = false;
bool testRequested  = false;

std::vector<AsyncEventSourceClient*> scanClients;
std::vector<AsyncEventSourceClient*> testClients;



// setup mDNS
void setupMDNS() {
    const char* preferredHostname = "pandagarage";
    String hostname = String(appConfig.name);
    hostname.toLowerCase();

    // Initialize mDNS with the fallback hostname
    if (!MDNS.begin(hostname)) {
        logger("Failed to start mDNS with fallback hostname!", "WiFi", LOG_ERROR);
        return;
    }
    delay(100);  // Allow mDNS to stabilize

    // Check if the preferred hostname is available
    if (static_cast<int>(MDNS.queryHost(preferredHostname)) == 0) {
        if (MDNS.begin(preferredHostname)) {
            hostname = preferredHostname;
        }
    }

    MDNS.addService("http", "tcp", 80);
    logger("Hostname set to http://" + hostname +".local", "BOOT", LOG_INFO);
}


// setup WiFi
void setupWifi() {

    connectionAttempts = 0;

    // Connect to Wi-Fi network
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.setHostname(appConfig.name);
    WiFi.setSleep(false);
    WiFi.begin(appConfig.wifiSsid, appConfig.wifiPwd);
    logger("Connecting to WiFi...", "BOOT", LOG_INFO);

    // Wait for connection with timeout
    auto status = WiFi.waitForConnectResult(WIFI_CONNECT_TIMEOUT_MS);

    if (status != WL_CONNECTED) {
        logger("WiFi connection failed!", "WiFi", LOG_ERROR);
        connectionAttempts = MAX_CONNECTION_ATTEMPTS;
        return;
    }

    // mDNS
    setupMDNS();

    // NTP
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    logger("NTP (UTC): ok", "BOOT", LOG_INFO);
    logger("WiFI: ok", "BOOT", LOG_INFO);
    logger("IP: " + WiFi.localIP().toString(), "BOOT", LOG_INFO);
}



// setup in AP Mode if no WiFi set
void setupWifiAp() {
    WiFi.mode(WIFI_AP_STA);
    //WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("PandaGarage");    

    dnsServer.start(53, "*", WiFi.softAPIP());

    sseScan.onConnect([](AsyncEventSourceClient *client) {
        scanClients.push_back(client);
        client->send("Scanning...", "status");
        scanRequested = true;
    });

    sseTest.onConnect([](AsyncEventSourceClient *client) {
        testClients.push_back(client);
        client->send("Testing...", "status");
        testRequested = true;
    });


    sseScan.onDisconnect([](AsyncEventSourceClient* client) {
        auto it = std::find(scanClients.begin(), scanClients.end(), client);
        if (it != scanClients.end()) {
            scanClients.erase(it);
        }
    });

    sseTest.onDisconnect([](AsyncEventSourceClient* client) {
        auto it = std::find(testClients.begin(), testClients.end(), client);
        if (it != testClients.end()) {
            testClients.erase(it);
        }
    });


    server.addHandler(&sseScan);
    server.addHandler(&sseTest);
}

void startScan() {
    if (!scanInProgress) {
        scanInProgress = true;
        WiFi.scanDelete();
        WiFi.scanNetworks(true);
    }
}

// Called when scanComplete() ≥ 0
void deliverScanResults(int n) {
    for (auto client : scanClients) {
        for (int i = 0; i < n; i++) {
            JsonDocument doc;
            doc["ssid"] = WiFi.SSID(i);
            doc["rssi"] = WiFi.RSSI(i);
            doc["bssid"] = WiFi.BSSIDstr(i);
            doc["channel"] = WiFi.channel(i);
            doc["encryption"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "Open" : "Secured";

            String jsonMessage;
            serializeJson(doc, jsonMessage);

            client->send(jsonMessage.c_str(), "network-found", millis());
        }
        client->send(String(n).c_str(), "scan-finished", millis());
    }
    scanClients.clear();
    scanInProgress = false;
}



// test wifi connection when in AP mode
void deliverTestResults(bool result) {
    for (auto client : testClients) {
        
        if (result) {
            client->send("{\"status\":\"connected\"}", "network-test-result", millis());
        } else {
            client->send("{\"status\":\"failed\"}", "network-test-result", millis());
        }
    }

    testClients.clear();
}

void WiFiEvent(WiFiEvent_t event) {
    switch(event) {
      case SYSTEM_EVENT_STA_GOT_IP:
        logger("WiFi connection successful!", "WiFi", LOG_INFO);
        deliverTestResults(true);

        
        // save the credentials
        pref.begin("wifi");
        pref.putString("ssid", appConfig.wifiSsidTest);
        pref.putString("pwd", appConfig.wifiPwdTest);
        pref.putBool("set", true);
        pref.end();


        testRequested  = false;
        break;

      case SYSTEM_EVENT_STA_DISCONNECTED:
        logger("WiFi connection failed!", "WiFi", LOG_ERROR);
        deliverTestResults(false);
        testRequested  = false;
        break;

      default:
        logger("Event: " + String(event), "WiFi", LOG_DEBUG);
        break;
    }
}

void testWifiStaConnection() {
    
    logger("Testing credentials...", "WiFi", LOG_INFO);

    WiFi.begin(appConfig.wifiSsidTest, appConfig.wifiPwdTest);
    WiFi.onEvent(WiFiEvent);
}




// loop for WiFi
void wifiLoop() {

    // check wifi connection
    if (appConfig.wifiSet) {
        if (WiFi.status() != WL_CONNECTED) {
            unsigned long currentTime = millis();

            if (currentTime - lastAttemptTime > reconnectDelay) {
                lastAttemptTime = currentTime;
                connectionAttempts++;
                
                if (connectionAttempts < MAX_CONNECTION_ATTEMPTS) {
                    logger("Attempting WiFi reconnection...", "WiFi", LOG_DEBUG);
                    WiFi.disconnect();
                    WiFi.reconnect();
                    
                } else {
                    logger("Max connection attempts reached. Switching to AP mode.", "WiFi", LOG_ERROR);
                    WiFi.disconnect();
                    setupWifiAp();
                }
                
                // Increase backoff delay for next attempt
                reconnectDelay = min(reconnectDelay * 2, maxReconnectDelay);
            }

            if (millis() - lastAttemptTime > reconnectInterval) {
                lastAttemptTime = millis();
                logger("Lost WiFi connection. Attempting reconnect...", "WiFi", LOG_WARNING);
                WiFi.disconnect();
                WiFi.reconnect();
            }
        }
    } else {
        dnsServer.processNextRequest();
    }


    // WiFi scan request
    if (scanRequested) {
        startScan();
        scanRequested = false;
    }
    
    // Poll for scan completion
    if (scanInProgress) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            deliverScanResults(n);
        }
    }

    // WiFi test request
    if (testRequested) {
        testWifiStaConnection();
        testRequested = false;
    }
}

#endif