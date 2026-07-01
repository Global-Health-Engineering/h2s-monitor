#include <unity.h>
#include "calibration.h"

void setUp(void) {}
void tearDown(void) {}

// ── voltage_to_ppm ────────────────────────────────────────────────────────────

// At V_cell == v_zero, current is zero → 0 ppm.
void test_ppm_at_vref_is_zero(void) {
    float v_zero = ADS1115_VDD * 0.20f;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, voltage_to_ppm(v_zero, v_zero));
}

// Voltages below v_zero mean negative current → negative ppm (signed output).
void test_ppm_below_vref_is_negative(void) {
    float v_zero = ADS1115_VDD * 0.20f;
    TEST_ASSERT_TRUE(voltage_to_ppm(0.0f, v_zero) < 0.0f);
    TEST_ASSERT_TRUE(voltage_to_ppm(0.3f, v_zero) < 0.0f);
}

// 100 ppm → I = 100 × 85 nA = 8500 nA
// V_cell = v_zero + 8500e-9 × 35000 = v_zero + 0.2975 V
void test_ppm_100ppm(void) {
    float v_zero = ADS1115_VDD * 0.20f;
    float v_cell = v_zero + (100.0f * 85.0f * 1e-9f * SENSOR_TIA_GAIN_OHMS);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 100.0f, voltage_to_ppm(v_cell, v_zero));
}

// 2000 ppm is the top of the detection range.
void test_ppm_2000ppm(void) {
    float v_zero = ADS1115_VDD * 0.20f;
    float v_cell = v_zero + (2000.0f * 85.0f * 1e-9f * SENSOR_TIA_GAIN_OHMS);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 2000.0f, voltage_to_ppm(v_cell, v_zero));
}

// ── voltage_to_celsius ────────────────────────────────────────────────────────

// At 25°C, NTC = 10 kΩ = pullup → voltage divider midpoint = 1.65 V.
void test_celsius_at_25c(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 25.0f, voltage_to_celsius(1.65f));
}

// At 0°C, NTC ≈ 32.65 kΩ → V = 3.3 × 10k / (10k + 32.65k) ≈ 0.773 V
void test_celsius_at_0c(void) {
    float r_0c   = 10000.0f * expf((1.0f / 273.15f - 1.0f / 298.15f) * 3950.0f);
    float v_0c   = 3.3f * 10000.0f / (10000.0f + r_0c);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, voltage_to_celsius(v_0c));
}

// Boundary voltages (0V or 3.3V mean open/short circuit) must return 0, not crash.
void test_celsius_boundary_no_crash(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, voltage_to_celsius(0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, voltage_to_celsius(3.3f));
}

// ── voltage_to_battery_mv ─────────────────────────────────────────────────────

// Full LiPo (4200 mV): V_adc = 4.2 × 51k/151k ≈ 1.4185 V → 4200 mV back.
void test_battery_full_4200mv(void) {
    float v_adc = 4.2f * 51000.0f / 151000.0f;
    TEST_ASSERT_UINT32_WITHIN(10, 4200, voltage_to_battery_mv(v_adc));
}

// Low-battery threshold (3600 mV): V_adc = 3.6 × 51k/151k ≈ 1.2159 V.
void test_battery_low_threshold_3600mv(void) {
    float v_adc = 3.6f * 51000.0f / 151000.0f;
    TEST_ASSERT_UINT32_WITHIN(10, 3600, voltage_to_battery_mv(v_adc));
}

// Dead battery floor (3000 mV).
void test_battery_dead_3000mv(void) {
    float v_adc = 3.0f * 51000.0f / 151000.0f;
    TEST_ASSERT_UINT32_WITHIN(10, 3000, voltage_to_battery_mv(v_adc));
}

// ── Runner ────────────────────────────────────────────────────────────────────

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_ppm_at_vref_is_zero);
    RUN_TEST(test_ppm_below_vref_is_negative);
    RUN_TEST(test_ppm_100ppm);
    RUN_TEST(test_ppm_2000ppm);

    RUN_TEST(test_celsius_at_25c);
    RUN_TEST(test_celsius_at_0c);
    RUN_TEST(test_celsius_boundary_no_crash);

    RUN_TEST(test_battery_full_4200mv);
    RUN_TEST(test_battery_low_threshold_3600mv);
    RUN_TEST(test_battery_dead_3000mv);

    return UNITY_END();
}
