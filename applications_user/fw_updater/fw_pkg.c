#include "fw_pkg.h"

#include <furi_hal.h>
#include <furi_hal_power.h>
#include <furi_hal_bt.h>
#include <furi_hal_version.h>
#include <ble_glue.h>
#include <flipper_format/flipper_format.h>
#include <toolbox/crc32_calc.h>
#include <toolbox/path.h>
#include <toolbox/version.h>
#include <string.h>

#define TAG "FwUpdater"

#define MANIFEST_FILETYPE "Flipper firmware upgrade configuration"

#define KEY_INFO       "Info"
#define KEY_TARGET     "Target"
#define KEY_LOADER     "Loader"
#define KEY_LOADER_CRC "Loader CRC"
#define KEY_FIRMWARE   "Firmware"
#define KEY_RADIO      "Radio"
#define KEY_RADIO_VER  "Radio version"
#define KEY_RADIO_CRC  "Radio CRC"
#define KEY_RESOURCES  "Resources"
#define KEY_OB_REF     "OB reference"

#define OB_RAW_SIZE_BYTES   0x80
#define OB_SFSA_BYTE_OFFSET (14 * 8)

#define STACK_TYPE_BLE_FULL         0x01
#define STACK_TYPE_BLE_HCI          0x02
#define STACK_TYPE_BLE_LIGHT        0x03
#define STACK_TYPE_BLE_BEACON       0x04
#define STACK_TYPE_BLE_BASIC        0x05
#define STACK_TYPE_BLE_FULL_EXT_ADV 0x06
#define STACK_TYPE_BLE_HCI_EXT_ADV  0x07

#define BATTERY_MIN_FIRMWARE 20
#define BATTERY_MIN_RADIO    50

#define UPDATE_MIN_FREE_SPACE (256UL * 1024UL)

const char* fw_pkg_radio_type_name(uint8_t stack_type) {
    switch(stack_type) {
    case STACK_TYPE_BLE_FULL:
        return "Full";
    case STACK_TYPE_BLE_HCI:
        return "HCI";
    case STACK_TYPE_BLE_LIGHT:
        return "Light";
    case STACK_TYPE_BLE_BEACON:
        return "Beacon";
    case STACK_TYPE_BLE_BASIC:
        return "Basic";
    case STACK_TYPE_BLE_FULL_EXT_ADV:
        return "Full+Adv";
    case STACK_TYPE_BLE_HCI_EXT_ADV:
        return "HCI+Adv";
    default:
        return "Stack";
    }
}

static uint8_t fw_pkg_device_sfsa(void) {
    return (uint8_t)((READ_BIT(FLASH->SFR, FLASH_SFR_SFSA) >> FLASH_SFR_SFSA_Pos) & 0xFF);
}

static void fw_pkg_note_add(char notes[][FW_CHECK_NOTE_LEN], uint8_t* count, const char* text) {
    if(*count >= FW_CHECK_MAX_NOTES) {
        return;
    }
    snprintf(notes[*count], FW_CHECK_NOTE_LEN, "%s", text);
    (*count)++;
}

static void fw_pkg_copy_string(char* dest, size_t dest_size, FuriString* source) {
    snprintf(dest, dest_size, "%s", furi_string_get_cstr(source));
}

bool fw_pkg_manifest_load(Storage* storage, const char* manifest_path, FwManifest* manifest) {
    memset(manifest, 0, sizeof(FwManifest));

    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* value = furi_string_alloc();
    FuriString* filetype = furi_string_alloc();
    uint8_t* ob_reference = malloc(OB_RAW_SIZE_BYTES);

    do {
        if(!flipper_format_file_open_existing(file, manifest_path)) {
            break;
        }

        if(!flipper_format_read_header(file, filetype, &manifest->manifest_version) ||
           furi_string_cmp_str(filetype, MANIFEST_FILETYPE) != 0) {
            break;
        }

        if(!flipper_format_read_string(file, KEY_INFO, value)) {
            break;
        }
        fw_pkg_copy_string(manifest->info, sizeof(manifest->info), value);

        if(!flipper_format_read_uint32(file, KEY_TARGET, &manifest->target, 1)) {
            break;
        }

        if(!flipper_format_read_string(file, KEY_LOADER, value)) {
            break;
        }
        fw_pkg_copy_string(manifest->loader, sizeof(manifest->loader), value);

        if(!flipper_format_read_hex(
               file, KEY_LOADER_CRC, (uint8_t*)&manifest->loader_crc, sizeof(uint32_t))) {
            break;
        }

        if(flipper_format_read_string(file, KEY_FIRMWARE, value)) {
            fw_pkg_copy_string(manifest->firmware, sizeof(manifest->firmware), value);
        }
        if(flipper_format_read_string(file, KEY_RADIO, value)) {
            fw_pkg_copy_string(manifest->radio, sizeof(manifest->radio), value);
        }
        flipper_format_read_hex(
            file, KEY_RADIO_VER, manifest->radio_version.raw, sizeof(manifest->radio_version.raw));
        flipper_format_read_hex(
            file, KEY_RADIO_CRC, (uint8_t*)&manifest->radio_crc, sizeof(uint32_t));
        if(flipper_format_read_string(file, KEY_RESOURCES, value)) {
            fw_pkg_copy_string(manifest->resources, sizeof(manifest->resources), value);
        }

        if(flipper_format_read_hex(file, KEY_OB_REF, ob_reference, OB_RAW_SIZE_BYTES)) {
            manifest->has_ob = true;
            manifest->ob_sfsa = ob_reference[OB_SFSA_BYTE_OFFSET];
        }

        manifest->valid = manifest->firmware[0] || manifest->radio[0] || manifest->resources[0];
    } while(false);

    free(ob_reference);
    furi_string_free(filetype);
    furi_string_free(value);
    flipper_format_free(file);

    return manifest->valid;
}

bool fw_pkg_get_armed_path(Storage* storage, FuriString* out_path) {
    furi_string_reset(out_path);

    File* file = storage_file_alloc(storage);
    char* buffer = malloc(FW_PKG_PATH_LEN);

    if(storage_file_open(file, FW_PKG_POINTER_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const size_t read = storage_file_read(file, buffer, FW_PKG_PATH_LEN - 1);
        if(read && (read < FW_PKG_PATH_LEN - 1)) {
            buffer[read] = '\0';
            furi_string_set(out_path, buffer);
        }
    }

    free(buffer);
    storage_file_free(file);
    return !furi_string_empty(out_path);
}

static char fw_pkg_tolower(char c) {
    return ((c >= 'A') && (c <= 'Z')) ? (char)(c - 'A' + 'a') : c;
}

static bool fw_pkg_isalnum(char c) {
    return ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'));
}

static bool fw_pkg_has_digit(const char* s) {
    for(; *s; s++) {
        if((*s >= '0') && (*s <= '9')) {
            return true;
        }
    }
    return false;
}

static bool fw_pkg_looks_like_version(const char* s) {
    if(!s[0] || !fw_pkg_has_digit(s)) {
        return false;
    }
    for(const char* p = s; *p; p++) {
        const char c = *p;
        if(((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'))) {
            return false;
        }
        if(!fw_pkg_isalnum(c) && (c != '.') && (c != '-') && (c != '_') && (c != ' ')) {
            return false;
        }
    }
    return true;
}

static bool fw_pkg_looks_like_ofw(const char* s) {
    return s && (s[0] == '1') && (s[1] == '.') && fw_pkg_has_digit(s + 2);
}

static const char* fw_pkg_find_ci(const char* haystack, const char* needle) {
    if(!needle[0]) {
        return haystack;
    }
    for(const char* h = haystack; *h; h++) {
        const char* a = h;
        const char* b = needle;
        while(*a && *b && (fw_pkg_tolower(*a) == fw_pkg_tolower(*b))) {
            a++;
            b++;
        }
        if(*b == '\0') {
            return h;
        }
    }
    return NULL;
}

static bool fw_pkg_token_in(const char* haystack, const char* token) {
    const char* found = fw_pkg_find_ci(haystack, token);
    while(found) {
        const bool left_ok = (found == haystack) || !fw_pkg_isalnum(found[-1]);
        const char* after = found + strlen(token);
        const bool right_ok = (*after == '\0') || !fw_pkg_isalnum(*after);
        if(left_ok && right_ok) {
            return true;
        }
        found = fw_pkg_find_ci(found + 1, token);
    }
    return false;
}

static void fw_pkg_trim_separators(char* s) {
    char* start = s;
    while(*start && ((*start == '-') || (*start == '_') || (*start == '.') || (*start == ' '))) {
        start++;
    }
    if(start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    size_t length = strlen(s);
    while(length && ((s[length - 1] == '-') || (s[length - 1] == '_') || (s[length - 1] == '.') ||
                     (s[length - 1] == ' '))) {
        s[--length] = '\0';
    }
}

static void fw_pkg_strip_suffix_ci(char* name, const char* suffix) {
    const size_t name_len = strlen(name);
    const size_t suffix_len = strlen(suffix);
    if(name_len <= suffix_len) {
        return;
    }

    char* tail = name + name_len - suffix_len;
    if(fw_pkg_find_ci(tail, suffix) == tail) {
        *tail = '\0';
    }
}

static void fw_pkg_strip_prefix_ci(char* name, const char* prefix) {
    const size_t prefix_len = strlen(prefix);
    if(fw_pkg_find_ci(name, prefix) == name) {
        memmove(name, name + prefix_len, strlen(name + prefix_len) + 1);
    }
}

static void fw_pkg_clean_source_name(char* dest, size_t dest_size, const char* source) {
    snprintf(dest, dest_size, "%s", source);

    fw_pkg_strip_suffix_ci(dest, ".tar.gz");
    fw_pkg_strip_suffix_ci(dest, ".tgz");
    fw_pkg_strip_suffix_ci(dest, ".zip");

    for(size_t before = strlen(dest) + 1; strlen(dest) < before;) {
        before = strlen(dest);
        fw_pkg_strip_prefix_ci(dest, "flipper-z-");
        fw_pkg_strip_prefix_ci(dest, "flipper_z_");
        fw_pkg_strip_prefix_ci(dest, "f18-update-");
        fw_pkg_strip_prefix_ci(dest, "f7-update-");
        fw_pkg_strip_prefix_ci(dest, "f7_update_");
        fw_pkg_strip_prefix_ci(dest, "update-");
    }
}

static void fw_pkg_title_case(char* dest, size_t dest_size, const char* src) {
    bool cap = true;
    size_t o = 0;
    for(size_t i = 0; src[i] && (o + 1 < dest_size); i++) {
        char c = src[i];
        if((c == '-') || (c == '_') || (c == '.')) {
            dest[o++] = ' ';
            cap = true;
            continue;
        }
        if(cap && (c >= 'a') && (c <= 'z')) {
            c = (char)(c - 'a' + 'A');
        } else if(!cap && (c >= 'A') && (c <= 'Z')) {
            c = (char)(c - 'A' + 'a');
        }
        dest[o++] = c;
        cap = false;
    }
    dest[o] = '\0';
}

static void fw_pkg_strip_token(char* s, const char* token) {
    char* found = (char*)fw_pkg_find_ci(s, token);
    if(!found) {
        return;
    }
    const size_t token_len = strlen(token);
    memmove(found, found + token_len, strlen(found + token_len) + 1);
    fw_pkg_trim_separators(s);
}

typedef struct {
    const char* token;
    const char* name;
} FwPkgKnownName;

static const FwPkgKnownName fw_pkg_known_names[] = {
    {"the_doctors_firmware", "The Doctor"},
    {"doctors_firmware", "The Doctor"},
    {"doctor-firmware", "The Doctor"},
    {"doctor_firmware", "The Doctor"},
    {"thedoctorz", "The Doctor"},
    {"the_doctor", "The Doctor"},
    {"roguemaster", "RogueMaster"},
    {"momentum", "Momentum"},
    {"official", "Official"},
    {"xtreme", "Xtreme"},
    {"mntm", "Momentum"},
    {"xfw", "Xtreme"},
    {"ofw", "Official"},
    {"local", "The Doctor"},
};

static const FwPkgKnownName fw_pkg_origin_tokens[] = {
    {"The_Doctors_Firmware", "The Doctor"},
    {"RogueMaster", "RogueMaster"},
    {"Momentum", "Momentum"},
    {"Xtreme", "Xtreme"},
    {"Official", "Official"},
};

static const char* fw_pkg_match_known_name(const char* text, const char** token_out) {
    for(size_t i = 0; i < COUNT_OF(fw_pkg_known_names); i++) {
        if(fw_pkg_token_in(text, fw_pkg_known_names[i].token)) {
            if(token_out) {
                *token_out = fw_pkg_known_names[i].token;
            }
            return fw_pkg_known_names[i].name;
        }
    }
    return NULL;
}

static bool fw_pkg_find_mem_ci(const uint8_t* hay, size_t hay_len, const char* needle) {
    const size_t nlen = strlen(needle);
    if(!nlen || (nlen > hay_len)) {
        return false;
    }

    for(size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;
        while((j < nlen) && (fw_pkg_tolower((char)hay[i + j]) == fw_pkg_tolower(needle[j]))) {
            j++;
        }
        if(j != nlen) {
            continue;
        }
        const bool left_ok = (i == 0) || !fw_pkg_isalnum((char)hay[i - 1]);
        const bool right_ok = ((i + nlen) == hay_len) || !fw_pkg_isalnum((char)hay[i + nlen]);
        if(left_ok && right_ok) {
            return true;
        }
    }

    return false;
}

#define FW_PKG_ORIGIN_TAIL    (96UL * 1024UL)
#define FW_PKG_ORIGIN_CHUNK   2048UL
#define FW_PKG_ORIGIN_OVERLAP 16UL

static bool fw_pkg_probe_image_origin(
    Storage* storage,
    const char* manifest_path,
    const char* firmware_file,
    char* out_name,
    size_t out_size) {
    if(!firmware_file[0]) {
        return false;
    }

    FuriString* path = furi_string_alloc();
    path_extract_dirname(manifest_path, path);
    path_append(path, firmware_file);

    File* file = storage_file_alloc(storage);
    uint8_t* chunk = malloc(FW_PKG_ORIGIN_CHUNK + FW_PKG_ORIGIN_OVERLAP);
    bool found = false;

    do {
        if(!chunk) {
            break;
        }
        if(!storage_file_open(file, furi_string_get_cstr(path), FSAM_READ, FSOM_OPEN_EXISTING)) {
            break;
        }

        const uint64_t size = storage_file_size(file);
        if(size < 32) {
            break;
        }

        const uint32_t tail = (size > FW_PKG_ORIGIN_TAIL) ? (uint32_t)FW_PKG_ORIGIN_TAIL :
                                                            (uint32_t)size;
        if(!storage_file_seek(file, (uint32_t)(size - tail), true)) {
            break;
        }

        memset(chunk, 0, FW_PKG_ORIGIN_OVERLAP);
        uint32_t prefix = 0;
        uint32_t left = tail;
        while(left && !found) {
            const uint32_t n = (left > FW_PKG_ORIGIN_CHUNK) ? (uint32_t)FW_PKG_ORIGIN_CHUNK : left;
            if(storage_file_read(file, chunk + prefix, n) != n) {
                break;
            }

            for(size_t i = 0; i < COUNT_OF(fw_pkg_origin_tokens); i++) {
                if(fw_pkg_find_mem_ci(chunk, n + prefix, fw_pkg_origin_tokens[i].token)) {
                    snprintf(out_name, out_size, "%s", fw_pkg_origin_tokens[i].name);
                    found = true;
                    break;
                }
            }

            const uint32_t total = n + prefix;
            if(total >= FW_PKG_ORIGIN_OVERLAP) {
                memmove(chunk, chunk + total - FW_PKG_ORIGIN_OVERLAP, FW_PKG_ORIGIN_OVERLAP);
                prefix = (uint32_t)FW_PKG_ORIGIN_OVERLAP;
            } else {
                prefix = total;
            }
            left -= n;
        }
    } while(false);

    free(chunk);
    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(path);
    return found;
}

static void fw_pkg_strip_version_tail(char* s) {
    for(char* p = s; *p; p++) {
        if(((*p == '-') || (*p == '_') || (*p == '.')) && (p != s) && (p[1] >= '0') &&
           (p[1] <= '9')) {
            *p = '\0';
            return;
        }
    }
}

static void fw_pkg_make_label(
    char* name,
    size_t name_size,
    char* label,
    size_t label_size,
    const char* info,
    const char* source_name,
    const char* probed_origin) {
    char cleaned[FW_PKG_LABEL_LEN];
    fw_pkg_clean_source_name(cleaned, sizeof(cleaned), source_name);

    const char* token = NULL;
    const char* known = NULL;
    if(probed_origin && probed_origin[0]) {
        known = probed_origin;
    } else {
        known = fw_pkg_match_known_name(info, &token);
        if(!known) {
            known = fw_pkg_match_known_name(cleaned, &token);
        }
    }

    char named[FW_PKG_LABEL_LEN];
    snprintf(named, sizeof(named), "%s", cleaned);
    fw_pkg_strip_version_tail(named);

    if(known) {
        snprintf(name, name_size, "%s", known);
    } else if(fw_pkg_looks_like_ofw(info) || fw_pkg_looks_like_ofw(cleaned)) {
        snprintf(name, name_size, "%s", "Official");
    } else if(named[0] && !fw_pkg_looks_like_version(named)) {
        fw_pkg_title_case(name, name_size, named);
    } else if(info[0]) {
        snprintf(name, name_size, "%s", info);
    } else if(cleaned[0]) {
        snprintf(name, name_size, "%s", cleaned);
    } else {
        snprintf(name, name_size, "%s", "Firmware");
    }

    char version[FW_PKG_LABEL_LEN];
    version[0] = '\0';
    if(info[0] && fw_pkg_has_digit(info)) {
        snprintf(version, sizeof(version), "%s", info);
    } else if(cleaned[0] && fw_pkg_has_digit(cleaned)) {
        snprintf(version, sizeof(version), "%s", cleaned);
    }

    if(token && version[0]) {
        fw_pkg_strip_token(version, token);
        if(!fw_pkg_has_digit(version)) {
            version[0] = '\0';
        }
    }

    if(version[0] && (strcasecmp(name, version) != 0)) {
        size_t used = strlen(name);
        if(used > label_size - 1U) used = label_size - 1U;
        memcpy(label, name, used);
        if(used + 1U < label_size) {
            label[used++] = ' ';
            size_t room = label_size - used - 1U;
            size_t version_len = strlen(version);
            if(version_len > room) version_len = room;
            memcpy(label + used, version, version_len);
            used += version_len;
        }
        label[used] = '\0';
    } else {
        snprintf(label, label_size, "%s", name);
    }
}

static bool fw_pkg_entry_from_folder(
    Storage* storage,
    const char* dir_name,
    const char* armed_path,
    FwPkgEntry* entry) {
    memset(entry, 0, sizeof(FwPkgEntry));
    entry->kind = FwPkgKindFolder;

    if(snprintf(
           entry->path,
           sizeof(entry->path),
           "%s/%s/%s",
           FW_PKG_UPDATE_DIR,
           dir_name,
           FW_PKG_MANIFEST_NAME) >= (int)sizeof(entry->path)) {
        return false;
    }

    FwManifest manifest;
    if(!fw_pkg_manifest_load(storage, entry->path, &manifest)) {
        return false;
    }

    entry->valid = true;
    entry->target = manifest.target;
    entry->has_firmware = manifest.firmware[0] != '\0';
    entry->has_radio = manifest.radio[0] != '\0';
    entry->has_resources = manifest.resources[0] != '\0';
    entry->armed = armed_path && (strcmp(armed_path, entry->path) == 0);

    char probed[FW_PKG_NAME_LEN];
    probed[0] = '\0';
    fw_pkg_probe_image_origin(storage, entry->path, manifest.firmware, probed, sizeof(probed));
    fw_pkg_make_label(
        entry->name,
        sizeof(entry->name),
        entry->label,
        sizeof(entry->label),
        manifest.info,
        dir_name,
        probed);

    return true;
}

static bool fw_pkg_entry_from_archive(const char* file_name, uint64_t size, FwPkgEntry* entry) {
    memset(entry, 0, sizeof(FwPkgEntry));
    entry->kind = FwPkgKindArchive;
    entry->size = size;

    if(snprintf(entry->path, sizeof(entry->path), "%s/%s", FW_PKG_UPDATE_DIR, file_name) >=
       (int)sizeof(entry->path)) {
        return false;
    }

    fw_pkg_make_label(
        entry->name, sizeof(entry->name), entry->label, sizeof(entry->label), "", file_name, NULL);
    return true;
}

static bool fw_pkg_name_has_tgz_suffix(const char* name) {
    const size_t length = strlen(name);
    static const char* suffixes[] = {".tgz", ".tar.gz"};

    for(size_t i = 0; i < COUNT_OF(suffixes); i++) {
        const size_t suffix_length = strlen(suffixes[i]);
        if((length > suffix_length) &&
           (strcasecmp(name + length - suffix_length, suffixes[i]) == 0)) {
            return true;
        }
    }

    return false;
}

static void fw_pkg_mark_current(FwPkgEntry* entries, size_t count) {
    const Version* running = furi_hal_version_get_firmware_version();
    if(!running || !count) {
        return;
    }

    const char* origin = version_get_firmware_origin(running);
    const char* version = version_get_version(running);
    const char* token = NULL;
    const char* known = origin ? fw_pkg_match_known_name(origin, &token) : NULL;

    char running_name[FW_PKG_NAME_LEN];
    if(known) {
        snprintf(running_name, sizeof(running_name), "%s", known);
    } else if(origin && origin[0]) {
        fw_pkg_title_case(running_name, sizeof(running_name), origin);
    } else {
        return;
    }

    int32_t best = -1;
    int best_score = 0;
    for(size_t i = 0; i < count; i++) {
        if(strcasecmp(entries[i].name, running_name) != 0) {
            continue;
        }

        int score = 1;
        if(version && version[0] && fw_pkg_token_in(entries[i].label, version)) {
            score += 2;
        }
        if(entries[i].kind == FwPkgKindFolder) {
            score += 1;
        }
        if(score > best_score) {
            best = (int32_t)i;
            best_score = score;
        }
    }

    if(best >= 0) {
        entries[best].current = true;
    }
}

static void fw_pkg_disambiguate_names(FwPkgEntry* entries, size_t count) {
    for(size_t i = 0; i < count; i++) {
        size_t same = 0;
        for(size_t j = 0; j < count; j++) {
            if(strcasecmp(entries[i].name, entries[j].name) == 0) {
                same++;
            }
        }
        if((same > 1) && (strcasecmp(entries[i].name, entries[i].label) != 0)) {
            snprintf(entries[i].name, sizeof(entries[i].name), "%s", entries[i].label);
        }
    }
}

size_t fw_pkg_scan(Storage* storage, FwPkgEntry* entries, size_t max_entries) {
    File* dir = storage_file_alloc(storage);
    FuriString* armed = furi_string_alloc();
    char* name = malloc(FW_PKG_PATH_LEN);
    size_t count = 0;

    const bool has_armed = fw_pkg_get_armed_path(storage, armed);

    if(storage_dir_open(dir, FW_PKG_UPDATE_DIR)) {
        FileInfo info;
        while(count < max_entries) {
            if(!storage_dir_read(dir, &info, name, FW_PKG_PATH_LEN)) {
                break;
            }

            if(name[0] == '.') {
                continue;
            }

            bool added = false;
            if(file_info_is_dir(&info)) {
                added = fw_pkg_entry_from_folder(
                    storage, name, has_armed ? furi_string_get_cstr(armed) : NULL, &entries[count]);
            } else if(fw_pkg_name_has_tgz_suffix(name)) {
                added = fw_pkg_entry_from_archive(name, info.size, &entries[count]);
            }

            if(added) {
                count++;
            }
        }
        storage_dir_close(dir);
    }

    free(name);
    furi_string_free(armed);
    storage_file_free(dir);

    fw_pkg_mark_current(entries, count);
    fw_pkg_disambiguate_names(entries, count);
    return count;
}

static bool fw_pkg_file_crc(
    Storage* storage,
    const char* manifest_path,
    const char* file_name,
    uint32_t* out_crc) {
    FuriString* path = furi_string_alloc();
    path_extract_dirname(manifest_path, path);
    path_append(path, file_name);

    File* file = storage_file_alloc(storage);
    bool found = false;

    if(storage_file_open(file, furi_string_get_cstr(path), FSAM_READ, FSOM_OPEN_EXISTING)) {
        found = true;
        if(out_crc) {
            *out_crc = crc32_calc_file(file, NULL, NULL);
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_string_free(path);
    return found;
}

static void fw_pkg_check_radio(const FwManifest* manifest, FwCheck* check) {
    if(manifest->radio[0] == '\0') {
        check->radio_action = FwRadioActionNone;
        return;
    }

    const BleGlueC2Info* c2 = ble_glue_get_c2_info();
    const FwRadioVersion* want = &manifest->radio_version;

    snprintf(
        check->radio_to,
        sizeof(check->radio_to),
        "%s %u.%u.%u",
        fw_pkg_radio_type_name(want->v.type),
        want->v.major,
        want->v.minor,
        want->v.sub);

    const bool version_match =
        (c2->VersionMajor == want->v.major) && (c2->VersionMinor == want->v.minor) &&
        (c2->VersionSub == want->v.sub) && (c2->VersionBranch == want->v.branch) &&
        (c2->VersionReleaseType == want->v.release);

    snprintf(
        check->radio_from,
        sizeof(check->radio_from),
        "%s %u.%u.%u",
        fw_pkg_radio_type_name(c2->StackType),
        c2->VersionMajor,
        c2->VersionMinor,
        c2->VersionSub);

    if(version_match && (c2->StackType == want->v.type)) {
        check->radio_action = FwRadioActionKeep;
    } else {
        check->radio_action = FwRadioActionReflash;
    }
}

void fw_pkg_check(
    Storage* storage,
    const char* manifest_path,
    const FwManifest* manifest,
    FwCheck* check) {
    memset(check, 0, sizeof(FwCheck));

    if(!manifest->valid) {
        fw_pkg_note_add(check->blockers, &check->blocker_count, "Manifest is invalid");
        return;
    }

    if(manifest->manifest_version < FW_PKG_MIN_MANIFEST_VERSION) {
        fw_pkg_note_add(check->blockers, &check->blocker_count, "Package format is too old");
    }

    /* Pre-production devices report no target and accept any firmware */
    const uint8_t hw_target = furi_hal_version_get_hw_target();
    if(hw_target && (hw_target != manifest->target)) {
        char note[FW_CHECK_NOTE_LEN];
        snprintf(
            note,
            sizeof(note),
            "Built for f%lu, this is f%u",
            (unsigned long)manifest->target,
            hw_target);
        fw_pkg_note_add(check->blockers, &check->blocker_count, note);
    }

    uint32_t loader_crc = 0;
    if(!fw_pkg_file_crc(storage, manifest_path, manifest->loader, &loader_crc)) {
        fw_pkg_note_add(check->blockers, &check->blocker_count, "Stage2 loader is missing");
    } else if(loader_crc != manifest->loader_crc) {
        fw_pkg_note_add(check->blockers, &check->blocker_count, "Stage2 loader is corrupted");
    }

    if(manifest->firmware[0] &&
       !fw_pkg_file_crc(storage, manifest_path, manifest->firmware, NULL)) {
        fw_pkg_note_add(check->blockers, &check->blocker_count, "Firmware image is missing");
    }

    if(manifest->resources[0] &&
       !fw_pkg_file_crc(storage, manifest_path, manifest->resources, NULL)) {
        fw_pkg_note_add(check->blockers, &check->blocker_count, "Resource bundle is missing");
    }

    fw_pkg_check_radio(manifest, check);

    if(manifest->radio[0]) {
        uint32_t radio_crc = 0;
        if(!fw_pkg_file_crc(storage, manifest_path, manifest->radio, &radio_crc)) {
            fw_pkg_note_add(check->blockers, &check->blocker_count, "Radio image is missing");
        } else if(manifest->radio_crc && (radio_crc != manifest->radio_crc)) {
            fw_pkg_note_add(check->blockers, &check->blocker_count, "Radio image is corrupted");
        }
    }

    if(check->radio_action == FwRadioActionReflash) {
        fw_pkg_note_add(
            check->warnings, &check->warning_count, "Radio reflash: keep it powered, ~2 min");
    } else if(manifest->has_ob) {
        const uint8_t device_sfsa = fw_pkg_device_sfsa();
        if(device_sfsa && (manifest->ob_sfsa != device_sfsa)) {
            fw_pkg_note_add(
                check->blockers,
                &check->blocker_count,
                "Needs a different radio stack, but has none");
        }
    }

    uint64_t free_space = 0;
    if(storage_common_fs_info(storage, STORAGE_EXT_PATH_PREFIX, NULL, &free_space) != FSE_OK) {
        fw_pkg_note_add(check->blockers, &check->blocker_count, "SD card is not ready");
    } else if(free_space < UPDATE_MIN_FREE_SPACE) {
        fw_pkg_note_add(check->blockers, &check->blocker_count, "SD card is too full to back up");
    }

    check->battery = furi_hal_power_get_pct();
    check->charging = furi_hal_power_is_charging();
    check->battery_required = (check->radio_action == FwRadioActionReflash) ? BATTERY_MIN_RADIO :
                                                                              BATTERY_MIN_FIRMWARE;

    if(!check->charging && (check->battery < check->battery_required)) {
        char note[FW_CHECK_NOTE_LEN];
        snprintf(
            note, sizeof(note), "Battery %u%%, need %u%%", check->battery, check->battery_required);
        fw_pkg_note_add(check->blockers, &check->blocker_count, note);
    }

    check->installable = (check->blocker_count == 0);
}

const char* fw_pkg_arm(Storage* storage, const char* manifest_path) {
    const size_t path_length = strlen(manifest_path);
    if((path_length == 0) || (path_length >= FW_PKG_PATH_LEN)) {
        return "Manifest path is invalid";
    }

    File* file = storage_file_alloc(storage);
    bool written = false;

    if(storage_file_open(file, FW_PKG_POINTER_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        written = storage_file_write(file, manifest_path, path_length) == path_length;
        storage_file_close(file);
    }
    storage_file_free(file);

    if(!written) {
        return "Could not write update pointer";
    }

    FuriString* readback = furi_string_alloc();
    const bool verified = fw_pkg_get_armed_path(storage, readback) &&
                          (furi_string_cmp_str(readback, manifest_path) == 0);
    furi_string_free(readback);

    if(!verified) {
        storage_simply_remove(storage, FW_PKG_POINTER_FILE);
        return "Update pointer did not stick";
    }

    furi_hal_rtc_set_boot_mode(FuriHalRtcBootModePreUpdate);
    return NULL;
}

void fw_pkg_disarm(Storage* storage) {
    furi_hal_rtc_set_boot_mode(FuriHalRtcBootModeNormal);
    storage_simply_remove(storage, FW_PKG_POINTER_FILE);
}
