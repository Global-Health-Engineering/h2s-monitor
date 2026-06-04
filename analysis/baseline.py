import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from scipy import stats

# ── 1. Load data ──────────────────────────────────────────────────────────────
# Adjust the filename and column names to match your serial monitor output.
# Expected format: a CSV with at least a timestamp column and a concentration
# column in ppm. If your serial output is not a clean CSV, adjust the separator
# and column names accordingly.

df = pd.read_csv("/Users/flurin/Desktop/BSc_Thesis_GHE/03_XIAO_Firmware/H2S_Sensor/data/h2s_20260601_115100.csv")

df = df.rename(columns={
    "datetime": "time",
    "ppm": "concentration"
})

df["time"] = pd.to_datetime(df["time"])
df["elapsed_min"] = (df["time"] - df["time"].iloc[0]).dt.total_seconds() / 60
df = df[df["elapsed_min"] <= 60]

# ── 2. Compute statistics ─────────────────────────────────────────────────────
mean_ppm   = df["concentration"].mean()
std_ppm    = df["concentration"].std()
pp_noise   = df["concentration"].max() - df["concentration"].min()

# Linear drift fit
slope, intercept, r_value, p_value, std_err = stats.linregress(
    df["elapsed_min"], df["concentration"]
)
drift_per_hour = slope * 60  # convert from ppm/min to ppm/h

print("── Baseline Statistics ──────────────────────────────")
print(f"Mean output:        {mean_ppm:.2f} ppm")
print(f"Standard deviation: {std_ppm:.2f} ppm")
print(f"Peak-to-peak noise: {pp_noise:.2f} ppm")
print(f"Linear drift slope: {drift_per_hour:.4f} ppm/h")
print(f"R²:                 {r_value**2:.4f}")
print("─────────────────────────────────────────────────────")

# ── 3. Plot time series ───────────────────────────────────────────────────────
ROLLING_WINDOW = 10  # samples (~30 s at 3 s/sample)

roll = df["concentration"].rolling(ROLLING_WINDOW, center=True)
roll_mean = roll.mean()
roll_std  = roll.std()

fig, ax = plt.subplots(figsize=(10, 4))

# Raw samples — faint markers so spikes are visible but not dominant
ax.scatter(
    df["elapsed_min"],
    df["concentration"],
    color="#440154",
    s=6,
    alpha=0.25,
    zorder=2,
    label="Raw samples"
)

# Shaded ±1 σ noise band around the rolling mean
ax.fill_between(
    df["elapsed_min"],
    (roll_mean - roll_std).clip(lower=0),
    roll_mean + roll_std,
    color="#31688E",  # viridis blue
    alpha=0.20,
    zorder=3,
    label="±1 σ band"
)

# Rolling mean — the main readable signal
ax.plot(
    df["elapsed_min"],
    roll_mean,
    color="#31688E",
    linewidth=1.8,
    zorder=4,
    label=f"Rolling mean (n={ROLLING_WINDOW})"
)

# Linear drift trend
trend_line = intercept + slope * df["elapsed_min"]
ax.plot(
    df["elapsed_min"],
    trend_line,
    color="#FDE725",
    linewidth=1.4,
    linestyle="--",
    zorder=5,
    label=f"Linear trend ({drift_per_hour:.3f} ppm/h)"
)

# Formatting
ax.set_xlabel("Elapsed time (min)")
ax.set_ylabel("H$_2$S concentration (ppm)")
ax.set_xlim(left=0)
ax.set_ylim(-2, 4)
ax.legend(frameon=False, fontsize=9)
ax.spines["top"].set_visible(False)
ax.spines["right"].set_visible(False)

plt.tight_layout()

# Save as PDF for clean vector output in LaTeX
out_dir = "/Users/flurin/Desktop/BSc_Thesis_GHE/04_Data_Analysis"
plt.savefig(f"{out_dir}/baseline_timeseries.pdf", dpi=300, bbox_inches="tight")
plt.savefig(f"{out_dir}/baseline_timeseries.png", dpi=300, bbox_inches="tight")
print("Figure saved as baseline_timeseries.pdf and baseline_timeseries.png")

# Save statistics to text file alongside the figures
stats_path = f"{out_dir}/baseline_statistics.txt"
with open(stats_path, "w") as f:
    f.write("── Baseline Statistics ──────────────────────────────\n")
    f.write(f"Mean output:        {mean_ppm:.2f} ppm\n")
    f.write(f"Standard deviation: {std_ppm:.2f} ppm\n")
    f.write(f"Peak-to-peak noise: {pp_noise:.2f} ppm\n")
    f.write(f"Linear drift slope: {drift_per_hour:.4f} ppm/h\n")
    f.write(f"R²:                 {r_value**2:.4f}\n")
    f.write("─────────────────────────────────────────────────────\n")
print(f"Statistics saved as baseline_statistics.txt")