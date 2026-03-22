#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include "Config.h"
#include "lvgl.h"

class DisplayLogic {
public:
    void begin();
    void loop();
    void updateUI(RemoteState &state);
    void setContrast(int level);
    void turnOff();
    void showMessage(const char* msg);
    void showFileContent(const char* title, const char* content);
    
    void showProgressPopup(const char* title, const char* msg, int percent);
    void closeProgressPopup();
    
    void forceRebuild(); 
    void loadBackgroundFromSD(); // CÚ CHỐT: Đọc ảnh từ thẻ nhớ SD

private:
    static void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);
    static void my_indev_read(lv_indev_drv_t * drv, lv_indev_data_t*data);
    void buildMenu(const char* items[], int count);
    
    int currentMenuCount = 0;
    MenuLevel lastMenuType = MENU_NONE;
    lv_obj_t* menuButtons[15];
};

extern DisplayLogic display;
extern Adafruit_ST7789 tft;

#endif