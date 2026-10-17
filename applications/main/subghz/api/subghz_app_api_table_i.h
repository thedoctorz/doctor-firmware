#include "../subghz_i.h"

static constexpr auto subghz_app_api_table = sort(create_array_t<sym_entry>(
    API_METHOD(subghz_txrx_get_setting, SubGhzSetting*, (SubGhzTxRx*)),
    API_METHOD(subghz_txrx_radio_device_is_frequency_valid, bool, (SubGhzTxRx*, uint32_t)),
    API_METHOD(subghz_last_settings_save, bool, (SubGhzLastSettings*)),
    API_METHOD(subghz_txrx_radio_device_get, SubGhzRadioDeviceType, (SubGhzTxRx*)),
    API_METHOD(subghz_file_name_clear, void, (SubGhz*)),
    API_METHOD(
        subghz_txrx_set_preset,
        void,
        (SubGhzTxRx*, const char*, uint32_t, uint8_t*, size_t))));
