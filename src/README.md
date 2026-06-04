# Source Code

Firmware and software developed for the H2S sensor system.

## Structure

```
src/
└── firmware/
```

## `firmware/`

Written in C++, targeting the ESP-IDF framework (v6.0.0) with FreeRTOS, compiled via PlatformIO.

### Architecture

The application runs four concurrent FreeRTOS tasks:

| Task | Cycle | Responsibility |
|---|---|---|
| `SensorTask` | 3 s | Reads LMP91002 output via ADS1115, computes H2S concentration, pushes to queues |
| `DisplayTask` | 1 s | Refreshes OLED with concentration, time, temperature, battery status |
| `StorageTask` | — | Appends each sample as a CSV row to MicroSD card |
| `PowerTask` | — | Monitors battery voltage, handles controlled shutdown sequence |

Inter-task communication uses two FreeRTOS queues (`q_display`, `q_storage`). The shared I2C bus and SD filesystem are each protected by a mutex.

### Sensor Conversion

The LMP91002 is configured with a 35 kΩ transimpedance resistor and internal reference at 20% Vcc (Vref = 0.66 V). H2S concentration is computed as:

```
I_nA  = (Vcell - Vzero) / 35000 × 1e9
c_ppm = I_nA / 85
```

On each boot, the firmware discards 5 warm-up readings then averages 10 readings in clean air to compute `Vzero`, stored in non-volatile memory.

### Building and Flashing

Requires [PlatformIO](https://platformio.org/) with the `seeed_xiao_esp32c6` board target.

```bash
cd src/firmware
pio run --target upload
```

### Serial Data Capture

Due to a MicroSD routing error in the current PCB revision, data can be captured via USB serial using:

```bash
python src/firmware/tools/capture_serial.py
```
