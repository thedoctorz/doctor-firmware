/**
 * @file classic_list.h
 * The stock firmware's main menu list, as a standalone view.
 */

#pragma once

#include <gui/icon.h>
#include <gui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ClassicList ClassicList;

typedef void (*ClassicListCallback)(void* context, uint32_t index);

ClassicList* classic_list_alloc(void);

void classic_list_free(ClassicList* list);

View* classic_list_get_view(ClassicList* list);

/** Remove every item and rewind to the top */
void classic_list_reset(ClassicList* list);

void classic_list_add_item(
    ClassicList* list,
    const char* label,
    const Icon* icon,
    uint32_t index,
    ClassicListCallback callback,
    void* context);

void classic_list_add_item_bitmap(
    ClassicList* list,
    const char* label,
    const uint8_t* bitmap,
    uint32_t index,
    ClassicListCallback callback,
    void* context);

/** Select the item at @p position, ignored if there is no such item */
void classic_list_set_selected_item(ClassicList* list, uint32_t position);

uint32_t classic_list_get_selected_item(ClassicList* list);

#ifdef __cplusplus
}
#endif
