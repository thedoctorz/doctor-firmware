#include "menu.h"

#include <assets_icons.h>
#include <furi.h>
#include <stddef.h>

#define TAG "Menu"

#define MENU_SCROLL_INTERVAL_MS (333)

static_assert(
    offsetof(MenuItem, label) == 0 && offsetof(MenuItem, icon) == 4 && sizeof(MenuItem) == 20,
    "MenuItem layout and stride are plugin ABI");
static_assert(
    offsetof(MenuModel, items) == 0 && offsetof(MenuModel, count) == 4 &&
        offsetof(MenuModel, position) == 8 && offsetof(MenuModel, scroll_counter) == 12 &&
        offsetof(MenuModel, offset) == 16 && sizeof(MenuModel) == 24,
    "MenuModel layout is plugin ABI - appending is compatible, moving a field is not");
static_assert(
    offsetof(MenuStyle, draw) == 0 && sizeof(MenuStyle) == 8,
    "MenuStyle is the plugin-owned vtable");

struct Menu {
    View* view;
    FuriTimer* scroll_timer;
    bool entered;
};

static const uint8_t menu_dsi_slot_cx[5] = {8, 34, 64, 94, 120};
#define MENU_DSI_ICON_CY      29
#define MENU_DSI_CENTER_SCALE 2
#define MENU_DSI_LABEL_Y      46
#define MENU_DSI_BAR_Y        61
#define MENU_DSI_BAR_X        14
#define MENU_DSI_BAR_W        100

static void menu_draw_dsi(Canvas* canvas, MenuModel* model) {
    size_t position = model->position;
    size_t items_count = model->count;
    MenuItem* item;

    for(int8_t slot = 0; slot < 5; slot++) {
        if(slot == 2) continue;
        int32_t offset = slot - 2;
        size_t index = (position + items_count + offset) % items_count;
        item = &model->items[index];
        canvas_draw_icon_animation(
            canvas, menu_dsi_slot_cx[slot] - 7, MENU_DSI_ICON_CY - 7, item->icon);
    }
    item = &model->items[position];
    canvas_draw_icon_animation_scaled(
        canvas,
        menu_dsi_slot_cx[2] - 7 * MENU_DSI_CENTER_SCALE,
        MENU_DSI_ICON_CY - 7 * MENU_DSI_CENTER_SCALE,
        item->icon,
        MENU_DSI_CENTER_SCALE);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, MENU_DSI_LABEL_Y, AlignCenter, AlignTop, item->label);

    if(items_count > 1) {
        canvas_draw_line(
            canvas,
            MENU_DSI_BAR_X,
            MENU_DSI_BAR_Y,
            MENU_DSI_BAR_X + MENU_DSI_BAR_W,
            MENU_DSI_BAR_Y);
        size_t seg_w = MENU_DSI_BAR_W / items_count;
        if(seg_w < 8) seg_w = 8;
        size_t seg_x = MENU_DSI_BAR_X + (MENU_DSI_BAR_W - seg_w) * position / (items_count - 1);
        canvas_draw_box(canvas, seg_x, MENU_DSI_BAR_Y - 1, seg_w, 3);
    }
}

static void menu_draw_callback(Canvas* canvas, void* _model) {
    MenuModel* model = _model;

    canvas_clear(canvas);

    if(model->position >= model->count) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Empty");
    } else if(model->style) {
        model->style->draw(canvas, model);
    } else {
        menu_draw_dsi(canvas, model);
    }
}

static void menu_set_position(MenuModel* model, size_t position) {
    if(position >= model->count || position == model->position) return;
    if(model->position < model->count) {
        icon_animation_stop(model->items[model->position].icon);
    }
    model->position = position;
    model->scroll_counter = 0;
    icon_animation_start(model->items[position].icon);
}

static bool menu_process_move(Menu* menu, InputKey key) {
    bool consumed = false;
    bool dropped = false;
    size_t requested = 0;
    size_t count = 0;
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(model->style) {
                consumed = true;
                if(model->position < model->count) {
                    requested = model->style->navigate(model, key);
                    count = model->count;
                    dropped = requested >= count;
                    menu_set_position(model, requested);
                }
            } else {
                const bool prev_key = key == InputKeyLeft;
                const bool next_key = key == InputKeyRight;
                if(prev_key || next_key) {
                    consumed = true;
                    if(model->position < model->count) {
                        size_t position = model->position;
                        if(prev_key) {
                            position = position ? position - 1 : model->count - 1;
                        } else {
                            position = (position + 1) % model->count;
                        }
                        menu_set_position(model, position);
                    }
                }
            }
        },
        consumed);
    if(dropped) {
        FURI_LOG_W(TAG, "Style asked for item %zu of %zu on key %d", requested, count, key);
    }
    return consumed;
}

static void menu_process_ok(Menu* menu) {
    MenuItem* item = NULL;
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(model->position < model->count) {
                item = &model->items[model->position];
            }
        },
        true);
    if(item && item->callback) {
        item->callback(item->callback_context, item->index);
    }
}

static bool menu_input_callback(InputEvent* event, void* context) {
    Menu* menu = context;

    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    switch(event->key) {
    case InputKeyOk:
        if(event->type != InputTypeShort) return false;
        menu_process_ok(menu);
        return true;
    case InputKeyUp:
    case InputKeyDown:
    case InputKeyLeft:
    case InputKeyRight:
        return menu_process_move(menu, event->key);
    default:
        return false;
    }
}

static void menu_scroll_timer_callback(void* context) {
    Menu* menu = context;
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(model->style) {
                model->scroll_counter++;
            }
        },
        model->style != NULL);
}

static void menu_enter(void* context) {
    Menu* menu = context;
    menu->entered = true;
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(model->position < model->count) {
                icon_animation_start(model->items[model->position].icon);
            }
            model->scroll_counter = 0;
        },
        false);
    furi_timer_start(menu->scroll_timer, furi_ms_to_ticks(MENU_SCROLL_INTERVAL_MS));
}

static void menu_exit(void* context) {
    Menu* menu = context;
    menu->entered = false;
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(model->position < model->count) {
                icon_animation_stop(model->items[model->position].icon);
            }
        },
        false);
    furi_timer_stop(menu->scroll_timer);
}

Menu* menu_alloc(void) {
    Menu* menu = malloc(sizeof(Menu));
    menu->entered = false;
    menu->view = view_alloc();
    view_set_context(menu->view, menu);
    view_allocate_model(menu->view, ViewModelTypeLocking, sizeof(MenuModel));
    view_set_draw_callback(menu->view, menu_draw_callback);
    view_set_input_callback(menu->view, menu_input_callback);
    view_set_enter_callback(menu->view, menu_enter);
    view_set_exit_callback(menu->view, menu_exit);
    menu->scroll_timer = furi_timer_alloc(menu_scroll_timer_callback, FuriTimerTypePeriodic, menu);

    with_view_model(menu->view, MenuModel * model, { memset(model, 0, sizeof(MenuModel)); }, true);

    return menu;
}

void menu_free(Menu* menu) {
    furi_check(menu);

    furi_timer_free(menu->scroll_timer);
    menu_reset(menu);
    view_free(menu->view);

    free(menu);
}

View* menu_get_view(Menu* menu) {
    furi_check(menu);
    return menu->view;
}

void menu_add_item(
    Menu* menu,
    const char* label,
    const Icon* icon,
    uint32_t index,
    MenuItemCallback callback,
    void* context) {
    furi_check(menu);
    furi_check(label);

    with_view_model(
        menu->view,
        MenuModel * model,
        {
            model->items = realloc(model->items, (model->count + 1) * sizeof(MenuItem));
            MenuItem* item = &model->items[model->count++];
            item->label = label;
            item->icon = icon_animation_alloc(icon ? icon : &A_Plugins_14);
            view_tie_icon_animation(menu->view, item->icon);
            item->index = index;
            item->callback = callback;
            item->callback_context = context;
            if(menu->entered && model->count == 1) {
                icon_animation_start(item->icon);
            }
        },
        true);
}

void menu_reset(Menu* menu) {
    furi_check(menu);

    MenuItem* items = NULL;
    size_t count = 0;
    with_view_model(
        menu->view,
        MenuModel * model,
        {
            items = model->items;
            count = model->count;
            model->items = NULL;
            model->count = 0;
            model->position = 0;
            model->scroll_counter = 0;
            model->offset = 0;
        },
        true);

    for(size_t i = 0; i < count; i++) {
        icon_animation_free(items[i].icon);
    }
    free(items);
}

void menu_set_selected_item(Menu* menu, uint32_t index) {
    furi_check(menu);

    with_view_model(
        menu->view,
        MenuModel * model,
        {
            if(index < model->count) {
                model->position = index;
                model->scroll_counter = 0;
            }
        },
        true);
}

void menu_set_style(Menu* menu, const MenuStyle* style) {
    furi_check(menu);

    with_view_model(
        menu->view,
        MenuModel * model,
        {
            model->style = style;
            model->scroll_counter = 0;
            model->offset = 0;
        },
        true);
}
