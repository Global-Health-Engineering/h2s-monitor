#ifndef CALIBRATION_H
#define CALIBRATION_H

#include "app_config.h"
#include <math.h>
#include <stdint.h>

// H2S concentration from LMP91002 output voltage.
// v_cell: ADS1115 AIN0 reading in Volts.
// v_zero: measured AIN0 voltage in clean air (set by zero-calibration sequence).
// Returns signed ppm — negative values indicate noise below the zero reference.
static inline float voltage_to_ppm(float v_cell, float v_zero) {
    float i_na = (v_cell - v_zero) / (float)SENSOR_TIA_GAIN_OHMS * 1e9f;
    return i_na / 85.0f;
}

// Temperature from NTC thermistor voltage.
// v_therm: ADS1115 AIN2 reading in Volts.
// Circuit: 3.3V → 10kΩ pullup → AIN2 → NTC → GND. B=3950, R0=10kΩ at 25°C.
static inline float voltage_to_celsius(float v_therm) {
    if (v_therm <= 0.0f || v_therm >= 3.3f) return 0.0f;
    float r_therm  = 10000.0f * v_therm / (3.3f - v_therm);
    float t_kelvin = 1.0f / (1.0f / 298.15f + logf(r_therm / 10000.0f) / 3950.0f);
    return t_kelvin - 273.15f;
}

// Battery voltage from resistor-divider ADC reading.
// v_adc: ADS1115 AIN1 reading in Volts (R_top=100kΩ, R_bottom=51kΩ).
// Returns battery voltage in mV.
static inline uint32_t voltage_to_battery_mv(float v_adc) {
    float v_batt = v_adc * (100000.0f + 51000.0f) / 51000.0f;
    return (uint32_t)(v_batt * 1000.0f);
}

#endif // CALIBRATION_H
