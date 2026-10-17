#include "archive_apps.h"
#include "archive_browser.h"

static const char* const known_apps[] = {
    [ArchiveAppTypeSetting] = "setting",
    [ArchiveAppTypeSearch] = "search",
};

ArchiveAppTypeEnum archive_get_app_type(const char* path) {
    const char* app_name = strchr(path, ':');
    if(app_name == NULL) {
        return ArchiveAppTypeUnknown;
    }
    app_name++;

    for(size_t i = 0; i < COUNT_OF(known_apps); i++) { //-V1008
        if(strncmp(app_name, known_apps[i], strlen(known_apps[i])) == 0) {
            return i;
        }
    }
    return ArchiveAppTypeUnknown;
}

bool archive_app_is_available(void* context, const char* path) {
    UNUSED(context);
    furi_assert(path);

    ArchiveAppTypeEnum app = archive_get_app_type(path);

    if(app == ArchiveAppTypeSetting) {
        return true;
    } else if(app == ArchiveAppTypeSearch) {
        return true;
    } else {
        return false;
    }
}

bool archive_app_read_dir(void* context, const char* path) {
    furi_assert(context);
    furi_assert(path);
    ArchiveBrowserView* browser = context;

    ArchiveAppTypeEnum app = archive_get_app_type(path);

    if(app == ArchiveAppTypeSetting) {
        archive_file_array_rm_all(browser);
        archive_add_app_item(browser, path);
        return true;
    } else if(app == ArchiveAppTypeSearch) {
        return true;
    } else {
        return false;
    }
}

void archive_app_delete_file(void* context, const char* path) {
    furi_assert(context);
    furi_assert(path);
    ArchiveBrowserView* browser = context;

    ArchiveAppTypeEnum app = archive_get_app_type(path);
    bool res = false;

    if(app == ArchiveAppTypeSetting) {
        // can't delete a setting!
        UNUSED(res);
    }

    if(res) {
        archive_file_array_rm_selected(browser);
    }
}
