#pragma once

#include "../findmy.h"
#include "../findmy_state.h"
#include <gui/view.h>

typedef enum {
    FindMyMainEventToggle,
    FindMyMainEventBackground,
    FindMyMainEventConfig,
    FindMyMainEventIntervalUp,
    FindMyMainEventIntervalDown,
    FindMyMainEventQuit,
} FindMyMainEvent;

typedef struct FindMyMain FindMyMain;
typedef void (*FindMyMainCallback)(FindMyMainEvent event, void* context);

FindMyMain* findmy_main_alloc(FindMy* app);
void findmy_main_free(FindMyMain* findmy_main);
View* findmy_main_get_view(FindMyMain* findmy_main);

void findmy_main_set_callback(FindMyMain* findmy_main, FindMyMainCallback callback, void* context);

void findmy_main_update_active(FindMyMain* findmy_main, bool active);
void findmy_main_update_interval(FindMyMain* findmy_main, uint8_t interval);
void findmy_main_toggle_mac(FindMyMain* findmy_main, bool show_mac);
void findmy_main_update_mac(FindMyMain* findmy_main, uint8_t* mac);
void findmy_main_update_type(FindMyMain* findmy_main, FindMyType type);
