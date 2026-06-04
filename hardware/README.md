# Hardware

All hardware design files for the H2S sensor system, including the custom PCB and the 3D-printed enclosure and gas flow chamber.

## Structure

```
hardware/
├── design/     # PCB design files (KiCad) and CAD models (STL)
└── testing/    # Hardware testing documentation and results
```

## System Overview

The hardware consists of three main physical components:

**Custom PCB** — designed in KiCad and manufactured by JLCPCB. Integrates the Seeed XIAO ESP32-C6 microcontroller, TI LMP91002 electrochemical analog front end, TI ADS1115 16-bit ADC, DS3232M RTC, SSD1306 OLED display, TI BQ25185 power management IC (USB-C charging, battery protection), MicroSD card slot, and N-channel MOSFET pump driver.

**3D-printed enclosure** — two-part PLA shell (bottom houses PCB, top carries display). Openings provide access to USB-C port, power button, MicroSD slot, and tube connectors.

**3D-printed flow chamber** — cylindrical chamber that seals around the electrochemical sensor body via an O-ring. A small internal cavity between the sensor face and the chamber top is flushed continuously by the diaphragm pump. Must be printed in resin for a gas-tight seal in field deployment; PLA is acceptable for bench testing only.

## Known Issues

- PCB revision V5 has a routing error on two of the four SPI lines to the MicroSD card, causing a short circuit that prevents SD card communication. SD logging is non-functional until corrected in the next revision.
