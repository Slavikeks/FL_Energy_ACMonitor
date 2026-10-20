# Midea XYE / CCM connection

The controller communicates with compatible Midea air conditioners over the XYE/CCM protocol on a half-duplex RS-485 bus. XYE is **not Modbus RTU**; a Modbus adapter or Modbus configuration will not work.

## Controller-side signals

The current WT32-ETH01 firmware uses UART2:

| WT32-ETH01 | RS-485 transceiver | Purpose |
| --- | --- | --- |
| GPIO 17 / TX | DI / driver input | Transmit XYE bytes |
| GPIO 5 / RX | RO / receiver output | Receive XYE bytes |
| GND | Transceiver logic ground | Logic reference |
| — | A/B (or vendor-labelled pair) | Differential pair to the XYE interface |

```text
WT32-ETH01                 RS-485 transceiver                 Conditioner
GPIO 17 / TX  ------------> DI
GPIO 5 / RX   <------------ RO
GND           ------------ GND
                             A  =============================== XYE/CCM A*
                             B  =============================== XYE/CCM B*

*Check the interface documentation: A/B polarity labels can differ by vendor.
```

Use a transceiver that switches direction automatically. The firmware does not drive a DE/RE control pin. Do not connect ESP32 UART pins directly to the air-conditioner bus.

Connect the differential pair to the XYE/CCM terminals according to the transceiver and air-conditioner interface documentation. A/B labels and polarity conventions differ among vendors; if the bus does not respond, verify polarity before changing protocol settings. Follow the equipment manufacturer's requirements for reference ground, isolation, termination, and biasing. This repository does not define a universal mains-side or connector pinout.

## Bus settings and commissioning

- 4800 baud, 8 data bits, no parity, 1 stop bit (8N1).
- Half-duplex, one active XYE master at a time.
- Device addresses are 0–63. The web scan checks this protocol range.
- The configuration registry supports up to 16 conditioners.

Before registering devices, confirm that addresses are unique and no other controller or CCM master is issuing requests on the same bus. Scan from Settings, then register the addresses that should be monitored. A discovered address is not automatically added to the registry.

Start with one conditioner. Check that power, mode, setpoint, and available temperatures agree with the local remote/controller. Test individual commands while the group is in manual mode. Validate each model before placing it under AUTO control; protocol support varies across air-conditioner families.

The owner has reported one installation with four conditioners online and correct telemetry. This does not certify every Midea model or wiring arrangement. Sensor and compressor-flag behavior can vary by model; see the [compatibility notes](../README.md#hardware-and-compatibility).
