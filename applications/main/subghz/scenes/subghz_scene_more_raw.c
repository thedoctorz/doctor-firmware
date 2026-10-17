#include "../subghz_i.h"
#include <loader/loader.h>

#define SUBGHZ_RAW_EDIT_APP_PATH "/ext/apps/Sub-GHz/subghz_raw_edit.fap"

enum SubmenuIndex {
    SubmenuIndexDecode,
    SubmenuIndexEditSignal,
    SubmenuIndexEdit,
    SubmenuIndexDelete,
    SubmenuIndexSetFrequency,
};

void subghz_scene_more_raw_submenu_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

void subghz_scene_more_raw_on_enter(void* context) {
    SubGhz* subghz = context;

    if(!subghz_path_is_file(subghz->file_path)) {
        subghz_restore_raw_file_path(subghz);
    }

    submenu_add_item(
        subghz->submenu,
        "Decode",
        SubmenuIndexDecode,
        subghz_scene_more_raw_submenu_callback,
        subghz);

    submenu_add_item(
        subghz->submenu,
        "Edit",
        SubmenuIndexEditSignal,
        subghz_scene_more_raw_submenu_callback,
        subghz);

    submenu_add_item(
        subghz->submenu,
        "Set Frequency",
        SubmenuIndexSetFrequency,
        subghz_scene_more_raw_submenu_callback,
        subghz);

    submenu_add_item(
        subghz->submenu,
        "Rename",
        SubmenuIndexEdit,
        subghz_scene_more_raw_submenu_callback,
        subghz);

    submenu_add_item(
        subghz->submenu,
        "Delete",
        SubmenuIndexDelete,
        subghz_scene_more_raw_submenu_callback,
        subghz);

    submenu_set_selected_item(
        subghz->submenu, scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneMoreRAW));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_more_raw_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubmenuIndexDelete) {
            if(subghz_file_available(subghz)) {
                scene_manager_set_scene_state(
                    subghz->scene_manager, SubGhzSceneReadRAW, SubGhzCustomEventManagerNoSet);
                scene_manager_set_scene_state(
                    subghz->scene_manager, SubGhzSceneMoreRAW, SubmenuIndexDelete);
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneDeleteRAW);
                return true;
            } else {
                if(!scene_manager_search_and_switch_to_previous_scene(
                       subghz->scene_manager, SubGhzSceneStart)) {
                    scene_manager_stop(subghz->scene_manager);
                    view_dispatcher_stop(subghz->view_dispatcher);
                }
            }
        } else if(event.event == SubmenuIndexSetFrequency) {
            if(subghz_file_available(subghz) && subghz_path_is_file(subghz->file_path)) {
                scene_manager_set_scene_state(
                    subghz->scene_manager, SubGhzSceneMoreRAW, SubmenuIndexSetFrequency);
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSetFrequency);
                return true;
            } else {
                if(!scene_manager_search_and_switch_to_previous_scene(
                       subghz->scene_manager, SubGhzSceneStart)) {
                    scene_manager_stop(subghz->scene_manager);
                    view_dispatcher_stop(subghz->view_dispatcher);
                }
            }
        } else if(event.event == SubmenuIndexEdit) {
            if(subghz_file_available(subghz)) {
                furi_string_reset(subghz->file_path_tmp);
                scene_manager_set_scene_state(
                    subghz->scene_manager, SubGhzSceneMoreRAW, SubmenuIndexEdit);
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSaveName);
                return true;
            } else {
                if(!scene_manager_search_and_switch_to_previous_scene(
                       subghz->scene_manager, SubGhzSceneStart)) {
                    scene_manager_stop(subghz->scene_manager);
                    view_dispatcher_stop(subghz->view_dispatcher);
                }
            }
        } else if(event.event == SubmenuIndexEditSignal) {
            if(subghz_file_available(subghz)) {
                Loader* loader = furi_record_open(RECORD_LOADER);
                loader_enqueue_launch(
                    loader,
                    SUBGHZ_RAW_EDIT_APP_PATH,
                    furi_string_get_cstr(subghz->file_path),
                    LoaderDeferredLaunchFlagGui);
                furi_record_close(RECORD_LOADER);
                scene_manager_stop(subghz->scene_manager);
                view_dispatcher_stop(subghz->view_dispatcher);
                return true;
            } else {
                if(!scene_manager_search_and_switch_to_previous_scene(
                       subghz->scene_manager, SubGhzSceneStart)) {
                    scene_manager_stop(subghz->scene_manager);
                    view_dispatcher_stop(subghz->view_dispatcher);
                }
            }
        } else if(event.event == SubmenuIndexDecode) {
            if(subghz_file_available(subghz)) {
                scene_manager_set_scene_state(
                    subghz->scene_manager, SubGhzSceneMoreRAW, SubmenuIndexDecode);
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneDecodeRAW);
                return true;
            } else {
                if(!scene_manager_search_and_switch_to_previous_scene(
                       subghz->scene_manager, SubGhzSceneStart)) {
                    scene_manager_stop(subghz->scene_manager);
                    view_dispatcher_stop(subghz->view_dispatcher);
                }
            }
        }
    }
    return false;
}

void subghz_scene_more_raw_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}
