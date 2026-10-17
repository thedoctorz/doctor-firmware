#include <furi.h>
#include <furi_hal_sd.h>
#include <furi_hal_usb.h>
#include <furi_hal_version.h>

#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/elements.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include "mass_storage_usb.h"

#define TAG "UsbMscApp"

#define USB_MSC_VIEW_ID        (0)
#define USB_MSC_TICK_PERIOD_MS (250)

typedef enum {
    UsbMscCustomEventEject = 100,
} UsbMscCustomEvent;

typedef struct {
    bool sd_present; // card detected and usable
    bool usb_ok; // MSC interface started
    bool ejected; // host issued eject
    uint64_t capacity; // bytes
    uint32_t kb_read;
    uint32_t kb_written;
} UsbMscModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* view;
    Storage* storage;
    NotificationApp* notifications;
    MassStorageUsb* usb;
    bool sd_unmounted;

    // Updated from the USB worker thread, sampled by the GUI tick.
    volatile uint32_t read_blocks;
    volatile uint32_t write_blocks;
    uint32_t last_read;
    uint32_t last_write;
} UsbMscApp;

static bool usb_msc_read(
    void* ctx,
    uint32_t lba,
    uint16_t count,
    uint8_t* out,
    uint32_t* out_len,
    uint32_t out_cap) {
    UsbMscApp* app = ctx;

    uint32_t max_blocks = out_cap / SCSI_BLOCK_SIZE;
    uint32_t blocks = count;
    if(blocks > max_blocks) blocks = max_blocks;
    if(blocks == 0) return false;

    // out is malloc'd in the worker, so it is 32-bit aligned.
    if(furi_hal_sd_read_blocks((uint32_t*)(void*)out, lba, blocks) != FuriStatusOk) {
        return false;
    }

    *out_len = blocks * SCSI_BLOCK_SIZE;
    app->read_blocks += blocks;
    return true;
}

static bool usb_msc_write(void* ctx, uint32_t lba, uint16_t count, uint8_t* buf, uint32_t len) {
    UNUSED(len);
    UsbMscApp* app = ctx;
    if(count == 0) return true;

    if(furi_hal_sd_write_blocks((const uint32_t*)(void*)buf, lba, count) != FuriStatusOk) {
        return false;
    }

    app->write_blocks += count;
    return true;
}

static uint32_t usb_msc_num_blocks(void* ctx) {
    UNUSED(ctx);
    FuriHalSdInfo info;
    if(furi_hal_sd_info(&info) != FuriStatusOk) return 0;
    return info.logical_block_count;
}

static void usb_msc_eject(void* ctx) {
    UsbMscApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, UsbMscCustomEventEject);
}

// --- GUI ---

static void usb_msc_draw_callback(Canvas* canvas, void* model) {
    UsbMscModel* m = model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "USB Storage");
    canvas_set_font(canvas, FontSecondary);

    if(!m->sd_present) {
        elements_multiline_text_aligned(
            canvas, 64, 34, AlignCenter, AlignCenter, "No SD card inserted.\nPress Back to exit.");
        return;
    }

    if(!m->usb_ok) {
        elements_multiline_text_aligned(
            canvas,
            64,
            34,
            AlignCenter,
            AlignCenter,
            "USB is busy.\nDisconnect other USB\napp, then retry.");
        return;
    }

    char line[32];
    uint32_t mb = (uint32_t)(m->capacity / (1024ULL * 1024ULL));
    snprintf(line, sizeof(line), "Card: %lu MB", (unsigned long)mb);
    canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignTop, line);

    snprintf(
        line,
        sizeof(line),
        "R %lu KB   W %lu KB",
        (unsigned long)m->kb_read,
        (unsigned long)m->kb_written);
    canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignTop, line);

    canvas_set_font(canvas, FontPrimary);
    if(m->ejected) {
        elements_multiline_text_aligned(
            canvas, 64, 42, AlignCenter, AlignTop, "EJECTED\nPRESS BACK");
    } else {
        elements_multiline_text_aligned(
            canvas, 64, 42, AlignCenter, AlignTop, "WAIT FOR EJECT\nDON'T PRESS BACK");
    }
}

static bool usb_msc_custom_event_callback(void* context, uint32_t event) {
    UsbMscApp* app = context;
    if(event == UsbMscCustomEventEject) {
        with_view_model(app->view, UsbMscModel * model, { model->ejected = true; }, true);
        notification_message(app->notifications, &sequence_single_vibro);
        return true;
    }
    return false;
}

static bool usb_msc_navigation_event_callback(void* context) {
    UNUSED(context);
    // Returning false stops the view dispatcher and exits the app.
    return false;
}

static void usb_msc_tick_event_callback(void* context) {
    UsbMscApp* app = context;
    uint32_t r = app->read_blocks;
    uint32_t w = app->write_blocks;
    bool activity = (r != app->last_read) || (w != app->last_write);

    with_view_model(
        app->view,
        UsbMscModel * model,
        {
            model->kb_read = r / 2; // 512-byte blocks -> KiB
            model->kb_written = w / 2;
        },
        true);

    if(activity) {
        notification_message(app->notifications, &sequence_blink_blue_10);
        app->last_read = r;
        app->last_write = w;
    }
}

static UsbMscApp* usb_msc_app_alloc(void) {
    UsbMscApp* app = malloc(sizeof(UsbMscApp));
    memset(app, 0, sizeof(UsbMscApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->storage = furi_record_open(RECORD_STORAGE);

    app->view_dispatcher = view_dispatcher_alloc();
    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(UsbMscModel));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, usb_msc_draw_callback);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, usb_msc_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, usb_msc_navigation_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, usb_msc_tick_event_callback, USB_MSC_TICK_PERIOD_MS);
    view_dispatcher_add_view(app->view_dispatcher, USB_MSC_VIEW_ID, app->view);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    return app;
}

static void usb_msc_app_free(UsbMscApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, USB_MSC_VIEW_ID);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

static void usb_msc_app_start(UsbMscApp* app) {
    bool sd_present = furi_hal_sd_is_present();
    FuriHalSdInfo info = {0};
    bool have_info = sd_present && (furi_hal_sd_info(&info) == FuriStatusOk) &&
                     (info.logical_block_count > 0);

    with_view_model(
        app->view,
        UsbMscModel * model,
        {
            model->sd_present = have_info;
            model->usb_ok = false;
            model->ejected = false;
            model->capacity = have_info ? info.capacity : 0;
            model->kb_read = 0;
            model->kb_written = 0;
        },
        true);

    if(!have_info) {
        FURI_LOG_W(TAG, "SD card not available");
        return;
    }

    // Take the filesystem offline so the host owns the raw blocks exclusively.
    storage_sd_unmount(app->storage);
    app->sd_unmounted = true;

    SCSIDeviceFunc fn = {
        .ctx = app,
        .read = usb_msc_read,
        .write = usb_msc_write,
        .num_blocks = usb_msc_num_blocks,
        .eject = usb_msc_eject,
    };

    const char* label = furi_hal_version_get_device_name_ptr();
    app->usb = mass_storage_usb_start(label ? label : "Flipper SD", fn);

    bool usb_ok = app->usb != NULL;
    with_view_model(app->view, UsbMscModel * model, { model->usb_ok = usb_ok; }, true);

    if(usb_ok) {
        notification_message(app->notifications, &sequence_set_green_255);
    }
}

static void usb_msc_app_stop(UsbMscApp* app) {
    if(app->usb) {
        mass_storage_usb_stop(app->usb);
        app->usb = NULL;
    }
    if(app->sd_unmounted) {
        storage_sd_mount(app->storage);
        app->sd_unmounted = false;
    }
    notification_message(app->notifications, &sequence_reset_green);
}

int32_t usb_msc_app(void* p) {
    UNUSED(p);
    UsbMscApp* app = usb_msc_app_alloc();

    usb_msc_app_start(app);

    view_dispatcher_switch_to_view(app->view_dispatcher, USB_MSC_VIEW_ID);
    view_dispatcher_run(app->view_dispatcher);

    usb_msc_app_stop(app);
    usb_msc_app_free(app);
    return 0;
}
