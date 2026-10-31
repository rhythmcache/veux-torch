# VeuxTorch

**VeuxTorch** is a proof-of-concept (PoC) Android application and native C driver (`libflashctl`) designed to control the flashlight LED brightness on Xiaomi Redmi Note 11 Pro 5G / POCO X4 Pro 5G (`veux` / `peux`) devices.

Unlike the stock flashlight implementation which only allows binary on/off switching at fixed factory levels, VeuxTorch communicates directly with the Qualcomm Camera Control Interface (CCI) to adjust the Kinetic KTD2691 flash driver's current register across 44 discrete hardware levels.

---

## Background & Architecture

On the Qualcomm SM6375 platform (`veux` / `peux`), the rear camera flash is driven by a Kinetic KTD2691 LED driver mapped to CCI master 1 at 7-bit slave address `0x64` (8-bit wire address `0xC8`).

Accessing the hardware directly bypasses higher-level camera APIs:
1. **`/dev/v4l-subdev15` (`qcom,camera-flash@0`)**: Receives I2C random write packets and custom GPIO power sequences.
2. **`/dev/video0` (Qualcomm Camera Request Manager / CRM)**: Creates hardware sessions and manages shared DMA command buffers.
3. **Power Sequencing**: Pulls high `tlmm 134` (`SENSOR_CUSTOM_GPIO2`, sequence type 11) via a verified 56-byte power-up buffer before dispatching I2C writes.
4. **Brightness Register**: Adjusts register `0x04` between `0x01` (lowest dim) and `0x2C` (44 decimal, verified stock maximum).

---

## Safety & Safeguards

- **Strict Hardware Ceiling**: Register `0x04` is permanently clamped to `0x2C` (44 decimal) inside the native library to prevent overdriving the LED.
- **Inactivity Watchdog**: A background monotonic watchdog thread automatically powers down the LED if no application heartbeat is received for 5 seconds.
- **Clean Failure Recovery**: Every error path immediately sends hardware stream-off packets (`0x01=0x00`) and frees all acquired CRM sessions and DMA handles.

---

## Dynamic Camera Provider Management

Qualcomm's CRM kernel driver enforces strict single-client exclusivity (`if (g_dev.open_cnt >= 1) return -EALREADY;`). Under normal operation, Android's background camera HAL holds `/dev/video0` open 24/7.

To ensure broad compatibility across ROMs (MIUI, HyperOS, LineageOS, AOSP, Pixel Experience):
- **No hardcoded service names**: The app dynamically queries Android init properties (`getprop | grep [init.svc.*camera*provider*]`) to discover the exact camera HAL service running on the system (e.g. `vendor.camera-provider-2-4`, `vendor.camera-provider-2-5`, or `camera-provider-default`).
- **Automatic pause & restore**: The detected camera provider is temporarily stopped when the flashlight turns on and automatically resumed the moment the flashlight turns off or the app is closed.

---

## Disclaimer

This software is a proof-of-concept for hardware research. Driving camera flash LEDs outside of vendor HAL frameworks carries inherent risk. Use responsibly.
