# Analysis

Python scripts for processing and analysing H2S sensor measurement data.

## Scripts

### `baseline.py`

Analyses the baseline stability of the sensor in clean ambient air. Reads a CSV file from `data/raw_data/baseline_stability/`, computes summary statistics (mean, standard deviation, peak-to-peak noise, linear drift slope), and generates a time series plot with a rolling mean and linear trend overlay.

**Input:** `data/raw_data/baseline_stability/h2s_20260601_115100.csv`
**Output:** summary statistics printed to console, timeseries plot

**Usage:**
```bash
python analysis/baseline.py
```

**Dependencies:** pandas, matplotlib, numpy

## Results Summary

Baseline stability test conducted over one hour in clean ambient air at ~27°C with the diaphragm pump running at nominal voltage:

| Metric | Value |
|---|---|
| Mean output | 0.33 ppm |
| Standard deviation | 0.51 ppm |
| Peak-to-peak noise | 3.65 ppm |
| Linear drift slope | 0.271 ppm/h |

Peak-to-peak noise of 3.65 ppm is well below the 10 ppm detection threshold defined as the minimum relevant concentration for biogas monitoring.
