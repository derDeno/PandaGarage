/*
 * logging functions including serial and file logging
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <string.h>
#include <time.h>
#include "config.h"

#define LOG_MSG_LEN 128

const size_t MAX_LOG_FILE_SIZE = 50 * 1024;  // 50 KB
extern AppConfig appConfig;

QueueHandle_t logQueue = NULL;
TaskHandle_t logTaskHandle = NULL;

// keep track of log file sizes to avoid checking the filesystem on each log
const TickType_t LOG_TRIM_INTERVAL = pdMS_TO_TICKS(60000);  // periodic trim check
size_t logCsvSize = 0;
size_t logAccessSize = 0;
TickType_t lastTrimCsv = 0;
TickType_t lastTrimAccess = 0;

struct LogMessage {
    const char *fileName;
    time_t timestamp;
    char data[LOG_MSG_LEN];
};

enum LOG_LVL {
    LOG_NONE,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR
};

// escape strings for csv output if needed
String escapeCSVField(const String &field) {
    bool needsQuotes = field.indexOf(';') != -1 ||
                       field.indexOf('"') != -1 ||
                       field.indexOf('\n') != -1;

    if (!needsQuotes) {
        return field;
    }

    String escaped = "\"";  // opening quote

    for (unsigned int i = 0; i < field.length(); i++) {
        char c = field[i];
        if (c == '"') {
            escaped += "\"\"";  // escape " as ""
        } else {
            escaped += c;
        }
    }

    escaped += "\"";  // closing quote
    return escaped;
}


// check the log file size and trim if needed
void checkLogFileSize(const char *fileName) {
    File logFile = LittleFS.open(fileName, "r");
    if (!logFile) {
#if DEBUG
        Serial.println("Failed to open log file for reading");
#endif
        return;
    }

    // Check the current file size
    size_t fileSize = logFile.size();
    logFile.close();

    // If the file exceeds the max size, trim the file
    if (fileSize > MAX_LOG_FILE_SIZE) {
        logFile = LittleFS.open(fileName, "r");
        String newContent;
        size_t bytesToTrim = fileSize - MAX_LOG_FILE_SIZE;

        // Skip older lines until the file is under size limit
        size_t currentSize = 0;
        while (logFile.available()) {
            String line = logFile.readStringUntil('\n');
            currentSize += line.length() + 1;  // +1 for newline character

            // Once we have skipped enough lines to be under the limit, start storing
            if (currentSize > bytesToTrim) {
                newContent += line + "\n";
            }
        }

        logFile.close();

        // Write the new trimmed content back to the file
        logFile = LittleFS.open(fileName, "w");
        if (logFile) {
            logFile.print(newContent);
            logFile.close();
#if DEBUG
            Serial.println("Log file trimmed successfully");
#endif
        } else {
#if DEBUG
            Serial.println("Failed to open log file for writing");
#endif
        }
    }
}


void logTask(void *parameter) {
    LogMessage msg;

    // initialize file size tracking
    File logFile = LittleFS.open("/log.csv", "r");
    if (logFile) {
        logCsvSize = logFile.size();
        logFile.close();
    }
    logFile = LittleFS.open("/log-access.txt", "r");
    if (logFile) {
        logAccessSize = logFile.size();
        logFile.close();
    }
    lastTrimCsv = xTaskGetTickCount();
    lastTrimAccess = lastTrimCsv;

    while (true) {
        if (xQueueReceive(logQueue, &msg, portMAX_DELAY) == pdPASS) {
            struct tm timeinfo_local;
            char timeStringBuff[25];
            if (localtime_r(&msg.timestamp, &timeinfo_local)) {
                strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo_local);
            } else {
                strncpy(timeStringBuff, "1970-01-01 00:00:00", sizeof(timeStringBuff));
                timeStringBuff[sizeof(timeStringBuff) - 1] = '\0';
            }

            String finalMessage;
            if (strcmp(msg.fileName, "/log-access.txt") == 0) {
                finalMessage = String("[") + timeStringBuff + "] - " + msg.data;
            } else {
                finalMessage = String(timeStringBuff) + ";" + msg.data;
            }

            size_t msgLen = finalMessage.length() + 1;  // include newline
            TickType_t now = xTaskGetTickCount();

            size_t *fileSize;
            TickType_t *lastTrim;
            if (strcmp(msg.fileName, "/log-access.txt") == 0) {
                fileSize = &logAccessSize;
                lastTrim = &lastTrimAccess;
            } else {
                fileSize = &logCsvSize;
                lastTrim = &lastTrimCsv;
            }

            if ((*fileSize + msgLen > MAX_LOG_FILE_SIZE) ||
                (now - *lastTrim > LOG_TRIM_INTERVAL)) {
                checkLogFileSize(msg.fileName);
                logFile = LittleFS.open(msg.fileName, "r");
                if (logFile) {
                    *fileSize = logFile.size();
                    logFile.close();
                } else {
                    *fileSize = 0;
                }
                *lastTrim = now;
            }

            logFile = LittleFS.open(msg.fileName, "a");
            if (logFile) {
                logFile.println(finalMessage);
                logFile.close();
                *fileSize += msgLen;
            }
#if DEBUG
            Serial.println(msg.data);
#endif
        }
    }
}


void initLogger() {
    if (logQueue == NULL) {
        logQueue = xQueueCreate(20, sizeof(LogMessage));
        xTaskCreatePinnedToCore(logTask, "LogTask", 4096, NULL, 1, &logTaskHandle, 1);
    }
}

// log data to serial and file
void logger(String logData, String tag = "", LOG_LVL level = LOG_DEBUG) {
    // skip logging if the provided level is lower than the configured log level or the configured is 0
    if (level < appConfig.logLevel || appConfig.logLevel == LOG_NONE) {
        return;
    }

    // create the log in csv format. time;lvl;tag;data
    String escaped = escapeCSVField(logData);
    String message = String(level) + ";" + tag + ";" + escaped;

    // logging set to true so log to file using background task
    if (logQueue != NULL) {
        LogMessage msg;
        msg.fileName = "/log.csv";
        msg.timestamp = time(nullptr);
        strncpy(msg.data, message.c_str(), LOG_MSG_LEN - 1);
        msg.data[LOG_MSG_LEN - 1] = '\0';
        if (xQueueSend(logQueue, &msg, 0) != pdPASS) {
#if DEBUG
            Serial.println("Log queue full, dropping debug log");
#endif
        }
    }
}

// access log to file
void loggerAccess(String logData, String source) {
    // if access logging is false quit
    if (!appConfig.logAccess) {
        return;
    }

    String logMessage = "[" + source + "] - " + logData;
    if (logQueue != NULL) {
        LogMessage msg;
        msg.fileName = "/log-access.txt";
        msg.timestamp = time(nullptr);
        strncpy(msg.data, logMessage.c_str(), LOG_MSG_LEN - 1);
        msg.data[LOG_MSG_LEN - 1] = '\0';
        if (xQueueSend(logQueue, &msg, 0) != pdPASS) {
#if DEBUG
            Serial.println("Log queue full, dropping access log");
#endif
        }
    }
}

// delete log file
void deleteLogFile(const char *fileName) {
    if (LittleFS.exists(fileName)) {
        LittleFS.remove(fileName);
    }
}


class StringPrinter : public Print {
    String &out;

   public:
    StringPrinter(String &s) : out(s) {}
    size_t write(uint8_t c) override {
        out += (char)c;
        return 1;
    }
};
