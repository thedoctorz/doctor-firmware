#pragma once

#include <furi.h>
#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FW_PKG_UPDATE_DIR    EXT_PATH("update")
#define FW_PKG_POINTER_FILE  EXT_PATH(".fupdate")
#define FW_PKG_MANIFEST_NAME "update.fuf"

#define FW_PKG_MAX       24
#define FW_PKG_PATH_LEN  160
#define FW_PKG_NAME_LEN  48
#define FW_PKG_LABEL_LEN 48
#define FW_PKG_FILE_LEN  48

#define FW_PKG_MIN_MANIFEST_VERSION 2

typedef enum {
    FwPkgKindFolder,
    FwPkgKindArchive,
} FwPkgKind;

typedef enum {
    FwRadioActionNone,
    FwRadioActionKeep,
    FwRadioActionReflash,
} FwRadioAction;

typedef union {
    uint8_t raw[6];
    struct {
        uint8_t major;
        uint8_t minor;
        uint8_t sub;
        uint8_t branch;
        uint8_t release;
        uint8_t type;
    } v;
} FwRadioVersion;

typedef struct {
    FwPkgKind kind;
    char path[FW_PKG_PATH_LEN];
    char name[FW_PKG_NAME_LEN];
    char label[FW_PKG_LABEL_LEN];
    uint64_t size;
    uint32_t target;
    bool valid;
    bool has_firmware;
    bool has_radio;
    bool has_resources;
    bool armed;
    bool current;
} FwPkgEntry;

typedef struct {
    uint32_t manifest_version;
    uint32_t target;
    char info[FW_PKG_LABEL_LEN];
    char loader[FW_PKG_FILE_LEN];
    uint32_t loader_crc;
    char firmware[FW_PKG_FILE_LEN];
    char radio[FW_PKG_FILE_LEN];
    uint32_t radio_crc;
    FwRadioVersion radio_version;
    char resources[FW_PKG_FILE_LEN];
    bool has_ob;
    uint8_t ob_sfsa;
    bool valid;
} FwManifest;

#define FW_CHECK_MAX_NOTES 5
#define FW_CHECK_NOTE_LEN  56

typedef struct {
    bool installable;
    FwRadioAction radio_action;
    char radio_from[28];
    char radio_to[28];
    uint8_t battery;
    bool charging;
    uint8_t battery_required;
    char blockers[FW_CHECK_MAX_NOTES][FW_CHECK_NOTE_LEN];
    uint8_t blocker_count;
    char warnings[FW_CHECK_MAX_NOTES][FW_CHECK_NOTE_LEN];
    uint8_t warning_count;
} FwCheck;

size_t fw_pkg_scan(Storage* storage, FwPkgEntry* entries, size_t max_entries);

bool fw_pkg_manifest_load(Storage* storage, const char* manifest_path, FwManifest* manifest);

void fw_pkg_check(
    Storage* storage,
    const char* manifest_path,
    const FwManifest* manifest,
    FwCheck* check);

const char* fw_pkg_arm(Storage* storage, const char* manifest_path);

void fw_pkg_disarm(Storage* storage);

bool fw_pkg_get_armed_path(Storage* storage, FuriString* out_path);

const char* fw_pkg_radio_type_name(uint8_t stack_type);

#ifdef __cplusplus
}
#endif
