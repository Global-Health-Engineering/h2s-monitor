# H2S Monitor

Design and Development of a Low-Cost Hydrogen Sulfide Sensor System for Biogas Monitoring

**Author:** Flurin Vital
**Supervisors:** Prof. Dr. Elizabeth Tilley, Dr. Jakub Tkaczuk
**Institution:** ETH Zürich, Global Health Engineering (GHE)
**Date:** June 2026

## Overview

This project develops a low-cost, field-deployable hydrogen sulfide (H2S) sensor system for monitoring biogas quality. Biogas systems in low-resource settings frequently lack affordable tools for measuring H2S — a toxic and corrosive gas produced during anaerobic digestion that causes equipment degradation and safety hazards. Commercial H2S meters capable of covering the relevant concentration range (0–2000 ppm) cost upwards of CHF 4000 and are unsuitable for continuous field deployment.

The developed system integrates a three-electrode electrochemical H2S sensor into a single compact battery-powered device with a custom PCB, 3D-printed enclosure and gas flow chamber, and C++ firmware running on FreeRTOS. Total component cost is CHF 193.80 — approximately 95% cheaper than the commercial reference instrument (Dräger X-am 8000).

## System Components

| Component | Part |
|---|---|
| Microcontroller | Seeed XIAO ESP32-C6 |
| H2S Sensor | SUCH electrochemical, 0–2000 ppm, 85 ± 25 nA/ppm |
| Analog Front End | Texas Instruments LMP91002 |
| ADC | Texas Instruments ADS1115 (16-bit) |
| Display | 0.96" OLED, SSD1306 driver |
| RTC | Maxim DS3232M |
| Power Management | Texas Instruments BQ25185 |
| Storage | MicroSD card (SPI) |
| Gas Pump | DC micro diaphragm pump, 1.2 L/min |
| PCB | KiCad design, manufactured by JLCPCB |
| Enclosure | 3D-printed PLA (case) + resin (flow chamber) |

## Repository Structure

```
├── analysis/       # Python analysis scripts
├── data/           # Raw and derived measurement data
├── docs/           # Reports and presentation slides
├── grading/        # Grading rubric
├── hardware/       # PCB design files and CAD models
├── media/          # Photos and videos of the device
└── src/            # Firmware source code
```
