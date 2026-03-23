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
    Serial.println("[DISPLAY] Initializing ST7789 (240x240)...");
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

    Serial.println("[DISPLAY] LVGL UI Initialized.");
}

void DisplayLogic::loadBackgroundFromSD() {
    FsFile file = sd_bg.open(appState.bgFilePath, O_READ);
    if (!file) file = sd_bg.open("/bg.bin", O_READ);
    if (!file) return;

    if (file.size() < 115200) { file.close(); return; }

    if (bg_data_buffer != NULL) { heap_caps_free(bg_data_buffer); bg_data_buffer = NULL; }

    bg_data_buffer = (uint8_t*)heap_caps_malloc(115200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (bg_data_buffer) {
        // [FIX LỖI BG ĐEN]: Gom byte bằng vòng lặp để đảm bảo lấy đủ dữ liệu dù SPI bị trễ
        size_t totalRead = 0;
        while (totalRead < 115200) {
            int r = file.read(bg_data_buffer + totalRead, 4096);
            if (r <= 0) break;
            totalRead += r;
        }

        if (totalRead >= 115200) {
            custom_bg.header.always_zero = 0;
            custom_bg.header.w = 240;
            custom_bg.header.h = 240;
            custom_bg.header.cf = LV_IMG_CF_TRUE_COLOR;
            custom_bg.data_size = 115200;
            custom_bg.data = bg_data_buffer;
            
            // Ép LVGL xóa bộ nhớ đệm (Cache) để vẽ lại khung nền mới thay vì load khung rỗng/đen
            lv_img_cache_invalidate_src(NULL);
            
            lv_obj_set_style_bg_img_src(objects.main, &custom_bg, 0);
            lv_obj_set_style_bg_opa(objects.main, 0, 0);
            lv_obj_set_style_bg_img_src(objects.menu, &custom_bg, 0);
            lv_obj_set_style_bg_opa(objects.menu, 0, 0);
            lv_obj_set_style_bg_img_src(objects.stock, &custom_bg, 0);
            lv_obj_set_style_bg_opa(objects.stock, 0, 0);
        }
    }
    file.close();
}

void DisplayLogic::loop() { lv_timer_handler(); }

void DisplayLogic::buildMenu(const char* items[], int count) {
    lv_obj_clean(objects.cont_menu_text); 
    currentMenuCount = count;
    for(int i = 0; i < count; i++) {
        lv_obj_t * btn = lv_btn_create(objects.cont_menu_text);
        lv_obj_set_size(btn, 220, 35); 
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), LV_PART_MAIN); 
        lv_obj_set_style_bg_opa(btn, 150, LV_PART_MAIN); 
        lv_obj_set_style_text_color(btn, lv_color_white(), LV_PART_MAIN);
        
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_ORANGE), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, 255, LV_STATE_CHECKED); 
        lv_obj_set_style_text_color(btn, lv_color_black(), LV_STATE_CHECKED);
        
        lv_obj_t * label = lv_label_create(btn);
        lv_label_set_text(label, items[i]);
        lv_obj_center(label); 
        
        // [FIX LỖI KẸT FOCUS]: Đã gỡ bỏ thao tác gán mảng menuButtons[i] = btn; để tránh vỡ Stack
    }
}

void DisplayLogic::forceRebuild() { lastMenuType = (MenuLevel)-1; }

void DisplayLogic::updateUI(RemoteState &state) {
    if (state.currentMenu == MENU_STOCK) {
        if (lv_scr_act() != objects.stock) { lv_scr_load(objects.stock); lastMenuType = (MenuLevel)-1; }
        if (objects.stock_roller != NULL) lv_roller_set_selected(objects.stock_roller, state.stockIndex, LV_ANIM_ON);
        return;
    }

    if (state.currentMenu == MENU_NONE || state.currentMenu == MENU_SET_SLEEP || state.currentMenu == MENU_SET_BACKLIGHT) {
        if (lv_scr_act() != objects.main) { lv_scr_load(objects.main); lastMenuType = (MenuLevel)-1; }
        lv_bar_set_value(objects.ui_batbar, state.batteryLevel, LV_ANIM_ON);
        lv_label_set_text_fmt(objects.bat_value, "%d%%", state.batteryLevel);
        
        int val = 0; const char* title = ""; lv_color_t arc_color;

        if (state.currentMenu == MENU_SET_SLEEP) {
            val = (state.sleepTimeout - 30) * 100 / (300 - 30);
            lv_label_set_text_fmt(objects.value, "%ds", state.sleepTimeout);
            title = "SLEEP TIMER"; arc_color = lv_palette_main(LV_PALETTE_CYAN);
        } else if (state.currentMenu == MENU_SET_BACKLIGHT) {
            val = state.oledBrightness;
            lv_label_set_text_fmt(objects.value, "%d%%", val);
            title = "BACKLIGHT"; arc_color = lv_palette_main(LV_PALETTE_PURPLE);
        } else {
            val = state.isTempMode ? state.temperature : state.brightness;
            lv_label_set_text_fmt(objects.value, "%d%%", val);
            title = state.isTempMode ? "COLOR TEMP" : "BRIGHTNESS";
            arc_color = state.isTempMode ? lv_palette_main(LV_PALETTE_ORANGE) : lv_color_hex(0xffff7200);
        }

        lv_arc_set_value(objects.arc_value, val);
        lv_label_set_text(objects.label_value, title);
        lv_obj_set_style_arc_color(objects.arc_value, arc_color, LV_PART_INDICATOR);
    } 
    else {
        if (lv_scr_act() != objects.menu) lv_scr_load(objects.menu);

        if (state.currentMenu != lastMenuType) {
            if (state.currentMenu == MENU_MAIN) {
                lv_label_set_text(objects.label_menu, "Main Menu"); buildMenu(mainMenuItems, 7); 
            } 
            else if (state.currentMenu == MENU_CONTROL) {
                lv_label_set_text(objects.label_menu, "Control Setup"); buildMenu(controlMenuItems, 5); 
            }
            else if (state.currentMenu == MENU_LAMP) {
                lv_label_set_text(objects.label_menu, "Lamp Setup"); buildMenu(lampMenuItems, 5);
            }
            else if (state.currentMenu == MENU_USB_MODE || state.currentMenu == MENU_OTA || state.currentMenu == MENU_SELECT_BG) {
                // Nới rộng bộ nhớ đệm Array lên 30 để triệt tiêu lỗi Tràn Bộ Nhớ làm đứng Encoder
                const char* items[30]; 
                int count = 0;
                
                if (state.currentMenu == MENU_USB_MODE) {
                    lv_label_set_text(objects.label_menu, "SD Card Files"); storage.loadFiles();
                    for (int i = 0; i < storage.fileCount; i++) items[i] = storage.fileNames[i];
                    count = storage.fileCount;
                } else if (state.currentMenu == MENU_OTA) {
                    lv_label_set_text(objects.label_menu, "Select Version");
                    for (int i = 0; i < ota.versionCount && i < 14; i++) items[i] = ota.versions[i].name;
                    count = ota.versionCount;
                } else if (state.currentMenu == MENU_SELECT_BG) {
                    lv_label_set_text(objects.label_menu, "Select BG"); storage.loadBgFiles();
                    for (int i = 0; i < storage.bgFileCount; i++) items[i] = storage.bgFileNames[i];
                    count = storage.bgFileCount;
                }
                items[count] = "Back"; buildMenu(items, count + 1);
            }
            lastMenuType = state.currentMenu;
        }

        // [FIX LỖI KẸT FOCUS]: Gọi object con trực tiếp từ LVGL cha, thay vì truy cập mảng rủi ro
        for (int i = 0; i < currentMenuCount; i++) {
            lv_obj_t* btn = lv_obj_get_child(objects.cont_menu_text, i);
            if (btn == NULL) continue;
            
            if (i == state.menuIndex) {
                lv_obj_add_state(btn, LV_STATE_CHECKED);
                lv_obj_scroll_to_view(btn, LV_ANIM_ON);
            } else {
                lv_obj_clear_state(btn, LV_STATE_CHECKED);
            }
        }
    }
}

void DisplayLogic::setContrast(int level) { ledcWrite(BACKLIGHT_CHANNEL, map(level, 0, 100, 0, 255)); }
void DisplayLogic::turnOff() { ledcWrite(BACKLIGHT_CHANNEL, 0); tft.enableDisplay(false); }

static lv_obj_t * file_overlay = NULL;
static char* current_file_buffer = NULL;

void DisplayLogic::showFileContent(const char* title, const char* content) {
    if (title != NULL && strcmp(title, "SCROLL_UP") == 0) { if (file_overlay) lv_obj_scroll_by(file_overlay, 0, 40, LV_ANIM_ON); return; }
    if (title != NULL && strcmp(title, "SCROLL_DOWN") == 0) { if (file_overlay) lv_obj_scroll_by(file_overlay, 0, -40, LV_ANIM_ON); return; }

    if (file_overlay != NULL) { lv_obj_del(file_overlay); file_overlay = NULL; }
    if (current_file_buffer != NULL) { storage.freePSRAMBuffer(current_file_buffer); current_file_buffer = NULL; }
    if (title == NULL && content == NULL) return; 

    current_file_buffer = (char*)content;

    file_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(file_overlay, 230, 230); lv_obj_center(file_overlay);
    lv_obj_set_style_bg_color(file_overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(file_overlay, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_border_width(file_overlay, 2, 0);
    lv_obj_set_style_pad_all(file_overlay, 10, 0);
    lv_obj_set_scroll_dir(file_overlay, LV_DIR_VER);

    lv_obj_t * label_title = lv_label_create(file_overlay);
    lv_label_set_text(label_title, title);
    lv_obj_set_style_text_color(label_title, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t * line = lv_line_create(file_overlay);
    static lv_point_t line_points[] = { {0, 0}, {200, 0} };
    lv_line_set_points(line, line_points, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(0x444444), 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t * label_content = lv_label_create(file_overlay);
    lv_label_set_long_mode(label_content, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label_content, 200);
    lv_obj_set_style_text_font(label_content, &lv_font_montserrat_12, 0); 
    lv_obj_set_style_text_color(label_content, lv_color_hex(0xFFFFFF), 0); 
    if (content) lv_label_set_text_static(label_content, current_file_buffer);
    else lv_label_set_text(label_content, "SD Card Error!");
    lv_obj_align(label_content, LV_ALIGN_TOP_LEFT, 0, 30);
}

static lv_obj_t * ota_overlay = NULL;
static lv_obj_t * ota_label_content = NULL;
static lv_obj_t * ota_bar = NULL;

void DisplayLogic::showProgressPopup(const char* title, const char* msg, int percent) {
    if (ota_overlay == NULL) {
        ota_overlay = lv_obj_create(lv_scr_act());
        lv_obj_set_size(ota_overlay, 230, 230); lv_obj_center(ota_overlay);
        lv_obj_set_style_bg_color(ota_overlay, lv_color_hex(0x111111), 0);
        lv_obj_set_style_border_color(ota_overlay, lv_palette_main(LV_PALETTE_ORANGE), 0);
        lv_obj_set_style_border_width(ota_overlay, 2, 0);
        lv_obj_set_style_pad_all(ota_overlay, 10, 0);
        lv_obj_set_scroll_dir(ota_overlay, LV_DIR_NONE);

        lv_obj_t * label_title = lv_label_create(ota_overlay);
        lv_label_set_text(label_title, title); 
        lv_obj_set_style_text_color(label_title, lv_palette_main(LV_PALETTE_ORANGE), 0);
        lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t * line = lv_line_create(ota_overlay);
        static lv_point_t line_points[] = { {0, 0}, {200, 0} };
        lv_line_set_points(line, line_points, 2);
        lv_obj_set_style_line_color(line, lv_color_hex(0x444444), 0);
        lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 20);

        ota_label_content = lv_label_create(ota_overlay);
        lv_label_set_long_mode(ota_label_content, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ota_label_content, 200);
        lv_obj_set_style_text_font(ota_label_content, &lv_font_montserrat_12, 0); 
        lv_obj_set_style_text_color(ota_label_content, lv_color_hex(0xFFFFFF), 0); 
        lv_obj_align(ota_label_content, LV_ALIGN_TOP_LEFT, 0, 30);

        ota_bar = lv_bar_create(ota_overlay);
        lv_obj_set_size(ota_bar, 200, 20); lv_obj_align(ota_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x444444), LV_PART_MAIN);
        lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x00cc00), LV_PART_INDICATOR);
    } else {
        lv_obj_t * label_title = lv_obj_get_child(ota_overlay, 0);
        if(label_title) lv_label_set_text(label_title, title);
    }
    if (msg != NULL) lv_label_set_text(ota_label_content, msg);
    if (percent >= 0 && percent <= 100) lv_bar_set_value(ota_bar, percent, LV_ANIM_ON);
}

void DisplayLogic::closeProgressPopup() {
    if (ota_overlay != NULL) { lv_obj_del(ota_overlay); ota_overlay = NULL; ota_label_content = NULL; ota_bar = NULL; }
}

bool DisplayLogic::showImagePreview(FsFile& file) {
    if(!file) return false;
    
    if (!scr_image_preview) {
        scr_image_preview = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_image_preview, lv_color_black(), 0);
    }

    if (preview_data_buffer != NULL) { heap_caps_free(preview_data_buffer); preview_data_buffer = NULL; }
    preview_data_buffer = (uint8_t*)heap_caps_malloc(115200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!preview_data_buffer) return false;

    size_t totalRead = 0;
    while (totalRead < 115200) {
        int r = file.read(preview_data_buffer + totalRead, 4096);
        if (r <= 0) break;
        totalRead += r;
    }
    
    if (totalRead >= 115200) {
        preview_img_dsc.header.always_zero = 0;
        preview_img_dsc.header.w = 240;
        preview_img_dsc.header.h = 240;
        preview_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        preview_img_dsc.data_size = 115200;
        preview_img_dsc.data = preview_data_buffer;

        lv_img_cache_invalidate_src(NULL);

        if (!preview_img_obj) {
            preview_img_obj = lv_img_create(scr_image_preview);
            lv_obj_center(preview_img_obj);
        }
        lv_img_set_src(preview_img_obj, &preview_img_dsc);
        lv_scr_load(scr_image_preview);
        return true;
    }
    return false;
}

void DisplayLogic::closeImagePreview() {
    if (preview_data_buffer != NULL) { heap_caps_free(preview_data_buffer); preview_data_buffer = NULL; }
    if (preview_img_obj) { lv_img_set_src(preview_img_obj, ""); }
    
    if (lv_scr_act() == scr_image_preview && objects.menu != NULL) {
        lv_scr_load(objects.menu);
    }
}