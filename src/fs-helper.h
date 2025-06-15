#ifndef FS_HELPER_H
#define FS_HELPER_H

#include <LittleFS.h>
#include <esp_efuse.h>
#include <esp_partition.h>
#include <esp_core_dump.h>

/**
 * Read the version from the filesystem
 * @param versionBuffer     Buffer to store the version
 * @param bufferSize        Size of the buffer
 */
void readFsVersion(char* versionBuffer, size_t bufferSize) {
    File versionFile = LittleFS.open("/version.txt", "r");
    if (!versionFile) {
        Serial.println("Failed to open FS version file");
        versionBuffer[0] = '\0';
        return;
    }

    // Read characters until newline or end of file
    size_t index = 0;
    while (index < bufferSize - 1 && versionFile.available()) {
        char c = versionFile.read();
        if (c == '\n' || c == '\r') {
            break;
        }
        versionBuffer[index++] = c;
    }

    versionBuffer[index] = '\0';
    versionFile.close();
}

/**
 * Initialize the filesystem
 */
void initFs() {
    if (!LittleFS.begin()) {
        Serial.write("LittleFS mount failed, formatting... \n");
        LittleFS.format();

    } else if (!LittleFS.exists("/version.txt")) {
        Serial.println("Version file missing");

    } else {
        Serial.println("Filesystem mounted.");
    }
}


void getEfuseData(char*& serial, char*& revision) {
    static char serialBuf[32];
    static char revisionBuf[16];

    // Define a list of pointers to field descriptors
    static const esp_efuse_desc_t CUSTOM_BLOCK3_FIELD_ENTRY = {
        .efuse_block = (esp_efuse_block_t)3,
        .bit_start = 0,
        .bit_count = 136};

    static const esp_efuse_desc_t* CUSTOM_BLOCK3_FIELD[] = {
        &CUSTOM_BLOCK3_FIELD_ENTRY,
        NULL  // terminator
    };

    uint8_t raw[17] = {0};
    esp_err_t err = esp_efuse_read_field_blob(CUSTOM_BLOCK3_FIELD, raw, 136);
    if (err != ESP_OK) {
        Serial.println("eFuse: Failed to read: " + String(esp_err_to_name(err)));
        serial = (char*)"n/a";
        revision = (char*)"n/a";
        return;
    }

    char efuseString[18];
    memcpy(efuseString, raw, 17);
    efuseString[17] = '\0';

    char* delimiter = strchr(efuseString, '|');
    if (delimiter) {
        *delimiter = '\0';
        strncpy(serialBuf, efuseString, sizeof(serialBuf) - 1);
        strncpy(revisionBuf, delimiter + 1, sizeof(revisionBuf) - 1);
        serial = serialBuf;
        revision = revisionBuf;
    } else {
        serial = (char*)"n/a";
        revision = (char*)"n/a";
    }
}

#endif