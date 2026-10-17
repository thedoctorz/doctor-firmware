#include "fw_pkg.h"
#include "fw_extract.h"
#include "fw_views.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_version.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/popup.h>
#include <notification/notification_messages.h>
#include <toolbox/version.h>

#define TAG "FwUpdater"

#define POPUP_TIMEOUT_MS 2500

typedef enum {
    FwViewIdList,
    FwViewIdDetail,
    FwViewIdBusy,
    FwViewIdPopup,
} FwViewId;

typedef enum {
    FwJobScan,
    FwJobCheck,
    FwJobExtract,
} FwJob;

typedef enum {
    FwCustomEventJobDone = 0x100,
    FwCustomEventInstall,
    FwCustomEventCancelPending,
    FwCustomEventOpenSelected,
    FwCustomEventPopupDone,
} FwCustomEvent;

typedef struct {
    Gui* gui;
    Storage* storage;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;

    FwListView* list;
    FwDetailView* detail;
    FwBusyView* busy;
    Popup* popup;

    FwPkgEntry* entries;
    size_t entry_count;

    char manifest_path[FW_PKG_PATH_LEN];
    char package_label[FW_PKG_LABEL_LEN];
    FwManifest manifest;
    FwCheck check;
    bool package_armed;

    FuriThread* worker;
    FwJob job;
    size_t job_index;
    bool job_success;
    FuriString* job_error;

    FwViewId current_view;
    FwViewId view_after_popup;
} FwUpdater;

static void fw_updater_switch_view(FwUpdater* app, FwViewId view_id) {
    app->current_view = view_id;
    view_dispatcher_switch_to_view(app->view_dispatcher, view_id);
}

static void fw_updater_pretty_origin(char* out, size_t out_size, const char* origin) {
    bool cap = true;
    size_t used = 0;

    for(size_t i = 0; origin && origin[i] && (used + 1 < out_size); i++) {
        char c = origin[i];
        if((c == '_') || (c == '-')) {
            out[used++] = ' ';
            cap = true;
            continue;
        }
        if(cap && (c >= 'a') && (c <= 'z')) {
            c = (char)(c - 'a' + 'A');
        }
        out[used++] = c;
        cap = false;
    }
    out[used] = '\0';
}

static void fw_updater_describe_package(FwUpdater* app) {
    char line[FW_DETAIL_LINE_LEN];

    fw_detail_view_set_package(
        app->detail,
        app->package_label,
        app->check.installable && !app->package_armed,
        app->package_armed);

    if(app->package_armed) {
        fw_detail_view_add_line(app->detail, FwLineWarning, "Already armed for next boot");
    }

    const Version* running = furi_hal_version_get_firmware_version();
    if(running) {
        char origin[32];
        fw_updater_pretty_origin(origin, sizeof(origin), version_get_firmware_origin(running));
        if(origin[0]) {
            snprintf(line, sizeof(line), "Now: %s %s", origin, version_get_version(running));
        } else {
            snprintf(line, sizeof(line), "Now: %s", version_get_version(running));
        }
        fw_detail_view_add_line(app->detail, FwLineInfo, line);
    }

    size_t used = snprintf(line, sizeof(line), "Has: ");
    if(app->manifest.firmware[0]) {
        used += snprintf(line + used, sizeof(line) - used, "firmware");
    }
    if(app->manifest.radio[0]) {
        used += snprintf(line + used, sizeof(line) - used, "%sradio", (used > 5) ? ", " : "");
    }
    if(app->manifest.resources[0]) {
        used += snprintf(line + used, sizeof(line) - used, "%sresources", (used > 5) ? ", " : "");
    }
    fw_detail_view_add_line(app->detail, FwLineInfo, line);

    switch(app->check.radio_action) {
    case FwRadioActionNone:
        fw_detail_view_add_line(app->detail, FwLineInfo, "Radio: left as is");
        break;
    case FwRadioActionKeep:
        snprintf(line, sizeof(line), "Radio: %s", app->check.radio_to);
        fw_detail_view_add_line(app->detail, FwLineInfo, line);
        fw_detail_view_add_line(app->detail, FwLineInfo, "   already installed, kept");
        break;
    case FwRadioActionReflash:
        snprintf(line, sizeof(line), "Radio: %s", app->check.radio_from);
        fw_detail_view_add_line(app->detail, FwLineInfo, line);
        snprintf(line, sizeof(line), "   to: %s", app->check.radio_to);
        fw_detail_view_add_line(app->detail, FwLineInfo, line);
        break;
    }

    snprintf(
        line,
        sizeof(line),
        "Battery: %u%%%s",
        app->check.battery,
        app->check.charging ? ", charging" : "");
    fw_detail_view_add_line(app->detail, FwLineInfo, line);

    for(uint8_t i = 0; i < app->check.blocker_count; i++) {
        fw_detail_view_add_line(app->detail, FwLineBlocker, app->check.blockers[i]);
    }
    for(uint8_t i = 0; i < app->check.warning_count; i++) {
        fw_detail_view_add_line(app->detail, FwLineWarning, app->check.warnings[i]);
    }
}

static void fw_updater_show_popup(
    FwUpdater* app,
    const char* header,
    const char* text,
    FwViewId next_view) {
    app->view_after_popup = next_view;
    popup_reset(app->popup);
    popup_set_header(app->popup, header, 64, 12, AlignCenter, AlignBottom);
    popup_set_text(app->popup, text, 64, 32, AlignCenter, AlignTop);
    popup_set_timeout(app->popup, POPUP_TIMEOUT_MS);
    popup_enable_timeout(app->popup);
    fw_updater_switch_view(app, FwViewIdPopup);
}

static void fw_updater_rescan(FwUpdater* app) {
    app->entry_count = fw_pkg_scan(app->storage, app->entries, FW_PKG_MAX);
    fw_list_view_set_entries(app->list, app->entries, app->entry_count);
}

static void fw_updater_extract_progress(FwExtractStage stage, uint8_t percent, void* context) {
    FwUpdater* app = context;
    fw_busy_view_set(
        app->busy,
        "Unpacking",
        (stage == FwExtractStageDecompress) ? "Decompressing archive" : "Writing files");
    fw_busy_view_set_progress(app->busy, percent);
}

static int32_t fw_updater_worker(void* context) {
    FwUpdater* app = context;

    if(app->job == FwJobScan) {
        app->entry_count = fw_pkg_scan(app->storage, app->entries, FW_PKG_MAX);
        app->job_success = true;
    } else if(app->job == FwJobCheck) {
        app->job_success = fw_pkg_manifest_load(app->storage, app->manifest_path, &app->manifest);
        if(app->job_success) {
            fw_pkg_check(app->storage, app->manifest_path, &app->manifest, &app->check);
        } else {
            furi_string_set(app->job_error, "Manifest could not be read");
        }
    } else {
        app->job_success = fw_extract_archive(
            app->storage,
            app->entries[app->job_index].path,
            fw_updater_extract_progress,
            app,
            app->job_error);
    }

    view_dispatcher_send_custom_event(app->view_dispatcher, FwCustomEventJobDone);
    return 0;
}

static void
    fw_updater_start_job(FwUpdater* app, FwJob job, const char* title, const char* detail) {
    app->job = job;
    app->job_success = false;
    furi_string_reset(app->job_error);

    if(job == FwJobScan) {
        view_dispatcher_show_loading(app->view_dispatcher);
    } else {
        fw_busy_view_set(app->busy, title, detail);
        fw_updater_switch_view(app, FwViewIdBusy);
    }

    furi_thread_start(app->worker);
}

static void fw_updater_finish_job(FwUpdater* app) {
    furi_thread_join(app->worker);

    if(app->job == FwJobScan) {
        fw_list_view_set_entries(app->list, app->entries, app->entry_count);
        fw_updater_switch_view(app, FwViewIdList);
        return;
    }

    if(app->job == FwJobCheck) {
        if(app->job_success) {
            fw_updater_describe_package(app);
            fw_updater_switch_view(app, FwViewIdDetail);
        } else {
            fw_updater_show_popup(
                app, "Cannot open", furi_string_get_cstr(app->job_error), FwViewIdList);
        }
        return;
    }

    if(app->job_success) {
        fw_updater_rescan(app);
        notification_message(app->notifications, &sequence_success);
        fw_updater_show_popup(app, "Unpacked", "Package is ready\nto install", FwViewIdList);
    } else {
        notification_message(app->notifications, &sequence_error);
        fw_updater_show_popup(
            app, "Unpack failed", furi_string_get_cstr(app->job_error), FwViewIdList);
    }
}

static void fw_updater_open_selected(FwUpdater* app) {
    const size_t index = fw_list_view_get_selected(app->list);
    if(index >= app->entry_count) {
        return;
    }

    const FwPkgEntry* entry = &app->entries[index];

    if(entry->kind == FwPkgKindArchive) {
        app->job_index = index;
        fw_updater_start_job(app, FwJobExtract, "Unpacking", "Reading archive");
        return;
    }

    if(!entry->valid) {
        fw_updater_show_popup(app, "Bad package", "This manifest\ncannot be read", FwViewIdList);
        return;
    }

    snprintf(app->manifest_path, sizeof(app->manifest_path), "%s", entry->path);
    snprintf(app->package_label, sizeof(app->package_label), "%s", entry->label);
    app->package_armed = entry->armed;

    fw_updater_start_job(app, FwJobCheck, "Checking", "Verifying package");
}

static void fw_updater_install(FwUpdater* app) {
    const char* error = fw_pkg_arm(app->storage, app->manifest_path);

    if(error) {
        notification_message(app->notifications, &sequence_error);
        fw_updater_show_popup(app, "Cannot install", error, FwViewIdDetail);
        return;
    }

    notification_message(app->notifications, &sequence_success);
    fw_busy_view_set(app->busy, "Installing", "Rebooting into updater");
    fw_busy_view_set_progress(app->busy, 100);
    fw_updater_switch_view(app, FwViewIdBusy);

    furi_delay_ms(600);
    furi_hal_power_reset();
}

static void fw_updater_cancel_pending(FwUpdater* app) {
    fw_pkg_disarm(app->storage);
    app->package_armed = false;
    fw_updater_rescan(app);
    fw_updater_describe_package(app);
    fw_updater_show_popup(app, "Cancelled", "No update is\npending now", FwViewIdDetail);
}

static bool fw_updater_custom_event(void* context, uint32_t event) {
    FwUpdater* app = context;

    switch(event) {
    case FwCustomEventOpenSelected:
        fw_updater_open_selected(app);
        return true;
    case FwCustomEventJobDone:
        fw_updater_finish_job(app);
        return true;
    case FwCustomEventInstall:
        fw_updater_install(app);
        return true;
    case FwCustomEventCancelPending:
        fw_updater_cancel_pending(app);
        return true;
    case FwCustomEventPopupDone:
        fw_updater_switch_view(app, app->view_after_popup);
        return true;
    default:
        return false;
    }
}

static void fw_updater_view_callback(FwViewEvent event, void* context) {
    FwUpdater* app = context;

    static const uint32_t mapping[] = {
        [FwEventOpenSelected] = FwCustomEventOpenSelected,
        [FwEventInstall] = FwCustomEventInstall,
        [FwEventCancelPending] = FwCustomEventCancelPending,
    };

    view_dispatcher_send_custom_event(app->view_dispatcher, mapping[event]);
}

static void fw_updater_popup_callback(void* context) {
    FwUpdater* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, FwCustomEventPopupDone);
}

static bool fw_updater_back_event(void* context) {
    FwUpdater* app = context;

    if(app->current_view == FwViewIdDetail) {
        fw_updater_switch_view(app, FwViewIdList);
        return true;
    }

    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

static FwUpdater* fw_updater_alloc(void) {
    FwUpdater* app = malloc(sizeof(FwUpdater));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->entries = malloc(sizeof(FwPkgEntry) * FW_PKG_MAX);
    app->entry_count = 0;
    app->job_error = furi_string_alloc();
    app->package_armed = false;
    app->current_view = FwViewIdList;
    app->view_after_popup = FwViewIdList;

    app->view_dispatcher = view_dispatcher_alloc();
    app->list = fw_list_view_alloc();
    app->detail = fw_detail_view_alloc();
    app->busy = fw_busy_view_alloc();
    app->popup = popup_alloc();

    fw_list_view_set_callback(app->list, fw_updater_view_callback, app);
    fw_detail_view_set_callback(app->detail, fw_updater_view_callback, app);
    popup_set_callback(app->popup, fw_updater_popup_callback);
    popup_set_context(app->popup, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, fw_updater_custom_event);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, fw_updater_back_event);

    view_dispatcher_add_view(app->view_dispatcher, FwViewIdList, fw_list_view_get_view(app->list));
    view_dispatcher_add_view(
        app->view_dispatcher, FwViewIdDetail, fw_detail_view_get_view(app->detail));
    view_dispatcher_add_view(app->view_dispatcher, FwViewIdBusy, fw_busy_view_get_view(app->busy));
    view_dispatcher_add_view(app->view_dispatcher, FwViewIdPopup, popup_get_view(app->popup));

    app->worker = furi_thread_alloc_ex("FwUpdaterWorker", 4096, fw_updater_worker, app);

    return app;
}

static void fw_updater_free(FwUpdater* app) {
    furi_thread_free(app->worker);

    view_dispatcher_remove_view(app->view_dispatcher, FwViewIdList);
    view_dispatcher_remove_view(app->view_dispatcher, FwViewIdDetail);
    view_dispatcher_remove_view(app->view_dispatcher, FwViewIdBusy);
    view_dispatcher_remove_view(app->view_dispatcher, FwViewIdPopup);

    popup_free(app->popup);
    fw_busy_view_free(app->busy);
    fw_detail_view_free(app->detail);
    fw_list_view_free(app->list);
    view_dispatcher_free(app->view_dispatcher);

    furi_string_free(app->job_error);
    free(app->entries);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);

    free(app);
}

int32_t fw_updater_app(void* p) {
    UNUSED(p);

    FwUpdater* app = fw_updater_alloc();

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    fw_updater_start_job(app, FwJobScan, "Loading", "Looking for packages");
    view_dispatcher_run(app->view_dispatcher);

    fw_updater_free(app);
    return 0;
}
