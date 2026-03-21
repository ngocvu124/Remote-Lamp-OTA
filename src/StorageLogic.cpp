#include "StorageLogic.h"
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

StorageLogic storage;
extern SdFs sd_bg; 

void StorageLogic::begin() {
    // Luôn bắt đầu bằng cách thử Mount thẻ với config SHARED_SPI
    if (sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(10)))) {
        isReady = true;
        loadFiles();
    } else {
        isReady = false;
    }
}

void StorageLogic::loadFiles() {
    fileCount = 0;
    
    // CÚ CHỐT 2: Luôn mở lại thư mục gốc để làm mới danh sách
    FsFile dir = sd_bg.open("/");
    if (!dir) {
        // Nếu mở thất bại, thử Mount lại một phát nữa cho chắc
        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(10)));
        dir = sd_bg.open("/");
        if (!dir) { isReady = false; return; }
    }
    isReady = true;

    // CÚ CHỐT 3: Quan trọng nhất - Đưa con trỏ quét file về đầu thẻ nhớ
    dir.rewind();

    FsFile file;
    while (file.openNext(&dir, O_READ)) {
        if (!file.isHidden() && !file.isDir() && fileCount < 14) {
            file.getName(fileNames[fileCount], 32);
            fileCount++;
        }
        file.close();
    }
    dir.close();
}

char* StorageLogic::readFileToPSRAM(const char* filename) {
    if (!isReady) return NULL;

    char path[40];
    sprintf(path, "/%s", filename);
    FsFile file = sd_bg.open(path, O_READ);
    if (!file) return NULL;

    if (strstr(filename, ".bin") != NULL) {
        file.close();
        char* buffer = (char*)heap_caps_malloc(128, MALLOC_CAP_SPIRAM);
        if (!buffer) buffer = (char*)malloc(128);
        strcpy(buffer, "[BINARY FILE]\n\nKhong the hien thi noi dung file nay tren man hinh.\nFile nay dung de nap he thong hoac hinh nen.");
        return buffer;
    }

    size_t size = file.size();
    size_t readSize = size > 2048 ? 2048 : size;

    char* buffer = (char*)heap_caps_malloc(readSize + 1, MALLOC_CAP_SPIRAM);
    if (!buffer) buffer = (char*)malloc(readSize + 1);

    if (buffer) {
        file.read(buffer, readSize);
        buffer[readSize] = '\0'; 
    }
    file.close();
    return buffer;
}

void StorageLogic::freePSRAMBuffer(char* buffer) {
    if (buffer) {
        heap_caps_free(buffer);
    }
}

void StorageLogic::saveConfig(RemoteState &state) {
    if (!isReady) return;
    FsFile file = sd_bg.open("/config.json", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
        StaticJsonDocument<256> doc;
        doc["sleepTimeout"] = state.sleepTimeout;
        doc["oledBrightness"] = state.oledBrightness;
        serializeJson(doc, file);
        file.close();
    }
}

bool StorageLogic::loadConfig(RemoteState &state) {
    if (!isReady) return false;
    FsFile file = sd_bg.open("/config.json", O_READ);
    if (file) {
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, file);
        if (!error) {
            state.sleepTimeout = doc["sleepTimeout"] | 30;
            state.oledBrightness = doc["oledBrightness"] | 50;
        }
        file.close();
        return true;
    }
    return false;
}