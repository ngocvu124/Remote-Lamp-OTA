#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

#define FIRMWARE_VERSION "v1.4.0.1"
#define FIRMWARE_NAME "Add file explore function"

#define WIFI_CHANNEL 6 
const uint8_t BROADCAST_ADDRESS[] = {0xDC, 0x06, 0x75, 0x69, 0x1C, 0xA4};

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

// --- FIX LỖI DISPLAY: Kích thước màn hình ---
#define SCREEN_WIDTH    240
#define SCREEN_HEIGHT   240

#define BAT_SAMPLES 10
#define BAT_CALIBRATION_FACTOR 1.0

#define STACK_GUI       16384
#define STACK_NETWORK   16384  
#define STACK_SYSTEM    16384
#define STACK_WEB       8192  

#define PRIO_INPUT      5     
#define PRIO_GUI        4
#define PRIO_SYSTEM     3
#define PRIO_NETWORK    2     
#define PRIO_WEB        2

enum EncoderEvent {
    ENC_IDLE,
    ENC_UP,
    ENC_DOWN,
    ENC_CLICK,
    ENC_LONG_PRESS
};

typedef struct struct_message {
  int mode;
  int brightness;  
  int temperature; 
  char sysCmd;
} struct_message;

enum MenuLevel {
    MENU_NONE = 0,
    MENU_MAIN = 1,
    MENU_CONTROL = 2,
    MENU_LAMP = 3,
    MENU_SET_SLEEP = 4,
    MENU_SET_BACKLIGHT = 5,
    MENU_USB_MODE = 6,
    MENU_STOCK = 7,
    MENU_OTA = 8,
    MENU_CHANGE_BG_LOCAL = 9,
    MENU_WEB_SERVER = 10
};

struct RemoteState {
    int brightness = 50;
    int temperature = 50;
    bool isTempMode = false;
    int batteryLevel = 100;
    int sleepTimeout = 60;
    int oledBrightness = 80;
    MenuLevel currentMenu = MENU_NONE;
    int menuIndex = 0;
    int stockIndex = 0;
};

#endif