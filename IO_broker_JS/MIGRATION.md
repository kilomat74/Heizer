# ioBroker Migration (Heizer)

## Ziel
Schaltlogik stabilisieren durch klare Trennung:
- Decision -> schreibt nur `masterSwitch`
- Mapper -> schreibt nur den realen Zigbee-Aktor
- Feedback -> meldet echten Zustand + Verbrauch an ESP32

## Schritte
1. Altes kombiniertes Script `Heizer_Orchestrator` deaktivieren.
2. Drei neue Skripte anlegen und Inhalte aus folgenden Dateien uebernehmen:
   - `01_Heizer_Decision.js`
   - `02_Heizer_Mapper.js`
   - `03_Heizer_Feedback.js`
3. Skripte starten (Reihenfolge: Decision, Mapper, Feedback).
4. In Yahka/Apple Home auf virtuelle Punkte mappen:
   - `0_userdata.0.Heizer.mode` oder
   - `0_userdata.0.Heizer.remoteEnable`
5. Kein weiteres Script darf direkt `zigbee.0.0c4314fffe52af5b.state` schreiben.

## Schnelltest
1. `mode = MANUAL_ON` -> `masterSwitch=true` -> Zigbee EIN
2. `mode = MANUAL_OFF` -> `masterSwitch=false` -> Zigbee AUS
3. `mode = AUTO` + Temp > off-threshold -> `masterSwitch=false`
4. MQTT Rueckmeldung:
   - `mqtt.0.heizer.room.status` muss `heater_state` (IST) senden
