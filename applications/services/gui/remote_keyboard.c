#include "remote_keyboard.h"

#include <furi.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/number_input.h>
#include <gui/modules/text_input.h>

#define REMOTE_KEYBOARD_SLOTS 4

bool text_input_remote_insert(TextInput* text_input, const char* text);
bool text_input_remote_backspace(TextInput* text_input);
bool text_input_remote_submit(TextInput* text_input);
bool byte_input_remote_insert(ByteInput* byte_input, const char* text);
bool byte_input_remote_backspace(ByteInput* byte_input);
bool byte_input_remote_submit(ByteInput* byte_input);
bool number_input_remote_insert(NumberInput* number_input, const char* text);
bool number_input_remote_backspace(NumberInput* number_input);
bool number_input_remote_submit(NumberInput* number_input);

typedef struct {
    RemoteKeyboardFocusCallback callback;
    void* context;
} RemoteKeyboardSlot;

static struct {
    FuriMutex* mutex;
    TextInput* text;
    ByteInput* byte;
    NumberInput* number;
    uint32_t busy;
    uint32_t publishing; // Focus callbacks running outside the lock
    RemoteKeyboardSlot slots[REMOTE_KEYBOARD_SLOTS];
} remote_keyboard;

static void remote_keyboard_lock(void) {
    if(remote_keyboard.mutex == NULL) {
        FuriMutex* created = furi_mutex_alloc(FuriMutexTypeNormal);
        FURI_CRITICAL_ENTER();
        if(remote_keyboard.mutex == NULL) {
            remote_keyboard.mutex = created;
            created = NULL;
        }
        FURI_CRITICAL_EXIT();
        if(created != NULL) furi_mutex_free(created);
    }
    furi_check(furi_mutex_acquire(remote_keyboard.mutex, FuriWaitForever) == FuriStatusOk);
}

static void remote_keyboard_unlock(void) {
    furi_check(furi_mutex_release(remote_keyboard.mutex) == FuriStatusOk);
}

static RemoteKeyboardKind remote_keyboard_kind_locked(void) {
    if(remote_keyboard.text != NULL) return RemoteKeyboardText;
    if(remote_keyboard.byte != NULL) return RemoteKeyboardByte;
    if(remote_keyboard.number != NULL) return RemoteKeyboardNumber;
    return RemoteKeyboardNone;
}

static void remote_keyboard_publish(RemoteKeyboardKind kind) {
    RemoteKeyboardFocusCallback callbacks[REMOTE_KEYBOARD_SLOTS];
    void* contexts[REMOTE_KEYBOARD_SLOTS];
    size_t count = 0;

    remote_keyboard_lock();
    for(size_t i = 0; i < REMOTE_KEYBOARD_SLOTS; i++) {
        if(remote_keyboard.slots[i].callback == NULL) continue;
        callbacks[count] = remote_keyboard.slots[i].callback;
        contexts[count] = remote_keyboard.slots[i].context;
        count++;
    }
    remote_keyboard.publishing++;
    remote_keyboard_unlock();

    for(size_t i = 0; i < count; i++) {
        callbacks[i](kind, contexts[i]);
    }

    remote_keyboard_lock();
    remote_keyboard.publishing--;
    remote_keyboard_unlock();
}

static void remote_keyboard_wait_idle_locked(void) {
    while(remote_keyboard.busy > 0) {
        remote_keyboard_unlock();
        furi_delay_tick(1);
        remote_keyboard_lock();
    }
}

void remote_keyboard_add_focus_callback(RemoteKeyboardFocusCallback callback, void* context) {
    furi_check(callback);
    remote_keyboard_lock();
    bool stored = false;
    for(size_t i = 0; i < REMOTE_KEYBOARD_SLOTS; i++) {
        if(remote_keyboard.slots[i].callback != NULL) continue;
        remote_keyboard.slots[i].callback = callback;
        remote_keyboard.slots[i].context = context;
        stored = true;
        break;
    }
    RemoteKeyboardKind kind = remote_keyboard_kind_locked();
    remote_keyboard_unlock();
    if(!stored) {
        FURI_LOG_W("RemoteKbd", "focus callback list full");
        return;
    }
    callback(kind, context);
}

void remote_keyboard_remove_focus_callback(RemoteKeyboardFocusCallback callback, void* context) {
    remote_keyboard_lock();
    for(size_t i = 0; i < REMOTE_KEYBOARD_SLOTS; i++) {
        if(remote_keyboard.slots[i].callback != callback) continue;
        if(remote_keyboard.slots[i].context != context) continue;
        remote_keyboard.slots[i].callback = NULL;
        remote_keyboard.slots[i].context = NULL;
        break;
    }
    // A publish may have copied this callback before it was removed; the caller frees the
    // context as soon as this returns, so wait the copy out
    while(remote_keyboard.publishing > 0) {
        remote_keyboard_unlock();
        furi_delay_tick(1);
        remote_keyboard_lock();
    }
    remote_keyboard_unlock();
}

RemoteKeyboardKind remote_keyboard_kind(void) {
    remote_keyboard_lock();
    RemoteKeyboardKind kind = remote_keyboard_kind_locked();
    remote_keyboard_unlock();
    return kind;
}

static bool remote_keyboard_apply_insert(const char* text) {
    remote_keyboard_lock();
    TextInput* text_input = remote_keyboard.text;
    ByteInput* byte_input = remote_keyboard.byte;
    NumberInput* number_input = remote_keyboard.number;
    if(text_input != NULL || byte_input != NULL || number_input != NULL) {
        remote_keyboard.busy++;
    }
    remote_keyboard_unlock();

    bool ok = false;
    if(text_input != NULL) {
        ok = text_input_remote_insert(text_input, text);
    } else if(byte_input != NULL) {
        ok = byte_input_remote_insert(byte_input, text);
    } else if(number_input != NULL) {
        ok = number_input_remote_insert(number_input, text);
    }

    if(text_input != NULL || byte_input != NULL || number_input != NULL) {
        remote_keyboard_lock();
        remote_keyboard.busy--;
        remote_keyboard_unlock();
    }
    return ok;
}

bool remote_keyboard_insert(const char* text) {
    if(text == NULL) text = "";
    return remote_keyboard_apply_insert(text);
}

bool remote_keyboard_backspace(void) {
    remote_keyboard_lock();
    TextInput* text_input = remote_keyboard.text;
    ByteInput* byte_input = remote_keyboard.byte;
    NumberInput* number_input = remote_keyboard.number;
    if(text_input != NULL || byte_input != NULL || number_input != NULL) {
        remote_keyboard.busy++;
    }
    remote_keyboard_unlock();

    bool ok = false;
    if(text_input != NULL) {
        ok = text_input_remote_backspace(text_input);
    } else if(byte_input != NULL) {
        ok = byte_input_remote_backspace(byte_input);
    } else if(number_input != NULL) {
        ok = number_input_remote_backspace(number_input);
    }

    if(text_input != NULL || byte_input != NULL || number_input != NULL) {
        remote_keyboard_lock();
        remote_keyboard.busy--;
        remote_keyboard_unlock();
    }
    return ok;
}

bool remote_keyboard_submit(void) {
    remote_keyboard_lock();
    TextInput* text_input = remote_keyboard.text;
    ByteInput* byte_input = remote_keyboard.byte;
    NumberInput* number_input = remote_keyboard.number;
    if(text_input != NULL || byte_input != NULL || number_input != NULL) {
        remote_keyboard.busy++;
    }
    remote_keyboard_unlock();

    bool ok = false;
    if(text_input != NULL) {
        ok = text_input_remote_submit(text_input);
    } else if(byte_input != NULL) {
        ok = byte_input_remote_submit(byte_input);
    } else if(number_input != NULL) {
        ok = number_input_remote_submit(number_input);
    }

    if(text_input != NULL || byte_input != NULL || number_input != NULL) {
        remote_keyboard_lock();
        remote_keyboard.busy--;
        remote_keyboard_unlock();
    }
    return ok;
}

static void remote_keyboard_acquire(void** slot, void* instance) {
    remote_keyboard_lock();
    RemoteKeyboardKind before = remote_keyboard_kind_locked();
    *slot = instance;
    RemoteKeyboardKind after = remote_keyboard_kind_locked();
    remote_keyboard_unlock();
    if(before != after) remote_keyboard_publish(after);
}

static void remote_keyboard_release(void** slot, void* instance) {
    remote_keyboard_lock();
    RemoteKeyboardKind before = remote_keyboard_kind_locked();
    if(*slot == instance) *slot = NULL;
    remote_keyboard_wait_idle_locked();
    RemoteKeyboardKind after = remote_keyboard_kind_locked();
    remote_keyboard_unlock();
    if(before != after) remote_keyboard_publish(after);
}

void remote_keyboard_acquire_text(void* text_input) {
    remote_keyboard_acquire((void**)&remote_keyboard.text, text_input);
}

void remote_keyboard_release_text(void* text_input) {
    remote_keyboard_release((void**)&remote_keyboard.text, text_input);
}

void remote_keyboard_acquire_byte(void* byte_input) {
    remote_keyboard_acquire((void**)&remote_keyboard.byte, byte_input);
}

void remote_keyboard_release_byte(void* byte_input) {
    remote_keyboard_release((void**)&remote_keyboard.byte, byte_input);
}

void remote_keyboard_acquire_number(void* number_input) {
    remote_keyboard_acquire((void**)&remote_keyboard.number, number_input);
}

void remote_keyboard_release_number(void* number_input) {
    remote_keyboard_release((void**)&remote_keyboard.number, number_input);
}
