# ESP32-C3 Wi-Fi GPIO Switcher

A simple ESP32-C3 project for controlling a GPIO pin through a web browser over Wi-Fi. Built using ESP-IDF.

The ESP32 hosts a webpage with ON/OFF controls and allows users to configure their local Wi-Fi network. Once connected, it can be accessed through `http://fan.local` using mDNS.

## Features

- Control GPIO4 through a web interface.
- Configure Wi-Fi through the ESP32's own access point.
- Save Wi-Fi credentials in flash memory (NVS).
- Automatically reconnect to the configured network.
- Access the webpage using mDNS.
- Works without internet or a dedicated mobile application.

## Hardware

- ESP32-C3
- Optional relay module and NPN transistor for switching external loads

GPIO4 is configured as the output:

| Command | GPIO4 |
|---|---|
| ON | HIGH (3.3 V) |
| OFF | LOW (0 V) |

## Setup

Requires ESP-IDF 6.1.

Install the mDNS component:

```bash
idf.py add-dependency "espressif/mdns"
```

Build and flash:

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

## Usage

1. Connect to the `Fan-Control` Wi-Fi network.
2. Password: `FanControl2026`
3. Open `http://192.168.4.1`.
4. Configure your local Wi-Fi SSID and password.
5. Connect your device to the same local network.
6. Open `http://fan.local` to control GPIO4.

The access point remains available even without a local Wi-Fi connection.

## HTTP API

| Method | Endpoint | Description |
|---|---|---|
| GET | `/` | Web interface |
| POST | `/api/on` | Set GPIO HIGH |
| POST | `/api/off` | Set GPIO LOW |
| GET | `/api/status` | GPIO and Wi-Fi status |
| POST | `/api/wifi` | Configure Wi-Fi credentials |

## Example Applications

### AC Fan Controller

GPIO4 can control an active-low relay module through an NPN transistor such as the BC549.

```text
ESP32-C3 GPIO4 -- 2.2kΩ -- BC549 Base
BC549 Collector ----------- Relay IN
BC549 Emitter ------------- GND
```

The relay module requires a suitable separate supply and a compatible input circuit.

This can be used to switch an AC fan remotely, provided the relay is appropriately rated for the motor load. Mains switching requires proper isolation, enclosure, protection, and an independent physical disconnect.

### Other Applications

- Switching LEDs
- Controlling low-voltage DC loads through suitable drivers
- Remote switching of external devices

## Notes

- The ESP32-C3 supports 2.4 GHz Wi-Fi.
- mDNS availability depends on the local network and client device.
- The current web interface has no authentication. Do not expose it to the internet.
- Change the default access-point password before deployment.
- The hostname and access-point name are currently `fan.local` and `Fan-Control`; these can be changed in the source code.

## License

See `LICENSE`.
