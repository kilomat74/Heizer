# HEIZER_Control (ESP-IDF)

ESP32-Projekt mit:
- AHT10 Temperatur-/Luftfeuchtesensor (I2C)
- ST7789P3 Display, 76x284 (SPI)
- Web-UI (Zieltemperatur + Hysterese-Slider)
- MQTT-Anbindung (z. B. ioBroker auf Raspberry Pi)
- Heizungsrelais mit AUTO/ON/OFF (optionaler lokaler Fallback)
- Architektur: ESP32 als UI/Sensor-Cockpit, ioBroker als zentrale Schaltlogik

## Verdrahtung (aktueller Code)
### AHT10
- `VIN` -> 3V3
- `GND` -> GND
- `SDA` -> GPIO21
- `SCL` -> GPIO22

### ST7789P3
- `VCC` -> 3V3
- `GND` -> GND
- `SCL` -> GPIO18
- `SDA` -> GPIO23
- `CS` -> GPIO5
- `DC` -> GPIO16
- `RST` -> GPIO17
- `BL` -> GPIO27

### Relais
- IN -> GPIO26
- VCC/GND entsprechend Modul

## Wichtige MQTT Topics
- Publish Telemetrie/Status: `heizer/room/state`
- Publish Konfiguration: `heizer/room/config`
- Publish Heizanforderung: `heizer/room/request`
- Publish Verfuegbarkeit: `heizer/room/availability`
- Subscribe Commands: `heizer/room/cmd` (`AUTO`, `ON`, `OFF`, `MANUAL_ON`, `MANUAL_OFF`, `TEST_TOGGLE`)
- Subscribe Leistung: `zigbee/0/0c4314fffe52af5b/load_power`
- Subscribe ioBroker Status-Feedback: `heizer/room/status`
- Optionaler Status-Mirror (Istzustand): `zigbee/0/0c4314fffe52af5b/state`

Hinweis:
- Standardmodus ist `automation_remote=true`: ioBroker entscheidet final, ESP32 liefert Request/Telemetrie.
- Der im Display/WebUI gezeigte Heizerzustand ist der rueckgemeldete Ist-Zustand (wenn vorhanden).

## Build + Flash
```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.wchusbserial14240 flash
idf.py -p /dev/cu.wchusbserial14240 monitor
```

## Web-UI
Im Browser die IP des ESP32 aufrufen, z. B.:
- `http://192.168.178.123/`

## Hinweise
- WLAN/MQTT und Pins stehen oben in `main/main.c` als `#define`.
- Falls das ST7789-Bild gedreht/verschoben ist, passe ich dir sofort `swap_xy/mirror/gap` an.

## ioBroker: Robuste 3-Skript-Architektur
Fuer eine stabile Schaltung ohne Trigger-Schleifen sind die Skripte in `IO_broker_JS/` getrennt:

1. `01_Heizer_Decision.js`
- bewertet Modus/Temperatur/Freigabe
- schreibt nur `0_userdata.0.Heizer.masterSwitch` (SOLL)
- schreibt **nicht** auf den Zigbee-Aktor

2. `02_Heizer_Mapper.js`
- einzige Stelle, die `zigbee.0.0c4314fffe52af5b.state` schreibt
- mappt `masterSwitch -> zigbee state`
- mit Mindestabstand/Debounce gegen Doppelschalten

3. `03_Heizer_Feedback.js`
- liest realen Schaltzustand + Leistung
- berechnet Session kWh/EUR
- publiziert Rueckmeldung an `mqtt.0.heizer.room.status`

### Fuehrende Datenpunkte
- `0_userdata.0.Heizer.masterSwitch` = gewuenschter Zustand (SOLL)
- `0_userdata.0.Heizer.actualSwitch` = realer Zigbee-Zustand (IST)
- `0_userdata.0.Heizer.mode` = `AUTO|MANUAL_ON|MANUAL_OFF`
- `0_userdata.0.Heizer.remoteEnable` = globale Freigabe (z. B. fuer Yahka)

### Yahka / Apple Home Empfehlung
- Yahka auf `0_userdata.0.Heizer.mode` oder `0_userdata.0.Heizer.remoteEnable` mappen
- nicht direkt den Zigbee-Aktor als Hauptlogik schreiben
