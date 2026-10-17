#include "classic_list.h"

#include <furi.h>
#include <gui/elements.h>

#define CLASSIC_LIST_BITMAP_SIZE 32
#define CLASSIC_LIST_BITMAP_PX   10
#define CLASSIC_LIST_LABEL_X     22
#define CLASSIC_LIST_LABEL_WIDTH 100
#define CLASSIC_LIST_ICON_X      4
#define CLASSIC_LIST_ROWS        3

#define CLASSIC_LIST_BITMAP_INSET 2

typedef struct {
    char* label;
    const Icon* icon;
    uint8_t* bitmap;
    uint32_t index;
    ClassicListCallback callback;
    void* context;
} ClassicListItem;

typedef struct {
    ClassicListItem* items;
    size_t count;
    size_t capacity;
    size_t position;
    FuriString* scratch;
} ClassicListModel;

struct ClassicList {
    View* view;
};

static const uint8_t classic_list_icon_y[CLASSIC_LIST_ROWS] = {3, 25, 47};
static const uint8_t classic_list_label_y[CLASSIC_LIST_ROWS] = {14, 36, 58};

static void classic_list_item_clear(ClassicListItem* item) {
    free(item->label);
    free(item->bitmap);
}

static void classic_list_model_reset(ClassicListModel* model) {
    for(size_t i = 0; i < model->count; i++) {
        classic_list_item_clear(&model->items[i]);
    }
    model->count = 0;
    model->position = 0;
}

static ClassicListItem* classic_list_model_add(ClassicListModel* model) {
    if(model->count == model->capacity) {
        const size_t capacity = model->capacity ? model->capacity * 2 : 8;
        model->items = realloc(model->items, capacity * sizeof(ClassicListItem)); //-V701
        model->capacity = capacity;
    }

    ClassicListItem* item = &model->items[model->count++];
    memset(item, 0, sizeof(ClassicListItem));
    return item;
}

static void classic_list_draw_item(Canvas* canvas, const ClassicListItem* item, uint8_t y) {
    if(item->icon) {
        canvas_draw_icon(canvas, CLASSIC_LIST_ICON_X, y, item->icon);
    } else if(item->bitmap) {
        canvas_draw_bitmap(
            canvas,
            CLASSIC_LIST_ICON_X + CLASSIC_LIST_BITMAP_INSET,
            y + CLASSIC_LIST_BITMAP_INSET,
            CLASSIC_LIST_BITMAP_PX,
            CLASSIC_LIST_BITMAP_PX,
            item->bitmap);
    }
}

static void classic_list_draw_callback(Canvas* canvas, void* _model) {
    ClassicListModel* model = _model;

    canvas_clear(canvas);

    const size_t count = model->count;
    if(!count) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 32, "Empty");
        elements_scrollbar(canvas, 0, 0);
        return;
    }

    for(size_t row = 0; row < CLASSIC_LIST_ROWS; row++) {
        const ClassicListItem* item = &model->items[(row + model->position + count - 1) % count];

        canvas_set_font(canvas, row == 1 ? FontPrimary : FontSecondary);
        classic_list_draw_item(canvas, item, classic_list_icon_y[row]);

        if(!item->label) {
            continue;
        }
        furi_string_set_str(model->scratch, item->label);
        elements_string_fit_width(canvas, model->scratch, CLASSIC_LIST_LABEL_WIDTH);
        canvas_draw_str(
            canvas,
            CLASSIC_LIST_LABEL_X,
            classic_list_label_y[row],
            furi_string_get_cstr(model->scratch));
    }

    elements_frame(canvas, 0, 21, 128 - 5, 21);
    elements_scrollbar(canvas, model->position, count);
}

static void classic_list_process_up(ClassicList* list) {
    with_view_model(
        list->view,
        ClassicListModel * model,
        {
            if(model->count) {
                model->position = (model->position + model->count - 1) % model->count;
            }
        },
        true);
}

static void classic_list_process_down(ClassicList* list) {
    with_view_model(
        list->view,
        ClassicListModel * model,
        {
            if(model->count) {
                model->position = (model->position + 1) % model->count;
            }
        },
        true);
}

static void classic_list_process_ok(ClassicList* list) {
    ClassicListCallback callback = NULL;
    void* context = NULL;
    uint32_t index = 0;

    with_view_model(
        list->view,
        ClassicListModel * model,
        {
            if(model->position < model->count) {
                const ClassicListItem* item = &model->items[model->position];
                callback = item->callback;
                context = item->context;
                index = item->index;
            }
        },
        false);

    if(callback) {
        callback(context, index);
    }
}

static bool classic_list_input_callback(InputEvent* event, void* context) {
    ClassicList* list = context;
    bool consumed = false;

    if(event->type == InputTypeShort) {
        if(event->key == InputKeyUp) {
            consumed = true;
            classic_list_process_up(list);
        } else if(event->key == InputKeyDown) {
            consumed = true;
            classic_list_process_down(list);
        } else if(event->key == InputKeyOk) {
            consumed = true;
            classic_list_process_ok(list);
        }
    } else if(event->type == InputTypeRepeat) {
        if(event->key == InputKeyUp) {
            consumed = true;
            classic_list_process_up(list);
        } else if(event->key == InputKeyDown) {
            consumed = true;
            classic_list_process_down(list);
        }
    }

    return consumed;
}

ClassicList* classic_list_alloc(void) {
    ClassicList* list = malloc(sizeof(ClassicList));
    list->view = view_alloc();

    view_set_context(list->view, list);
    view_allocate_model(list->view, ViewModelTypeLocking, sizeof(ClassicListModel));
    view_set_draw_callback(list->view, classic_list_draw_callback);
    view_set_input_callback(list->view, classic_list_input_callback);

    with_view_model(
        list->view,
        ClassicListModel * model,
        {
            model->items = NULL;
            model->count = 0;
            model->capacity = 0;
            model->position = 0;
            model->scratch = furi_string_alloc();
        },
        false);

    return list;
}

void classic_list_free(ClassicList* list) {
    furi_check(list);

    with_view_model(
        list->view,
        ClassicListModel * model,
        {
            classic_list_model_reset(model);
            free(model->items);
            furi_string_free(model->scratch);
        },
        false);

    view_free(list->view);
    free(list);
}

View* classic_list_get_view(ClassicList* list) {
    furi_check(list);
    return list->view;
}

void classic_list_reset(ClassicList* list) {
    furi_check(list);

    with_view_model(
        list->view, ClassicListModel * model, { classic_list_model_reset(model); }, true);
}

void classic_list_add_item(
    ClassicList* list,
    const char* label,
    const Icon* icon,
    uint32_t index,
    ClassicListCallback callback,
    void* context) {
    furi_check(list);
    furi_check(label);

    with_view_model(
        list->view,
        ClassicListModel * model,
        {
            ClassicListItem* item = classic_list_model_add(model);
            item->label = strdup(label);
            item->icon = icon;
            item->index = index;
            item->callback = callback;
            item->context = context;
        },
        true);
}

void classic_list_add_item_bitmap(
    ClassicList* list,
    const char* label,
    const uint8_t* bitmap,
    uint32_t index,
    ClassicListCallback callback,
    void* context) {
    furi_check(list);
    furi_check(label);
    furi_check(bitmap);

    with_view_model(
        list->view,
        ClassicListModel * model,
        {
            ClassicListItem* item = classic_list_model_add(model);
            item->label = strdup(label);
            item->bitmap = malloc(CLASSIC_LIST_BITMAP_SIZE);
            memcpy(item->bitmap, bitmap, CLASSIC_LIST_BITMAP_SIZE);
            item->index = index;
            item->callback = callback;
            item->context = context;
        },
        true);
}

void classic_list_set_selected_item(ClassicList* list, uint32_t position) {
    furi_check(list);

    with_view_model(
        list->view,
        ClassicListModel * model,
        {
            if(position < model->count) {
                model->position = position;
            }
        },
        true);
}

uint32_t classic_list_get_selected_item(ClassicList* list) {
    furi_check(list);

    uint32_t position = 0;
    with_view_model(list->view, ClassicListModel * model, { position = model->position; }, false);

    return position;
}
