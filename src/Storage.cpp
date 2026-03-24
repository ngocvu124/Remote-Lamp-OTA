#include "Storage.h"
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

StorageLogic storage;
extern SdFs sd_bg; 

// --- CÚ CHỐT: HÀM TÁT THẺ NHỚ TỈNH DẬY ---
// Bất cứ khi nào nghi ngờ thẻ nhớ bị màn hình làm nhiễu đơ ngang, gọi hàm này!
static void wakeupSD() {
    // Khóa mõm màn hình lại để nó không nghe lén tín hiệu
    pinMode(SCR_CS_PIN, OUTPUT);
    digitalWrite(SCR_CS_PIN, HIGH);
    
    // Ép khởi tạo lại thẻ nhớ ở tốc độ 4MHz (Chậm mà chắc, tuyệt đối không bao giờ rớt tín hiệu)
    sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4)));
}

void StorageLogic::begin() {
    Serial.println("\n[STORAGE] --- SD CARD INIT ---");
    wakeupSD();
    
    if (sd_bg.card()->errorCode()) {
        isReady = false;
        Serial.println("[STORAGE] SD Mount FAILED!");
        return;
    }
    
    isReady = true;
    Serial.println("[STORAGE] SD Mount OK (4MHz Rock-Solid)!");

    if (!sd_bg.exists("/background")) {
        sd_bg.mkdir("/background");
    }
    
    loadFiles();
    loadBgFiles(); 
}

void StorageLogic::loadFiles() {
    if (!isReady) return;
    wakeupSD(); // Ép thức tỉnh SD Card trước khi mở Explorer
    
    fileCount = 0;
    FsFile dir = sd_bg.open("/", O_READ); 
    if (!dir) return;
    dir.rewindDirectory();
    
    while (fileCount < 15) {
        FsFile file = dir.openNextFile();
        if (!file) break; // Nhờ wakeupSD(), nếu có file chắc chắn nó sẽ đọc được qua đoạn này
        if (!file.isDirectory()) {
            file.getName(fileNames[fileCount], 32);
            fileCount++; // Hiện toàn bộ file, kể cả config.json để bạn thấy
        }
        file.close();
    }
    dir.close();
    Serial.printf("[STORAGE] Loaded %d files in ROOT\n", fileCount);
}

void StorageLogic::loadBgFiles() {
    if (!isReady) return;
    wakeupSD(); // Ép thức tỉnh SD Card trước khi chọn Background
    
    bgFileCount = 0;
    FsFile dir = sd_bg.open("/background", O_READ); 
    if (!dir) return;
    dir.rewindDirectory();
    
    while (bgFileCount < 15) {
        FsFile file = dir.openNextFile();
        if (!file) break;
        if (!file.isDirectory()) {
            file.getName(bgFileNames[bgFileCount], 32);
            bgFileCount++;
        }
        file.close();
    }
    dir.close();
    Serial.printf("[STORAGE] Loaded %d files in /background\n", bgFileCount);
}

char* StorageLogic::readFileToPSRAM(const char* filename) {
    if (!isReady) return NULL;
    wakeupSD(); // Ép thức tỉnh trước khi bơm file Text ra màn hình
    
    FsFile file = sd_bg.open(filename, O_READ);
    if (!file) return NULL;

    size_t fileSize = file.size();
    if (fileSize == 0 || fileSize > 1024 * 1024) { 
        file.close();
        return NULL;
    }

    char* buffer = (char*)heap_caps_malloc(fileSize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer) {
        size_t readSize = file.read(buffer, fileSize);
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
    wakeupSD(); // Ép thức tỉnh trước khi ghi file
    
    FsFile file = sd_bg.open("/config.json", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
        StaticJsonDocument<512> doc;
        doc["sleepTimeout"] = state.sleepTimeout;
        doc["oledBrightness"] = state.oledBrightness;
        doc["brightness"] = state.brightness;
        doc["temperature"] = state.temperature;
        doc["bgFilePath"] = state.bgFilePath;
        serializeJson(doc, file);
        file.close();
    }
}

bool StorageLogic::loadConfig(RemoteState &state) {
    if (!isReady) return false;
    wakeupSD();
    
    FsFile file = sd_bg.open("/config.json", O_READ);
    if (file) {
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, file);
        if (!error) {
            state.sleepTimeout = doc["sleepTimeout"] | 60;
            state.oledBrightness = doc["oledBrightness"] | 50;
            state.brightness = doc["brightness"] | 50;
            state.temperature = doc["temperature"] | 50;
            const char* bg = doc["bgFilePath"];
            if (bg) {
                strncpy(state.bgFilePath, bg, sizeof(state.bgFilePath));
            } else {
                strcpy(state.bgFilePath, "/bg.bin");
            }
        }
        file.close();
        return true;
    }
    strcpy(state.bgFilePath, "/bg.bin");
    return false;
}