# Smart Parking IoT System

A smart parking setup built on an ESP32 that sends its data to the cloud (ThingSpeak)
over MQTT and HTTP, plus a Python app to monitor it and control it remotely. The system
detects when a car arrives, opens and closes a barrier, reads temperature and humidity,
turns on a light when it gets dark, and charges a price that changes depending on the
time of day.

It has three parts that work together: the firmware on the ESP32 (C++), the ThingSpeak
channel in the middle, and the Python client that talks to it.

## What it does

- Detects a vehicle with an HC-SR04 ultrasonic sensor using a calibrated distance threshold.
- Opens and closes a servo barrier, with separate logic for entry and exit, auto-close
  timers and a safety timeout.
- Reads temperature and humidity with a DHT11 and raises an alarm if either gets too high.
- Turns a light on automatically using an LDR when the surroundings get dark.
- Shows occupancy with green and red LEDs.
- Calculates a price using a simulated clock, applying peak, off-peak and night
  multipliers to a base 30 minute rate and keeping a running total per vehicle.
- Publishes occupancy, climate, barrier state, rate and total to ThingSpeak over MQTT,
  and reads a command field over HTTP to open or close remotely.
- Displays live readings (temperature, humidity, occupancy, rate and simulated time) on
  a 16x2 I2C LCD.
- Includes a Python client that reads the latest channel state, prints a formatted
  dashboard, sends entry and exit commands (with rate limit handling and retries) and
  prints a billing summary at checkout.

## How the pieces connect

```
  Sensors  ->  ESP32 firmware  --MQTT publish-->  ThingSpeak  --HTTP read-->  Python client
 (ultrasonic,   (state machine,                    (cloud        (live dashboard +
  DHT11, LDR)    pricing, barrier)                  channel)       entry/exit commands)
       ^                                                                  |
       +----------------- command field (field7) <------------------------+
```

## Layout

```
firmware/
  SmartParking/SmartParking.ino   Main parking firmware (ESP32)
  LedTest/LedTest.ino             Bring-up sketch that tests 12 LEDs in sequence
client/
  iot_app.py                      Python monitoring and control CLI
```

## Hardware

- ESP32 dev board
- HC-SR04 ultrasonic sensor
- SG90 or similar servo for the barrier
- DHT11 temperature and humidity sensor
- LDR with a resistor for ambient light
- 16x2 I2C LCD (address 0x27)
- Status LEDs (green and red) and a controllable light

## Firmware libraries

WiFi, PubSubClient, HTTPClient, Wire, LCD_I2C, DHT, ESP32Servo.

## Configuration

Credentials are not committed. Before flashing, replace the placeholder values in
`firmware/SmartParking/SmartParking.ino`:

```cpp
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* mqttUser = "YOUR_MQTT_USERNAME";
// ThingSpeak channel id and read/write API keys
```

And in `client/iot_app.py`:

```python
ID_CANAL        = 0000000
LLAVE_LECTURA   = "YOUR_THINGSPEAK_READ_KEY"
LLAVE_ESCRITURA = "YOUR_THINGSPEAK_WRITE_KEY"
```

## Running the Python client

```bash
pip install requests
python client/iot_app.py
```

The menu lets you refresh the data, open the entry to park, open the exit to pay and
leave, or quit.

## ThingSpeak fields

| Field | Meaning |
|-------|---------|
| field1 | Occupied time in simulated minutes |
| field2 | Temperature in Celsius |
| field3 | Humidity in percent |
| field4 | Barrier state (0 closed, 1 open) |
| field5 | Occupancy (0 free, 1 occupied) |
| field6 | Rate per 30 minutes |
| field7 | Command (1 entry, 2 exit) |
| field8 | Running total |

Built as a final IoT project. The firmware is written in C++ for Arduino and the client
in Python.
