# LambLisp OTA Factory Loader (llloader)

The **llloader** is the factory-partition recovery app for OTA-capable ESP32 targets. It lives in
the `factory` slot (0x10000) and streams a new LambLisp VM image into an OTA slot (aux-storage /
network / UART or BLE operator console). A VM that never `mark_valid`s rolls back to factory ->
the loader, so the device stays recoverable.

## Contents
- `src/`                 -- loader source (pure ESP-IDF).
- `platformio.ini`       -- sample PlatformIO project (build/flash at the offsets below).
- `sdkconfig.defaults`, `CMakeLists.txt` -- IDF config.
- `partitions/partitions_4M_ota.csv`    -- shared table (factory @0x10000, ota_0 @0x130000).
- `llloader.bin` (+ bootloader/partitions/ota_data) -- prebuilt classic-ESP32 image, when present.

## Flash (classic-ESP32, 4MB, partitions_4M_ota.csv)
| Offset   | Image                  |
|----------|------------------------|
| 0x1000   | bootloader.bin         |
| 0x8000   | partitions.bin         |
| 0xE000   | ota_data_initial.bin   |
| 0x10000  | llloader.bin (factory) |
| 0x130000 | LambLisp.bin  (ota_0)  |

Build from source: `pio run`.  Flash the factory app: `pio run --target upload` (offset 0x10000).
