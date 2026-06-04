# Data

This directory contains all measurement data collected during development and testing of the H2S sensor system.

## Structure

```
data/
├── raw_data/
├── derived_data/
└── metadata/
```

## Data Format

Raw data is logged by the firmware as CSV files at a 3-second sampling interval. Each row contains one sensor reading. Due to a PCB routing error affecting the MicroSD SPI lines in the current hardware revision, data was recorded via a serial connection to a computer using a capture script (`src/firmware/tools/capture_serial.py`) rather than directly to the SD card.

## Variables

| Column | Unit | Description |
|---|---|---|
| timestamp | YYYY-MM-DD HH:MM:SS | Wall-clock time from DS3232M RTC |
| h2s_ppm | ppm | H2S concentration computed from sensor current |
| temperature | °C | On-board temperature from thermistor |
| battery_v | V | Battery voltage from resistor divider on ADS1115 |
