#pragma once

#include <gui/view.h>
#include "fw_pkg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FW_DETAIL_MAX_LINES 10
#define FW_DETAIL_LINE_LEN  38

typedef enum {
    FwLineInfo,
    FwLineWarning,
    FwLineBlocker,
} FwLineKind;

typedef enum {
    FwEventOpenSelected,
    FwEventInstall,
    FwEventCancelPending,
} FwViewEvent;

typedef void (*FwViewCallback)(FwViewEvent event, void* context);

typedef struct FwListView FwListView;

FwListView* fw_list_view_alloc(void);
void fw_list_view_free(FwListView* list);
View* fw_list_view_get_view(FwListView* list);
void fw_list_view_set_callback(FwListView* list, FwViewCallback callback, void* context);

void fw_list_view_set_entries(FwListView* list, FwPkgEntry* entries, size_t count);

size_t fw_list_view_get_selected(FwListView* list);

typedef struct FwDetailView FwDetailView;

FwDetailView* fw_detail_view_alloc(void);
void fw_detail_view_free(FwDetailView* detail);
View* fw_detail_view_get_view(FwDetailView* detail);
void fw_detail_view_set_callback(FwDetailView* detail, FwViewCallback callback, void* context);

void fw_detail_view_set_package(
    FwDetailView* detail,
    const char* title,
    bool installable,
    bool armed);

void fw_detail_view_add_line(FwDetailView* detail, FwLineKind kind, const char* text);

typedef struct FwBusyView FwBusyView;

FwBusyView* fw_busy_view_alloc(void);
void fw_busy_view_free(FwBusyView* busy);
View* fw_busy_view_get_view(FwBusyView* busy);

void fw_busy_view_set(FwBusyView* busy, const char* title, const char* detail);
void fw_busy_view_set_progress(FwBusyView* busy, uint8_t percent);

#ifdef __cplusplus
}
#endif
