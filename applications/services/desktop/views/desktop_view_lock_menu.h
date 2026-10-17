#pragma once

#include <gui/view.h>
#include "desktop_events.h"
#include <bt/bt_service/bt.h>

#define HINT_TIMEOUT 2

typedef struct NotificationApp NotificationApp;

typedef struct DesktopLockMenuView DesktopLockMenuView;

typedef void (*DesktopLockMenuViewCallback)(DesktopEvent event, void* context);

struct DesktopLockMenuView {
    View* view;
    DesktopLockMenuViewCallback callback;
    void* context;

    NotificationApp* notification;
    Bt* bt;
    bool save_notification;
    bool save_bt;
};

typedef struct {
    uint8_t idx;
    bool stealth_mode;
    DesktopLockMenuView* lock_menu;
} DesktopLockMenuViewModel;

void desktop_lock_menu_set_callback(
    DesktopLockMenuView* lock_menu,
    DesktopLockMenuViewCallback callback,
    void* context);

View* desktop_lock_menu_get_view(DesktopLockMenuView* lock_menu);
void desktop_lock_menu_set_stealth_mode_state(DesktopLockMenuView* lock_menu, bool stealth_mode);
void desktop_lock_menu_set_idx(DesktopLockMenuView* lock_menu, uint8_t idx);
DesktopLockMenuView* desktop_lock_menu_alloc(void);
void desktop_lock_menu_free(DesktopLockMenuView* lock_menu);
