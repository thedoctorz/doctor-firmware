#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RemoteKeyboardNone = 0,
    RemoteKeyboardText = 1,
    RemoteKeyboardByte = 2,
    RemoteKeyboardNumber = 3,
} RemoteKeyboardKind;

typedef void (*RemoteKeyboardFocusCallback)(RemoteKeyboardKind kind, void* context);

void remote_keyboard_add_focus_callback(RemoteKeyboardFocusCallback callback, void* context);

void remote_keyboard_remove_focus_callback(RemoteKeyboardFocusCallback callback, void* context);

RemoteKeyboardKind remote_keyboard_kind(void);

bool remote_keyboard_insert(const char* text);
bool remote_keyboard_backspace(void);
bool remote_keyboard_submit(void);

void remote_keyboard_acquire_text(void* text_input);
void remote_keyboard_release_text(void* text_input);
void remote_keyboard_acquire_byte(void* byte_input);
void remote_keyboard_release_byte(void* byte_input);
void remote_keyboard_acquire_number(void* number_input);
void remote_keyboard_release_number(void* number_input);
