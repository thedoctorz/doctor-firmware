#include "../subghz_i.h"
#include "../helpers/subghz_custom_event.h"
#include <lib/toolbox/crc32_calc.h>
#include <stdlib.h>
#include <string.h>

#define TAG "SubGhzCleanDupes"

#define SUBGHZ_CLEAN_DUPES_MAX_DEPTH    8
#define SUBGHZ_CLEAN_DUPES_NAME_LEN     256
#define SUBGHZ_CLEAN_DUPES_PATH_LEN     384
#define SUBGHZ_CLEAN_DUPES_TICK_MS      150
#define SUBGHZ_CLEAN_DUPES_STACK        3072
#define SUBGHZ_CLEAN_DUPES_CRC_CHUNK    256
#define SUBGHZ_CLEAN_DUPES_YIELD_BYTES  4096
#define SUBGHZ_CLEAN_DUPES_DELETE_BATCH 24
#define SUBGHZ_CLEAN_DUPES_IDX_BUF      256

#define SUBGHZ_CLEAN_DUPES_BAR_X 12
#define SUBGHZ_CLEAN_DUPES_BAR_Y 38
#define SUBGHZ_CLEAN_DUPES_BAR_W 104
#define SUBGHZ_CLEAN_DUPES_BAR_H 8

#define SUBGHZ_CLEAN_DUPES_TMP_DIR  EXT_PATH(".tmp")
#define SUBGHZ_CLEAN_DUPES_IDX_PATH EXT_PATH(".tmp/subghz_clean_dupes.idx")

typedef enum {
    SubGhzCleanDupesStateScanning,
    SubGhzCleanDupesStateSummary,
    SubGhzCleanDupesStateDeleting,
    SubGhzCleanDupesStateDone,
} SubGhzCleanDupesState;

// One scanned .sub file: its full path plus a cheap fingerprint (size + CRC32).
typedef struct {
    uint32_t rec_off;
    uint32_t size;
    uint32_t crc;
    uint8_t hashed;
} SubGhzDupEntry;

typedef struct {
    char* path;
    uint8_t depth;
} SubGhzDupDir;

typedef struct {
    uint32_t size;
    uint16_t count;
} SubGhzSizeSlot;

typedef struct {
    File* file;
    uint8_t buf[SUBGHZ_CLEAN_DUPES_IDX_BUF];
    size_t used;
    uint32_t pos;
} SubGhzDupIdxOut;

typedef struct {
    FuriThread* worker;
    FuriTimer* timer;
    volatile bool cancel;
    volatile bool scan_finished;
    volatile bool hashing;
    volatile bool truncated;
    volatile bool io_error;
    volatile size_t collected;
    volatile size_t hash_total;
    volatile size_t hash_done;
    volatile size_t delete_done;
    uint8_t anim;
    char line_a[32];
    char line_b[40];

    uint32_t* delete_offs;
    size_t delete_count;
    size_t delete_capacity;
    size_t group_count;
    size_t removed;
    uint64_t bytes;
    SubGhzCleanDupesState state;
} SubGhzCleanDupes;

static SubGhzCleanDupes* subghz_clean_dupes_get(SubGhz* subghz) {
    return (SubGhzCleanDupes*)(uintptr_t)scene_manager_get_scene_state(
        subghz->scene_manager, SubGhzSceneCleanDupes);
}

static void* subghz_clean_dupes_mem_grow(void* ptr, size_t old_bytes, size_t new_bytes) {
    if(new_bytes == 0) {
        free(ptr);
        return NULL;
    }
    void* next = malloc(new_bytes);
    if(!next) return NULL;
    if(ptr) {
        memcpy(next, ptr, old_bytes);
        free(ptr);
    }
    return next;
}

static bool subghz_clean_dupes_grow_array(
    void** ptr,
    size_t* capacity,
    size_t elem_size,
    size_t min_capacity) {
    size_t old_cap = *capacity;
    size_t new_cap = old_cap ? (old_cap * 2) : min_capacity;
    void* next = subghz_clean_dupes_mem_grow(*ptr, old_cap * elem_size, new_cap * elem_size);
    if(!next && old_cap) {
        new_cap = old_cap + 16;
        next = subghz_clean_dupes_mem_grow(*ptr, old_cap * elem_size, new_cap * elem_size);
    }
    if(!next) return false;
    *ptr = next;
    *capacity = new_cap;
    return true;
}

static char* subghz_clean_dupes_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = malloc(n);
    if(!d) return NULL;
    memcpy(d, s, n);
    return d;
}

static bool subghz_clean_dupes_is_sub_name(const char* name) {
    size_t n = strlen(name);
    if(n < 4) return false;
    return name[n - 4] == '.' && (name[n - 3] | 0x20) == 's' && (name[n - 2] | 0x20) == 'u' &&
           (name[n - 1] | 0x20) == 'b';
}

static bool subghz_clean_dupes_idx_flush(SubGhzDupIdxOut* out) {
    if(out->used == 0) return true;
    size_t n = storage_file_write(out->file, out->buf, out->used);
    if(n != out->used) return false;
    out->used = 0;
    return true;
}

static bool subghz_clean_dupes_idx_write(SubGhzDupIdxOut* out, const void* data, size_t len) {
    const uint8_t* p = data;
    while(len) {
        size_t room = sizeof(out->buf) - out->used;
        size_t chunk = (len < room) ? len : room;
        memcpy(out->buf + out->used, p, chunk);
        out->used += chunk;
        p += chunk;
        len -= chunk;
        out->pos += chunk;
        if(out->used == sizeof(out->buf) && !subghz_clean_dupes_idx_flush(out)) return false;
    }
    return true;
}

static bool subghz_clean_dupes_idx_write_record(
    SubGhzDupIdxOut* out,
    uint32_t size,
    const char* path,
    uint32_t* rec_off) {
    size_t path_len = strlen(path);
    if(path_len == 0 || path_len > 0xFFFFu) return false;
    uint16_t len = (uint16_t)path_len;
    *rec_off = out->pos;
    return subghz_clean_dupes_idx_write(out, &size, sizeof(size)) &&
           subghz_clean_dupes_idx_write(out, &len, sizeof(len)) &&
           subghz_clean_dupes_idx_write(out, path, path_len);
}

static bool
    subghz_clean_dupes_idx_read_path(File* file, uint32_t rec_off, char* path, size_t path_sz) {
    if(!storage_file_seek(file, rec_off, true)) return false;
    uint32_t size = 0;
    uint16_t len = 0;
    if(storage_file_read(file, &size, sizeof(size)) != sizeof(size)) return false;
    if(storage_file_read(file, &len, sizeof(len)) != sizeof(len)) return false;
    if(len >= path_sz) return false;
    if(storage_file_read(file, path, len) != len) return false;
    path[len] = '\0';
    return true;
}

static int subghz_clean_dupes_idx_read_next(
    File* file,
    uint32_t* rec_off,
    uint32_t* size,
    char* path,
    size_t path_sz) {
    uint32_t off = (uint32_t)storage_file_tell(file);
    uint32_t sz = 0;
    uint16_t len = 0;
    size_t n = storage_file_read(file, &sz, sizeof(sz));
    if(n == 0) return 0;
    if(n != sizeof(sz)) return 0;
    if(storage_file_read(file, &len, sizeof(len)) != sizeof(len)) return 0;
    if(len >= path_sz) {
        storage_file_seek(file, len, false);
        return -1;
    }
    if(storage_file_read(file, path, len) != len) return 0;
    path[len] = '\0';
    *rec_off = off;
    *size = sz;
    return 1;
}

static bool subghz_clean_dupes_size_map_add(
    SubGhzSizeSlot** slots,
    size_t* cap,
    size_t* used,
    uint32_t size) {
    if(*cap == 0) {
        *slots = malloc(64 * sizeof(SubGhzSizeSlot));
        if(!*slots) return false;
        memset(*slots, 0, 64 * sizeof(SubGhzSizeSlot));
        *cap = 64;
    }

    if((*used + 1) * 10 > (*cap) * 7) {
        size_t new_cap = (*cap) * 2;
        SubGhzSizeSlot* grown = malloc(new_cap * sizeof(SubGhzSizeSlot));
        if(!grown) return false;
        memset(grown, 0, new_cap * sizeof(SubGhzSizeSlot));
        for(size_t i = 0; i < *cap; i++) {
            if((*slots)[i].count == 0) continue;
            uint32_t s = (*slots)[i].size;
            size_t idx = (size_t)(s * 2654435761u) & (new_cap - 1);
            while(grown[idx].count != 0)
                idx = (idx + 1) & (new_cap - 1);
            grown[idx] = (*slots)[i];
        }
        free(*slots);
        *slots = grown;
        *cap = new_cap;
    }

    size_t mask = (*cap) - 1;
    size_t idx = (size_t)(size * 2654435761u) & mask;
    while((*slots)[idx].count != 0 && (*slots)[idx].size != size)
        idx = (idx + 1) & mask;
    if((*slots)[idx].count == 0) {
        (*slots)[idx].size = size;
        (*slots)[idx].count = 1;
        (*used)++;
    } else if((*slots)[idx].count < 0xFFFFu) {
        (*slots)[idx].count++;
    }
    return true;
}

static uint16_t subghz_clean_dupes_size_map_get(
    SubGhzSizeSlot* slots,
    size_t cap,
    uint32_t size,
    bool overflow) {
    if(cap == 0 || !slots) return overflow ? 2 : 0;
    size_t mask = cap - 1;
    size_t idx = (size_t)(size * 2654435761u) & mask;
    size_t start = idx;
    while(slots[idx].count != 0) {
        if(slots[idx].size == size) return slots[idx].count;
        idx = (idx + 1) & mask;
        if(idx == start) break;
    }
    return overflow ? 2 : 0;
}

static bool subghz_clean_dupes_entries_push(
    SubGhzDupEntry** entries,
    size_t* count,
    size_t* capacity,
    uint32_t rec_off,
    uint32_t size) {
    if(*count == *capacity) {
        if(!subghz_clean_dupes_grow_array((void**)entries, capacity, sizeof(SubGhzDupEntry), 64)) {
            return false;
        }
    }
    (*entries)[*count].rec_off = rec_off;
    (*entries)[*count].size = size;
    (*entries)[*count].crc = 0;
    (*entries)[*count].hashed = 0;
    (*count)++;
    return true;
}

static bool
    subghz_clean_dupes_delete_list_push(SubGhzCleanDupes* ctx, uint32_t rec_off, uint32_t size) {
    if(ctx->delete_count == ctx->delete_capacity) {
        if(!subghz_clean_dupes_grow_array(
               (void**)&ctx->delete_offs, &ctx->delete_capacity, sizeof(uint32_t), 16)) {
            return false;
        }
    }
    ctx->delete_offs[ctx->delete_count++] = rec_off;
    ctx->bytes += size;
    return true;
}

static int subghz_clean_dupes_compare_size(const void* a, const void* b) {
    const SubGhzDupEntry* ea = a;
    const SubGhzDupEntry* eb = b;
    if(ea->size != eb->size) return (ea->size < eb->size) ? -1 : 1;
    return 0;
}

static int subghz_clean_dupes_compare_sizecrc(const void* a, const void* b) {
    const SubGhzDupEntry* ea = a;
    const SubGhzDupEntry* eb = b;
    if(ea->size != eb->size) return (ea->size < eb->size) ? -1 : 1;
    if(ea->crc != eb->crc) return (ea->crc < eb->crc) ? -1 : 1;
    return 0;
}

static void subghz_clean_dupes_sift_down(
    SubGhzDupEntry* a,
    size_t n,
    size_t i,
    int (*cmp)(const void*, const void*)) {
    while(true) {
        size_t l = (i * 2) + 1;
        size_t r = l + 1;
        size_t m = i;
        if(l < n && cmp(&a[l], &a[m]) > 0) m = l;
        if(r < n && cmp(&a[r], &a[m]) > 0) m = r;
        if(m == i) break;
        SubGhzDupEntry tmp = a[i];
        a[i] = a[m];
        a[m] = tmp;
        i = m;
    }
}

static void
    subghz_clean_dupes_heapsort(SubGhzDupEntry* a, size_t n, int (*cmp)(const void*, const void*)) {
    if(n < 2) return;
    for(size_t i = n / 2; i > 0; i--) {
        subghz_clean_dupes_sift_down(a, n, i - 1, cmp);
    }
    for(size_t end = n; end > 1; end--) {
        SubGhzDupEntry tmp = a[0];
        a[0] = a[end - 1];
        a[end - 1] = tmp;
        subghz_clean_dupes_sift_down(a, end - 1, 0, cmp);
    }
}

static bool subghz_clean_dupes_dir_push(
    SubGhzDupDir** dirs,
    size_t* count,
    size_t* capacity,
    const char* path,
    uint8_t depth) {
    if(*count == *capacity) {
        if(!subghz_clean_dupes_grow_array((void**)dirs, capacity, sizeof(SubGhzDupDir), 8)) {
            return false;
        }
    }
    char* copy = subghz_clean_dupes_strdup(path);
    if(!copy) return false;
    (*dirs)[*count].path = copy;
    (*dirs)[*count].depth = depth;
    (*count)++;
    return true;
}

static uint32_t subghz_clean_dupes_crc_file(File* file, volatile bool* cancel) {
    uint8_t buf[SUBGHZ_CLEAN_DUPES_CRC_CHUNK];
    uint32_t crc = 0;
    size_t since_yield = 0;
    while(!*cancel) {
        size_t n = storage_file_read(file, buf, sizeof(buf));
        if(n == 0) break;
        crc = crc32_calc_buffer(crc, buf, n);
        since_yield += n;
        if(since_yield >= SUBGHZ_CLEAN_DUPES_YIELD_BYTES) {
            furi_thread_yield();
            since_yield = 0;
        }
    }
    return crc;
}

static void subghz_clean_dupes_collect(
    Storage* storage,
    SubGhzDupIdxOut* idx,
    SubGhzSizeSlot** slots,
    size_t* map_cap,
    size_t* map_used,
    bool* map_overflow,
    SubGhzCleanDupes* ctx) {
    SubGhzDupDir* dirs = NULL;
    size_t dir_count = 0;
    size_t dir_capacity = 0;
    if(!subghz_clean_dupes_dir_push(&dirs, &dir_count, &dir_capacity, SUBGHZ_APP_FOLDER, 0)) {
        ctx->truncated = true;
        return;
    }

    File* dir = storage_file_alloc(storage);
    FuriString* path = furi_string_alloc();
    char* name = malloc(SUBGHZ_CLEAN_DUPES_NAME_LEN);
    if(!name) {
        ctx->truncated = true;
    }

    while(!ctx->cancel && dir_count > 0) {
        dir_count--;
        SubGhzDupDir cur = dirs[dir_count];

        if(storage_dir_open(dir, cur.path)) {
            FileInfo info;
            while(!ctx->cancel &&
                  storage_dir_read(dir, &info, name, SUBGHZ_CLEAN_DUPES_NAME_LEN)) {
                if(!name || name[0] == '\0' || name[0] == '.') continue;
                furi_string_printf(path, "%s/%s", cur.path, name);
                if(file_info_is_dir(&info)) {
                    if(cur.depth < SUBGHZ_CLEAN_DUPES_MAX_DEPTH) {
                        if(!subghz_clean_dupes_dir_push(
                               &dirs,
                               &dir_count,
                               &dir_capacity,
                               furi_string_get_cstr(path),
                               cur.depth + 1)) {
                            ctx->truncated = true;
                        }
                    }
                } else if(subghz_clean_dupes_is_sub_name(name)) {
                    uint32_t rec_off = 0;
                    uint32_t size = (uint32_t)info.size;
                    if(!subghz_clean_dupes_idx_write_record(
                           idx, size, furi_string_get_cstr(path), &rec_off)) {
                        ctx->io_error = true;
                        break;
                    }
                    if(!subghz_clean_dupes_size_map_add(slots, map_cap, map_used, size)) {
                        *map_overflow = true;
                    }
                    ctx->collected++;
                    if((ctx->collected & 7u) == 0) furi_thread_yield();
                }
            }
        }
        storage_dir_close(dir);
        free(cur.path);
        if(ctx->io_error) break;
    }

    for(size_t i = 0; i < dir_count; i++) {
        free(dirs[i].path);
    }
    free(dirs);
    free(name);
    furi_string_free(path);
    storage_file_free(dir);
}

static void
    subghz_clean_dupes_group_entries(SubGhzCleanDupes* ctx, SubGhzDupEntry* entries, size_t count) {
    if(count < 2) return;
    subghz_clean_dupes_heapsort(entries, count, subghz_clean_dupes_compare_sizecrc);
    size_t i = 0;
    while(i < count) {
        size_t j = i + 1;
        while(j < count && entries[i].hashed && entries[j].hashed &&
              entries[i].size == entries[j].size && entries[i].crc == entries[j].crc) {
            j++;
        }
        if(j - i > 1) {
            ctx->group_count++;
            for(size_t k = i + 1; k < j; k++) {
                if(!subghz_clean_dupes_delete_list_push(ctx, entries[k].rec_off, entries[k].size)) {
                    ctx->truncated = true;
                    return;
                }
            }
        }
        i = j;
    }
}

static bool subghz_clean_dupes_hash_entries(
    Storage* storage,
    File* idx,
    SubGhzCleanDupes* ctx,
    SubGhzDupEntry* entries,
    size_t count) {
    char* path = malloc(SUBGHZ_CLEAN_DUPES_PATH_LEN);
    if(!path) return false;
    File* file = storage_file_alloc(storage);

    subghz_clean_dupes_heapsort(entries, count, subghz_clean_dupes_compare_size);
    size_t i = 0;
    while(i < count && !ctx->cancel) {
        size_t j = i + 1;
        while(j < count && entries[i].size == entries[j].size)
            j++;
        if(j - i > 1) {
            for(size_t k = i; k < j && !ctx->cancel; k++) {
                if(subghz_clean_dupes_idx_read_path(
                       idx, entries[k].rec_off, path, SUBGHZ_CLEAN_DUPES_PATH_LEN) &&
                   storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
                    uint32_t crc = subghz_clean_dupes_crc_file(file, &ctx->cancel);
                    storage_file_close(file);
                    if(!ctx->cancel) {
                        entries[k].crc = crc;
                        entries[k].hashed = 1;
                    }
                }
                ctx->hash_done++;
            }
        }
        i = j;
    }

    storage_file_free(file);
    free(path);
    return !ctx->cancel;
}

static int32_t subghz_clean_dupes_worker(void* context) {
    SubGhzCleanDupes* ctx = context;
    Storage* storage = furi_record_open(RECORD_STORAGE);

    storage_simply_mkdir(storage, SUBGHZ_CLEAN_DUPES_TMP_DIR);
    storage_simply_remove(storage, SUBGHZ_CLEAN_DUPES_IDX_PATH);

    File* idx_file = storage_file_alloc(storage);
    SubGhzDupIdxOut idx = {0};
    idx.file = idx_file;

    SubGhzSizeSlot* slots = NULL;
    size_t map_cap = 0;
    size_t map_used = 0;
    bool map_overflow = false;

    if(!storage_file_open(idx_file, SUBGHZ_CLEAN_DUPES_IDX_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ctx->io_error = true;
        storage_file_close(idx_file);
    } else {
        subghz_clean_dupes_collect(storage, &idx, &slots, &map_cap, &map_used, &map_overflow, ctx);
        if(!subghz_clean_dupes_idx_flush(&idx)) ctx->io_error = true;
        storage_file_close(idx_file);
    }

    size_t to_hash = 0;
    for(size_t i = 0; i < map_cap; i++) {
        if(slots && slots[i].count > 1) to_hash += slots[i].count;
    }
    bool need_hash = (to_hash > 0) || map_overflow;

    if(!ctx->cancel && ctx->collected > 1 && need_hash) {
        bool idx_open = storage_file_open(
            idx_file, SUBGHZ_CLEAN_DUPES_IDX_PATH, FSAM_READ, FSOM_OPEN_EXISTING);
        if(!idx_open) {
            ctx->io_error = true;
            storage_file_close(idx_file);
        } else {
            SubGhzDupEntry* entries = NULL;
            size_t count = 0;
            size_t capacity = 0;
            char* path = malloc(SUBGHZ_CLEAN_DUPES_PATH_LEN);
            bool loaded = (path != NULL);
            while(loaded && !ctx->cancel) {
                uint32_t rec_off = 0;
                uint32_t size = 0;
                int rec = subghz_clean_dupes_idx_read_next(
                    idx_file, &rec_off, &size, path, SUBGHZ_CLEAN_DUPES_PATH_LEN);
                if(rec == 0) break;
                if(rec < 0) continue;
                if(subghz_clean_dupes_size_map_get(slots, map_cap, size, map_overflow) > 1) {
                    if(!subghz_clean_dupes_entries_push(
                           &entries, &count, &capacity, rec_off, size)) {
                        loaded = false;
                        break;
                    }
                }
            }
            free(path);

            if(loaded && count > 1) {
                ctx->hash_total = count;
                ctx->hashing = true;
                storage_file_seek(idx_file, 0, true);
                if(subghz_clean_dupes_hash_entries(storage, idx_file, ctx, entries, count)) {
                    subghz_clean_dupes_group_entries(ctx, entries, count);
                }
            } else if(!loaded && slots && map_cap > 0) {
                ctx->hash_total = to_hash ? to_hash : ctx->collected;
                ctx->hashing = true;
                File* sub = storage_file_alloc(storage);
                char* pbuf = malloc(SUBGHZ_CLEAN_DUPES_PATH_LEN);
                if(pbuf) {
                    for(size_t s = 0; s < map_cap && !ctx->cancel; s++) {
                        if(slots[s].count < 2) continue;
                        SubGhzDupEntry* group = NULL;
                        size_t gcount = 0;
                        size_t gcap = 0;
                        storage_file_seek(idx_file, 0, true);
                        while(!ctx->cancel) {
                            uint32_t rec_off = 0;
                            uint32_t size = 0;
                            int rec = subghz_clean_dupes_idx_read_next(
                                idx_file, &rec_off, &size, pbuf, SUBGHZ_CLEAN_DUPES_PATH_LEN);
                            if(rec == 0) break;
                            if(rec < 0) continue;
                            if(size != slots[s].size) continue;
                            if(!subghz_clean_dupes_entries_push(
                                   &group, &gcount, &gcap, rec_off, size)) {
                                ctx->truncated = true;
                                break;
                            }
                        }
                        for(size_t k = 0; k < gcount && !ctx->cancel; k++) {
                            if(subghz_clean_dupes_idx_read_path(
                                   idx_file, group[k].rec_off, pbuf, SUBGHZ_CLEAN_DUPES_PATH_LEN) &&
                               storage_file_open(sub, pbuf, FSAM_READ, FSOM_OPEN_EXISTING)) {
                                uint32_t crc = subghz_clean_dupes_crc_file(sub, &ctx->cancel);
                                storage_file_close(sub);
                                if(!ctx->cancel) {
                                    group[k].crc = crc;
                                    group[k].hashed = 1;
                                }
                            }
                            ctx->hash_done++;
                        }
                        if(!ctx->cancel && !ctx->truncated) {
                            subghz_clean_dupes_group_entries(ctx, group, gcount);
                        }
                        free(group);
                    }
                } else {
                    ctx->truncated = true;
                }
                free(pbuf);
                storage_file_free(sub);
            }
            free(entries);
            storage_file_close(idx_file);
        }
    }

    free(slots);
    storage_file_free(idx_file);
    furi_record_close(RECORD_STORAGE);

    if(ctx->truncated) {
        FURI_LOG_W(
            TAG,
            "Scan finished with skipped dirs/groups (%u files)",
            (unsigned int)ctx->collected);
    }

    ctx->scan_finished = true;
    return 0;
}

static void subghz_clean_dupes_join_worker(SubGhzCleanDupes* ctx) {
    if(ctx->worker == NULL) return;
    furi_thread_join(ctx->worker);
    furi_thread_free(ctx->worker);
    ctx->worker = NULL;
}

static void subghz_clean_dupes_free(SubGhzCleanDupes* ctx) {
    if(ctx == NULL) return;
    free(ctx->delete_offs);
    free(ctx);
}

static void
    subghz_clean_dupes_button_callback(GuiButtonType result, InputType type, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    if(type != InputTypeShort) return;
    if(result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneCleanDupesOk);
    } else if(result == GuiButtonTypeLeft || result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneCleanDupesBack);
    }
}

static void subghz_clean_dupes_timer_callback(void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventSceneCleanDupesProgress);
}

static void subghz_clean_dupes_draw_bar(Widget* widget, uint8_t fill_w, uint8_t block_x) {
    widget_add_rect_element(
        widget,
        SUBGHZ_CLEAN_DUPES_BAR_X,
        SUBGHZ_CLEAN_DUPES_BAR_Y,
        SUBGHZ_CLEAN_DUPES_BAR_W,
        SUBGHZ_CLEAN_DUPES_BAR_H,
        2,
        false);
    if(fill_w > 0) {
        if(fill_w > SUBGHZ_CLEAN_DUPES_BAR_W - 4) fill_w = SUBGHZ_CLEAN_DUPES_BAR_W - 4;
        widget_add_rect_element(
            widget,
            SUBGHZ_CLEAN_DUPES_BAR_X + 2,
            SUBGHZ_CLEAN_DUPES_BAR_Y + 2,
            fill_w,
            SUBGHZ_CLEAN_DUPES_BAR_H - 4,
            0,
            true);
    } else if(block_x > 0) {
        widget_add_rect_element(
            widget,
            SUBGHZ_CLEAN_DUPES_BAR_X + 2 + block_x,
            SUBGHZ_CLEAN_DUPES_BAR_Y + 2,
            16,
            SUBGHZ_CLEAN_DUPES_BAR_H - 4,
            0,
            true);
    }
}

static void subghz_clean_dupes_show_progress(SubGhz* subghz, SubGhzCleanDupes* ctx) {
    widget_reset(subghz->widget);

    widget_add_string_element(
        subghz->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Clean Duplicates");

    uint8_t fill_w = 0;
    uint8_t block_x = 0;
    const uint8_t inner_w = SUBGHZ_CLEAN_DUPES_BAR_W - 4;

    if(ctx->state == SubGhzCleanDupesStateDeleting) {
        snprintf(ctx->line_a, sizeof(ctx->line_a), "Deleting...");
        snprintf(
            ctx->line_b,
            sizeof(ctx->line_b),
            "%u / %u",
            (unsigned int)ctx->delete_done,
            (unsigned int)ctx->delete_count);
        if(ctx->delete_count > 0) {
            fill_w = (uint8_t)(((uint32_t)ctx->delete_done * inner_w) / ctx->delete_count);
        }
    } else if(ctx->hashing) {
        snprintf(ctx->line_a, sizeof(ctx->line_a), "Checking files");
        snprintf(
            ctx->line_b,
            sizeof(ctx->line_b),
            "%u / %u",
            (unsigned int)ctx->hash_done,
            (unsigned int)ctx->hash_total);
        if(ctx->hash_total > 0) {
            fill_w = (uint8_t)(((uint32_t)ctx->hash_done * inner_w) / ctx->hash_total);
        }
    } else {
        snprintf(ctx->line_a, sizeof(ctx->line_a), "Scanning files");
        snprintf(ctx->line_b, sizeof(ctx->line_b), "Found %u", (unsigned int)ctx->collected);
        ctx->anim++;
        uint8_t travel = (inner_w > 16) ? (inner_w - 16) : 1;
        uint16_t phase = (uint16_t)((ctx->anim * 2) % (uint16_t)(2 * travel));
        block_x = (phase <= travel) ? (uint8_t)phase : (uint8_t)(2 * travel - phase);
        if(block_x == 0) block_x = 1;
    }

    widget_add_string_element(
        subghz->widget, 64, 14, AlignCenter, AlignTop, FontSecondary, ctx->line_a);
    widget_add_string_element(
        subghz->widget, 64, 24, AlignCenter, AlignTop, FontSecondary, ctx->line_b);
    subghz_clean_dupes_draw_bar(subghz->widget, fill_w, block_x);
    widget_add_string_element(
        subghz->widget, 64, 52, AlignCenter, AlignTop, FontSecondary, "Press Back to cancel");

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

static void subghz_clean_dupes_show_summary(SubGhz* subghz, SubGhzCleanDupes* ctx) {
    widget_reset(subghz->widget);

    FuriString* text = furi_string_alloc();
    if(ctx->io_error && ctx->collected == 0) {
        furi_string_set(text, "\e#Scan failed\e#\nCould not write the\nindex file on SD.");
        widget_add_text_box_element(
            subghz->widget,
            0,
            0,
            128,
            52,
            AlignCenter,
            AlignCenter,
            furi_string_get_cstr(text),
            false);
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeCenter,
            "Back",
            subghz_clean_dupes_button_callback,
            subghz);
    } else if(ctx->delete_count == 0) {
        furi_string_printf(
            text,
            "\e#No duplicates found\e#\nChecked %u .sub files.\nYour library is clean.",
            (unsigned int)ctx->collected);
        widget_add_text_box_element(
            subghz->widget,
            0,
            0,
            128,
            52,
            AlignCenter,
            AlignCenter,
            furi_string_get_cstr(text),
            false);
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeCenter,
            "Back",
            subghz_clean_dupes_button_callback,
            subghz);
    } else {
        furi_string_printf(
            text,
            "\e#Duplicate .sub files\e#\n%u extras in %u groups\n(~%lu KB) of %u files.\nKeeps 1 copy of each.",
            (unsigned int)ctx->delete_count,
            (unsigned int)ctx->group_count,
            (unsigned long)(ctx->bytes / 1024),
            (unsigned int)ctx->collected);
        widget_add_text_box_element(
            subghz->widget, 0, 0, 128, 52, AlignCenter, AlignTop, furi_string_get_cstr(text), false);
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeLeft,
            "Cancel",
            subghz_clean_dupes_button_callback,
            subghz);
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeRight,
            "Delete",
            subghz_clean_dupes_button_callback,
            subghz);
    }
    furi_string_free(text);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

static void subghz_clean_dupes_show_result(SubGhz* subghz, SubGhzCleanDupes* ctx) {
    widget_reset(subghz->widget);

    FuriString* text = furi_string_alloc();
    furi_string_printf(
        text, "\e#Done\e#\nRemoved %u duplicate files.", (unsigned int)ctx->removed);
    widget_add_text_box_element(
        subghz->widget, 0, 0, 128, 52, AlignCenter, AlignCenter, furi_string_get_cstr(text), false);
    widget_add_button_element(
        subghz->widget, GuiButtonTypeCenter, "Back", subghz_clean_dupes_button_callback, subghz);
    furi_string_free(text);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

void subghz_scene_clean_dupes_on_enter(void* context) {
    SubGhz* subghz = context;

    SubGhzCleanDupes* ctx = malloc(sizeof(SubGhzCleanDupes));
    memset(ctx, 0, sizeof(SubGhzCleanDupes));
    ctx->state = SubGhzCleanDupesStateScanning;
    scene_manager_set_scene_state(
        subghz->scene_manager, SubGhzSceneCleanDupes, (uint32_t)(uintptr_t)ctx);

    subghz_clean_dupes_show_progress(subghz, ctx);

    ctx->worker = furi_thread_alloc_ex(
        "SubGhzCleanDupes", SUBGHZ_CLEAN_DUPES_STACK, subghz_clean_dupes_worker, ctx);
    furi_thread_set_priority(ctx->worker, FuriThreadPriorityLow);
    furi_thread_start(ctx->worker);

    ctx->timer =
        furi_timer_alloc(subghz_clean_dupes_timer_callback, FuriTimerTypePeriodic, subghz);
    furi_timer_start(ctx->timer, furi_ms_to_ticks(SUBGHZ_CLEAN_DUPES_TICK_MS));
}

bool subghz_scene_clean_dupes_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    SubGhzCleanDupes* ctx = subghz_clean_dupes_get(subghz);
    if(ctx == NULL) return false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventSceneCleanDupesProgress) {
            if(ctx->state == SubGhzCleanDupesStateDeleting) {
                Storage* storage = furi_record_open(RECORD_STORAGE);
                File* idx = storage_file_alloc(storage);
                char path[SUBGHZ_CLEAN_DUPES_PATH_LEN];
                if(storage_file_open(
                       idx, SUBGHZ_CLEAN_DUPES_IDX_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
                    size_t batch = SUBGHZ_CLEAN_DUPES_DELETE_BATCH;
                    while(batch-- > 0 && ctx->delete_done < ctx->delete_count) {
                        if(subghz_clean_dupes_idx_read_path(
                               idx, ctx->delete_offs[ctx->delete_done], path, sizeof(path))) {
                            if(storage_simply_remove(storage, path)) {
                                ctx->removed++;
                            }
                        }
                        ctx->delete_done++;
                    }
                    storage_file_close(idx);
                } else {
                    ctx->delete_done = ctx->delete_count;
                }
                storage_file_free(idx);
                furi_record_close(RECORD_STORAGE);
                if(ctx->delete_done >= ctx->delete_count) {
                    furi_timer_stop(ctx->timer);
                    ctx->state = SubGhzCleanDupesStateDone;
                    subghz_clean_dupes_show_result(subghz, ctx);
                } else {
                    subghz_clean_dupes_show_progress(subghz, ctx);
                }
            } else if(ctx->state == SubGhzCleanDupesStateScanning) {
                if(ctx->scan_finished) {
                    furi_timer_stop(ctx->timer);
                    subghz_clean_dupes_join_worker(ctx);
                    ctx->state = SubGhzCleanDupesStateSummary;
                    subghz_clean_dupes_show_summary(subghz, ctx);
                } else {
                    subghz_clean_dupes_show_progress(subghz, ctx);
                }
            }
            return true;
        } else if(event.event == SubGhzCustomEventSceneCleanDupesOk) {
            if(ctx->state == SubGhzCleanDupesStateSummary && ctx->delete_count > 0) {
                ctx->state = SubGhzCleanDupesStateDeleting;
                ctx->delete_done = 0;
                ctx->removed = 0;
                subghz_clean_dupes_show_progress(subghz, ctx);
                furi_timer_start(ctx->timer, furi_ms_to_ticks(SUBGHZ_CLEAN_DUPES_TICK_MS));
            }
            return true;
        } else if(event.event == SubGhzCustomEventSceneCleanDupesBack) {
            scene_manager_previous_scene(subghz->scene_manager);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(ctx->state == SubGhzCleanDupesStateScanning ||
           ctx->state == SubGhzCleanDupesStateDeleting) {
            ctx->cancel = true;
            scene_manager_previous_scene(subghz->scene_manager);
            return true;
        }
    }
    return false;
}

void subghz_scene_clean_dupes_on_exit(void* context) {
    SubGhz* subghz = context;
    SubGhzCleanDupes* ctx = subghz_clean_dupes_get(subghz);
    if(ctx != NULL) {
        ctx->cancel = true;
        if(ctx->timer != NULL) {
            furi_timer_stop(ctx->timer);
            furi_timer_free(ctx->timer);
        }
        subghz_clean_dupes_join_worker(ctx);
        subghz_clean_dupes_free(ctx);
    }
    scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneCleanDupes, 0);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_remove(storage, SUBGHZ_CLEAN_DUPES_IDX_PATH);
    furi_record_close(RECORD_STORAGE);

    popup_reset(subghz->popup);
    widget_reset(subghz->widget);
}
