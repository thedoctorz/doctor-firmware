#include "cc1101_ext.h"
#include <lib/subghz/devices/cc1101_configs.h>

#include <furi_hal_version.h>
#include <furi_hal_rtc.h>
#include <furi_hal_spi.h>
#include <furi_hal_interrupt.h>
#include <furi_hal_resources.h>
#include <furi_hal_bus.h>

#include <stm32wbxx_ll_dma.h>
#include <stm32wbxx_ll_dmamux.h>
#include <stm32wbxx_ll_exti.h>
#include <stm32wbxx_ll_spi.h>
#include <furi_hal_cortex.h>

#include <furi.h>
#include <cc1101.h>
#include <cc1101_image.h>
#include <stdio.h>

#define TAG "SubGhzDeviceCc1101Ext"

#define SUBGHZ_DEVICE_CC1101_EXT_TX_GPIO               (&gpio_ext_pb2)
#define SUBGHZ_DEVICE_CC1101_EXT_E07_AMP_GPIO          &gpio_ext_pc3
#define SUBGHZ_DEVICE_CC1101_EXT_FORCE_DANGEROUS_RANGE false

#define SUBGHZ_DEVICE_CC1101_CONFIG_VER 1

/* DMA Channels definition */
#define SUBGHZ_DEVICE_CC1101_EXT_DMA             (DMA2)
#define SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_CHANNEL (LL_DMA_CHANNEL_3)
#define SUBGHZ_DEVICE_CC1101_EXT_DMA_CH4_CHANNEL (LL_DMA_CHANNEL_4)
#define SUBGHZ_DEVICE_CC1101_EXT_DMA_CH5_CHANNEL (LL_DMA_CHANNEL_5)
#define SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_IRQ     (FuriHalInterruptIdDma2Ch3)
#define SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_DEF \
    SUBGHZ_DEVICE_CC1101_EXT_DMA, SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_CHANNEL
#define SUBGHZ_DEVICE_CC1101_EXT_DMA_CH4_DEF \
    SUBGHZ_DEVICE_CC1101_EXT_DMA, SUBGHZ_DEVICE_CC1101_EXT_DMA_CH4_CHANNEL
#define SUBGHZ_DEVICE_CC1101_EXT_DMA_CH5_DEF \
    SUBGHZ_DEVICE_CC1101_EXT_DMA, SUBGHZ_DEVICE_CC1101_EXT_DMA_CH5_CHANNEL

/** Low level buffer dimensions and guard times */
#define SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_BUFFER_FULL (256u)
#define SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_BUFFER_HALF \
    (SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_BUFFER_FULL / 2)
#define SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_GUARD_TIME (999u >> 1)

/** SubGhz state */
typedef enum {
    SubGhzDeviceCC1101ExtStateInit, /**< Init pending */
    SubGhzDeviceCC1101ExtStateIdle, /**< Idle, energy save mode */
    SubGhzDeviceCC1101ExtStateAsyncRx, /**< Async RX started */
    SubGhzDeviceCC1101ExtStateAsyncTx, /**< Async TX started, DMA and timer is on */
} SubGhzDeviceCC1101ExtState;

/** SubGhz regulation, receive transmission on the current frequency for the
 * region */
typedef enum {
    SubGhzDeviceCC1101ExtRegulationOnlyRx, /**only Rx*/
    SubGhzDeviceCC1101ExtRegulationTxRx, /**TxRx*/
} SubGhzDeviceCC1101ExtRegulation;

typedef enum {
    SubGhzDeviceCC1101ExtAsyncTxMiddlewareStateIdle,
    SubGhzDeviceCC1101ExtAsyncTxMiddlewareStateReset,
    SubGhzDeviceCC1101ExtAsyncTxMiddlewareStateRun,
} SubGhzDeviceCC1101ExtAsyncTxMiddlewareState;

typedef struct {
    SubGhzDeviceCC1101ExtAsyncTxMiddlewareState state;
    bool is_odd_level;
    uint32_t adder_duration;
} SubGhzDeviceCC1101ExtAsyncTxMiddleware;

typedef struct {
    uint32_t* buffer;
    SubGhzDeviceCC1101ExtCallback callback;
    void* callback_context;
    uint32_t gpio_tx_buff[2];
    uint32_t debug_gpio_buff[2];
    SubGhzDeviceCC1101ExtAsyncTxMiddleware middleware;
} SubGhzDeviceCC1101ExtAsyncTx;

#define CC1101_RX_SAMPLES       2048u
#define CC1101_RX_SERVICE_TICKS 500u
typedef struct {
    uint32_t capture_delta_duration;
    SubGhzDeviceCC1101ExtCaptureCallback capture_callback;
    void* capture_callback_context;
    uint32_t timer_ticks;
    uint32_t last_ticks;
    uint16_t idle_wraps;
    bool discard_next;
    bool last_pin_high;
    uint16_t* timestamps;
    uint8_t* levels;
    uint32_t read_index;
    uint32_t clear_mask;
    uint32_t saved_request[3];
    bool irq_was_enabled;
    bool event_was_enabled;
    bool stopping;
    bool flooding;
    uint32_t service_ticks;
} SubGhzDeviceCC1101ExtAsyncRx;

typedef struct {
    volatile SubGhzDeviceCC1101ExtState state;
    volatile SubGhzDeviceCC1101ExtRegulation regulation;
    const GpioPin* async_mirror_pin;
    const FuriHalSpiBusHandle* spi_bus_handle;
    const GpioPin* g0_pin;
    SubGhzDeviceCC1101ExtAsyncTx async_tx;
    SubGhzDeviceCC1101ExtAsyncRx async_rx;
    bool amp_and_leds;
    bool extended_range;
    volatile bool hop_blank;
} SubGhzDeviceCC1101Ext;

static SubGhzDeviceCC1101Ext* subghz_device_cc1101_ext = NULL;

#define SUBGHZ_DEVICE_CC1101_EXT_FSCAL_CACHE_SIZE 16
#define SUBGHZ_DEVICE_CC1101_EXT_FSCAL_TTL_MS     30000
#define SUBGHZ_DEVICE_CC1101_EXT_FS_AUTOCAL_MASK  0x30
#define SUBGHZ_DEVICE_CC1101_EXT_FSCAL1_UNLOCKED  0x3F

#define SUBGHZ_DEVICE_CC1101_EXT_HOP_STATE_TIMEOUT_US 10000
#define SUBGHZ_DEVICE_CC1101_EXT_HOP_CAL_TIMEOUT_US   20000

typedef struct {
    uint32_t frequency;
    uint32_t stamp;
    uint8_t fsctrl1, fsctrl0, channr, mdmcfg1, mdmcfg0;
    uint8_t fscal3, fscal2, fscal1;
} SubGhzDeviceCC1101ExtFscalEntry;

static CC1101Image subghz_device_cc1101_ext_por;
static CC1101Image subghz_device_cc1101_ext_shadow;
static bool subghz_device_cc1101_ext_por_valid = false;
static bool subghz_device_cc1101_ext_shadow_valid = false;
static uint32_t subghz_device_cc1101_ext_hop_frequency = 0;
static bool subghz_device_cc1101_ext_hop_spi_held = false;
static SubGhzDeviceCC1101ExtFscalEntry
    subghz_device_cc1101_ext_fscal[SUBGHZ_DEVICE_CC1101_EXT_FSCAL_CACHE_SIZE];

void subghz_device_cc1101_ext_shadow_invalidate(void) {
    subghz_device_cc1101_ext_shadow_valid = false;
    subghz_device_cc1101_ext_hop_frequency = 0;
}

void subghz_device_cc1101_ext_hop_cache_reset(void) {
    memset(subghz_device_cc1101_ext_fscal, 0, sizeof(subghz_device_cc1101_ext_fscal));
}

static void subghz_device_cc1101_ext_shadow_set_from(const uint8_t* data) {
    if(!subghz_device_cc1101_ext_por_valid) return;

    subghz_device_cc1101_ext_shadow = subghz_device_cc1101_ext_por;
    subghz_device_cc1101_ext_shadow.reg[CC1101_IOCFG0] = CC1101IocfgHighImpedance;
    subghz_device_cc1101_ext_shadow.reg[CC1101_IOCFG2] = CC1101IocfgHighImpedance;
    cc1101_image_apply_preset(
        &subghz_device_cc1101_ext_shadow, data, &subghz_device_cc1101_ext_shadow);
    subghz_device_cc1101_ext_shadow_valid = true;
}

static void subghz_device_cc1101_ext_spi_event_callback(
    const FuriHalSpiBusHandle* handle,
    FuriHalSpiBusHandleEvent event) {
    if(event == FuriHalSpiBusHandleEventInit) {
        furi_hal_gpio_write(handle->cs, true);
        furi_hal_gpio_init(handle->cs, GpioModeOutputPushPull, GpioPullUp, GpioSpeedLow);
    } else if(event == FuriHalSpiBusHandleEventDeinit) {
        furi_hal_gpio_write(handle->cs, true);
        furi_hal_gpio_init(handle->cs, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    } else if(event == FuriHalSpiBusHandleEventActivate) {
        LL_SPI_Init(handle->bus->spi, (LL_SPI_InitTypeDef*)&furi_hal_spi_preset_1edge_low_2m);
        LL_SPI_SetRxFIFOThreshold(handle->bus->spi, LL_SPI_RX_FIFO_TH_QUARTER);
        LL_SPI_Enable(handle->bus->spi);
        furi_hal_gpio_init_ex(
            handle->miso, GpioModeAltFunctionPushPull, GpioPullDown, GpioSpeedLow, GpioAltFn5SPI1);
        furi_hal_gpio_init_ex(
            handle->mosi, GpioModeAltFunctionPushPull, GpioPullDown, GpioSpeedLow, GpioAltFn5SPI1);
        furi_hal_gpio_init_ex(
            handle->sck, GpioModeAltFunctionPushPull, GpioPullDown, GpioSpeedLow, GpioAltFn5SPI1);
        furi_hal_gpio_write(handle->cs, false);
    } else if(event == FuriHalSpiBusHandleEventDeactivate) {
        furi_hal_gpio_write(handle->cs, true);
        furi_hal_gpio_init(handle->miso, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
        furi_hal_gpio_init(handle->mosi, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
        furi_hal_gpio_init(handle->sck, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
        LL_SPI_Disable(handle->bus->spi);
    }
}

enum {
    SubGhzDeviceCC1101ExtLinkUnknown,
    SubGhzDeviceCC1101ExtLinkGood,
    SubGhzDeviceCC1101ExtLinkBad,
};
static volatile uint8_t subghz_device_cc1101_ext_link = SubGhzDeviceCC1101ExtLinkUnknown;

static const FuriHalSpiBusHandle subghz_device_cc1101_ext_spi_handle = {
    .bus = &furi_hal_spi_bus_r,
    .callback = subghz_device_cc1101_ext_spi_event_callback,
    .miso = &gpio_ext_pa6,
    .mosi = &gpio_ext_pa7,
    .sck = &gpio_ext_pb3,
    .cs = &gpio_ext_pa4,
};

static void subghz_device_cc1101_ext_miso_spi(const FuriHalSpiBusHandle* handle) {
    furi_hal_gpio_init_ex(
        handle->miso, GpioModeAltFunctionPushPull, GpioPullDown, GpioSpeedLow, GpioAltFn5SPI1);
}

static bool subghz_device_cc1101_ext_version_is_cc1101(void) {
    const FuriHalSpiBusHandle* handle = subghz_device_cc1101_ext->spi_bus_handle;

    furi_hal_gpio_init(handle->miso, GpioModeInput, GpioPullUp, GpioSpeedLow);
    furi_hal_gpio_write(handle->cs, false);

    bool ready = false;
    FuriHalCortexTimer ready_timer = furi_hal_cortex_timer_get(10 * 1000);
    while(!furi_hal_cortex_timer_is_expired(ready_timer)) {
        if(!furi_hal_gpio_read(handle->miso)) {
            ready = true;
            break;
        }
    }

    if(!ready) {
        furi_hal_gpio_write(handle->cs, true);
        furi_hal_gpio_init(handle->miso, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
        return false;
    }

    subghz_device_cc1101_ext_miso_spi(handle);
    uint8_t tx[2] = {CC1101_STATUS_VERSION | CC1101_BURST | CC1101_READ, 0};
    uint8_t rx[2] = {0};
    bool trx_ok = furi_hal_spi_bus_trx(handle, tx, rx, sizeof(tx), 20);
    bool is_cc1101 = trx_ok && ((rx[0] & 0x80) == 0) && (rx[1] == 0x04 || rx[1] == 0x14);
    if(!is_cc1101) {
        FURI_LOG_E(TAG, "VERSION %02X %02X", rx[0], rx[1]);
        furi_hal_gpio_write(handle->cs, true);
        furi_hal_gpio_init(handle->miso, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    }
    return is_cc1101;
}

static bool subghz_device_cc1101_ext_check_init(void) {
    furi_assert(subghz_device_cc1101_ext->state == SubGhzDeviceCC1101ExtStateInit);
    subghz_device_cc1101_ext->state = SubGhzDeviceCC1101ExtStateIdle;

    bool ret = false;
    CC1101Status cc1101_status = {0};

    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    FuriHalCortexTimer timer = furi_hal_cortex_timer_get(100 * 1000);
    do {
        // Reset
        furi_hal_gpio_init(
            subghz_device_cc1101_ext->g0_pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
        if(!subghz_device_cc1101_ext_version_is_cc1101()) {
            break;
        }

        cc1101_status = cc1101_reset(subghz_device_cc1101_ext->spi_bus_handle);
        if(cc1101_status.CHIP_RDYn != 0) {
            //timeout or error
            break;
        }

        furi_hal_gpio_init_ex(
            subghz_device_cc1101_ext->spi_bus_handle->miso,
            GpioModeAltFunctionPushPull,
            GpioPullDown,
            GpioSpeedVeryHigh,
            GpioAltFn5SPI1);

        cc1101_image_read(subghz_device_cc1101_ext->spi_bus_handle, &subghz_device_cc1101_ext_por);
        subghz_device_cc1101_ext_por_valid = true;

        cc1101_status = cc1101_write_reg(
            subghz_device_cc1101_ext->spi_bus_handle, CC1101_IOCFG0, CC1101IocfgHighImpedance);
        if(cc1101_status.CHIP_RDYn != 0) {
            //timeout or error
            break;
        }
        // Prepare GD0 for power on self test
        furi_hal_gpio_init(
            subghz_device_cc1101_ext->g0_pin, GpioModeInput, GpioPullUp, GpioSpeedLow);

        // GD0 low
        cc1101_status = cc1101_write_reg(
            subghz_device_cc1101_ext->spi_bus_handle, CC1101_IOCFG0, CC1101IocfgHW);
        if(cc1101_status.CHIP_RDYn != 0) {
            //timeout or error
            break;
        }
        while(furi_hal_gpio_read(subghz_device_cc1101_ext->g0_pin) != false) {
            if(furi_hal_cortex_timer_is_expired(timer)) {
                //timeout
                break;
            }
        }
        if(furi_hal_cortex_timer_is_expired(timer)) {
            //timeout
            break;
        }

        // GD0 high
        furi_hal_gpio_init(
            subghz_device_cc1101_ext->g0_pin, GpioModeInput, GpioPullDown, GpioSpeedLow);
        cc1101_status = cc1101_write_reg(
            subghz_device_cc1101_ext->spi_bus_handle,
            CC1101_IOCFG0,
            CC1101IocfgHW | CC1101_IOCFG_INV);
        if(cc1101_status.CHIP_RDYn != 0) {
            //timeout or error
            break;
        }
        while(furi_hal_gpio_read(subghz_device_cc1101_ext->g0_pin) != true) {
            if(furi_hal_cortex_timer_is_expired(timer)) {
                //timeout
                break;
            }
        }
        if(furi_hal_cortex_timer_is_expired(timer)) {
            //timeout
            break;
        }

        // Reset GD0 to floating state
        cc1101_status = cc1101_write_reg(
            subghz_device_cc1101_ext->spi_bus_handle, CC1101_IOCFG0, CC1101IocfgHighImpedance);
        if(cc1101_status.CHIP_RDYn != 0) {
            //timeout or error
            break;
        }
        furi_hal_gpio_init(
            subghz_device_cc1101_ext->g0_pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);

        // Reset GDO2 (!TX/RX) to floating state
        cc1101_status = cc1101_write_reg(
            subghz_device_cc1101_ext->spi_bus_handle, CC1101_IOCFG2, CC1101IocfgHighImpedance);
        if(cc1101_status.CHIP_RDYn != 0) {
            //timeout or error
            break;
        }

        // Go to sleep
        cc1101_status = cc1101_shutdown(subghz_device_cc1101_ext->spi_bus_handle);
        if(cc1101_status.CHIP_RDYn != 0) {
            //timeout or error
            break;
        }
        ret = true;
    } while(false);

    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);

    if(ret) {
        FURI_LOG_I(TAG, "Init OK");
    } else {
        FURI_LOG_E(TAG, "Init failed");
        furi_hal_gpio_init(
            subghz_device_cc1101_ext->g0_pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    }
    return ret;
}

bool subghz_device_cc1101_ext_alloc(SubGhzDeviceConf* conf) {
    furi_assert(subghz_device_cc1101_ext == NULL);
    subghz_device_cc1101_ext = malloc(sizeof(SubGhzDeviceCC1101Ext));
    subghz_device_cc1101_ext->state = SubGhzDeviceCC1101ExtStateInit;
    subghz_device_cc1101_ext->regulation = SubGhzDeviceCC1101ExtRegulationTxRx;
    subghz_device_cc1101_ext->async_mirror_pin = NULL;
    subghz_device_cc1101_ext->spi_bus_handle = &subghz_device_cc1101_ext_spi_handle;
    subghz_device_cc1101_ext->g0_pin = SUBGHZ_DEVICE_CC1101_EXT_TX_GPIO;
    subghz_device_cc1101_ext->amp_and_leds = false;
    subghz_device_cc1101_ext->extended_range = false;
    subghz_device_cc1101_ext->hop_blank = false;
    if(conf) {
        if(conf->ver == SUBGHZ_DEVICE_CC1101_CONFIG_VER) {
            subghz_device_cc1101_ext->amp_and_leds = conf->amp_and_leds;
            subghz_device_cc1101_ext->extended_range = conf->extended_range;
        } else {
            FURI_LOG_E(TAG, "Config version mismatch");
        }
    }

    subghz_device_cc1101_ext->async_rx.capture_delta_duration = 0;

    furi_hal_spi_bus_handle_init(subghz_device_cc1101_ext->spi_bus_handle);

    bool ok = subghz_device_cc1101_ext_check_init();
    if(ok && subghz_device_cc1101_ext->amp_and_leds) {
        furi_hal_gpio_init_simple(SUBGHZ_DEVICE_CC1101_EXT_E07_AMP_GPIO, GpioModeOutputPushPull);
    }

    return ok;
}

void subghz_device_cc1101_ext_free(void) {
    furi_assert(subghz_device_cc1101_ext != NULL);

    subghz_device_cc1101_ext_hop_end();
    furi_hal_spi_bus_handle_deinit(subghz_device_cc1101_ext->spi_bus_handle);
    if(subghz_device_cc1101_ext->amp_and_leds) {
        furi_hal_gpio_init_simple(SUBGHZ_DEVICE_CC1101_EXT_E07_AMP_GPIO, GpioModeAnalog);
    }

    subghz_device_cc1101_ext_shadow_invalidate();
    subghz_device_cc1101_ext_hop_cache_reset();

    free(subghz_device_cc1101_ext);
    subghz_device_cc1101_ext = NULL;
}

void subghz_device_cc1101_ext_set_async_mirror_pin(const GpioPin* pin) {
    FURI_CRITICAL_ENTER();
    const GpioPin* previous = subghz_device_cc1101_ext->async_mirror_pin;
    subghz_device_cc1101_ext->async_mirror_pin = pin;
    if(subghz_device_cc1101_ext->state == SubGhzDeviceCC1101ExtStateAsyncRx) {
        if(previous && previous != pin)
            furi_hal_gpio_init(previous, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
        if(pin) {
            furi_hal_gpio_init(pin, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
            LL_DMA_ClearFlag_TC5(DMA2);
            LL_DMA_EnableIT_TC(DMA2, LL_DMA_CHANNEL_5);
        } else {
            LL_DMA_DisableIT_TC(DMA2, LL_DMA_CHANNEL_5);
        }
    }
    FURI_CRITICAL_EXIT();
}

const GpioPin* subghz_device_cc1101_ext_get_data_gpio(void) {
    return subghz_device_cc1101_ext->g0_pin;
}

bool subghz_device_cc1101_ext_is_connect(void) {
    bool ret = false;

    if(subghz_device_cc1101_ext == NULL) { // not initialized
        ret = subghz_device_cc1101_ext_alloc(NULL);
        subghz_device_cc1101_ext_free();
    } else { // initialized
        FURI_CRITICAL_ENTER();
        const uint8_t link = subghz_device_cc1101_ext_link;
        subghz_device_cc1101_ext_link = SubGhzDeviceCC1101ExtLinkUnknown;
        FURI_CRITICAL_EXIT();
        if(link == SubGhzDeviceCC1101ExtLinkGood) return true;
        furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
        //a genuine CC1101 reads PARTNUM as 0x00, same as an empty bus; only VERSION
        //separates them
        uint8_t version = cc1101_get_version(subghz_device_cc1101_ext->spi_bus_handle);
        furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
        ret = (version != 0) && (version != 0xFF);
    }

    return ret;
}

void subghz_device_cc1101_ext_sleep(void) {
    furi_assert(subghz_device_cc1101_ext->state == SubGhzDeviceCC1101ExtStateIdle);
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);

    cc1101_switch_to_idle(subghz_device_cc1101_ext->spi_bus_handle);

    cc1101_write_reg(
        subghz_device_cc1101_ext->spi_bus_handle, CC1101_IOCFG0, CC1101IocfgHighImpedance);
    furi_hal_gpio_init(subghz_device_cc1101_ext->g0_pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);

    cc1101_shutdown(subghz_device_cc1101_ext->spi_bus_handle);

    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);

    subghz_device_cc1101_ext_shadow_invalidate();
}

void subghz_device_cc1101_ext_dump_state(void) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    printf(
        "[subghz_device_cc1101_ext] cc1101 chip %d, version %d\r\n",
        cc1101_get_partnumber(subghz_device_cc1101_ext->spi_bus_handle),
        cc1101_get_version(subghz_device_cc1101_ext->spi_bus_handle));
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
}

void subghz_device_cc1101_ext_load_custom_preset(const uint8_t* preset_data) {
    //load config
    subghz_device_cc1101_ext_reset();
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    uint32_t i = 0;
    uint8_t pa[8] = {0};
    while(preset_data[i]) {
        if((preset_data[i] & 0xC0) == 0) {
            cc1101_write_reg(
                subghz_device_cc1101_ext->spi_bus_handle, preset_data[i], preset_data[i + 1]);
        }
        i += 2;
    }
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);

    subghz_device_cc1101_ext_shadow_set_from(preset_data);

    //load pa table
    memcpy(&pa[0], &preset_data[i + 2], 8);
    subghz_device_cc1101_ext_load_patable(pa);

    //show debug
    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug)) {
        i = 0;
        FURI_LOG_D(TAG, "Loading custom preset");
        while(preset_data[i]) {
            FURI_LOG_D(TAG, "Reg[%lu]: %02X=%02X", i, preset_data[i], preset_data[i + 1]);
            i += 2;
        }
        for(uint8_t y = i; y < i + 10; y++) {
            FURI_LOG_D(TAG, "PA[%u]:  %02X", y, preset_data[y]);
        }
    }
}

void subghz_device_cc1101_ext_load_registers(const uint8_t* data) {
    subghz_device_cc1101_ext_reset();
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    uint32_t i = 0;
    while(data[i]) {
        if((data[i] & 0xC0) == 0) {
            cc1101_write_reg(subghz_device_cc1101_ext->spi_bus_handle, data[i], data[i + 1]);
        }
        i += 2;
    }
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);

    subghz_device_cc1101_ext_shadow_set_from(data);
}

void subghz_device_cc1101_ext_load_patable(const uint8_t data[8]) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    cc1101_set_pa_table(subghz_device_cc1101_ext->spi_bus_handle, data);
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
}

void subghz_device_cc1101_ext_write_packet(const uint8_t* data, uint8_t size) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    cc1101_flush_tx(subghz_device_cc1101_ext->spi_bus_handle);
    cc1101_write_reg(subghz_device_cc1101_ext->spi_bus_handle, CC1101_FIFO, size);
    cc1101_write_fifo(subghz_device_cc1101_ext->spi_bus_handle, data, size);
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
}

void subghz_device_cc1101_ext_flush_rx(void) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    cc1101_flush_rx(subghz_device_cc1101_ext->spi_bus_handle);
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
}

void subghz_device_cc1101_ext_flush_tx(void) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    cc1101_flush_tx(subghz_device_cc1101_ext->spi_bus_handle);
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
}

bool subghz_device_cc1101_ext_rx_pipe_not_empty(void) {
    CC1101RxBytes status[1];
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    cc1101_read_reg(
        subghz_device_cc1101_ext->spi_bus_handle,
        (CC1101_STATUS_RXBYTES) | CC1101_BURST,
        (uint8_t*)status);
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
    // TODO: Find reason why RXFIFO_OVERFLOW doesnt work correctly
    if(status->NUM_RXBYTES > 0) {
        return true;
    } else {
        return false;
    }
}

bool subghz_device_cc1101_ext_is_rx_data_crc_valid(void) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    uint8_t data[1];
    cc1101_read_reg(
        subghz_device_cc1101_ext->spi_bus_handle, CC1101_STATUS_LQI | CC1101_BURST, data);
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
    if((data[0] >> 7) & 0x01) {
        return true;
    } else {
        return false;
    }
}

void subghz_device_cc1101_ext_read_packet(uint8_t* data, uint8_t* size) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    cc1101_read_fifo(subghz_device_cc1101_ext->spi_bus_handle, data, size);
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
}

void subghz_device_cc1101_ext_shutdown(void) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    // Reset and shutdown
    cc1101_shutdown(subghz_device_cc1101_ext->spi_bus_handle);
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
}

void subghz_device_cc1101_ext_reset(void) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    furi_hal_gpio_init(subghz_device_cc1101_ext->g0_pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    cc1101_switch_to_idle(subghz_device_cc1101_ext->spi_bus_handle);
    cc1101_reset(subghz_device_cc1101_ext->spi_bus_handle);

    cc1101_image_read(subghz_device_cc1101_ext->spi_bus_handle, &subghz_device_cc1101_ext_por);
    subghz_device_cc1101_ext_por_valid = true;

    // Warning: push pull cc1101 clock output on GD0
    cc1101_write_reg(
        subghz_device_cc1101_ext->spi_bus_handle, CC1101_IOCFG0, CC1101IocfgHighImpedance);
    // Reset GDO2 (!TX/RX) to floating state
    cc1101_write_reg(
        subghz_device_cc1101_ext->spi_bus_handle, CC1101_IOCFG2, CC1101IocfgHighImpedance);

    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);

    subghz_device_cc1101_ext_shadow = subghz_device_cc1101_ext_por;
    subghz_device_cc1101_ext_shadow.reg[CC1101_IOCFG0] = CC1101IocfgHighImpedance;
    subghz_device_cc1101_ext_shadow.reg[CC1101_IOCFG2] = CC1101IocfgHighImpedance;
    subghz_device_cc1101_ext_shadow_valid = true;
    subghz_device_cc1101_ext_hop_frequency = 0;
}

//The module is hot-pluggable, so a state transition that never happens has to be
//survivable rather than fatal. The state is logged because it separates a module
//that is gone (its bus reads back as IDLE) from one that is wedged
static bool subghz_device_cc1101_ext_wait_state(CC1101State state, const char* name) {
    if(cc1101_wait_status_state(subghz_device_cc1101_ext->spi_bus_handle, state, 10000)) {
        return true;
    }
    const CC1101Status status =
        cc1101_strobe(subghz_device_cc1101_ext->spi_bus_handle, CC1101_STROBE_SNOP);
    FURI_LOG_E(TAG, "Timeout switching to %s, chip state %u", name, status.STATE);
    return false;
}

void subghz_device_cc1101_ext_idle(void) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    cc1101_switch_to_idle(subghz_device_cc1101_ext->spi_bus_handle);
    subghz_device_cc1101_ext_wait_state(CC1101StateIDLE, "IDLE");

    //unconditional - leaving the E07 amp keyed is worse than an unreached state
    furi_hal_gpio_write(SUBGHZ_DEVICE_CC1101_EXT_E07_AMP_GPIO, 0);
    // Reset GDO2 (!TX/RX) to floating state
    cc1101_write_reg(
        subghz_device_cc1101_ext->spi_bus_handle, CC1101_IOCFG2, CC1101IocfgHighImpedance);

    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
}

void subghz_device_cc1101_ext_rx(void) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    cc1101_switch_to_rx(subghz_device_cc1101_ext->spi_bus_handle);
    subghz_device_cc1101_ext_wait_state(CC1101StateRX, "RX");

    if(subghz_device_cc1101_ext->amp_and_leds) {
        furi_hal_gpio_write(SUBGHZ_DEVICE_CC1101_EXT_E07_AMP_GPIO, 0);
        // Go GDO2 (!TX/RX) to high (RX state)
        cc1101_write_reg(
            subghz_device_cc1101_ext->spi_bus_handle,
            CC1101_IOCFG2,
            CC1101IocfgHW | CC1101_IOCFG_INV);
    }

    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
}

bool subghz_device_cc1101_ext_tx(void) {
    if(subghz_device_cc1101_ext->regulation != SubGhzDeviceCC1101ExtRegulationTxRx) return false;
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    cc1101_switch_to_tx(subghz_device_cc1101_ext->spi_bus_handle);
    const bool in_tx = subghz_device_cc1101_ext_wait_state(CC1101StateTX, "TX");

    //never key the amp at a chip we have no confirmation is transmitting
    if(in_tx && subghz_device_cc1101_ext->amp_and_leds) {
        furi_hal_gpio_write(SUBGHZ_DEVICE_CC1101_EXT_E07_AMP_GPIO, 1);
        // Go GDO2 (!TX/RX) to low (TX state)
        cc1101_write_reg(subghz_device_cc1101_ext->spi_bus_handle, CC1101_IOCFG2, CC1101IocfgHW);
    }

    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
    return in_tx;
}
static inline uint8_t subghz_device_cc1101_ext_rssi_raw(void) {
    uint8_t tx[2] = {CC1101_STATUS_RSSI | CC1101_BURST | CC1101_READ, 0};
    uint8_t rx[2] = {0};
    furi_hal_spi_bus_trx(subghz_device_cc1101_ext->spi_bus_handle, tx, rx, 2, CC1101_TIMEOUT);
    if((rx[0] & 0xF0) != (CC1101StateRX << 4)) {
        subghz_device_cc1101_ext_link = SubGhzDeviceCC1101ExtLinkBad;
        return 0x80;
    } else if(subghz_device_cc1101_ext_link == SubGhzDeviceCC1101ExtLinkUnknown) {
        subghz_device_cc1101_ext_link = SubGhzDeviceCC1101ExtLinkGood;
    }
    return rx[1];
}

float subghz_device_cc1101_ext_get_rssi(void) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    int32_t rssi_dec = subghz_device_cc1101_ext_rssi_raw();
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);

    float rssi = rssi_dec;
    if(rssi_dec >= 128) {
        rssi = ((rssi - 256.0f) / 2.0f) - 74.0f;
    } else {
        rssi = (rssi / 2.0f) - 74.0f;
    }

    return rssi;
}

uint32_t subghz_device_cc1101_ext_rssi_burst(uint8_t* out, size_t count, uint32_t period_us) {
    furi_check(out);
    if(count == 0) return 0;

    const uint32_t cycles_per_us = furi_hal_cortex_instructions_per_microsecond();
    const uint32_t period_cycles = period_us * cycles_per_us;

    const bool spi_owned = subghz_device_cc1101_ext_hop_spi_held;
    if(!spi_owned) furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    const uint32_t t0 = DWT->CYCCNT;
    for(size_t i = 0; i < count; i++) {
        const uint32_t deadline = t0 + (uint32_t)i * period_cycles;
        while((int32_t)(DWT->CYCCNT - deadline) < 0) {
        }
        out[i] = subghz_device_cc1101_ext_rssi_raw();
    }
    const uint32_t span = DWT->CYCCNT - t0;
    if(!spi_owned) furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);

    return span / cycles_per_us;
}

uint8_t subghz_device_cc1101_ext_get_lqi(void) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    uint8_t data[1];
    cc1101_read_reg(
        subghz_device_cc1101_ext->spi_bus_handle, CC1101_STATUS_LQI | CC1101_BURST, data);
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
    return data[0] & 0x7F;
}

int8_t subghz_device_cc1101_ext_get_freq_est(void) {
    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    uint8_t data[1];
    cc1101_read_reg(
        subghz_device_cc1101_ext->spi_bus_handle, CC1101_STATUS_FREQEST | CC1101_BURST, data);
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
    return (int8_t)data[0];
}

bool subghz_device_cc1101_ext_is_frequency_valid(uint32_t value) {
    if(!(value >= 281000000 && value <= 361000000) &&
       !(value >= 378000000 && value <= 481000000) &&
       !(value >= 749000000 && value <= 962000000)) {
        return false;
    }

    return true;
}

bool subghz_device_cc1101_ext_is_tx_allowed(uint32_t value) {
    if(!(SUBGHZ_DEVICE_CC1101_EXT_FORCE_DANGEROUS_RANGE ||
         subghz_device_cc1101_ext->extended_range) &&
       !(value >= 299999755 && value <= 350000335) && // was increased from 348 to 350
       !(value >= 386999938 && value <= 467750000) && // was increased from 464 to 467.75
       !(value >= 778999847 && value <= 928000000)) {
        FURI_LOG_I(TAG, "Frequency blocked - outside default range");
        return false;
    } else if(
        (SUBGHZ_DEVICE_CC1101_EXT_FORCE_DANGEROUS_RANGE ||
         subghz_device_cc1101_ext->extended_range) &&
        !subghz_device_cc1101_ext_is_frequency_valid(value)) {
        FURI_LOG_I(TAG, "Frequency blocked - outside dangerous range");
        return false;
    }

    return true;
}

uint32_t subghz_device_cc1101_ext_set_frequency(uint32_t value) {
    if(subghz_device_cc1101_ext_is_tx_allowed(value)) {
        subghz_device_cc1101_ext->regulation = SubGhzDeviceCC1101ExtRegulationTxRx;
    } else {
        subghz_device_cc1101_ext->regulation = SubGhzDeviceCC1101ExtRegulationTxRx;
    }

    furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    uint32_t real_frequency =
        cc1101_set_frequency(subghz_device_cc1101_ext->spi_bus_handle, value);
    cc1101_calibrate(subghz_device_cc1101_ext->spi_bus_handle);

    //the hopper reaches this every hop - an unanswering module must not spin the
    //calling thread into the watchdog
    subghz_device_cc1101_ext_wait_state(CC1101StateIDLE, "IDLE after calibration");

    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);

    subghz_device_cc1101_ext_hop_frequency = value;
    return real_frequency;
}

static bool subghz_device_cc1101_ext_start_debug(void) {
    bool ret = false;
    if(subghz_device_cc1101_ext->async_mirror_pin != NULL) {
        furi_hal_gpio_init(
            subghz_device_cc1101_ext->async_mirror_pin,
            GpioModeOutputPushPull,
            GpioPullNo,
            GpioSpeedVeryHigh);
        ret = true;
    }
    return ret;
}

static bool subghz_device_cc1101_ext_stop_debug(void) {
    bool ret = false;
    if(subghz_device_cc1101_ext->async_mirror_pin != NULL) {
        furi_hal_gpio_init(
            subghz_device_cc1101_ext->async_mirror_pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
        ret = true;
    }
    return ret;
}

static void subghz_device_cc1101_ext_capture_requests(bool enable) {
    for(uint32_t i = 0; i < 3; i++) {
        if(enable)
            LL_DMAMUX_EnableRequestGen(DMAMUX1, i);
        else
            LL_DMAMUX_DisableRequestGen(DMAMUX1, i);
    }
}

static void subghz_device_cc1101_ext_mirror_ISR(void* context) {
    UNUSED(context);
    LL_DMA_ClearFlag_TC5(DMA2);
    if(subghz_device_cc1101_ext->async_mirror_pin)
        furi_hal_gpio_write(
            subghz_device_cc1101_ext->async_mirror_pin,
            furi_hal_gpio_read(subghz_device_cc1101_ext->g0_pin));
}

static uint32_t subghz_device_cc1101_ext_capture_now(void) {
    SubGhzDeviceCC1101ExtAsyncRx* rx = &subghz_device_cc1101_ext->async_rx;
    uint32_t counter = LL_TIM_GetCounter(TIM17);
    if(LL_TIM_IsActiveFlag_UPDATE(TIM17)) {
        LL_TIM_ClearFlag_UPDATE(TIM17);
        rx->timer_ticks += 0x10000u;
        if(rx->idle_wraps < 8193u) rx->idle_wraps++;
        counter = LL_TIM_GetCounter(TIM17);
    }
    return rx->timer_ticks + counter;
}

static void subghz_device_cc1101_ext_capture_rearm(void) {
    LL_EXTI_DisableIT_0_31(LL_EXTI_LINE_2);
    subghz_device_cc1101_ext_capture_requests(false);
    LL_DMA_DisableChannel(DMA2, LL_DMA_CHANNEL_3);
    LL_DMA_DisableChannel(DMA2, LL_DMA_CHANNEL_4);
    LL_DMA_DisableChannel(DMA2, LL_DMA_CHANNEL_5);
    LL_DMA_SetDataLength(DMA2, LL_DMA_CHANNEL_3, CC1101_RX_SAMPLES);
    LL_DMA_SetDataLength(DMA2, LL_DMA_CHANNEL_4, CC1101_RX_SAMPLES * 3u);
    LL_DMA_SetDataLength(DMA2, LL_DMA_CHANNEL_5, 1);
    LL_DMA_ClearFlag_GI3(DMA2);
    LL_DMA_ClearFlag_GI4(DMA2);
    LL_DMA_ClearFlag_GI5(DMA2);
    LL_DMAMUX_ClearFlag_RGO0(DMAMUX1);
    LL_DMAMUX_ClearFlag_RGO1(DMAMUX1);
    LL_DMAMUX_ClearFlag_RGO2(DMAMUX1);
    subghz_device_cc1101_ext->async_rx.read_index = 0;
    LL_DMA_EnableChannel(DMA2, LL_DMA_CHANNEL_3);
    LL_DMA_EnableChannel(DMA2, LL_DMA_CHANNEL_4);
    LL_DMA_EnableChannel(DMA2, LL_DMA_CHANNEL_5);
    LL_EXTI_ClearFlag_0_31(LL_EXTI_LINE_2);
    subghz_device_cc1101_ext_capture_requests(true);
    LL_EXTI_EnableIT_0_31(LL_EXTI_LINE_2);
}

static void subghz_device_cc1101_ext_capture_drain(void) {
    SubGhzDeviceCC1101ExtAsyncRx* rx = &subghz_device_cc1101_ext->async_rx;
    const bool lost = LL_DMA_IsActiveFlag_TE3(DMA2) || LL_DMA_IsActiveFlag_TE4(DMA2) ||
                      LL_DMA_IsActiveFlag_TE5(DMA2) ||
                      (LL_DMA_IsActiveFlag_HT4(DMA2) && LL_DMA_IsActiveFlag_TC4(DMA2)) ||
                      LL_DMAMUX_IsActiveFlag_RGO0(DMAMUX1) ||
                      LL_DMAMUX_IsActiveFlag_RGO1(DMAMUX1) || LL_DMAMUX_IsActiveFlag_RGO2(DMAMUX1);
    const uint32_t end =
        ((CC1101_RX_SAMPLES * 3u - LL_DMA_GetDataLength(DMA2, LL_DMA_CHANNEL_4)) / 3u) %
        CC1101_RX_SAMPLES;
    LL_DMA_ClearFlag_GI3(DMA2);
    LL_DMA_ClearFlag_GI4(DMA2);
    LL_DMA_ClearFlag_TE5(DMA2);
    LL_DMAMUX_ClearFlag_RGO0(DMAMUX1);
    LL_DMAMUX_ClearFlag_RGO1(DMAMUX1);
    LL_DMAMUX_ClearFlag_RGO2(DMAMUX1);
    // Service the timer overflow before anything can return: the lost path used to leave the
    // TIM17 update flag set, and with capture requests overrunning continuously (sub-microsecond
    // slicer glitches) every re-entry took the lost path again - the ISR never exited.
    __DMB();
    const uint32_t now = subghz_device_cc1101_ext_capture_now();
    // Edge flood guard. Real data tops out near 45k edges/s; a sustained rate above ~150k/s is
    // the pin toggling on its own (a module that reset and outputs its clock on GDO0, a loose
    // wire). Feeding that to the callback edge by edge keeps this ISR busy forever and freezes
    // the whole system, so the block is dropped as lost instead.
    const uint32_t backlog = (end + CC1101_RX_SAMPLES - rx->read_index) % CC1101_RX_SAMPLES;
    const uint32_t elapsed = now - rx->service_ticks; // 2 us units
    rx->service_ticks = now;
    const bool flood = !rx->stopping && backlog >= 64u && backlog * 10u > elapsed * 3u;
    if(lost || flood) {

        if(!rx->stopping)
            subghz_device_cc1101_ext_capture_rearm();
        else
            rx->read_index = end;
        rx->discard_next = true;
        if((!flood || !rx->flooding) && rx->capture_callback)
            rx->capture_callback(false, 0, rx->capture_callback_context);
        rx->flooding = flood;
        return;
    }
    rx->flooding = false;
    while(rx->read_index != end) {
        const uint16_t low = rx->timestamps[rx->read_index];
        const uint8_t* levels = &rx->levels[rx->read_index * 3u];
        const unsigned highs = !!(levels[0] & LL_GPIO_PIN_2) + !!(levels[1] & LL_GPIO_PIN_2) +
                               !!(levels[2] & LL_GPIO_PIN_2);
        const bool pin_high = highs >= 2;
        rx->read_index = (rx->read_index + 1) % CC1101_RX_SAMPLES;
        if(!rx->discard_next && pin_high == rx->last_pin_high) continue;
        rx->last_pin_high = pin_high;
        uint32_t stamp = (now & 0xFFFF0000u) | low;
        if((int32_t)(stamp - now) > 0) stamp -= 0x10000u;
        const uint32_t ticks = rx->idle_wraps >= 8193u ? 0x1FFFFFFFu :
                                                         MIN(stamp - rx->last_ticks, 0x1FFFFFFFu);
        rx->last_ticks = stamp;
        rx->idle_wraps = 0;
        if(subghz_device_cc1101_ext->hop_blank) {
            rx->discard_next = true;
        } else if(rx->discard_next) {
            rx->discard_next = false;
        } else if(ticks && rx->capture_callback) {
            rx->capture_callback(!pin_high, ticks << 1, rx->capture_callback_context);
        }
    }
}

static void subghz_device_cc1101_ext_capture_timer_ISR(void* context) {
    UNUSED(context);
    if(LL_TIM_IsActiveFlag_CC1(TIM17)) {
        LL_TIM_ClearFlag_CC1(TIM17);
        LL_TIM_OC_SetCompareCH1(
            TIM17, (LL_TIM_GetCounter(TIM17) + CC1101_RX_SERVICE_TICKS) & 0xFFFFu);
    }
    subghz_device_cc1101_ext_capture_drain();
}

static void subghz_device_cc1101_ext_capture_dma_start(void) {
    SubGhzDeviceCC1101ExtAsyncRx* rx = &subghz_device_cc1101_ext->async_rx;
    rx->timestamps = malloc(CC1101_RX_SAMPLES * sizeof(uint16_t));
    rx->levels = malloc(CC1101_RX_SAMPLES * 3u * sizeof(uint8_t));
    rx->read_index = 0;
    rx->clear_mask = LL_EXTI_LINE_2;
    rx->irq_was_enabled = NVIC_GetEnableIRQ(EXTI2_IRQn);
    rx->event_was_enabled = LL_EXTI_IsEnabledEvent_0_31(LL_EXTI_LINE_2);
    NVIC_DisableIRQ(EXTI2_IRQn);
    LL_EXTI_DisableIT_0_31(LL_EXTI_LINE_2);
    for(uint32_t i = 0; i < 3; i++) {
        furi_check(!LL_DMAMUX_IsEnabledRequestGen(DMAMUX1, i));
        DMAMUX_RequestGen_TypeDef* gen = (void*)((uintptr_t)DMAMUX1_RequestGenerator0 + 4 * i);
        rx->saved_request[i] = gen->RGCR;
        LL_DMAMUX_SetRequestSignalID(DMAMUX1, i, LL_DMAMUX_REQ_GEN_EXTI_LINE2);
        LL_DMAMUX_SetRequestGenPolarity(DMAMUX1, i, LL_DMAMUX_REQ_GEN_POL_RISING);
        LL_DMAMUX_SetGenRequestNb(DMAMUX1, i, i == 1 ? 3 : 1);
    }
    LL_DMA_InitTypeDef cfg = {0};
    cfg.PeriphOrM2MSrcAddress = (uint32_t)&TIM17->CNT;
    cfg.MemoryOrM2MDstAddress = (uint32_t)rx->timestamps;
    cfg.Direction = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
    cfg.Mode = LL_DMA_MODE_CIRCULAR;
    cfg.PeriphOrM2MSrcIncMode = LL_DMA_PERIPH_NOINCREMENT;
    cfg.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_INCREMENT;
    cfg.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_HALFWORD;
    cfg.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_HALFWORD;
    cfg.NbData = CC1101_RX_SAMPLES;
    cfg.PeriphRequest = LL_DMAMUX_REQ_GENERATOR0;
    cfg.Priority = LL_DMA_PRIORITY_VERYHIGH;
    LL_DMA_DeInit(DMA2, LL_DMA_CHANNEL_3);
    LL_DMA_Init(DMA2, LL_DMA_CHANNEL_3, &cfg);
    cfg.PeriphOrM2MSrcAddress = (uint32_t)&GPIOB->IDR;
    cfg.MemoryOrM2MDstAddress = (uint32_t)rx->levels;
    cfg.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_BYTE;
    cfg.PeriphRequest = LL_DMAMUX_REQ_GENERATOR1;
    cfg.NbData = CC1101_RX_SAMPLES * 3u;
    LL_DMA_DeInit(DMA2, LL_DMA_CHANNEL_4);
    LL_DMA_Init(DMA2, LL_DMA_CHANNEL_4, &cfg);
    cfg.PeriphOrM2MSrcAddress = (uint32_t)&EXTI->PR1;
    cfg.MemoryOrM2MDstAddress = (uint32_t)&rx->clear_mask;
    cfg.Direction = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
    cfg.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_NOINCREMENT;
    cfg.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_WORD;
    cfg.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_WORD;
    cfg.NbData = 1;
    cfg.PeriphRequest = LL_DMAMUX_REQ_GENERATOR2;
    LL_DMA_DeInit(DMA2, LL_DMA_CHANNEL_5);
    LL_DMA_Init(DMA2, LL_DMA_CHANNEL_5, &cfg);
    LL_DMA_ClearFlag_GI3(DMA2);
    LL_DMA_ClearFlag_GI4(DMA2);
    LL_DMA_ClearFlag_GI5(DMA2);
    LL_DMAMUX_ClearFlag_RGO0(DMAMUX1);
    LL_DMAMUX_ClearFlag_RGO1(DMAMUX1);
    LL_DMAMUX_ClearFlag_RGO2(DMAMUX1);
    furi_hal_interrupt_set_isr(
        FuriHalInterruptIdDma2Ch5, subghz_device_cc1101_ext_mirror_ISR, NULL);
    if(subghz_device_cc1101_ext->async_mirror_pin) LL_DMA_EnableIT_TC(DMA2, LL_DMA_CHANNEL_5);
    LL_DMA_EnableChannel(DMA2, LL_DMA_CHANNEL_3);
    LL_DMA_EnableChannel(DMA2, LL_DMA_CHANNEL_4);
    LL_DMA_EnableChannel(DMA2, LL_DMA_CHANNEL_5);
    LL_EXTI_EnableEvent_0_31(LL_EXTI_LINE_2);
    subghz_device_cc1101_ext_capture_requests(true);
    LL_EXTI_ClearFlag_0_31(LL_EXTI_LINE_2);
    NVIC_ClearPendingIRQ(EXTI2_IRQn);
    LL_EXTI_EnableIT_0_31(LL_EXTI_LINE_2);
}

static void subghz_device_cc1101_ext_capture_dma_stop(void) {
    SubGhzDeviceCC1101ExtAsyncRx* rx = &subghz_device_cc1101_ext->async_rx;
    LL_EXTI_DisableIT_0_31(LL_EXTI_LINE_2);
    subghz_device_cc1101_ext_capture_requests(false);
    LL_DMA_DisableChannel(DMA2, LL_DMA_CHANNEL_3);
    LL_DMA_DisableChannel(DMA2, LL_DMA_CHANNEL_4);
    LL_DMA_DisableChannel(DMA2, LL_DMA_CHANNEL_5);
    rx->stopping = true;
    subghz_device_cc1101_ext_capture_drain();
    LL_DMA_DeInit(DMA2, LL_DMA_CHANNEL_3);
    LL_DMA_DeInit(DMA2, LL_DMA_CHANNEL_4);
    LL_DMA_DeInit(DMA2, LL_DMA_CHANNEL_5);
    furi_hal_interrupt_set_isr(FuriHalInterruptIdDma2Ch5, NULL, NULL);
    for(uint32_t i = 0; i < 3; i++)
        ((DMAMUX_RequestGen_TypeDef*)((uintptr_t)DMAMUX1_RequestGenerator0 + 4 * i))->RGCR =
            rx->saved_request[i];
    if(!rx->event_was_enabled) LL_EXTI_DisableEvent_0_31(LL_EXTI_LINE_2);
    LL_EXTI_ClearFlag_0_31(LL_EXTI_LINE_2);
    NVIC_ClearPendingIRQ(EXTI2_IRQn);
    if(rx->irq_was_enabled) NVIC_EnableIRQ(EXTI2_IRQn);
    free(rx->timestamps);
    free(rx->levels);
    rx->timestamps = NULL;
    rx->levels = NULL;
}

void subghz_device_cc1101_ext_start_async_rx(
    SubGhzDeviceCC1101ExtCaptureCallback callback,
    void* context) {
    furi_assert(subghz_device_cc1101_ext->state == SubGhzDeviceCC1101ExtStateIdle);
    subghz_device_cc1101_ext->state = SubGhzDeviceCC1101ExtStateAsyncRx;

    subghz_device_cc1101_ext->hop_blank = false;

    subghz_device_cc1101_ext->async_rx.capture_callback = callback;
    subghz_device_cc1101_ext->async_rx.capture_callback_context = context;
    subghz_device_cc1101_ext->async_rx.timer_ticks = 0;
    subghz_device_cc1101_ext->async_rx.last_ticks = 0;
    subghz_device_cc1101_ext->async_rx.idle_wraps = 0;
    subghz_device_cc1101_ext->async_rx.discard_next = true;
    subghz_device_cc1101_ext->async_rx.stopping = false;
    subghz_device_cc1101_ext->async_rx.flooding = false;
    subghz_device_cc1101_ext->async_rx.service_ticks = 0;

    furi_hal_bus_enable(FuriHalBusTIM17);

    // Configure TIM
    LL_TIM_InitTypeDef TIM_InitStruct = {0};
    //Set the timer resolution to 2 us
    TIM_InitStruct.Prescaler = (64 << 1) - 1;
    TIM_InitStruct.CounterMode = LL_TIM_COUNTERMODE_UP;
    TIM_InitStruct.Autoreload = 0xFFFF;
    TIM_InitStruct.ClockDivision = LL_TIM_CLOCKDIVISION_DIV1;
    LL_TIM_Init(TIM17, &TIM_InitStruct);

    // Timer: advanced
    LL_TIM_SetClockSource(TIM17, LL_TIM_CLOCKSOURCE_INTERNAL);
    LL_TIM_DisableARRPreload(TIM17);
    LL_TIM_DisableDMAReq_TRIG(TIM17);
    LL_TIM_DisableIT_TRIG(TIM17);

    furi_hal_gpio_init(
        subghz_device_cc1101_ext->g0_pin, GpioModeInterruptRiseFall, GpioPullUp, GpioSpeedVeryHigh);
    furi_hal_gpio_remove_int_callback(subghz_device_cc1101_ext->g0_pin);

    // Start the overflow extension before accepting capture edges.
    LL_TIM_SetCounter(TIM17, 0);
    LL_TIM_ClearFlag_UPDATE(TIM17);
    furi_hal_interrupt_set_isr(
        FuriHalInterruptIdTim1TrgComTim17, subghz_device_cc1101_ext_capture_timer_ISR, NULL);
    LL_TIM_OC_SetMode(TIM17, LL_TIM_CHANNEL_CH1, LL_TIM_OCMODE_FROZEN);
    LL_TIM_OC_SetCompareCH1(TIM17, CC1101_RX_SERVICE_TICKS);
    LL_TIM_ClearFlag_CC1(TIM17);

    // Start debug
    subghz_device_cc1101_ext_start_debug();

    subghz_device_cc1101_ext_capture_dma_start();
    LL_TIM_EnableIT_CC1(TIM17);
    LL_TIM_EnableIT_UPDATE(TIM17);
    LL_TIM_EnableCounter(TIM17);

    // Switch to RX
    subghz_device_cc1101_ext_rx();

    //Clear the variable after the end of the session
    subghz_device_cc1101_ext->async_rx.capture_delta_duration = 0;
}

static SubGhzDeviceCC1101ExtFscalEntry*
    subghz_device_cc1101_ext_fscal_find(uint32_t frequency, const CC1101Image* image) {
    const uint32_t now = furi_get_tick();
    const bool match_mdmcfg = (image->reg[CC1101_CHANNR] != 0);

    for(size_t i = 0; i < SUBGHZ_DEVICE_CC1101_EXT_FSCAL_CACHE_SIZE; i++) {
        SubGhzDeviceCC1101ExtFscalEntry* e = &subghz_device_cc1101_ext_fscal[i];
        if(e->frequency != frequency) continue;
        if(e->fsctrl1 != image->reg[CC1101_FSCTRL1]) continue;
        if(e->fsctrl0 != image->reg[CC1101_FSCTRL0]) continue;
        if(e->channr != image->reg[CC1101_CHANNR]) continue;

        if(match_mdmcfg) {
            if(e->mdmcfg1 != image->reg[CC1101_MDMCFG1]) continue;
            if(e->mdmcfg0 != image->reg[CC1101_MDMCFG0]) continue;
        }
        if((now - e->stamp) > furi_ms_to_ticks(SUBGHZ_DEVICE_CC1101_EXT_FSCAL_TTL_MS)) continue;
        return e;
    }
    return NULL;
}

static SubGhzDeviceCC1101ExtFscalEntry* subghz_device_cc1101_ext_fscal_slot(void) {
    SubGhzDeviceCC1101ExtFscalEntry* oldest = &subghz_device_cc1101_ext_fscal[0];

    for(size_t i = 0; i < SUBGHZ_DEVICE_CC1101_EXT_FSCAL_CACHE_SIZE; i++) {
        SubGhzDeviceCC1101ExtFscalEntry* e = &subghz_device_cc1101_ext_fscal[i];
        if(e->frequency == 0) return e;
        if(e->stamp < oldest->stamp) oldest = e;
    }
    return oldest;
}

static bool subghz_device_cc1101_ext_hop_calibrate(uint32_t frequency, const CC1101Image* image) {
    cc1101_calibrate(subghz_device_cc1101_ext->spi_bus_handle);
    if(!cc1101_wait_status_state(
           subghz_device_cc1101_ext->spi_bus_handle,
           CC1101StateIDLE,
           SUBGHZ_DEVICE_CC1101_EXT_HOP_CAL_TIMEOUT_US)) {
        return false;
    }

    uint8_t fscal3 = 0, fscal2 = 0, fscal1 = 0;
    cc1101_read_reg(subghz_device_cc1101_ext->spi_bus_handle, CC1101_FSCAL3, &fscal3);
    cc1101_read_reg(subghz_device_cc1101_ext->spi_bus_handle, CC1101_FSCAL2, &fscal2);
    cc1101_read_reg(subghz_device_cc1101_ext->spi_bus_handle, CC1101_FSCAL1, &fscal1);

    if(fscal1 == SUBGHZ_DEVICE_CC1101_EXT_FSCAL1_UNLOCKED) {
        FURI_LOG_W(TAG, "PLL not locked at %lu Hz", frequency);
        return false;
    }

    SubGhzDeviceCC1101ExtFscalEntry* e = subghz_device_cc1101_ext_fscal_slot();
    e->frequency = frequency;
    e->stamp = furi_get_tick();
    e->fsctrl1 = image->reg[CC1101_FSCTRL1];
    e->fsctrl0 = image->reg[CC1101_FSCTRL0];
    e->channr = image->reg[CC1101_CHANNR];
    e->mdmcfg1 = image->reg[CC1101_MDMCFG1];
    e->mdmcfg0 = image->reg[CC1101_MDMCFG0];
    e->fscal3 = fscal3;
    e->fscal2 = fscal2;
    e->fscal1 = fscal1;
    return true;
}

bool subghz_device_cc1101_ext_hop_begin(void) {
    if(subghz_device_cc1101_ext == NULL) return false;
    if(subghz_device_cc1101_ext->state != SubGhzDeviceCC1101ExtStateAsyncRx) return false;
    if(!subghz_device_cc1101_ext_por_valid || !subghz_device_cc1101_ext_shadow_valid) return false;
    if(!subghz_device_cc1101_ext_hop_spi_held) {
        furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
        subghz_device_cc1101_ext_hop_spi_held = true;
    }
    return true;
}

bool subghz_device_cc1101_ext_hop_prepare(uint32_t frequency) {
    return subghz_device_cc1101_ext_is_frequency_valid(frequency);
}

void subghz_device_cc1101_ext_hop_end(void) {
    if(!subghz_device_cc1101_ext_hop_spi_held) return;
    furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
    subghz_device_cc1101_ext_hop_spi_held = false;
}

bool subghz_device_cc1101_ext_hop_goto(uint32_t frequency, const uint8_t* preset_data) {
    furi_check(preset_data);

    if(subghz_device_cc1101_ext == NULL) return false;
    if(subghz_device_cc1101_ext->state != SubGhzDeviceCC1101ExtStateAsyncRx) return false;
    if(!subghz_device_cc1101_ext_por_valid || !subghz_device_cc1101_ext_shadow_valid) return false;
    if(!subghz_device_cc1101_ext_is_frequency_valid(frequency)) return false;

    CC1101Image target;
    cc1101_image_apply_preset(&subghz_device_cc1101_ext_por, preset_data, &target);
    target.reg[CC1101_MCSM0] &= (uint8_t)~SUBGHZ_DEVICE_CC1101_EXT_FS_AUTOCAL_MASK;

    uint8_t prog[CC1101_IMAGE_PROG_MAX];
    const size_t prog_len =
        cc1101_image_diff(&subghz_device_cc1101_ext_shadow, &target, prog, sizeof(prog));

    const bool retune = (frequency != subghz_device_cc1101_ext_hop_frequency);
    const SubGhzDeviceCC1101ExtFscalEntry* cached =
        subghz_device_cc1101_ext_fscal_find(frequency, &target);

    subghz_device_cc1101_ext->hop_blank = true;

    const bool spi_owned = subghz_device_cc1101_ext_hop_spi_held;
    if(!spi_owned) {
        furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    }

    cc1101_switch_to_idle(subghz_device_cc1101_ext->spi_bus_handle);
    bool ok = cc1101_wait_status_state(
        subghz_device_cc1101_ext->spi_bus_handle,
        CC1101StateIDLE,
        SUBGHZ_DEVICE_CC1101_EXT_HOP_STATE_TIMEOUT_US);

    if(ok) {
        cc1101_image_write_prog(subghz_device_cc1101_ext->spi_bus_handle, prog, prog_len);

        if(retune) {
            cc1101_set_frequency(subghz_device_cc1101_ext->spi_bus_handle, frequency);
            subghz_device_cc1101_ext->regulation = SubGhzDeviceCC1101ExtRegulationTxRx;
        }

        if(cached) {
            cc1101_write_reg(
                subghz_device_cc1101_ext->spi_bus_handle, CC1101_FSCAL3, cached->fscal3);
            cc1101_write_reg(
                subghz_device_cc1101_ext->spi_bus_handle, CC1101_FSCAL2, cached->fscal2);
            cc1101_write_reg(
                subghz_device_cc1101_ext->spi_bus_handle, CC1101_FSCAL1, cached->fscal1);
        } else {
            ok = subghz_device_cc1101_ext_hop_calibrate(frequency, &target);
        }
    }

    if(ok) {
        cc1101_switch_to_rx(subghz_device_cc1101_ext->spi_bus_handle);
        ok = cc1101_wait_status_state(
            subghz_device_cc1101_ext->spi_bus_handle,
            CC1101StateRX,
            SUBGHZ_DEVICE_CC1101_EXT_HOP_STATE_TIMEOUT_US);
    }

    if(ok && subghz_device_cc1101_ext->amp_and_leds) {
        furi_hal_gpio_write(SUBGHZ_DEVICE_CC1101_EXT_E07_AMP_GPIO, 0);
        cc1101_write_reg(
            subghz_device_cc1101_ext->spi_bus_handle,
            CC1101_IOCFG2,
            CC1101IocfgHW | CC1101_IOCFG_INV);
        target.reg[CC1101_IOCFG2] = CC1101IocfgHW | CC1101_IOCFG_INV;
    } else {
        target.reg[CC1101_IOCFG2] = subghz_device_cc1101_ext_shadow.reg[CC1101_IOCFG2];
    }

    if(!spi_owned) {
        furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
    }

    if(ok) {
        subghz_device_cc1101_ext_shadow = target;
        subghz_device_cc1101_ext_hop_frequency = frequency;
    } else {
        subghz_device_cc1101_ext_shadow_invalidate();
        subghz_device_cc1101_ext->hop_blank = false;
        FURI_LOG_W(TAG, "Hop failed, shadow invalidated");
    }

    return ok;
}

void subghz_device_cc1101_ext_hop_resume(void) {
    if(subghz_device_cc1101_ext == NULL) return;
    if(subghz_device_cc1101_ext->state != SubGhzDeviceCC1101ExtStateAsyncRx) return;
    FURI_CRITICAL_ENTER();
    SubGhzDeviceCC1101ExtAsyncRx* rx = &subghz_device_cc1101_ext->async_rx;
    rx->read_index =
        ((CC1101_RX_SAMPLES * 3u - LL_DMA_GetDataLength(DMA2, LL_DMA_CHANNEL_4)) / 3u) %
        CC1101_RX_SAMPLES;
    LL_DMA_ClearFlag_GI4(DMA2);
    rx->last_ticks = subghz_device_cc1101_ext_capture_now();
    rx->idle_wraps = 0;
    rx->discard_next = true;
    subghz_device_cc1101_ext->hop_blank = false;
    FURI_CRITICAL_EXIT();
}

bool subghz_device_cc1101_ext_hop_read_image(uint8_t* out) {
    furi_check(out);
    if(subghz_device_cc1101_ext == NULL) return false;

    const bool spi_owned = subghz_device_cc1101_ext_hop_spi_held;
    if(!spi_owned) {
        furi_hal_spi_acquire(subghz_device_cc1101_ext->spi_bus_handle);
    }
    cc1101_image_read(subghz_device_cc1101_ext->spi_bus_handle, (CC1101Image*)out);
    if(!spi_owned) {
        furi_hal_spi_release(subghz_device_cc1101_ext->spi_bus_handle);
    }
    return true;
}

bool subghz_device_cc1101_ext_hop_get_shadow(uint8_t* out) {
    furi_check(out);
    if(!subghz_device_cc1101_ext_shadow_valid) return false;
    memcpy(out, subghz_device_cc1101_ext_shadow.reg, CC1101_IMAGE_REG_COUNT);
    return true;
}

void subghz_device_cc1101_ext_stop_async_rx(void) {
    furi_assert(subghz_device_cc1101_ext->state == SubGhzDeviceCC1101ExtStateAsyncRx);
    subghz_device_cc1101_ext->state = SubGhzDeviceCC1101ExtStateIdle;

    // Shutdown radio
    subghz_device_cc1101_ext_idle();

    FURI_CRITICAL_ENTER();
    furi_hal_gpio_remove_int_callback(subghz_device_cc1101_ext->g0_pin);
    LL_TIM_DisableIT_UPDATE(TIM17);
    LL_TIM_DisableIT_CC1(TIM17);
    subghz_device_cc1101_ext_capture_dma_stop();
    furi_hal_interrupt_set_isr(FuriHalInterruptIdTim1TrgComTim17, NULL, NULL);
    furi_hal_bus_disable(FuriHalBusTIM17);

    // Stop debug
    subghz_device_cc1101_ext_stop_debug();
    subghz_device_cc1101_ext->async_rx.capture_callback = NULL;

    FURI_CRITICAL_EXIT();
    furi_hal_gpio_init(subghz_device_cc1101_ext->g0_pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
}

void subghz_device_cc1101_ext_async_tx_middleware_idle(
    SubGhzDeviceCC1101ExtAsyncTxMiddleware* middleware) {
    middleware->state = SubGhzDeviceCC1101ExtAsyncTxMiddlewareStateIdle;
    middleware->is_odd_level = false;
    middleware->adder_duration = SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_GUARD_TIME;
}

static inline uint32_t subghz_device_cc1101_ext_async_tx_middleware_get_duration(
    SubGhzDeviceCC1101ExtAsyncTxMiddleware* middleware,
    SubGhzDeviceCC1101ExtCallback callback) {
    uint32_t ret = 0;
    bool is_level = false;

    if(middleware->state == SubGhzDeviceCC1101ExtAsyncTxMiddlewareStateReset) return 0;

    while(1) {
        LevelDuration ld = callback(subghz_device_cc1101_ext->async_tx.callback_context);
        if(level_duration_is_reset(ld)) {
            middleware->state = SubGhzDeviceCC1101ExtAsyncTxMiddlewareStateReset;
            if(!middleware->is_odd_level) {
                return 0;
            } else {
                return middleware->adder_duration;
            }
        } else if(level_duration_is_wait(ld)) {
            middleware->is_odd_level = !middleware->is_odd_level;
            ret = middleware->adder_duration + SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_GUARD_TIME;
            middleware->adder_duration = 0;
            return ret;
        }

        is_level = level_duration_get_level(ld);

        if(middleware->state == SubGhzDeviceCC1101ExtAsyncTxMiddlewareStateIdle) {
            if(is_level != middleware->is_odd_level) {
                middleware->state = SubGhzDeviceCC1101ExtAsyncTxMiddlewareStateRun;
                middleware->is_odd_level = is_level;
                middleware->adder_duration = level_duration_get_duration(ld);
                return SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_GUARD_TIME;
            } else {
                continue;
            }
        }

        if(middleware->state == SubGhzDeviceCC1101ExtAsyncTxMiddlewareStateRun) {
            if(is_level == middleware->is_odd_level) {
                middleware->adder_duration += level_duration_get_duration(ld);
                continue;
            } else {
                middleware->is_odd_level = is_level;
                ret = middleware->adder_duration;
                middleware->adder_duration = level_duration_get_duration(ld);
                return ret;
            }
        }
    }
}

static void subghz_device_cc1101_ext_async_tx_refill(uint32_t* buffer, size_t samples) {
    furi_assert(subghz_device_cc1101_ext->state == SubGhzDeviceCC1101ExtStateAsyncTx);

    while(samples > 0) {
        volatile uint32_t duration = subghz_device_cc1101_ext_async_tx_middleware_get_duration(
            &subghz_device_cc1101_ext->async_tx.middleware,
            subghz_device_cc1101_ext->async_tx.callback);
        if(duration == 0) {
            *buffer = 0;
            buffer++;
            samples--;
            LL_DMA_DisableIT_HT(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_DEF);
            LL_DMA_DisableIT_TC(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_DEF);
            if(LL_DMA_IsActiveFlag_HT3(SUBGHZ_DEVICE_CC1101_EXT_DMA)) {
                LL_DMA_ClearFlag_HT3(SUBGHZ_DEVICE_CC1101_EXT_DMA);
            }
            if(LL_DMA_IsActiveFlag_TC3(SUBGHZ_DEVICE_CC1101_EXT_DMA)) {
                LL_DMA_ClearFlag_TC3(SUBGHZ_DEVICE_CC1101_EXT_DMA);
            }
            break;
        } else {
            // Lowest possible value is 4us
            if(duration < 4) duration = 4;
            // Divide by 2 since timer resolution is 2us
            // Subtract 1 since we counting from 0
            *buffer = (duration >> 1) - 1;
            buffer++;
            samples--;
        }
    }
}

static void subghz_device_cc1101_ext_async_tx_dma_isr(void* context) {
    UNUSED(context);
    furi_assert(subghz_device_cc1101_ext->state == SubGhzDeviceCC1101ExtStateAsyncTx);

#if SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_CHANNEL == LL_DMA_CHANNEL_3
    if(LL_DMA_IsActiveFlag_HT3(SUBGHZ_DEVICE_CC1101_EXT_DMA)) {
        LL_DMA_ClearFlag_HT3(SUBGHZ_DEVICE_CC1101_EXT_DMA);
        subghz_device_cc1101_ext_async_tx_refill(
            subghz_device_cc1101_ext->async_tx.buffer,
            SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_BUFFER_HALF);
    }
    if(LL_DMA_IsActiveFlag_TC3(SUBGHZ_DEVICE_CC1101_EXT_DMA)) {
        LL_DMA_ClearFlag_TC3(SUBGHZ_DEVICE_CC1101_EXT_DMA);
        subghz_device_cc1101_ext_async_tx_refill(
            subghz_device_cc1101_ext->async_tx.buffer +
                SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_BUFFER_HALF,
            SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_BUFFER_HALF);
    }
#else
#error Update this code. Would you kindly?
#endif
}

bool subghz_device_cc1101_ext_start_async_tx(SubGhzDeviceCC1101ExtCallback callback, void* context) {
    furi_assert(subghz_device_cc1101_ext->state == SubGhzDeviceCC1101ExtStateIdle);
    furi_assert(callback);

    //If transmission is prohibited by regional settings
    if(subghz_device_cc1101_ext->regulation != SubGhzDeviceCC1101ExtRegulationTxRx) return false;

    subghz_device_cc1101_ext->async_tx.callback = callback;
    subghz_device_cc1101_ext->async_tx.callback_context = context;

    subghz_device_cc1101_ext->state = SubGhzDeviceCC1101ExtStateAsyncTx;

    subghz_device_cc1101_ext->async_tx.buffer =
        malloc(SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_BUFFER_FULL * sizeof(uint32_t));

    // use first DMA to update timer TIM17 durations, but TIM17 have not output chanel
    // so we use second DMA to transfer data from gpio_tx_buff directly to gpio pin using BSSR.
    // BSSR allow us tranfer data directly to pin in gpio port.

    //Signal generation with mem-to-mem DMA
    furi_hal_gpio_write(subghz_device_cc1101_ext->g0_pin, false);
    furi_hal_gpio_init(
        subghz_device_cc1101_ext->g0_pin, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);

    // Configure DMA to update timer TIM17 ARR by durations from buffer
    LL_DMA_SetMemoryAddress(
        SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_DEF, (uint32_t)subghz_device_cc1101_ext->async_tx.buffer);
    LL_DMA_SetPeriphAddress(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_DEF, (uint32_t) & (TIM17->ARR));
    LL_DMA_ConfigTransfer(
        SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_DEF,
        LL_DMA_DIRECTION_MEMORY_TO_PERIPH | LL_DMA_MODE_CIRCULAR | LL_DMA_PERIPH_NOINCREMENT |
            LL_DMA_MEMORY_INCREMENT | LL_DMA_PDATAALIGN_WORD | LL_DMA_MDATAALIGN_WORD |
            LL_DMA_PRIORITY_VERYHIGH);
    LL_DMA_SetDataLength(
        SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_DEF, SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_BUFFER_FULL);
    LL_DMA_SetPeriphRequest(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_DEF, LL_DMAMUX_REQ_TIM17_UP);

    LL_DMA_EnableIT_TC(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_DEF);
    LL_DMA_EnableIT_HT(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_DEF);
    LL_DMA_EnableChannel(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_DEF);

    furi_hal_interrupt_set_isr(
        SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_IRQ, subghz_device_cc1101_ext_async_tx_dma_isr, NULL);

    furi_hal_bus_enable(FuriHalBusTIM17);

    // Configure TIM 17
    // Set the timer resolution to 2 us
    LL_TIM_SetCounterMode(TIM17, LL_TIM_COUNTERMODE_UP);
    LL_TIM_SetClockDivision(TIM17, LL_TIM_CLOCKDIVISION_DIV1);
    LL_TIM_SetAutoReload(TIM17, 500);
    LL_TIM_SetPrescaler(TIM17, (64 << 1) - 1);
    LL_TIM_SetClockSource(TIM17, LL_TIM_CLOCKSOURCE_INTERNAL);
    LL_TIM_DisableARRPreload(TIM17);

    subghz_device_cc1101_ext_async_tx_middleware_idle(
        &subghz_device_cc1101_ext->async_tx.middleware);
    subghz_device_cc1101_ext_async_tx_refill(
        subghz_device_cc1101_ext->async_tx.buffer, SUBGHZ_DEVICE_CC1101_EXT_ASYNC_TX_BUFFER_FULL);

    // Configure DMA to transfer data from gpio_tx_buff directly to gpio pin using BSSR
    const GpioPin* gpio = subghz_device_cc1101_ext->g0_pin;

    subghz_device_cc1101_ext->async_tx.gpio_tx_buff[0] = (uint32_t)gpio->pin << GPIO_NUMBER;
    subghz_device_cc1101_ext->async_tx.gpio_tx_buff[1] = gpio->pin;

    LL_DMA_SetMemoryAddress(
        SUBGHZ_DEVICE_CC1101_EXT_DMA_CH4_DEF,
        (uint32_t)subghz_device_cc1101_ext->async_tx.gpio_tx_buff);
    LL_DMA_SetPeriphAddress(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH4_DEF, (uint32_t) & (gpio->port->BSRR));
    LL_DMA_ConfigTransfer(
        SUBGHZ_DEVICE_CC1101_EXT_DMA_CH4_DEF,
        LL_DMA_DIRECTION_MEMORY_TO_PERIPH | LL_DMA_MODE_CIRCULAR | LL_DMA_PERIPH_NOINCREMENT |
            LL_DMA_MEMORY_INCREMENT | LL_DMA_PDATAALIGN_WORD | LL_DMA_MDATAALIGN_WORD |
            LL_DMA_PRIORITY_HIGH);
    LL_DMA_SetDataLength(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH4_DEF, 2);
    LL_DMA_SetPeriphRequest(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH4_DEF, LL_DMAMUX_REQ_TIM17_UP);
    LL_DMA_EnableChannel(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH4_DEF);

    // Start debug
    if(subghz_device_cc1101_ext_start_debug()) {
        gpio = subghz_device_cc1101_ext->async_mirror_pin;
        subghz_device_cc1101_ext->async_tx.debug_gpio_buff[0] = (uint32_t)gpio->pin << GPIO_NUMBER;
        subghz_device_cc1101_ext->async_tx.debug_gpio_buff[1] = gpio->pin;

        LL_DMA_SetMemoryAddress(
            SUBGHZ_DEVICE_CC1101_EXT_DMA_CH5_DEF,
            (uint32_t)subghz_device_cc1101_ext->async_tx.debug_gpio_buff);
        LL_DMA_SetPeriphAddress(
            SUBGHZ_DEVICE_CC1101_EXT_DMA_CH5_DEF, (uint32_t) & (gpio->port->BSRR));
        LL_DMA_ConfigTransfer(
            SUBGHZ_DEVICE_CC1101_EXT_DMA_CH5_DEF,
            LL_DMA_DIRECTION_MEMORY_TO_PERIPH | LL_DMA_MODE_CIRCULAR | LL_DMA_PERIPH_NOINCREMENT |
                LL_DMA_MEMORY_INCREMENT | LL_DMA_PDATAALIGN_WORD | LL_DMA_MDATAALIGN_WORD |
                LL_DMA_PRIORITY_LOW);
        LL_DMA_SetDataLength(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH5_DEF, 2);
        LL_DMA_SetPeriphRequest(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH5_DEF, LL_DMAMUX_REQ_TIM17_UP);
        LL_DMA_EnableChannel(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH5_DEF);
    }

    // Start counter
    LL_TIM_EnableDMAReq_UPDATE(TIM17);

    subghz_device_cc1101_ext_tx();

    LL_TIM_SetCounter(TIM17, 0);
    LL_TIM_EnableCounter(TIM17);

    return true;
}

bool subghz_device_cc1101_ext_is_async_tx_complete(void) {
    return (subghz_device_cc1101_ext->state == SubGhzDeviceCC1101ExtStateAsyncTx) &&
           (LL_TIM_GetAutoReload(TIM17) == 0);
}

void subghz_device_cc1101_ext_stop_async_tx(void) {
    furi_assert(subghz_device_cc1101_ext->state == SubGhzDeviceCC1101ExtStateAsyncTx);

    // Shutdown radio
    subghz_device_cc1101_ext_idle();

    // Deinitialize GPIO
    furi_hal_gpio_write(subghz_device_cc1101_ext->g0_pin, false);
    furi_hal_gpio_init(subghz_device_cc1101_ext->g0_pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);

    // Deinitialize Timer
    furi_hal_bus_disable(FuriHalBusTIM17);
    furi_hal_interrupt_set_isr(FuriHalInterruptIdTim1TrgComTim17, NULL, NULL);

    // Deinitialize DMA
    LL_DMA_DeInit(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_DEF);
    LL_DMA_DisableChannel(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH4_DEF);
    furi_hal_interrupt_set_isr(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH3_IRQ, NULL, NULL);

    // Stop debug
    if(subghz_device_cc1101_ext_stop_debug()) {
        LL_DMA_DisableChannel(SUBGHZ_DEVICE_CC1101_EXT_DMA_CH5_DEF);
    }

    free(subghz_device_cc1101_ext->async_tx.buffer);

    subghz_device_cc1101_ext->state = SubGhzDeviceCC1101ExtStateIdle;
}
