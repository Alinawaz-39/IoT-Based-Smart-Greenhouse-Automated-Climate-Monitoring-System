# Smart Greenhouse using ESP32 (IoT PBL)

**Course:** Internet of Things (CS-481), Problem-Based Learning
**University:** Mehran University of Engineering and Technology, Jamshoro
**Department:** Computer Systems Engineering | **Batch:** 23CS | **Roll No:** 23CS039 | **Student Name:** Ali Nawaz
**Instructor:** Engr. Aniqa Khan

**Wokwi Simulation Link:** https://wokwi.com/projects/477756899494514689

---

## What is this project?

A small smart greenhouse that runs on the **Wokwi simulator** with an **ESP32**.

It checks temperature, humidity and soil moisture all the time, and it can switch a **fan** and a **water pump** on its own. It also shows the data on a small screen and sends it to the internet using **MQTT**.

## What it does

| Situation | What the system does |
|---|---|
| Temperature is above 35 °C | Fan turns ON and the buzzer beeps |
| Temperature falls below 33 °C | Fan turns OFF and the buzzer stops |
| Soil moisture is below 30 % | Water pump turns ON |
| Soil moisture is 40 % or more | Water pump turns OFF |

The fan turns off at 33 °C (not 35 °C) and the pump turns off at 40 % (not 30 %). This stops the relays from switching ON and OFF again and again when the value stays near the limit.

The OLED screen shows temperature, humidity, soil moisture, fan and pump status, and Wi-Fi/MQTT status. Data is also sent every 5 seconds as JSON to the Serial Monitor and to the MQTT broker.

## Parts used

| Part | Use |
|---|---|
| ESP32 DevKit V1 | Main controller with Wi-Fi |
| DHT22 | Temperature and humidity sensor |
| Potentiometer | Acts as the soil moisture sensor |
| OLED SSD1306 (I2C) | Shows data on screen |
| 2 x Relay module + LED | Fan (green LED) and pump (blue LED) |
| Buzzer | Gives the alert sound |

## Pin connections

| ESP32 Pin | Connected to |
|---|---|
| GPIO 15 | DHT22 data |
| GPIO 34 | Potentiometer (soil moisture) |
| GPIO 21 | OLED SDA |
| GPIO 22 | OLED SCL |
| GPIO 26 | Relay 1 (fan) |
| GPIO 27 | Relay 2 (pump) |
| GPIO 25 | Buzzer |

## Files in this repository

| File | What it is |
|---|---|
| `sketch.ino` | The ESP32 program (code) |
| `diagram.json` | The circuit for Wokwi |
| `libraries.txt` | The libraries needed |
| `README.md` | This file |

## How to run it on Wokwi

1. Open https://wokwi.com and start a **new ESP32 project**.
2. Copy `diagram.json` into the **diagram.json** tab.
3. Copy `sketch.ino` into the **sketch.ino** tab.
4. Create a file called `libraries.txt` and paste in the libraries from this repository.
5. Click the green **Start** button. The first start can take 1 to 2 minutes.
6. Open the **Serial Monitor** to see the data.

## How to test it

- **Hot:** click the DHT22 and set the temperature to 38 °C. The fan LED turns on and the buzzer beeps.
- **Cool:** set the temperature to 32 °C. The fan turns off.
- **Dry soil:** turn the potentiometer down below 30 %. The pump LED turns on.
- **Wet soil:** turn the potentiometer up above 40 %. The pump turns off.

## MQTT (sending data to the internet)

The project uses the free broker `broker.hivemq.com` (port 1883). The topics use my roll number so they do not mix with other students.

| Topic | Purpose |
|---|---|
| `muet/cs481/23cs039/greenhouse/telemetry` | The ESP32 sends sensor data here |
| `muet/cs481/23cs039/greenhouse/status` | Shows `online` or `offline` |
| `muet/cs481/23cs039/greenhouse/cmd` | Send commands to the ESP32 |

**Example data sent:**
```json
{"device":"greenhouse-01","temperature":36.5,"humidity":58.0,"soil_moisture":24,"fan":"ON","pump":"ON","alarm":true,"mode":"AUTO","wifi_rssi":-50,"uptime_s":125}
```

**To see the data live:** open the HiveMQ web client at https://www.hivemq.com/demos/websocket-client/, click Connect, and subscribe to `muet/cs481/23cs039/greenhouse/#`.

**Commands you can send to the cmd topic:** `fan_on`, `fan_off`, `pump_on`, `pump_off`, `auto`

## Problems faced and how I solved them

| Problem | Solution |
|---|---|
| Relay switched ON/OFF too fast near the limit | Used two limits (hysteresis) |
| Soil sensor reading was noisy | Took the average of 10 readings |
| Analog pin stops working when Wi-Fi is on | Used GPIO 34, which works with Wi-Fi |
| Program froze when using `delay()` | Used `millis()` timers instead |
| Libraries missing when compiling | Added them in `libraries.txt` |
| MQTT message too big to send | Increased the buffer size to 512 |

## Future improvements

- Use a real soil moisture sensor
- Add a light sensor
- Make a dashboard with Node-RED or Grafana
- Use secure MQTT with a password
