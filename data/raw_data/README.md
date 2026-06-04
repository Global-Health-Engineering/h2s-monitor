# Raw Data

Unprocessed sensor output as logged directly during measurements. Files are not modified after collection.

## Structure

```
raw_data/
└── baseline_stability/     # 1-hour baseline test in clean ambient air
```

## Datasets

### `baseline_stability/`

Continuous H2S sensor recording in clean ambient air with no H2S present. Used to characterise sensor noise, baseline offset, and drift.

| Parameter | Value |
|---|---|
| Duration | 1 hour |
| Sampling interval | 3 seconds |
| Ambient temperature | ~27°C |
| Pump state | Running at nominal voltage (3.7 V) |
| Logging method | Serial capture via `src/firmware/tools/capture_serial.py` |

**File naming convention:** `h2s_YYYYMMDD_HHMMSS.csv` (date and time of recording start)
