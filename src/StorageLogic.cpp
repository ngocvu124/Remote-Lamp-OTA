#include "StorageLogic.h"
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

StorageLogic storage;
extern SdFs sd_bg; 

void StorageLogic::begin() {
    Serial.println("\n[STORAGE] --- SD CARD DIAGNOSTIC ---");
    
    // Khởi tạo thẻ với cấu hình Shared SPI, tốc độ 10MHz
    // Dùng SdSpiConfig để ép chuẩn SPI dùng chung với màn hình
    if (sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(10)))) {
        isReady = true;
        
        // Kiểm tra định dạng thẻ
        if (sd_bg.fatType() == FAT_TYPE_FAT32) Serial.println("[STORAGE] Format: FAT32");
        else if (sd_bg.fatType() == FAT_TYPE_EXFAT) Serial.println("[STORAGE] Format: exFAT");
        else Serial.printf("[STORAGE] Format: Unknown (%d)\n", sd_bg.fatType());
        
        uint32_t sectors = sd_bg.card()->sectorCount();
        Serial.printf("[STORAGE] Card Size: %u MB\n", sectors / 2048);
        
        loadFiles();
    } else {
        isReady = false;
        Serial.println("[STORAGE] SD Card Mount FAILED!");
    }
    Serial.println("[STORAGE] ---------------------------\n");
}

void StorageLogic::loadFiles() {
    fileCount = 0;
    if (!isReady) return;

    // CÚ CHỐT: Mở trực tiếp thư mục gốc thay vì dùng chdir/open(".")
    FsFile dir = sd_bg.open("/"); 
    if (!dir) {
        Serial.println("[STORAGE] Error: Could not open '/' (Root). Thử Mount lại...");
        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(10)));
        dir = sd_bg.open("/");
        if (!dir) {
            Serial.println("[STORAGE] Critical Error: Root directory inaccessible.");
            return;
        }
    }

    dir.rewind(); // Đưa con trỏ về đầu danh sách file

    Serial.println("[STORAGE] Scanning root directory...");
    FsFile file;
    while (file.openNext(&dir, O_READ)) {
        char name[32];
        file.getName(name, 32);
        
        // Kiểm tra xem có phải file hợp lệ không
        if (!file.isDir() && !file.isHidden()) {
            // Chỉ lấy tối đa 14 file để hiển thị trên màn hình Remote
            if (fileCount < 14) {
                strcpy(fileNames[fileCount], name);
                Serial.printf("  [%d] %s (%llu bytes)\n", fileCount, name, (uint64_t)file.size());
                fileCount++;
            }
        }
        file.close();
    }
    dir.close();
    Serial.printf("[STORAGE] Scan finished. Total: %d files.\n", fileCount);
}

char* StorageLogic::readFileToPSRAM(const char* filename) {
    if (!isReady) return NULL;

    char path[40];
    sprintf(path, "/%s", filename);
    FsFile file = sd_bg.open(path, O_READ);
    if (!file) {
        Serial.printf("[STORAGE] Error: Cannot open %s\n", path);
        return NULL;
    }

    // Chặn file BIN để không in rác
    if (strstr(filename, ".bin") != NULL) {
        file.close();
        char* buffer = (char*)heap_caps_malloc(128, MALLOC_CAP_SPIRAM);
        if (!buffer) buffer = (char*)malloc(128);
        strcpy(buffer, "[BINARY FILE]\n\nFile he thong hoac hinh nen.\nKhong the xem noi dung text.");
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
    if (buffer) heap_caps_free(buffer);
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
        Serial.println("[STORAGE] Config saved.");
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
            Serial.println("[STORAGE] Config loaded.");
        }
        file.close();
        return true;
    }
    return false;
}