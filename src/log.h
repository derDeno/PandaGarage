/*
 * logging functions including serial and file logging
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#define LOG_MSG_LEN 128

struct tm timeinfo;
const size_t MAX_LOG_FILE_SIZE = 50 * 1024;  // 50 KB
extern AppConfig appConfig;

QueueHandle_t logQueue = NULL;
TaskHandle_t logTaskHandle = NULL;

struct LogMessage {
    const char *fileName;
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
        Serial.println("Failed to open log file for reading");
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
            Serial.println("Log file trimmed successfully");
        } else {
            Serial.println("Failed to open log file for writing");
        }
    }
}


void logTask(void *parameter) {
    LogMessage msg;
    while (true) {
        if (xQueueReceive(logQueue, &msg, portMAX_DELAY) == pdPASS) {
            checkLogFileSize(msg.fileName);
            File logFile = LittleFS.open(msg.fileName, "a");
            if (logFile) {
                logFile.println(msg.data);
                logFile.close();
            }
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
    // if false, no serial output to reduce load
    if (DEBUG) {
        Serial.println(logData);
    }

    // skip logging if the level is lower than the configured log level
    if (level < appConfig.logLevel) {
        return;
    }

    char timeStringBuff[25];
    if (getLocalTime(&timeinfo, 0)) {
        strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
    } else {
        strncpy(timeStringBuff, "1970-01-01 00:00:00", sizeof(timeStringBuff));
        timeStringBuff[sizeof(timeStringBuff) - 1] = '\0';
    }

    // create the log in csv format. time;lvl;tag;data
    String escaped = escapeCSVField(logData);
    String message = String(timeStringBuff) + ";" + level + ";" + tag + ";" + escaped;

    // logging set to true so log to file using background task
    if (logQueue != NULL) {
        LogMessage msg;
        msg.fileName = "/log.csv";
        strncpy(msg.data, message.c_str(), LOG_MSG_LEN - 1);
        msg.data[LOG_MSG_LEN - 1] = '\0';
        xQueueSend(logQueue, &msg, 0);
    }
}

// access log to file
void loggerAccess(String logData, String source) {
    // if access logging is false quit
    if (!appConfig.logAccess) {
        return;
    }

    char timeStringBuff[25];
    if (getLocalTime(&timeinfo, 0)) {
        strftime(timeStringBuff, sizeof(timeStringBuff), "[%Y-%m-%d %H:%M:%S]", &timeinfo);
    } else {
        strncpy(timeStringBuff, "[1970-01-01 00:00:00]", sizeof(timeStringBuff));
        timeStringBuff[sizeof(timeStringBuff) - 1] = '\0';
    }

    String logMessage = String(timeStringBuff);
    logMessage += " - [" + source + "] - " + logData;
    if (logQueue != NULL) {
        LogMessage msg;
        msg.fileName = "/log-access.txt";
        strncpy(msg.data, logMessage.c_str(), LOG_MSG_LEN - 1);
        msg.data[LOG_MSG_LEN - 1] = '\0';
        xQueueSend(logQueue, &msg, 0);
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
