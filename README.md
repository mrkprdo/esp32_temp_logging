# ESP32-S3 Temperature Logger

Real-time temperature monitoring system with web dashboard and persistent data logging.

![Dashboard](images/dashboard.jpg)

## Features

- Real-time temperature monitoring using ESP32-S3 internal sensor
- Web dashboard with live Chart.js visualization
- WebSocket for live updates (2s interval)
- Persistent storage in NVS flash (up to 1000 readings)
- WS2812 RGB LED indicator (Red → Green → Blue)
- REST API for data access

## Hardware

- **MCU:** ESP32-S3-SUPERMINI
- **LED:** WS2812 RGB (GPIO 48)
- **Power:** USB-C PD with buck converter

![Schematic](images/schematics.JPG)
![PCB](images/pcb.JPG)
![Prototype](images/prototype.jpg)

## Quick Start

### 1. Configure WiFi

Edit `main/main.c`:

```c
#define WIFI_SSID "YourSSID"
#define WIFI_PASS "YourPassword"
```

### 2. Build and Flash

```bash
idf.py build
idf.py -p COMx flash monitor
```

### 3. Access Dashboard

Check serial monitor for IP address, then open:

```
http://<ESP32-IP>/
```

## API

| Endpoint     | Method    | Description              |
| ------------ | --------- | ------------------------ |
| `/`          | GET       | Web dashboard            |
| `/api/temp`  | GET       | Get all readings (JSON)  |
| `/api/clear` | GET       | Clear all logs           |
| `/ws`        | WebSocket | Live temperature updates |

## Configuration

Edit `main/main.c`:

```c
#define LED_GPIO 48                      // WS2812 GPIO pin
#define MAX_TEMP_LOGS 1000               // Max stored readings
#define TEMP_LOG_INTERVAL_MS 5000        // Sample every 5s
#define WEBSOCKET_UPDATE_MS 2000         // WebSocket push every 2s
```

## How It Works

### Temperature Logging

- Samples ESP32-S3 internal sensor every 5 seconds
- Stores in circular buffer (1000 max)
- Auto-saves to NVS flash every 10 readings
- Data persists across power cycles

### WS2812 LED Control

- Custom RMT-based driver (no external libraries)
- Precise timing using ESP32-S3 RMT peripheral
- WS2812 protocol: T0H=0.4µs, T1H=0.8µs

### Web Interface

- Embedded HTML (no filesystem needed)
- Real-time updates via WebSocket
- Interactive Chart.js graph
- Stats: current, avg, min, max temps

## Project Structure

```
esp32_temp_logger/
├── main/
│   ├── main.c           # Application code
│   └── www/
│       └── index.html   # Web dashboard
├── images/              # Docs
└── sdkconfig.defaults   # ESP-IDF config
```

## Requirements

- ESP-IDF v5.x
- ESP32-S3 target

## License

Provided as-is for educational purposes.
