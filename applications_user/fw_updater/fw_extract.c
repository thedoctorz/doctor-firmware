#include "fw_extract.h"
#include "fw_pkg.h"
#include "gunzip.h"

#include <toolbox/tar/tar_archive.h>

#define TAG "FwExtract"

#define FW_EXTRACT_TEMP_TAR FW_PKG_UPDATE_DIR "/.fwupdate_extract.tar"

#define FW_EXTRACT_SPACE_MARGIN (1024UL * 1024UL)

typedef struct {
    FwExtractProgressCb callback;
    void* context;
    TarArchive* archive;
} FwExtractProgress;

static void fw_extract_decompress_cb(uint8_t percent, void* context) {
    FwExtractProgress* progress = context;
    if(progress->callback) {
        progress->callback(FwExtractStageDecompress, percent, progress->context);
    }
}

static bool fw_extract_unpack_cb(const char* name, bool is_directory, void* context) {
    UNUSED(name);
    UNUSED(is_directory);
    FwExtractProgress* progress = context;

    int32_t processed = 0;
    int32_t total = 0;
    if(progress->callback &&
       tar_archive_get_read_progress(progress->archive, &processed, &total) && (total > 0)) {
        progress->callback(
            FwExtractStageUnpack, (uint8_t)((processed * 100) / total), progress->context);
    }

    return true;
}

static bool fw_extract_unpacked_size(Storage* storage, const char* path, uint32_t* out_size) {
    File* file = storage_file_alloc(storage);
    bool success = false;

    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const uint64_t size = storage_file_size(file);
        uint8_t trailer[4];
        if((size > 8) && storage_file_seek(file, size - 4, true) &&
           (storage_file_read(file, trailer, sizeof(trailer)) == sizeof(trailer))) {
            *out_size = (uint32_t)trailer[0] | ((uint32_t)trailer[1] << 8) |
                        ((uint32_t)trailer[2] << 16) | ((uint32_t)trailer[3] << 24);
            success = true;
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    return success;
}

bool fw_extract_archive(
    Storage* storage,
    const char* archive_path,
    FwExtractProgressCb progress_cb,
    void* context,
    FuriString* error_out) {
    furi_assert(storage);
    furi_assert(archive_path);

    furi_string_reset(error_out);

    uint32_t unpacked_size = 0;
    if(!fw_extract_unpacked_size(storage, archive_path, &unpacked_size)) {
        furi_string_set(error_out, "Cannot read archive");
        return false;
    }

    uint64_t free_space = 0;
    if(storage_common_fs_info(storage, STORAGE_EXT_PATH_PREFIX, NULL, &free_space) != FSE_OK) {
        furi_string_set(error_out, "SD card is not ready");
        return false;
    }

    const uint64_t needed = ((uint64_t)unpacked_size * 2) + FW_EXTRACT_SPACE_MARGIN;
    if(free_space < needed) {
        furi_string_printf(
            error_out, "Needs %lu MB free", (unsigned long)(needed / (1024UL * 1024UL)));
        return false;
    }

    FwExtractProgress progress = {
        .callback = progress_cb,
        .context = context,
        .archive = NULL,
    };

    Gunzip* gunzip = gunzip_alloc(storage);
    const bool decompressed = gunzip_file(
        gunzip, archive_path, FW_EXTRACT_TEMP_TAR, fw_extract_decompress_cb, &progress);
    if(!decompressed) {
        furi_string_set(error_out, gunzip_last_error(gunzip));
    }
    gunzip_free(gunzip);

    bool success = false;

    if(decompressed) {
        TarArchive* archive = tar_archive_alloc(storage);
        progress.archive = archive;

        if(tar_archive_open(archive, FW_EXTRACT_TEMP_TAR, TarOpenModeRead)) {
            tar_archive_set_file_callback(archive, fw_extract_unpack_cb, &progress);
            success = tar_archive_unpack_to(archive, FW_PKG_UPDATE_DIR, NULL);
            if(!success) {
                furi_string_set(error_out, "Could not unpack archive");
            }
        } else {
            furi_string_set(error_out, "Decompressed data is not a tar");
        }

        tar_archive_free(archive);
    }

    storage_simply_remove(storage, FW_EXTRACT_TEMP_TAR);

    if(!success) {
        FURI_LOG_E(TAG, "%s: %s", archive_path, furi_string_get_cstr(error_out));
    }

    return success;
}
