#include "analyzer_export.h"
#include "analyzer_hunt_worker.h"
#include "analyzer_lock_worker.h"
#include "analyzer_preset.h"
#include "analyzer_session.h"
#include "analyzer_tune_worker.h"
#include "analyzer_view_hunt.h"
#include "analyzer_view_lock.h"
#include "analyzer_view_result.h"

#include <gui/modules/submenu.h>
#include "analyzer_view_set.h"
#include "analyzer_view_tune.h"
#include "helpers/radio_device_loader.h"

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <notification/notification_messages.h>

#include <string.h>

#define TAG "SubGhzAnalyzer"

#define ANALYZER_TICK_MS 100u

typedef enum {
    AnalyzerResultMenuUse,
    AnalyzerResultMenuSave,
    AnalyzerResultMenuRecord,
    AnalyzerResultMenuRetry,
} AnalyzerResultMenuItem;

typedef enum {
    AnalyzerViewIdHunt,
    AnalyzerViewIdSet,
    AnalyzerViewIdLock,
    AnalyzerViewIdTune,
    AnalyzerViewIdResult,
    AnalyzerViewIdResultMenu,
} AnalyzerViewId;

typedef enum {
    AnalyzerEventToHunt = 100,
    AnalyzerEventToSet,
    AnalyzerEventToLock,
    AnalyzerEventToTune,
    AnalyzerEventTuneStart,
    AnalyzerEventToResult,
    AnalyzerEventExit,
    AnalyzerEventHuntTrigger,
    AnalyzerEventLockRetune,
    AnalyzerEventLockZoom,
    AnalyzerEventSave,
    AnalyzerEventRecord,
    AnalyzerEventResultMenu,
    AnalyzerEventUseExisting,
} AnalyzerAppEvent;

#define ANALYZER_TUNE_START_DELAY_TICKS (1000u / ANALYZER_TICK_MS)

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    NotificationApp* notifications;

    const SubGhzDevice* device;
    SubGhzSetting* setting;

    AnalyzerSession session;
    AnalyzerPipelineStage stage;

    AnalyzerHuntWorker* hunt_worker;
    AnalyzerLockWorker* lock_worker;
    AnalyzerWorker* tune_worker;

    AnalyzerViewHunt* hunt_view;
    AnalyzerViewSet* set_view;
    AnalyzerViewLock* lock_view;
    AnalyzerViewTune* tune_view;
    AnalyzerViewResult* result_view;
    Submenu* result_menu;

    AnalyzerResult result;
    AnalyzerPresetMatch matches[ANALYZER_PRESET_MAX_RANK];
    uint8_t match_count;
    char save_name[ANALYZER_PRESET_NAME_LEN];
    int saved_index;

    bool tune_running;
    uint8_t tune_start_ticks;
} SubGhzAnalyzerApp;

static void subghz_analyzer_stop_workers(SubGhzAnalyzerApp* app) {
    if(analyzer_hunt_worker_is_running(app->hunt_worker)) {
        analyzer_hunt_worker_stop(app->hunt_worker);
    }
    if(analyzer_lock_worker_is_running(app->lock_worker)) {
        analyzer_lock_worker_fill_session(app->lock_worker, &app->session);
        analyzer_lock_worker_stop(app->lock_worker);
    }
    if(app->tune_running) {
        analyzer_worker_stop(app->tune_worker);
        app->tune_running = false;
    }
}

static void subghz_analyzer_enter_hunt(SubGhzAnalyzerApp* app) {
    subghz_analyzer_stop_workers(app);

    analyzer_session_clear_lock(&app->session);

    app->stage = AnalyzerPipelineHunt;
    analyzer_hunt_worker_set_trigger(app->hunt_worker, analyzer_view_hunt_trigger(app->hunt_view));
    analyzer_hunt_worker_start(app->hunt_worker);
    view_dispatcher_switch_to_view(app->view_dispatcher, AnalyzerViewIdHunt);
}

static void subghz_analyzer_enter_set(SubGhzAnalyzerApp* app) {
    subghz_analyzer_stop_workers(app);

    app->stage = AnalyzerPipelineSet;
    analyzer_view_set_set_frequency(app->set_view, app->session.frequency);
    view_dispatcher_switch_to_view(app->view_dispatcher, AnalyzerViewIdSet);
}

static void subghz_analyzer_enter_lock(SubGhzAnalyzerApp* app) {
    subghz_analyzer_stop_workers(app);

    app->stage = AnalyzerPipelineLock;
    analyzer_view_lock_set_frequency(app->lock_view, app->session.frequency);
    analyzer_lock_worker_start(
        app->lock_worker, app->session.frequency, analyzer_view_lock_zoom(app->lock_view));
    view_dispatcher_switch_to_view(app->view_dispatcher, AnalyzerViewIdLock);
}

static void subghz_analyzer_enter_tune(SubGhzAnalyzerApp* app) {
    subghz_analyzer_stop_workers(app);

    app->stage = AnalyzerPipelineTune;
    app->tune_running = false;
    app->tune_start_ticks = 0;

    AnalyzerProgress zero;
    memset(&zero, 0, sizeof(zero));
    analyzer_view_tune_set_progress(app->tune_view, &zero, app->session.frequency, 0);

    analyzer_view_tune_set_frequency(app->tune_view, app->session.frequency);
    analyzer_view_tune_set_phase(app->tune_view, AnalyzerTunePhaseArmed);
    view_dispatcher_switch_to_view(app->view_dispatcher, AnalyzerViewIdTune);
}

static void subghz_analyzer_begin_sweep(SubGhzAnalyzerApp* app) {
    if(!analyzer_worker_start(app->tune_worker, &app->session)) {
        FURI_LOG_E(TAG, "could not start the sweep");
        subghz_analyzer_enter_lock(app);
        return;
    }
    app->tune_running = true;
    analyzer_view_tune_set_phase(app->tune_view, AnalyzerTunePhaseRunning);
}

static void subghz_analyzer_enter_result(SubGhzAnalyzerApp* app) {
    if(app->tune_running) {
        analyzer_worker_stop(app->tune_worker);
        app->tune_running = false;
    }
    analyzer_worker_get_result(app->tune_worker, &app->result);

    app->stage = AnalyzerPipelineResult;
    app->saved_index = -1;

    app->match_count = analyzer_preset_rank(
        app->setting,
        &app->result.model,
        app->result.survey.valid ? app->result.survey.occupied_bw_hz : 0u,
        app->matches,
        ANALYZER_PRESET_MAX_RANK);

    analyzer_preset_derive_name(&app->result.model, app->save_name, sizeof(app->save_name));

    analyzer_view_result_set(
        app->result_view, &app->result, app->matches, app->match_count, app->save_name);
    view_dispatcher_switch_to_view(app->view_dispatcher, AnalyzerViewIdResult);
}

static AnalyzerSaveResult subghz_analyzer_save(SubGhzAnalyzerApp* app) {
    const AnalyzerSaveResult saved = analyzer_export_save_preset(
        &app->result.model,
        app->save_name,
        app->save_name,
        sizeof(app->save_name),
        &app->saved_index);

    if(saved == AnalyzerSaveCreated || saved == AnalyzerSaveReusedExisting) {
        subghz_setting_free(app->setting);
        app->setting = subghz_setting_alloc();
        subghz_setting_load(app->setting, ANALYZER_SETTING_USER_PATH);
        notification_message(app->notifications, &sequence_success);
    } else {
        notification_message(app->notifications, &sequence_error);
    }

    return saved;
}

static void subghz_analyzer_hunt_callback(void* context, AnalyzerHuntEvent event) {
    SubGhzAnalyzerApp* app = context;
    switch(event) {
    case AnalyzerHuntEventLock:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventToSet);
        break;
    case AnalyzerHuntEventExit:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventExit);
        break;
    case AnalyzerHuntEventTriggerChanged:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventHuntTrigger);
        break;
    default:
        break;
    }
}

static void subghz_analyzer_set_callback(void* context, AnalyzerSetEvent event) {
    SubGhzAnalyzerApp* app = context;
    if(event == AnalyzerSetEventLook) {
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventToLock);
    } else {
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventToHunt);
    }
}

static void subghz_analyzer_lock_callback(void* context, AnalyzerLockEvent event) {
    SubGhzAnalyzerApp* app = context;
    switch(event) {
    case AnalyzerLockEventTune:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventToTune);
        break;
    case AnalyzerLockEventBack:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventToSet);
        break;
    case AnalyzerLockEventZoomChanged:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventLockZoom);
        break;
    case AnalyzerLockEventRetune:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventLockRetune);
        break;
    default:
        break;
    }
}

static void subghz_analyzer_tune_callback(void* context, AnalyzerTuneEvent event) {
    SubGhzAnalyzerApp* app = context;
    if(event == AnalyzerTuneEventStart) {
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventTuneStart);
        return;
    }
    if(app->tune_running) {
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventToResult);
    } else {
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventToLock);
    }
}

static void subghz_analyzer_result_callback(void* context, AnalyzerResultEvent event) {
    SubGhzAnalyzerApp* app = context;
    switch(event) {
    case AnalyzerResultEventActions:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventResultMenu);
        break;
    case AnalyzerResultEventBack:
    default:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventToLock);
        break;
    }
}

static uint32_t subghz_analyzer_result_menu_back(void* context) {
    UNUSED(context);
    return AnalyzerViewIdResult;
}

static void subghz_analyzer_result_menu_callback(void* context, uint32_t index) {
    SubGhzAnalyzerApp* app = context;
    switch(index) {
    case AnalyzerResultMenuUse:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventUseExisting);
        break;
    case AnalyzerResultMenuSave:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventSave);
        break;
    case AnalyzerResultMenuRecord:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventRecord);
        break;
    default:
        view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventToTune);
        break;
    }
}

static void subghz_analyzer_show_result_menu(SubGhzAnalyzerApp* app) {
    char label[42];

    submenu_reset(app->result_menu);
    submenu_set_header(app->result_menu, "Result");

    const bool reuse = analyzer_view_result_reuse_existing(app->result_view);
    if(reuse) {
        snprintf(label, sizeof(label), "Use \"%s\"", app->matches[0].name);
        submenu_add_item(
            app->result_menu,
            label,
            AnalyzerResultMenuUse,
            subghz_analyzer_result_menu_callback,
            app);
    }

    snprintf(
        label, sizeof(label), reuse ? "Save copy \"%s\"" : "Save preset \"%s\"", app->save_name);
    submenu_add_item(
        app->result_menu, label, AnalyzerResultMenuSave, subghz_analyzer_result_menu_callback, app);
    submenu_add_item(
        app->result_menu,
        "Read RAW",
        AnalyzerResultMenuRecord,
        subghz_analyzer_result_menu_callback,
        app);
    submenu_add_item(
        app->result_menu,
        "Sweep again",
        AnalyzerResultMenuRetry,
        subghz_analyzer_result_menu_callback,
        app);

    submenu_set_selected_item(
        app->result_menu, reuse ? AnalyzerResultMenuUse : AnalyzerResultMenuSave);
    view_dispatcher_switch_to_view(app->view_dispatcher, AnalyzerViewIdResultMenu);
}

static bool subghz_analyzer_custom_event(void* context, uint32_t event) {
    SubGhzAnalyzerApp* app = context;

    switch(event) {
    case AnalyzerEventToHunt:
        subghz_analyzer_enter_hunt(app);
        return true;

    case AnalyzerEventToSet:
        if(app->stage == AnalyzerPipelineHunt) {
            app->session.frequency = analyzer_view_hunt_selected(app->hunt_view);
            app->session.frequency_manual = analyzer_view_hunt_is_manual(app->hunt_view);
            if(!app->session.frequency) return true;
        } else if(app->stage == AnalyzerPipelineLock) {
            app->session.frequency = analyzer_view_lock_frequency(app->lock_view);
        }
        subghz_analyzer_enter_set(app);
        return true;

    case AnalyzerEventToLock:
        if(app->stage == AnalyzerPipelineSet) {
            app->session.frequency = analyzer_view_set_frequency(app->set_view);
            if(!app->session.frequency || !analyzer_view_set_is_valid(app->set_view)) return true;
        } else if(app->stage == AnalyzerPipelineHunt) {
            app->session.frequency = analyzer_view_hunt_selected(app->hunt_view);
            app->session.frequency_manual = analyzer_view_hunt_is_manual(app->hunt_view);
            if(!app->session.frequency) return true;
        }
        subghz_analyzer_enter_lock(app);
        return true;

    case AnalyzerEventToTune:
        if(app->stage == AnalyzerPipelineLock) {
            app->session.frequency = analyzer_view_lock_frequency(app->lock_view);
        }
        subghz_analyzer_enter_tune(app);
        return true;

    case AnalyzerEventTuneStart:
        app->tune_start_ticks = ANALYZER_TUNE_START_DELAY_TICKS;
        analyzer_view_tune_set_phase(app->tune_view, AnalyzerTunePhaseStarting);
        return true;

    case AnalyzerEventToResult:
        subghz_analyzer_enter_result(app);
        return true;

    case AnalyzerEventExit:
        subghz_analyzer_stop_workers(app);
        view_dispatcher_stop(app->view_dispatcher);
        return true;

    case AnalyzerEventHuntTrigger:
        analyzer_hunt_worker_set_trigger(
            app->hunt_worker, analyzer_view_hunt_trigger(app->hunt_view));
        return true;

    case AnalyzerEventLockRetune:
        analyzer_lock_worker_retune(
            app->lock_worker, analyzer_view_lock_frequency(app->lock_view));
        return true;

    case AnalyzerEventLockZoom:
        analyzer_lock_worker_set_zoom(app->lock_worker, analyzer_view_lock_zoom(app->lock_view));
        return true;

    case AnalyzerEventResultMenu:
        subghz_analyzer_show_result_menu(app);
        return true;

    case AnalyzerEventUseExisting:
        analyzer_export_preselect(app->result.model.frequency, app->matches[0].setting_index);
        subghz_analyzer_stop_workers(app);
        analyzer_export_open_subghz(app->result.model.frequency, app->matches[0].setting_index);
        view_dispatcher_stop(app->view_dispatcher);
        return true;

    case AnalyzerEventSave: {
        const AnalyzerSaveResult saved = subghz_analyzer_save(app);
        analyzer_view_result_set(
            app->result_view, &app->result, app->matches, app->match_count, app->save_name);
        analyzer_view_result_notify(app->result_view, analyzer_export_result_text(saved));
        view_dispatcher_switch_to_view(app->view_dispatcher, AnalyzerViewIdResult);
        return true;
    }

    case AnalyzerEventRecord: {
        const AnalyzerSaveResult saved = subghz_analyzer_save(app);
        if(saved != AnalyzerSaveCreated && saved != AnalyzerSaveReusedExisting) {
            analyzer_view_result_notify(app->result_view, analyzer_export_result_text(saved));
            return true;
        }

        analyzer_export_preselect(app->result.model.frequency, app->saved_index);

        subghz_analyzer_stop_workers(app);
        analyzer_export_open_subghz(app->result.model.frequency, app->saved_index);
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }

    default:
        return false;
    }
}

static void subghz_analyzer_tick(void* context) {
    SubGhzAnalyzerApp* app = context;

    switch(app->stage) {
    case AnalyzerPipelineLock: {
        AnalyzerLockState state;
        analyzer_lock_worker_get_state(app->lock_worker, &state);
        analyzer_view_lock_set_state(app->lock_view, &state);
        break;
    }

    case AnalyzerPipelineTune: {
        if(app->tune_start_ticks > 0) {
            app->tune_start_ticks--;
            if(app->tune_start_ticks == 0) subghz_analyzer_begin_sweep(app);
            break;
        }
        if(!app->tune_running) break;

        AnalyzerProgress progress;
        analyzer_worker_get_progress(app->tune_worker, &progress);

        AnalyzerResult partial;
        analyzer_worker_get_result(app->tune_worker, &partial);
        analyzer_view_tune_set_progress(
            app->tune_view, &progress, app->session.frequency, partial.score);

        if(analyzer_worker_is_finished(app->tune_worker)) {
            view_dispatcher_send_custom_event(app->view_dispatcher, AnalyzerEventToResult);
        }
        break;
    }

    case AnalyzerPipelineResult:
        analyzer_view_result_tick(app->result_view);
        break;

    default:
        break;
    }
}

static bool subghz_analyzer_navigation(void* context) {
    SubGhzAnalyzerApp* app = context;
    subghz_analyzer_stop_workers(app);
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

static void
    subghz_analyzer_hunt_locked(void* context, uint32_t frequency, float rssi, bool locked) {
    SubGhzAnalyzerApp* app = context;
    analyzer_view_hunt_update(app->hunt_view, frequency, rssi, locked);

    if(!locked) return;
    notification_message(app->notifications, &sequence_blink_green_10);
}

static SubGhzAnalyzerApp* subghz_analyzer_alloc(void) {
    SubGhzAnalyzerApp* app = malloc(sizeof(SubGhzAnalyzerApp));
    memset(app, 0, sizeof(*app));

    subghz_devices_init();
    app->device = radio_device_loader_set(NULL, SubGhzRadioDeviceTypeInternal);

    app->setting = subghz_setting_alloc();
    subghz_setting_load(app->setting, ANALYZER_SETTING_USER_PATH);

    analyzer_session_init(&app->session);
    app->saved_index = -1;

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, subghz_analyzer_custom_event);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, subghz_analyzer_navigation);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, subghz_analyzer_tick, ANALYZER_TICK_MS);

    app->hunt_worker =
        analyzer_hunt_worker_alloc(app->device, app->setting, ANALYZER_HUNT_TRIGGER_DEFAULT);
    app->lock_worker = analyzer_lock_worker_alloc(app->device);
    app->tune_worker = analyzer_worker_alloc(app->device);

    analyzer_hunt_worker_set_callback(app->hunt_worker, subghz_analyzer_hunt_locked, app);

    app->hunt_view = analyzer_view_hunt_alloc();
    app->set_view = analyzer_view_set_alloc();
    app->lock_view = analyzer_view_lock_alloc();
    app->tune_view = analyzer_view_tune_alloc();
    app->result_view = analyzer_view_result_alloc();

    analyzer_view_hunt_set_callback(app->hunt_view, subghz_analyzer_hunt_callback, app);
    analyzer_view_set_set_callback(app->set_view, subghz_analyzer_set_callback, app);
    analyzer_view_lock_set_callback(app->lock_view, subghz_analyzer_lock_callback, app);
    analyzer_view_tune_set_callback(app->tune_view, subghz_analyzer_tune_callback, app);
    analyzer_view_result_set_callback(app->result_view, subghz_analyzer_result_callback, app);

    view_dispatcher_add_view(
        app->view_dispatcher, AnalyzerViewIdHunt, analyzer_view_hunt_get_view(app->hunt_view));
    view_dispatcher_add_view(
        app->view_dispatcher, AnalyzerViewIdSet, analyzer_view_set_get_view(app->set_view));
    view_dispatcher_add_view(
        app->view_dispatcher, AnalyzerViewIdLock, analyzer_view_lock_get_view(app->lock_view));
    view_dispatcher_add_view(
        app->view_dispatcher, AnalyzerViewIdTune, analyzer_view_tune_get_view(app->tune_view));
    view_dispatcher_add_view(
        app->view_dispatcher,
        AnalyzerViewIdResult,
        analyzer_view_result_get_view(app->result_view));

    app->result_menu = submenu_alloc();
    view_set_previous_callback(
        submenu_get_view(app->result_menu), subghz_analyzer_result_menu_back);
    view_dispatcher_add_view(
        app->view_dispatcher, AnalyzerViewIdResultMenu, submenu_get_view(app->result_menu));

    return app;
}

static void subghz_analyzer_free(SubGhzAnalyzerApp* app) {
    furi_check(app);

    subghz_analyzer_stop_workers(app);

    view_dispatcher_remove_view(app->view_dispatcher, AnalyzerViewIdHunt);
    view_dispatcher_remove_view(app->view_dispatcher, AnalyzerViewIdSet);
    view_dispatcher_remove_view(app->view_dispatcher, AnalyzerViewIdLock);
    view_dispatcher_remove_view(app->view_dispatcher, AnalyzerViewIdTune);
    view_dispatcher_remove_view(app->view_dispatcher, AnalyzerViewIdResult);
    view_dispatcher_remove_view(app->view_dispatcher, AnalyzerViewIdResultMenu);

    analyzer_view_hunt_free(app->hunt_view);
    analyzer_view_set_free(app->set_view);
    analyzer_view_lock_free(app->lock_view);
    analyzer_view_tune_free(app->tune_view);
    analyzer_view_result_free(app->result_view);
    submenu_free(app->result_menu);

    analyzer_hunt_worker_free(app->hunt_worker);
    analyzer_lock_worker_free(app->lock_worker);
    analyzer_worker_free(app->tune_worker);

    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    subghz_setting_free(app->setting);
    if(app->device) radio_device_loader_end(app->device);
    subghz_devices_deinit();

    free(app);
}

int32_t subghz_analyzer_app(void* p) {
    UNUSED(p);
    SubGhzAnalyzerApp* app = subghz_analyzer_alloc();

    if(!app->device) {
        FURI_LOG_E(TAG, "no radio device");
        subghz_analyzer_free(app);
        return 0;
    }

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    subghz_analyzer_enter_hunt(app);
    view_dispatcher_run(app->view_dispatcher);

    subghz_analyzer_free(app);
    return 0;
}
