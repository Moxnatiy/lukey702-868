# Soldering-iron MCU — planned

Firmware for the **soldering-iron** microcontroller of the Lukey 702/868 station
(ATmega8). Not started yet.

Planned steps (mirroring the hot-air subproject):

1. Read the iron MCU via USBASP — check whether it is read-locked (as the fan MCU was).
2. Recover the pin map from the schematic and the reference firmware (`Luk702_sir-SE`).
3. Reimplement in clean, documented C: temperature control (PID), display, buttons,
   presets, and an on-station calibration menu.

Setpoint range for the iron per the documentation: **200–480 °C**.

See [`../hot-air/`](../hot-air/) for the finished reference implementation and workflow.
