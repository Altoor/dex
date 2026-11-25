#include "battery.h"

#include <stddef.h>

#include "board.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "battery";

/* The board uses a 200K / 200K divider, so VBAT is twice the ADC voltage. */
#define DIVIDER_NUM 2
#define VBAT_EMPTY_MV 3300
#define VBAT_FULL_MV  4200

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_channel;
static bool s_cali_ok;
static bool s_ready;

esp_err_t battery_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BOARD_BATTERY_ADC_PIN, &unit,
                                                  &s_channel),
                        TAG, "map battery ADC pin");
    ESP_RETURN_ON_FALSE(unit == ADC_UNIT_1, ESP_ERR_NOT_SUPPORTED, TAG,
                        "battery ADC must be on ADC1");

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "ADC unit");

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_channel, &chan_cfg),
                        TAG, "ADC channel");

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = s_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK) {
        s_cali_ok = true;
    }
#endif

    s_ready = true;
    ESP_LOGI(TAG, "battery ADC ready (GPIO %d)", BOARD_BATTERY_ADC_PIN);
    return ESP_OK;
}

void battery_read(battery_status_t *out)
{
    if (out == NULL) {
        return;
    }

    out->valid = false;
    out->millivolts = 0;
    out->percent = 0;
    out->usb_present = usb_serial_jtag_is_connected();
    out->charging = out->usb_present;

    if (!s_ready) {
        return;
    }

    int raw = 0;
    if (adc_oneshot_read(s_adc, s_channel, &raw) != ESP_OK) {
        return;
    }

    int pin_mv = 0;
    if (s_cali_ok) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &pin_mv) != ESP_OK) {
            return;
        }
    } else {
        /* Rough fallback: 12 dB attenuation is ~3.1 V full scale. */
        pin_mv = (raw * 3100) / 4095;
    }

    int pack_mv = pin_mv * DIVIDER_NUM;
    int percent = (pack_mv - VBAT_EMPTY_MV) * 100 / (VBAT_FULL_MV - VBAT_EMPTY_MV);
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    out->valid = true;
    out->millivolts = pack_mv;
    out->percent = percent;
}
