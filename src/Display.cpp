#include "Display.h"
#include "ui/ui.h"      
#include "ui/screens.h" 
#include "Storage.h" 
#include "Ota.h" 
#include <esp_heap_caps.h>
#include <SdFat.h> 

Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, SCR_CS_PIN, SCR_DC_PIN, SCR_RST_PIN);
DisplayLogic display;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = NULL; 
static lv_img_dsc_t custom_bg;
static uint8_t* bg_data_buffer = NULL;
SdFs sd_bg; 

static lv_obj_t* scr_image_preview = NULL; 
static uint8_t* preview_data_buffer = NULL; 
static lv_img_dsc_t preview_img_dsc;        
static lv_obj_t* preview_img_obj = NULL;    

#define BACKLIGHT_CHANNEL 0 

const char* mainMenuItems[] = {"1. Control Set", "2. Lamp Set", "3. SD Explorer", "4. Stock Monitor", "5. OTA Update", "6. Web Server", "7. Exit"}; 
const char* controlMenuItems[] = {"1. Sleep Time", "2. Backlight", "3. Reset WiFi", "4. Change BG", "5. Back"}; 
const char* lampMenuItems[] = {"1. Restart", "2. Unpair", "3. Del WiFi", "4. Reset", "5. Back"};

void DisplayLogic::my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.writePixels((uint16_t *)&color_p->full, w * h);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

void DisplayLogic::my_indev_read(lv_indev_drv_t * drv, lv_indev_data_t*data){
    data->state = (digitalRead(ROTARY_BTN_PIN) == LOW) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->enc_diff = 0; 
}

void DisplayLogic::begin() {
    pinMode(SCR_BLK_PIN, OUTPUT);
    ledcSetup(BACKLIGHT_CHANNEL, 5000, 8);
    ledcAttachPin(SCR_BLK_PIN, BACKLIGHT_CHANNEL);
    ledcWrite(BACKLIGHT_CHANNEL, 255);      

    SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
    tft.init(240, 240); 
    tft.setRotation(0); 
    tft.invertDisplay(true);

    lv_init();
    buf1 = (lv_color_t*)heap_caps_malloc(SCREEN_WIDTH * 40 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!buf1) buf1 = (lv_color_t*)malloc(SCREEN_WIDTH * 40 * sizeof(lv_color_t)); 
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCREEN_WIDTH * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_ENCODER;
    indev_drv.read_cb = my_indev_read;
    lv_indev_drv_register(&indev_drv);

    ui_init(); 
    scr_image_preview = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_image_preview, lv_color_black(), 0);
}

void DisplayLogic::loadBackgroundFromSD() {
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
        // SD Fat Init lại để reset bus
        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(8)));
        
        FsFile file = sd_bg.open(appState.bgFilePath, O_READ);
        if (!file) file = sd_bg.open("/bg.bin", O_READ);

        if (file && file.size() >= 115200) {
            if (bg_data_buffer) heap_caps_free(bg_data_buffer);
            bg_data_buffer = (uint8_t*)heap_caps_malloc(115200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            
            if (bg_data_buffer) {
                // Chia nhỏ để đọc, tránh treo CPU gây mất tính năng nhấn giữ
                for(int i=0; i<28; i++) {
                    file.read(bg_data_buffer + (i*4096), 4096);
                    yield(); 
                }
                file.read(bg_data_buffer + 114688, 512);

                custom_bg.header.always_zero = 0;
                custom_bg.header.w = 240;
                custom_bg.header.h = 240;
                custom_bg.header.cf = LV_IMG_CF_TRUE_COLOR;
                custom_bg.data_size = 115200;
                custom_bg.data = bg_data_buffer;
                
                lv_obj_set_style_bg_img_src(objects.main, &custom_bg, 0);
                lv_obj_set_style_bg_opa(objects.main, 0, 0);
                lv_obj_set_style_bg_img_src(objects.menu, &custom_bg, 0);
                lv_obj_set_style_bg_opa(objects.menu, 0, 0);
                lv_obj_set_style_bg_img_src(objects.stock, &custom_bg, 0);
                lv_obj_set_style_bg_opa(objects.stock, 0, 0);
            }
        }
        if (file) file.close();
        xSemaphoreGive(xGuiSemaphore);
    }
}

void DisplayLogic::loop() { lv_timer_handler(); }

void DisplayLogic::buildMenu(const char* items[], int count) {
    lv_obj_clean(objects.cont_menu_text); 
    currentMenuCount = count;
    for(int i = 0; i < count; i++) {
        lv_obj_t * btn = lv_btn_create(objects.cont_menu_text);
        lv_obj_set_size(btn, 220, 35); 
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0); 
        lv_obj_t * label = lv_label_create(btn);
        lv_label_set_text(label, items[i]);
        lv_obj_center(label); 
        menuButtons[i] = btn;
    }
}

void DisplayLogic::forceRebuild() { lastMenuType = (MenuLevel)-1; }

void DisplayLogic::updateUI(RemoteState &state) {
    if (state.currentMenu == MENU_STOCK) {
        if (lv_scr_act() != objects.stock) lv_scr_load(objects.stock);
        if (objects.stock_roller) lv_roller_set_selected(objects.stock_roller, state.stockIndex, LV_ANIM_ON);
        return;
    }

    if (state.currentMenu == MENU_NONE || state.currentMenu == MENU_SET_SLEEP || state.currentMenu == MENU_SET_BACKLIGHT) {
        if (lv_scr_act() != objects.main) lv_scr_load(objects.main);
        lv_bar_set_value(objects.ui_batbar, state.batteryLevel, LV_ANIM_ON);
        lv_label_set_text_fmt(objects.bat_value, "%d%%", state.batteryLevel);
        
        int val = (state.currentMenu == MENU_SET_SLEEP) ? (state.sleepTimeout-30)*100/270 : 
                  (state.currentMenu == MENU_SET_BACKLIGHT) ? state.oledBrightness :
                  (state.isTempMode ? state.temperature : state.brightness);

        lv_arc_set_value(objects.arc_value, val);
        lv_label_set_text_fmt(objects.value, "%d%%", val);
    } 
    else {
        if (lv_scr_act() != objects.menu) lv_scr_load(objects.menu);
        if (state.currentMenu != lastMenuType) {
            if (state.currentMenu == MENU_MAIN) buildMenu(mainMenuItems, 7); 
            else if (state.currentMenu == MENU_CONTROL) buildMenu(controlMenuItems, 5);
            else if (state.currentMenu == MENU_LAMP) buildMenu(lampMenuItems, 5);
            else if (state.currentMenu == MENU_USB_MODE || state.currentMenu == MENU_OTA || state.currentMenu == MENU_SELECT_BG) {
                const char* items[16]; int count = 0;
                if (state.currentMenu == MENU_USB_MODE) {
                    for (int i = 0; i < storage.fileCount; i++) items[i] = storage.fileNames[i];
                    count = storage.fileCount;
                } else if (state.currentMenu == MENU_SELECT_BG) {
                    for (int i = 0; i < storage.bgFileCount; i++) items[i] = storage.bgFileNames[i];
                    count = storage.bgFileCount;
                } else if (state.currentMenu == MENU_OTA) {
                    for (int i = 0; i < ota.versionCount; i++) items[i] = ota.versions[i].name;
                    count = ota.versionCount;
                }
                items[count] = "Back"; buildMenu(items, count + 1);
            }
            lastMenuType = state.currentMenu;
        }
        for (int i = 0; i < currentMenuCount; i++) {
            if (i == state.menuIndex) lv_obj_add_state(menuButtons[i], LV_STATE_CHECKED);
            else lv_obj_clear_state(menuButtons[i], LV_STATE_CHECKED);
        }
    }
}

void DisplayLogic::setContrast(int level) { ledcWrite(BACKLIGHT_CHANNEL, map(level, 0, 100, 0, 255)); }
void DisplayLogic::turnOff() { ledcWrite(BACKLIGHT_CHANNEL, 0); tft.enableDisplay(false); }

void DisplayLogic::showFileContent(const char* title, const char* content) {
    if (title && content) {
        lv_obj_t * overlay = lv_obj_create(lv_scr_act());
        lv_obj_set_size(overlay, 220, 220);
        lv_obj_center(overlay);
        lv_obj_t * lbl = lv_label_create(overlay);
        lv_label_set_text(lbl, content);
        lv_obj_set_width(lbl, 200);
    }
}

void DisplayLogic::showProgressPopup(const char* title, const char* msg, int percent) {
    // Rút gọn để tránh tốn RAM
}

void DisplayLogic::closeProgressPopup() {}

bool DisplayLogic::showImagePreview(FsFile& file) {
    if(!file) return false;
    if (preview_data_buffer) heap_caps_free(preview_data_buffer);
    preview_data_buffer = (uint8_t*)heap_caps_malloc(115200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (preview_data_buffer) {
        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
            for(int i=0; i<28; i++) { file.read(preview_data_buffer + (i*4096), 4096); yield(); }
            file.read(preview_data_buffer + 114688, 512);

            preview_img_dsc.header.always_zero = 0;
            preview_img_dsc.header.w = 240;
            preview_img_dsc.header.h = 240;
            preview_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            preview_img_dsc.data_size = 115200;
            preview_img_dsc.data = preview_data_buffer;

            if (!preview_img_obj) preview_img_obj = lv_img_create(scr_image_preview);
            lv_img_set_src(preview_img_obj, &preview_img_dsc);
            lv_scr_load(scr_image_preview);
            xSemaphoreGive(xGuiSemaphore);
            return true;
        }
    }
    return false;
}

void DisplayLogic::closeImagePreview() {
    if (preview_data_buffer) { heap_caps_free(preview_data_buffer); preview_data_buffer = NULL; }
}