#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

// --- CẤU HÌNH THÔNG TIN OTA ---
#define FIRMWARE_VERSION "v1.6.2.0"
#define FIRMWARE_NAME "Lean Remote (Stock Removed)"

// --- CẤU HÌNH WIFI
extern char cachedSSID[32];

// --- CẤU HÌNH KẾT NỐI ESP-NOW ---
#define WIFI_CHANNEL 11
const uint8_t BROADCAST_ADDRESS[] = {0xDC, 0x06, 0x75, 0x69, 0x1C, 0xA4};

#define HOMEKIT_SETUP_CODE "46637726"
#define HOMEKIT_QR_ID "HSPN"
#define HOMEKIT_CATEGORY 5

#define LAMP_AP_SSID     "SmartLamp-AP"
#define LAMP_AP_PASSWORD "smartlamp"

// --- CẤU HÌNH PINOUT (ESP32-S3) ---
#define SPI_SCK_PIN     4
#define SPI_MISO_PIN    6
#define SPI_MOSI_PIN    5
#define SCR_CS_PIN      3
#define SCR_DC_PIN      2
#define SCR_RST_PIN     1
#define SCR_BLK_PIN     44 
#define SD_CS_PIN       7 
#define ROTARY_A_PIN    8
#define ROTARY_B_PIN    9
#define ROTARY_BTN_PIN  10
#define PIN_BATTERY     14
#define ROTARY_STEPS    4

// Nut encoder goc cua project: pull-up noi bo, nhan = muc LOW.
#define ROTARY_BTN_USE_PULLDOWN false
#define ROTARY_BTN_PRESSED_LEVEL LOW

// Chan wake phu de test loi phan cung nut/chân chinh.
// Mac dinh GPIO12 (RTC GPIO tren ESP32-S3). Noi nut tam thoi: GPIO12 <-> GND.
#define WAKE_AUX_PIN 12

// --- CẤU HÌNH ĐỌC PIN ---
#define BAT_CALIBRATION_FACTOR 1.0 
#define BAT_SAMPLES 20             

// --- CẤU HÌNH GIAO DIỆN & TÁC VỤ ---
#define SCREEN_WIDTH    240
#define SCREEN_HEIGHT   240

#define STACK_GUI       32768  
#define STACK_NETWORK   16384  
#define STACK_SYSTEM    16384
#define STACK_WEB       16384   

#define PRIO_INPUT      5     
#define PRIO_GUI        4
#define PRIO_SYSTEM     3
#define PRIO_NETWORK    2     
#define PRIO_WEB        2

// --- CẤU TRÚC SỰ KIỆN ENCODER ---
enum EncoderEvent {
    ENC_IDLE,
    ENC_UP,
    ENC_DOWN,
    ENC_CLICK,
    ENC_LONG_PRESS
};

// --- DATA STRUCT & ENUMS ---
typedef struct struct_message {
  int mode;
  int brightness;  
  int temperature; 
  char sysCmd;
    uint16_t requestId;
    char ackCmd;
    char ackOk;
    char setupCode[9];
    char qrId[5];
} struct_message;

// CÚ CHỐT: Danh sách các Menu (Đã thay USB_MODE thành ABOUT)
enum MenuLevel {
    MENU_NONE = 0,
    MENU_MAIN = 1,
    MENU_CONTROL = 2,
    MENU_LAMP = 3,
    MENU_SET_SLEEP = 4,
    MENU_SET_BACKLIGHT = 5,
    MENU_ABOUT = 6, 
    MENU_OTA = 7,
    MENU_WEB_SERVER = 8,
    MENU_SELECT_BG = 9,
    MENU_WIFI_SETUP = 10
};

struct RemoteState {
    int brightness = 50;
    int temperature = 50;
    bool isTempMode = false;
    
    int sleepTimeout = 60; 
    int oledBrightness = 50; 
    
    MenuLevel currentMenu = MENU_NONE;
    int menuIndex = 0;
    
    int batteryLevel = 100;
    char bgFilePath[64] = "/bg.bin";
};

extern char currentHomeKitSetupCode[9];
extern char currentHomeKitQrId[5];
extern bool homeKitQrSynced;

#endif