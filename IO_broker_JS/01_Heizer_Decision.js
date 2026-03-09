/************************************************************
 * Heizer Decision (ioBroker)
 * Aufgabe:
 * - Eingänge aus ESP32 (mqtt request/config/state) und virtuellen
 *   Bedienpunkten (mode/remoteEnable) auswerten
 * - AUSSCHLIESSLICH den virtuellen Sollzustand schreiben:
 *   0_userdata.0.Heizer.masterSwitch
 *
 * WICHTIG:
 * - Dieses Script schaltet NICHT direkt den Zigbee-Stecker.
 * - Reale Aktor-Schaltung passiert nur in 02_Heizer_Mapper.js
 ************************************************************/

const CFG = {
    // MQTT Objekte (mqtt.0)
    mqttRequestId: "mqtt.0.heizer.room.request",
    mqttConfigId: "mqtt.0.heizer.room.config",
    mqttStateId: "mqtt.0.heizer.room.state",

    // Virtuelle Führungsdatenpunkte
    udBase: "0_userdata.0.Heizer",
    masterSwitchId: "0_userdata.0.Heizer.masterSwitch", // SOLL
    modeId: "0_userdata.0.Heizer.mode",                 // AUTO|MANUAL_ON|MANUAL_OFF
    remoteEnableId: "0_userdata.0.Heizer.remoteEnable", // z.B. Yahka Freigabe

    // Technische Hilfspunkte (optional, für Nachvollziehbarkeit)
    desiredReasonId: "0_userdata.0.Heizer.desiredReason",
    desiredTempId: "0_userdata.0.Heizer.desiredTemp",
    targetId: "0_userdata.0.Heizer.targetC",
    windowId: "0_userdata.0.Heizer.windowC",
    lastDecisionTsId: "0_userdata.0.Heizer.lastDecisionTs",

    staleMs: 90 * 1000
};

function nowMs() { return Date.now(); }
function jParse(s) { try { return JSON.parse(s); } catch (e) { return null; } }
function num(v, d) { const n = Number(v); return Number.isFinite(n) ? n : d; }
function bool(v, d = false) {
    if (typeof v === "boolean") return v;
    if (typeof v === "number") return v !== 0;
    if (typeof v === "string") {
        const s = v.trim().toLowerCase();
        if (["true", "1", "on"].includes(s)) return true;
        if (["false", "0", "off"].includes(s)) return false;
    }
    return d;
}
function normalizeMode(v) {
    const s = String(v || "AUTO").toUpperCase();
    if (s === "ON") return "MANUAL_ON";
    if (s === "OFF") return "MANUAL_OFF";
    if (s === "MANUAL_ON" || s === "MANUAL_OFF" || s === "AUTO") return s;
    return "AUTO";
}
function exists(id) { return !!getObject(id); }
function ensure(id, def, common) {
    if (!exists(id)) createState(id, def, { ...common, read: true, write: true });
}
function getV(id, def = null) {
    const s = getState(id);
    return s ? s.val : def;
}
function setIfChanged(id, value, ack) {
    const cur = getState(id);
    if (!cur || cur.val !== value) setState(id, value, ack);
}

// Lokaler Laufzeitzustand
const R = {
    tempC: NaN,
    targetC: 21.0,
    windowC: 1.0,
    lastTempTs: 0,
    reqOn: false,
    reqReason: "init"
};

(function init() {
    ensure(CFG.masterSwitchId, false, { type: "boolean", role: "switch", name: "Heizer Sollzustand" });
    ensure(CFG.modeId, "AUTO", { type: "string", role: "text", name: "Heizer Modus" });
    ensure(CFG.remoteEnableId, true, { type: "boolean", role: "switch.enable", name: "Heizer Remote Enable" });
    ensure(CFG.desiredReasonId, "init", { type: "string", role: "text", name: "Heizer Soll-Grund" });
    ensure(CFG.desiredTempId, 0, { type: "number", role: "value.temperature", unit: "°C", name: "Heizer Temp fuer Entscheidung" });
    ensure(CFG.targetId, 21.0, { type: "number", role: "value.temperature", unit: "°C", name: "Heizer Solltemperatur" });
    ensure(CFG.windowId, 1.0, { type: "number", role: "value.temperature", unit: "°C", name: "Heizer Fenster" });
    ensure(CFG.lastDecisionTsId, 0, { type: "number", role: "value.time", unit: "ms", name: "Heizer letzte Entscheidung" });
})();

function applyConfigFromEsp(payload) {
    if (!payload || typeof payload !== "object") return;
    if (typeof payload.target_c === "number") R.targetC = payload.target_c;
    if (typeof payload.window_c === "number") R.windowC = payload.window_c;
    if (typeof payload.mode === "string") setIfChanged(CFG.modeId, normalizeMode(payload.mode), true);

    setIfChanged(CFG.targetId, R.targetC, true);
    setIfChanged(CFG.windowId, R.windowC, true);
}

function applyRequestFromEsp(payload) {
    if (!payload || typeof payload !== "object") return;
    if (typeof payload.heater_request_on === "boolean") R.reqOn = payload.heater_request_on;
    if (typeof payload.reason === "string") R.reqReason = payload.reason;
    if (typeof payload.temp_c === "number") {
        R.tempC = payload.temp_c;
        R.lastTempTs = nowMs();
    }
    if (typeof payload.target_c === "number") R.targetC = payload.target_c;
    if (typeof payload.window_c === "number") R.windowC = payload.window_c;
    if (typeof payload.mode === "string") setIfChanged(CFG.modeId, normalizeMode(payload.mode), true);

    setIfChanged(CFG.targetId, R.targetC, true);
    setIfChanged(CFG.windowId, R.windowC, true);
}

function applyStateFromEsp(payload) {
    if (!payload || typeof payload !== "object") return;
    const t = payload.telemetry && typeof payload.telemetry === "object" ? payload.telemetry : payload;
    if (typeof t.temp_c === "number") {
        R.tempC = t.temp_c;
        R.lastTempTs = nowMs();
    }
    if (payload.config && typeof payload.config === "object") {
        applyConfigFromEsp(payload.config);
    } else {
        applyConfigFromEsp(payload);
    }
}

function decide() {
    const mode = normalizeMode(getV(CFG.modeId, "AUTO"));
    const remoteEnable = bool(getV(CFG.remoteEnableId, true), true);
    const temp = R.tempC;
    const target = num(R.targetC, 21.0);
    const win = Math.max(0.2, num(R.windowC, 1.0));
    const stale = nowMs() - num(R.lastTempTs, 0) > CFG.staleMs;
    const onTh = target - win * 0.5;
    const offTh = target + win * 0.5;
    const curMaster = bool(getV(CFG.masterSwitchId, false), false);

    let desired = curMaster;
    let reason = "hold";

    if (!remoteEnable) {
        desired = false;
        reason = "remote_disabled";
    } else if (mode === "MANUAL_ON") {
        desired = true;
        reason = "manual_on";
    } else if (mode === "MANUAL_OFF") {
        desired = false;
        reason = "manual_off";
    } else {
        if (!Number.isFinite(temp)) {
            desired = false;
            reason = "sensor_invalid";
        } else if (stale) {
            desired = false;
            reason = "sensor_stale";
        } else if (temp <= onTh) {
            desired = true;
            reason = "below_on_threshold";
        } else if (temp >= offTh) {
            desired = false;
            reason = "above_off_threshold";
        } else {
            desired = curMaster;
            reason = desired ? "in_hysteresis_keep_on" : "in_hysteresis_keep_off";
        }
        // request vom ESP nur als Zusatzsignal in Hysteresezone
        if (reason.startsWith("in_hysteresis") && R.reqOn !== curMaster) {
            reason = R.reqOn ? "esp_request_on_hysteresis" : "esp_request_off_hysteresis";
        }
    }

    setIfChanged(CFG.masterSwitchId, !!desired, false);
    setIfChanged(CFG.desiredReasonId, reason, true);
    setIfChanged(CFG.desiredTempId, Number.isFinite(temp) ? temp : 0, true);
    setIfChanged(CFG.lastDecisionTsId, nowMs(), true);
}

on({ id: CFG.mqttConfigId, change: "any" }, (obj) => {
    const j = typeof obj.state.val === "string" ? jParse(obj.state.val) : obj.state.val;
    applyConfigFromEsp(j);
    decide();
});

on({ id: CFG.mqttRequestId, change: "any" }, (obj) => {
    const j = typeof obj.state.val === "string" ? jParse(obj.state.val) : obj.state.val;
    applyRequestFromEsp(j);
    decide();
});

on({ id: CFG.mqttStateId, change: "any" }, (obj) => {
    const j = typeof obj.state.val === "string" ? jParse(obj.state.val) : obj.state.val;
    applyStateFromEsp(j);
    decide();
});

on({ id: CFG.modeId, change: "any" }, () => decide());
on({ id: CFG.remoteEnableId, change: "any" }, () => decide());

schedule("*/5 * * * * *", () => decide());

decide();
log("[Heizer Decision] aktiv", "info");

