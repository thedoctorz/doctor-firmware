# Structure

## main

Applications for main Flipper menu.

- `archive`             - Archive and file manager 
- `bad_usb`             - Bad USB application
- `gpio`                - GPIO application: includes USART bridge and GPIO control
- `ibutton`             - iButton application, onewire keys and more
- `infrared`            - Infrared application, controls your IR devices
- `lfrfid`              - LF RFID application
- `nfc`                 - NFC application, HF rfid, EMV and etc
- `subghz`              - SubGhz application, 433 fobs and etc
- `u2f`                 - U2F Application


## services

Background services providing system APIs to applications.

- `applications.h`      - Firmware application list header
- `bt`                  - BLE service and application
- `cli`                 - Console service and API
- `crypto`              - Crypto cli tools
- `desktop`             - Desktop service
- `dialogs`             - Dialogs service: GUI Dialogs for your app
- `dolphin`             - Dolphin service and supplementary apps
- `gui`                 - GUI service and API
- `input`               - Input service
- `loader`              - Application loader service
- `notification`        - Notification service 
- `power`               - Power service
- `rpc`                 - RPC service and API
- `storage`             - Storage service, internal + sdcard


## settings

Small applications providing configuration for basic firmware and its services.

- `about`               - Small About application that shows flipper info
- `bt_settings_app`     - Bluetooth options
- `desktop_settings`    - Desktop configuration
- `dolphin_passport`    - Dolphin passport app
- `notification_settings` - LCD brightness, sound volume, etc configuration
- `power_settings_app`  - Basic power options
- `storage_settings`    - Storage settings app
- `system`              - System settings
- `input_settings_app`  - Basic input options


## system

Utility apps not visible in other menus, plus few external apps pre-packaged with the firmware.

- `hid_app`             - BLE & USB HID remote
- `js_app`              - JS engine runner
- `snake_game`          - Snake game
- `storage_move_to_sd`  - Data migration tool for internal storage
- `updater`             - Update service & application
