#ifndef DISPLAY_LOGIC_H
#define DISPLAY_LOGIC_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include "Config.h"
#include "lvgl.h"
#include <SdFat.h> 

class DisplayLogic {
public:
    void begin();
    uint32_t loop();
    void updateUI(RemoteState &state);
    void setContrast(int level);
    void turnOff();
    void turnOn();
    void showFileContent(const char* title, const char* content);
    void showProgressPopup(const char* title, const char* msg, int percent);
    void closeProgressPopup();
    void forceRebuild(); 
    void loadBackgroundFromSD(); 
    void showHomeKitQr();
    void closeHomeKitQr();

    // BOOT LOG: gọi trước khi LVGL sẵn sàng, dùng tft trực tiếp
    void bootPrint(const char* tag, const char* msg, bool ok = true);

    // CÁC HÀM XỬ LÝ PREVIEW ẢNH
    bool showImagePreview(FsFile& file);
    void closeImagePreview();

private:
    static void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);
    static void my_indev_read(lv_indev_drv_t * drv, lv_indev_data_t*data);
    void buildMenu(const char* items[], int count);
    
    int currentMenuCount = 0;
    MenuLevel lastMenuType = MENU_NONE;
    
    lv_obj_t* menuButtons[25]; 
};

extern DisplayLogic display;

#endif