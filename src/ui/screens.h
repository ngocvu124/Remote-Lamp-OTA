#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_MAIN = 1,
    SCREEN_ID_MENU = 2,
    SCREEN_ID_STOCK = 3,
    SCREEN_ID_U = 4,
    _SCREEN_ID_LAST = 4
};

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *menu;
    lv_obj_t *stock;
    lv_obj_t *u;
    lv_obj_t *ui_batbar;
    lv_obj_t *bat_value;
    lv_obj_t *label_value;
    lv_obj_t *value;
    lv_obj_t *arc_value;
    lv_obj_t *cont_menu;
    lv_obj_t *label_menu;
    lv_obj_t *cont_menu_text;
    lv_obj_t *text_button;
    lv_obj_t *stock_chart;
    lv_obj_t *stock_roller;
    lv_obj_t *obj0;
    lv_obj_t *percent;
    lv_obj_t *obj1;
    lv_obj_t *stock_price;
    lv_obj_t *stock_price_value;
    lv_obj_t *obj2;
    lv_obj_t *profit_loss;
    lv_obj_t *profit_loss_value;
    lv_obj_t *obj3;
    lv_obj_t *stock_status;
} objects_t;

extern objects_t objects;

void create_screen_main();
void tick_screen_main();

void create_screen_menu();
void tick_screen_menu();

void create_screen_stock();
void tick_screen_stock();

void create_screen_u();
void tick_screen_u();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/