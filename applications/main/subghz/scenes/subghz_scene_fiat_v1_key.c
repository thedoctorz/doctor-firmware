#include "../subghz_i.h"
#include "../helpers/subghz_custom_event.h"
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/protocols/fiat_v1.h>
#include <lib/subghz/protocols/renault_v1.h>

static char hitag2_key_text[13];

static void subghz_scene_fiat_v1_key_text_callback(void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventSceneFiatV1KeyDone);
}

static void subghz_scene_fiat_v1_key_replay(SubGhz* subghz) {
    notification_message(subghz->notifications, &sequence_error);
    subghz_block_generic_global.hitag2_replay_only = true;
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventSceneFiatV1KeyTxStart);
    scene_manager_previous_scene(subghz->scene_manager);
}

void subghz_scene_fiat_v1_key_on_enter(void* context) {
    SubGhz* subghz = context;

    memset(hitag2_key_text, 0, sizeof(hitag2_key_text));
    text_input_reset(subghz->text_input);
    text_input_set_header_text(subghz->text_input, "12 hex, empty=replay");
    text_input_set_result_callback(
        subghz->text_input,
        subghz_scene_fiat_v1_key_text_callback,
        subghz,
        hitag2_key_text,
        sizeof(hitag2_key_text),
        true);
    text_input_set_minimum_length(subghz->text_input, 0);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdTextInput);
}

bool subghz_scene_fiat_v1_key_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SubGhzCustomEventSceneFiatV1KeyDone) {
        uint8_t key[6] = {0};
        FlipperFormat* ff = subghz_get_current_signal_data(subghz);
        const bool parsed = fiat_v1_flipper_parse_hitag2_key_text(hitag2_key_text, key) ||
                            renault_v1_flipper_parse_hitag2_key_text(hitag2_key_text, key);
        if(!ff || !parsed ||
           (!fiat_v1_flipper_accept_hitag2_key(ff, key) &&
            !renault_v1_flipper_accept_hitag2_key(ff, key))) {
            subghz_scene_fiat_v1_key_replay(subghz);
            return true;
        }
        subghz_block_generic_global.hitag2_replay_only = false;
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneFiatV1KeyTxStart);
        scene_manager_previous_scene(subghz->scene_manager);
        return true;
    }
    return false;
}

void subghz_scene_fiat_v1_key_on_exit(void* context) {
    SubGhz* subghz = context;
    text_input_reset(subghz->text_input);
}
