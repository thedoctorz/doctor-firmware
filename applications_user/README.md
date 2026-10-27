Bundled apps shipped with The Doctor's Firmware (Sub-GHz focused).

`MENUEXTERNAL` apps listed in `applications/main/application.fam` land on the default main menu. `EXTERNAL` apps always go into the updater as `.fap` files under Apps (category from `fap_category`); add them to the main menu from Desktop settings if you want them there.

| App | Role | Ships as |
|-----|------|----------|
| `subghz_analyzer` | Find a signal and land on a recordable CC1101 preset | Main menu |
| `freq_analyzer` | Standalone frequency analyzer | Apps → Sub-GHz |
| `mod_analyzer` | AM vs FM from RSSI envelope | Apps → Sub-GHz |
| `spectrum_analyzer` | Spectrogram | Apps → Sub-GHz |
| `subghz_bruteforcer` | Static-code brute-force | Main menu |
| `subghz_raw_edit` | Trim RAW `.sub` captures | Apps → Sub-GHz |
| `nfc_sniffer` | ISO14443-A HF passive sniffer | Main menu |
| `fw_updater` | Install firmware packages from the SD card | Main menu |
| `usb_msc` | USB mass storage of the SD card | Apps → USB |
| `apps_menu` | Category list of SD apps | Apps → Tools |

Drop additional `.fap` sources here only when building your own extras. Community extra apps are **not** stored here: they are compiled in [doctor-plugins](https://github.com/thedoctorz/doctor-plugins) and shipped in the firmware `e` package.
