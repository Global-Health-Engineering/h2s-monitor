# Testing

Hardware testing documentation for the H2S sensor system.

## Tests Performed

### Baseline Stability (June 2026)
Continuous 1-hour recording in clean ambient air with pump running. See `data/raw_data/baseline_stability/` for raw data and `analysis/baseline.py` for analysis. Peak-to-peak noise: 3.65 ppm (below 10 ppm threshold).

### Gas Flow Verification
Theoretical minimum flush time of the flow chamber cavity (17.3 mL) at rated pump flow (1.2 L/min): ~0.87 s. Actual flush time at 3.7 V operating voltage not independently measured. Attempt to validate with decomposing egg as H2S source was inconclusive — no detectable concentration registered on reference meter.

### System Integration
All I2C peripherals (LMP91002, ADS1115, DS3232M, SSD1306) initialise and communicate correctly. MicroSD SPI communication non-functional due to PCB routing error on two SPI lines (to be corrected in next revision).

## Pending Validation

- Sensor response to known H2S concentrations (requires calibration gas)
- SD card end-to-end logging after PCB routing fix
- Gas-tight seal verification with resin-printed flow chamber
