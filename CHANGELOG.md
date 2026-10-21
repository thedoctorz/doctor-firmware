# Changelog

What The Doctor's Firmware adds on top of Unleashed.

## Added

### Sub-GHz protocols

31 cars protocols, so a total of 92 protocols all live from SubGHZ Read!

- **BMW** — 433 AM, transmit
- **Chrysler V0** — 315/433 AM, transmit
- **Fiat V0** — 315/433 AM, transmit
- **Fiat V1** — 315/433 AM/FM, transmit
- **Fiat V2** — 315/433 AM/FM, transmit
- **Ford V0** — 315/433 AM, transmit
- **Ford V1** — 315/433 FM, transmit
- **Ford V2** — 315/433 FM, transmit
- **Ford V3** — 315/433 AM/FM
- **Honda Static** — 315/433 FM, transmit
- **Honda V1** — 315/433 AM, transmit
- **Honda V2** — 315/433 FM, transmit
- **Kia V0, V3/V4, V5, V6, V7** — 315/433 FM, transmit
- **Kia V1, V2** — 315/433 AM, transmit
- **Mazda V0, V2** — 315/433 AM/FM, transmit
- **Mercedes** — 433 FM, transmit
- **Opel V1** — 433 AM
- **PSA** — 433 AM/FM, transmit. In-app helper when a capture still needs a key
- **Renault V0, V1** — 315/433 AM/FM, transmit
- **Scher-Khan** — 433 FM, decode only
- **Star Line** — 433 AM, transmit
- **Subaru** — 315/433 AM, transmit
- **Urmet** — 433 AM, transmit
- **VAG** — 434 AM, transmit (AUT64)


### Sub-GHz

- Dual receive on the internal and external radios. Defaults are 433.92 MHz AM650 and FM476. History marks them `[I]` and `[E]`.
- Clean Duplicates fingerprints `.sub` files by size and CRC32 and deletes copies.
- On-device editor for frequencies, the hopper list, and custom presets (`setting_user`).
- Direct frequency entry.
- Auto detection popup that names a connected CC1101 or Si4463.
- GPS from a USART NMEA module (auto baud) or from the mobile app gps. Saves write latitude and longitude into the `.sub`.
- Encode to RAW from a decoded signal.
- Edit RAW from more menu.
- Fast modulation and frequency hopping.

### Radio

- External Si4463 driver.
- CC1101 register image that writes only the registers that change (used by hopping).

### Companion App

Protobuf `thedoctorz/flipperzero-protobuf` at `670ba85`, schema 0.33.

- Decode a RAW file and stream hits.
- Read frequencies, hopper list, presets, and the default frequency.
- Start, stop, and reconfigure receive. Events are RSSI, hits, and stopped.
- Transmit a Flipper Format payload or a file, on the internal, external, or automatic radio.
- Remote keyboard: insert, backspace, and submit into text, byte, and number inputs.
- Screen-stream input focus is off unless the client asks for it.
- Ranged storage read.

### Platform

- BLE stack is `ble_full`.
- GAP scan and GAP central (connect, discover, read, write, notify, MTU). CLI: `bt scan`, `bt connect`.
- USB connect can share the SD card. USB Storage app exposes the card as a drive.
- Archive filename search.
- Desktop settings can edit the main menu (`/ext/apps_data/loader/mainmenu.txt`, up to 32 apps).
- Replaced external dolphin animation pack by a naughty one!

### Bundled apps

- SubGHz Analyzer — Deep signal analysis and land on a recordable CC1101 preset
- Sub-GHz Bruteforcer — Fixes
- Jammer
- NFC Sniffer — ISO14443-A passive HF sniffer
- Firmware Updater — verify and install SD packages on device

Further community apps are built in doctor-plugins and shipped in the `e` package.

## Removed

- NFC
- 125 kHz RFID
- iButton, 1-Wire, and the music worker
- JavaScript, HID, unit tests, and debug apps
- Firmware docs
- Bin RAW
