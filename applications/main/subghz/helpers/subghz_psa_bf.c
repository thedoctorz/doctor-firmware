#include "subghz_psa_bf.h"

#include "../helpers/subghz_custom_event.h"
#include <lib/subghz/protocols/renault_v1.h>
#include <lib/subghz/protocols/psa.h>

#define PSA_BF_PROGRESS_BAR_X 62
#define PSA_BF_PROGRESS_BAR_W 64
#define PSA_BF_PROGRESS_BAR_Y 24
#define PSA_BF_PROGRESS_BAR_H 8

typedef enum {
    SubGhzBfKindNone = 0,
    SubGhzBfKindPsa,
    SubGhzBfKindHitag2,
} SubGhzBfKind;

static SubGhzBfKind g_bf_kind = SubGhzBfKindNone;
static PsaBfState* g_bf_state = NULL;
static Hitag2BfState* g_hitag2_state = NULL;
static FuriThread* g_bf_thread = NULL;
static uint16_t g_bf_frame = 0;
static bool g_bf_ui_active = false;

static uint8_t subghz_bf_status(void) {
    if(g_bf_kind == SubGhzBfKindHitag2 && g_hitag2_state) {
        return g_hitag2_state->status;
    }
    if(g_bf_state) {
        return g_bf_state->status;
    }
    return PSA_BF_STATUS_IDLE;
}

static void subghz_bf_progress_values(uint32_t* cur, uint32_t* total) {
    if(g_bf_kind == SubGhzBfKindHitag2 && g_hitag2_state) {
        *cur = g_hitag2_state->progress_current;
        *total = g_hitag2_state->progress_total;
        return;
    }
    if(g_bf_state) {
        *cur = g_bf_state->progress_current;
        *total = g_bf_state->progress_total;
        return;
    }
    *cur = 0;
    *total = 0;
}

static void subghz_bf_set_cancel(void) {
    if(g_hitag2_state) {
        g_hitag2_state->cancel = 1;
    }
    if(g_bf_state) {
        g_bf_state->cancel = 1;
    }
}

static void subghz_bf_free_states(void) {
    if(g_bf_state) {
        free(g_bf_state);
        g_bf_state = NULL;
    }
    if(g_hitag2_state) {
        free(g_hitag2_state);
        g_hitag2_state = NULL;
    }
    g_bf_kind = SubGhzBfKindNone;
}

static void subghz_psa_bf_show_progress(SubGhz* subghz) {
    Widget* widget = subghz->widget;
    widget_reset(widget);
    widget_add_icon_element(widget, 0, 5, &I_DolphinWait_59x54);

    uint32_t cur = 0;
    uint32_t total = 0;
    subghz_bf_progress_values(&cur, &total);
    uint32_t pct_tenths = total ? (uint32_t)((uint64_t)cur * 1000 / total) : 0;
    if(pct_tenths > 1000) pct_tenths = 1000;

    if(g_bf_kind == SubGhzBfKindHitag2) {
        widget_add_string_element(widget, 62, 0, AlignLeft, AlignTop, FontPrimary, "Recover Key");
        widget_add_string_element(widget, 62, 12, AlignLeft, AlignTop, FontSecondary, "Max ETA:");
        widget_add_string_element(
            widget, 62, 22, AlignLeft, AlignTop, FontSecondary, "60 seconds");
    } else {
        widget_add_string_element(
            widget, 62, 0, AlignLeft, AlignTop, FontPrimary, "Bruteforcing...");
    }

    FuriString* pct_str =
        furi_string_alloc_printf("%lu.%u%%", pct_tenths / 10, (unsigned)(pct_tenths % 10));
    widget_add_string_element(
        widget,
        62,
        (g_bf_kind == SubGhzBfKindHitag2) ? 34 : 12,
        AlignLeft,
        AlignTop,
        FontSecondary,
        furi_string_get_cstr(pct_str));
    furi_string_free(pct_str);

    const uint8_t bar_y = (g_bf_kind == SubGhzBfKindHitag2) ? 46 : PSA_BF_PROGRESS_BAR_Y;
    widget_add_rect_element(
        widget,
        PSA_BF_PROGRESS_BAR_X,
        bar_y,
        PSA_BF_PROGRESS_BAR_W,
        PSA_BF_PROGRESS_BAR_H,
        2,
        false);

    uint8_t inner_w = PSA_BF_PROGRESS_BAR_W - 4;
    if(g_bf_kind == SubGhzBfKindHitag2) {
        uint8_t fill_w = total ? (uint8_t)(((uint64_t)cur * inner_w) / total) : 0;
        if(fill_w > 0) {
            widget_add_rect_element(
                widget,
                PSA_BF_PROGRESS_BAR_X + 2,
                bar_y + 2,
                fill_w,
                PSA_BF_PROGRESS_BAR_H - 4,
                0,
                true);
        }
    } else {
        g_bf_frame++;
        uint8_t block_w = 16;
        uint8_t travel = inner_w - block_w;
        uint16_t phase = (g_bf_frame * 2) % (uint16_t)(2 * travel);
        uint8_t block_x = (phase <= travel) ? (uint8_t)phase : (uint8_t)(2 * travel - phase);
        widget_add_rect_element(
            widget,
            PSA_BF_PROGRESS_BAR_X + 2 + block_x,
            bar_y + 2,
            block_w,
            PSA_BF_PROGRESS_BAR_H - 4,
            0,
            true);
    }
}

static void subghz_psa_bf_result_ok_callback(GuiButtonType result, InputType type, void* context) {
    SubGhz* subghz = context;
    if((type == InputTypeShort || type == InputTypeLong) && result == GuiButtonTypeCenter) {
        subghz_bf_free_states();
        g_bf_ui_active = false;
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoBfDone);
    }
}

static void subghz_psa_bf_show_result(SubGhz* subghz, uint8_t status) {
    Widget* widget = subghz->widget;
    widget_reset(widget);
    const char* title = (status == PSA_BF_STATUS_FOUND)     ? "Found!" :
                        (status == PSA_BF_STATUS_CANCELLED) ? "Cancelled" :
                                                              "Not found";
    if(status == PSA_BF_STATUS_FOUND) {
        widget_add_icon_element(widget, 0, 3, &I_DolphinDone_80x58);
        widget_add_string_element(widget, 82, 32, AlignLeft, AlignCenter, FontPrimary, title);
    } else if(status == PSA_BF_STATUS_CANCELLED) {
        widget_add_string_element(widget, 64, 0, AlignCenter, AlignTop, FontPrimary, title);
        widget_add_icon_element(widget, (128 - 45) / 2, 14, &I_WarningDolphin_45x42);
    } else {
        widget_add_string_element(widget, 64, 0, AlignCenter, AlignTop, FontPrimary, title);
    }
    widget_add_button_element(
        widget, GuiButtonTypeCenter, "OK", subghz_psa_bf_result_ok_callback, subghz);
}

static void subghz_psa_bf_finish(SubGhz* subghz) {
    uint8_t status = subghz_bf_status();

    if(g_bf_thread) {
        furi_thread_join(g_bf_thread);
        furi_thread_free(g_bf_thread);
        g_bf_thread = NULL;
    }

    FlipperFormat* ff = subghz_get_current_signal_data(subghz);
    if(status == PSA_BF_STATUS_FOUND) {
        if(ff) {
            if(g_bf_kind == SubGhzBfKindHitag2 && g_hitag2_state) {
                hitag2_bf_patch_flipper_format_on_success(ff, g_hitag2_state);
            } else if(g_bf_state) {
                psa_bf_patch_flipper_format_on_success(ff, g_bf_state);
            }
        }
        notification_message(subghz->notifications, &sequence_success);
    } else if(status == PSA_BF_STATUS_NOT_FOUND) {
        if(ff && g_bf_kind == SubGhzBfKindHitag2) {
            hitag2_bf_patch_flipper_format_on_miss(ff);
        }
        subghz_bf_free_states();
    } else {
        subghz_bf_free_states();
    }
    subghz_psa_bf_show_result(subghz, status);
}

bool subghz_psa_bf_needs_bruteforce(SubGhz* subghz) {
    if(g_bf_thread) return false;
    FlipperFormat* ff = subghz_get_current_signal_data(subghz);
    return psa_bf_needs_bruteforce(ff, true) || hitag2_bf_needs_bruteforce(ff, true);
}

bool subghz_psa_bf_start(SubGhz* subghz) {
    if(g_bf_thread) return false;

    FlipperFormat* ff = subghz_get_current_signal_data(subghz);
    if(!ff) return false;

    if(psa_bf_needs_bruteforce(ff, true)) {
        PsaBfState* state = malloc(sizeof(PsaBfState));
        if(!state) {
            notification_message(subghz->notifications, &sequence_error);
            return false;
        }
        if(!psa_bf_state_from_flipper_format(state, ff)) {
            free(state);
            notification_message(subghz->notifications, &sequence_error);
            return false;
        }
        state->on_done = NULL;
        state->on_done_ctx = NULL;
        g_bf_state = state;
        g_bf_kind = SubGhzBfKindPsa;
        g_bf_thread = furi_thread_alloc_ex("PsaBf", 2048, psa_brute_force_thread_entry, state);
    } else if(hitag2_bf_needs_bruteforce(ff, true)) {
        Hitag2BfState* state = malloc(sizeof(Hitag2BfState));
        if(!state) {
            notification_message(subghz->notifications, &sequence_error);
            return false;
        }
        if(!hitag2_bf_state_from_flipper_format(state, ff)) {
            free(state);
            notification_message(subghz->notifications, &sequence_error);
            return false;
        }
        state->on_done = NULL;
        state->on_done_ctx = NULL;
        g_hitag2_state = state;
        g_bf_kind = SubGhzBfKindHitag2;
        g_bf_thread =
            furi_thread_alloc_ex("Hitag2Bf", 2048, hitag2_brute_force_thread_entry, state);
    } else {
        return false;
    }

    if(!g_bf_thread) {
        subghz_bf_free_states();
        notification_message(subghz->notifications, &sequence_error);
        return false;
    }
    furi_thread_start(g_bf_thread);
    g_bf_ui_active = true;
    subghz_psa_bf_show_progress(subghz);
    return true;
}

bool subghz_psa_bf_is_running(void) {
    return g_bf_thread != NULL;
}

void subghz_psa_bf_cancel(SubGhz* subghz) {
    UNUSED(subghz);
    if(g_bf_thread && subghz_bf_status() == PSA_BF_STATUS_RUNNING) {
        subghz_bf_set_cancel();
    }
}

bool subghz_psa_bf_handle_tick(SubGhz* subghz) {
    if(!g_bf_thread) return false;
    if(!g_bf_state && !g_hitag2_state) return false;

    uint8_t status = subghz_bf_status();
    if(status == PSA_BF_STATUS_IDLE || status == PSA_BF_STATUS_RUNNING) {
        subghz_psa_bf_show_progress(subghz);
    } else {
        subghz_psa_bf_finish(subghz);
    }
    return true;
}

bool subghz_psa_bf_handle_back(void) {
    const bool active = g_bf_ui_active || (g_bf_thread != NULL);
    if(g_bf_thread) {
        subghz_bf_set_cancel();
        furi_thread_join(g_bf_thread);
        furi_thread_free(g_bf_thread);
        g_bf_thread = NULL;
    }
    subghz_bf_free_states();
    g_bf_frame = 0;
    g_bf_ui_active = false;
    return active;
}

void subghz_psa_bf_on_exit(void) {
    if(g_bf_thread) {
        subghz_bf_set_cancel();
        furi_thread_join(g_bf_thread);
        furi_thread_free(g_bf_thread);
        g_bf_thread = NULL;
    }
    subghz_bf_free_states();
    g_bf_frame = 0;
    g_bf_ui_active = false;
}
