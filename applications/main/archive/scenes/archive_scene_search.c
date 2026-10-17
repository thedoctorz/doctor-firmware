#include "../archive_i.h"
#include "../helpers/archive_files.h"
#include "../helpers/archive_browser.h"
#include "archive/views/archive_browser_view.h"
#include <toolbox/dir_walk.h>
#include <storage/storage.h>
#include <string.h>

#define TAG "Archive"

#define SCENE_SEARCH_CUSTOM_EVENT (0UL)

void archive_scene_search_text_input_callback(void* context) {
    ArchiveApp* archive = (ArchiveApp*)context;
    view_dispatcher_send_custom_event(archive->view_dispatcher, SCENE_SEARCH_CUSTOM_EVENT);
}

void archive_scene_search_on_enter(void* context) {
    ArchiveApp* archive = context;

    TextInput* text_input = archive->text_input;
    strlcpy(archive->text_store, "", MAX_NAME_LEN);
    text_input_set_header_text(text_input, "Search for files:");

    text_input_set_result_callback(
        text_input,
        archive_scene_search_text_input_callback,
        context,
        archive->text_store,
        MAX_NAME_LEN,
        false);

    view_dispatcher_switch_to_view(archive->view_dispatcher, ArchiveViewTextInput);
}

static int32_t archive_scene_search_dirwalk(void* context) {
    furi_assert(context);
    ArchiveApp* archive = context;

    uint32_t count = 1;
    DirWalk* dir_walk = dir_walk_alloc(furi_record_open(RECORD_STORAGE));
    FuriString* path = furi_string_alloc();
    FileInfo fileinfo;

    if(dir_walk_open(dir_walk, STORAGE_EXT_PATH_PREFIX)) {
        while(archive->browser->search_running) {
            DirWalkResult result = dir_walk_read(dir_walk, path, &fileinfo);
            if(result == DirWalkError) {
                archive_add_app_item(archive->browser, "/app:search/Error while searching!");
                archive_set_item_count(archive->browser, ++count);
                break;
            }
            if(result == DirWalkLast) {
                if(count == 1) {
                    archive_add_app_item(archive->browser, "/app:search/No results found!");
                    archive_set_item_count(archive->browser, ++count);
                }
                break;
            }
            if(!file_info_is_dir(&fileinfo)) {
                if(!archive->browser->search_running ||
                   !furi_string_start_with_str(archive->browser->path, "/app:search")) {
                    break;
                }
                size_t slash = furi_string_search_rchar(path, '/');
                const char* filename = furi_string_get_cstr(path);
                if(slash != FURI_STRING_FAILURE) {
                    filename += slash + 1;
                }
                if(strcasestr(filename, archive->text_store) != NULL) {
                    archive_add_file_item(archive->browser, false, furi_string_get_cstr(path));
                    archive_set_item_count(archive->browser, ++count);
                }
            }
        }
    } else {
        archive_add_app_item(archive->browser, "/app:search/Error while searching!");
        archive_set_item_count(archive->browser, ++count);
    }

    if(archive->browser->search_running &&
       furi_string_start_with_str(archive->browser->path, "/app:search")) {
        ArchiveFile_t* first = archive_get_file_at(archive->browser, 0);
        if(first) {
            furi_string_set(first->path, "/app:search/Search for files");
        }
    }
    archive->browser->search_running = false;
    scene_manager_set_scene_state(archive->scene_manager, ArchiveAppSceneSearch, false);

    furi_string_free(path);
    dir_walk_free(dir_walk);
    furi_record_close(RECORD_STORAGE);
    return 0;
}

bool archive_scene_search_on_event(void* context, SceneManagerEvent event) {
    ArchiveApp* archive = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SCENE_SEARCH_CUSTOM_EVENT) {
            archive_search_stop(archive);
            if(!furi_string_start_with_str(archive->browser->path, "/app:search")) {
                furi_string_set(archive->search_restore_path, archive->browser->path);
            }
            archive_file_array_rm_all(archive->browser);
            archive_add_app_item(archive->browser, "/app:search/Cancel search");
            archive_set_item_count(archive->browser, 1);
            furi_string_set(archive->browser->path, "/app:search");

            archive->browser->search_running = true;
            scene_manager_set_scene_state(archive->scene_manager, ArchiveAppSceneSearch, true);
            archive->search_thread = furi_thread_alloc_ex(
                "ArchiveSearchWorker", 2048, archive_scene_search_dirwalk, archive);
            furi_thread_start(archive->search_thread);

            scene_manager_previous_scene(archive->scene_manager);
            consumed = true;
        }
    }
    return consumed;
}

void archive_scene_search_on_exit(void* context) {
    ArchiveApp* archive = context;
    text_input_reset(archive->text_input);
}
