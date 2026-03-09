/************************************************************
 * Heizer Feedback (ioBroker)
 * Aufgabe:
 * - echten Zustand + Leistung einlesen
 * - Session-KWh/Kosten berechnen
 * - Rückmeldung an ESP auf mqtt.0.heizer.room.status publizieren
 ************************************************************/

const CFG = {
    mqttStatusOut: "mqtt.0.heizer.room.status",
    powerId: "zigbee.0.0c4314fffe52af5b.load_power",
    actualSwitchId: "0_userdata.0.Heizer.actualSwitch",
    masterSwitchId: "0_userdata.0.Heizer.masterSwitch",
    modeId: "0_userdata.0.Heizer.mode",
    reasonId: "0_userdata.0.Heizer.desiredReason",
    priceId: "0_userdata.0.Heizer.priceEurKwh",

    sessionBase: "0_userdata.0.Heizer.session",
    energyKwhId: "0_userdata.0.Heizer.session.energyKwh",
    costEurId: "0_userdata.0.Heizer.session.costEur",
    lastUpdateTsId: "0_userdata.0.Heizer.session.lastUpdateTs"
};

function nowMs() { return Date.now(); }
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

(function init() {
    ensure(CFG.priceId, 0.393, { type: "number", role: "value", unit: "EUR/kWh", name: "Heizer Preis" });
    ensure(CFG.energyKwhId, 0, { type: "number", role: "value.energy", unit: "kWh", name: "Heizer Session Energie" });
    ensure(CFG.costEurId, 0, { type: "number", role: "value.currency", unit: "EUR", name: "Heizer Session Kosten" });
    ensure(CFG.lastUpdateTsId, 0, { type: "number", role: "value.time", unit: "ms", name: "Heizer Session last update" });
})();

let lastTs = nowMs();
let lastPayload = "";

function integrate() {
    const ts = nowMs();
    let dtH = (ts - lastTs) / 3600000.0;
    if (dtH < 0 || dtH > 1) dtH = 0;
    lastTs = ts;

    let pW = num(getV(CFG.powerId, 0), 0);
    if (pW < 0 || pW > 20000) pW = 0;

    const oldKwh = num(getV(CFG.energyKwhId, 0), 0);
    const kwh = Math.max(0, oldKwh + (pW / 1000.0) * dtH);
    const price = Math.max(0, num(getV(CFG.priceId, 0.393), 0.393));
    const cost = kwh * price;

    setIfChanged(CFG.energyKwhId, kwh, true);
    setIfChanged(CFG.costEurId, cost, true);
    setIfChanged(CFG.lastUpdateTsId, ts, true);
}

function publishStatus() {
    const payload = JSON.stringify({
        heater_state: bool(getV(CFG.actualSwitchId, false), false),
        desired_state: bool(getV(CFG.masterSwitchId, false), false),
        mode_state: String(getV(CFG.modeId, "AUTO")),
        reason: String(getV(CFG.reasonId, "feedback")),
        power_w: num(getV(CFG.powerId, 0), 0),
        session_kwh: num(getV(CFG.energyKwhId, 0), 0),
        session_cost_eur: num(getV(CFG.costEurId, 0), 0),
        ts_ms: nowMs()
    });

    if (payload === lastPayload) return;
    lastPayload = payload;
    setState(CFG.mqttStatusOut, payload, false);
}

function tick() {
    integrate();
    publishStatus();
}

on({ id: CFG.actualSwitchId, change: "any" }, tick);
on({ id: CFG.masterSwitchId, change: "any" }, tick);
on({ id: CFG.modeId, change: "any" }, tick);
on({ id: CFG.reasonId, change: "any" }, tick);
on({ id: CFG.powerId, change: "any" }, tick);
on({ id: CFG.priceId, change: "any" }, tick);

schedule("*/5 * * * * *", tick);

tick();
log("[Heizer Feedback] aktiv", "info");

