# Design

PCB schematic and layout files, and CAD models for the enclosure and gas flow chamber.

## Structure

```
design/
├── pcb/            # KiCad project files and JLCPCB production files
└── case_CAD/       # STL files for 3D-printed enclosure and flow chamber
```

## PCB (`pcb/`)

Designed in KiCad. The schematic is organised into functional blocks:
- **Power management** — TI BQ25185 (USB-C charging, single-cell LiPo, power path)
- **Microcontroller** — Seeed XIAO ESP32-C6
- **Analog front end & ADC** — TI LMP91002 + TI ADS1115 for electrochemical sensor signal conditioning
- **Peripherals** — DS3232M RTC, SSD1306 OLED, MicroSD (SPI), pump MOSFET driver

Production files in `pcb/production/` and `pcb/jlcpcb/` were submitted to JLCPCB for fabrication and assembly. `pcb/PCB_Versions/` contains archived snapshots of earlier design iterations (V1–V5).

The electrochemical sensor uses a custom KiCad footprint with press-fit through-hole pads — the sensor cannot withstand soldering temperatures and must not be soldered.

## CAD (`case_CAD/`)

Designed in OnShape, exported as STL for 3D printing.

| File | Material | Notes |
|---|---|---|
| `Case/Final/Case_Bottom.stl` | PLA | Houses PCB |
| `Case/Final/Case_Top.stl` | PLA | Carries OLED display |
| `Flow_Chamber/Flow_Chamber_final.stl` | Resin (field) / PLA (bench test) | Seals around sensor body with O-ring |

`Case/V1`–`V5` and `Flow_Chamber/V1`–`V5` contain earlier design iterations.
