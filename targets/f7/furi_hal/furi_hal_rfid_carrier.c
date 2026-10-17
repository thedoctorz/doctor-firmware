
#include <furi_hal_rfid.h>
#include <furi_hal_resources.h>
#include <furi_hal_bus.h>
#include <furi.h>

#include <stm32wbxx_ll_tim.h>

#define FURI_HAL_RFID_READ_TIMER                TIM1
#define FURI_HAL_RFID_READ_TIMER_BUS            FuriHalBusTIM1
#define FURI_HAL_RFID_READ_TIMER_CHANNEL_CONFIG LL_TIM_CHANNEL_CH1

static void furi_hal_rfid_ibutton_pin_configure(void) {
    furi_hal_gpio_write(&gpio_ibutton, true);
    furi_hal_gpio_init(&gpio_ibutton, GpioModeOutputOpenDrain, GpioPullNo, GpioSpeedLow);
}

static void furi_hal_rfid_ibutton_pin_reset(void) {
    furi_hal_gpio_write(&gpio_ibutton, true);
    furi_hal_gpio_init(&gpio_ibutton, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
}

void furi_hal_rfid_pins_reset(void) {
    furi_hal_rfid_ibutton_pin_reset();

    furi_hal_gpio_init(&gpio_rfid_carrier_out, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_write(&gpio_rfid_carrier_out, false);

    furi_hal_gpio_init(&gpio_nfc_irq_rfid_pull, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_write(&gpio_nfc_irq_rfid_pull, true);

    furi_hal_gpio_init_simple(&gpio_rfid_carrier, GpioModeAnalog);
    furi_hal_gpio_init(&gpio_rfid_data_in, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
}

static void furi_hal_rfid_pins_read(void) {
    furi_hal_rfid_ibutton_pin_configure();
    furi_hal_gpio_write(&gpio_ibutton, false);

    furi_hal_gpio_init(&gpio_nfc_irq_rfid_pull, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_write(&gpio_nfc_irq_rfid_pull, false);

    furi_hal_gpio_init_ex(
        &gpio_rfid_carrier_out,
        GpioModeAltFunctionPushPull,
        GpioPullNo,
        GpioSpeedLow,
        GpioAltFn1TIM1);

    furi_hal_gpio_init(&gpio_rfid_data_in, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
}

void furi_hal_rfid_tim_read_continue(void) {
    LL_TIM_EnableAllOutputs(FURI_HAL_RFID_READ_TIMER);
}

void furi_hal_rfid_tim_read_pause(void) {
    LL_TIM_DisableAllOutputs(FURI_HAL_RFID_READ_TIMER);
}

void furi_hal_rfid_tim_read_stop(void) {
    furi_hal_bus_disable(FURI_HAL_RFID_READ_TIMER_BUS);
}

void furi_hal_rfid_tim_read_start(float freq, float duty_cycle) {
    furi_hal_bus_enable(FURI_HAL_RFID_READ_TIMER_BUS);

    furi_hal_rfid_pins_read();

    LL_TIM_InitTypeDef TIM_InitStruct = {0};
    TIM_InitStruct.Autoreload = (SystemCoreClock / freq) - 1;
    LL_TIM_Init(FURI_HAL_RFID_READ_TIMER, &TIM_InitStruct);
    LL_TIM_DisableARRPreload(FURI_HAL_RFID_READ_TIMER);

    LL_TIM_OC_InitTypeDef TIM_OC_InitStruct = {0};
    TIM_OC_InitStruct.OCMode = LL_TIM_OCMODE_PWM1;
    TIM_OC_InitStruct.OCNState = LL_TIM_OCSTATE_ENABLE;
    TIM_OC_InitStruct.CompareValue = TIM_InitStruct.Autoreload * duty_cycle;
    LL_TIM_OC_Init(
        FURI_HAL_RFID_READ_TIMER, FURI_HAL_RFID_READ_TIMER_CHANNEL_CONFIG, &TIM_OC_InitStruct);

    LL_TIM_EnableCounter(FURI_HAL_RFID_READ_TIMER);

    furi_hal_rfid_tim_read_continue();
}
