/************************************************************
 * Heizer Mapper (ioBroker)
 * Aufgabe:
 * - EINZIGE Stelle für reale Aktor-Schreibung
 * - mappt 0_userdata.0.Heizer.masterSwitch -> zigbee state
 * - Schutz vor Doppelschaltung / Trigger-Loops / Flattern
 ************************************************************/

const CFG = {
    masterSwitchId: "0_userdata.0.Heizer.masterSwitch",         // SOLL
    actualSwitchId: "0_userdata.0.Heizer.actualSwitch",         // IST Mirror
    mapStatusId: "0_userdata.0.Heizer.mapperStatus",            // debug
    mapLastWriteTsId: "0_userdata.0.Heizer.mapperLastWriteTs",  // debug

    zigbeeSwitchId: "zigbee.0.0c4314fffe52af5b.state",

    minSwitchGapMs: 2500,
    verifyDelayMs: 900
};

function nowMs() { return Date.now(); }
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
    ensure(CFG.masterSwitchId, false, { type: "boolean", role: "switch", name: "Heizer Master Soll" });
    ensure(CFG.actualSwitchId, false, { type: "boolean", role: "switch", name: "Heizer Ist (Zigbee)" });
    ensure(CFG.mapStatusId, "init", { type: "string", role: "text", name: "Heizer Mapper Status" });
    ensure(CFG.mapLastWriteTsId, 0, { type: "number", role: "value.time", unit: "ms", name: "Heizer Mapper last write" });
})();

let lastWriteTs = 0;
let writeInFlight = false;

function readActual() {
    return bool(getV(CFG.zigbeeSwitchId, false), false);
}

function syncActualMirror() {
    setIfChanged(CFG.actualSwitchId, readActual(), true);
}

function mapOnce(trigger) {
    const desired = bool(getV(CFG.masterSwitchId, false), false);
    const actual = readActual();

    syncActualMirror();

    if (desired === actual) {
        setIfChanged(CFG.mapStatusId, `noop:${trigger}`, true);
        return;
    }

    const now = nowMs();
    if (writeInFlight) {
        setIfChanged(CFG.mapStatusId, `skip_inflight:${trigger}`, true);
        return;
    }
    if (now - lastWriteTs < CFG.minSwitchGapMs) {
        setIfChanged(CFG.mapStatusId, `skip_gap:${trigger}`, true);
        return;
    }

    writeInFlight = true;
    lastWriteTs = now;
    setIfChanged(CFG.mapLastWriteTsId, now, true);
    setIfChanged(CFG.mapStatusId, `write_${desired ? "on" : "off"}:${trigger}`, true);

    setState(CFG.zigbeeSwitchId, desired, false);

    setTimeout(() => {
        const verified = readActual();
        syncActualMirror();
        setIfChanged(CFG.mapStatusId, verified === desired ? "verified" : "verify_mismatch", true);
        writeInFlight = false;
    }, CFG.verifyDelayMs);
}

on({ id: CFG.masterSwitchId, change: "any" }, () => mapOnce("master"));
on({ id: CFG.zigbeeSwitchId, change: "any" }, () => {
    syncActualMirror();
    mapOnce("zigbee_change");
});

schedule("*/10 * * * * *", () => mapOnce("timer"));

syncActualMirror();
mapOnce("startup");
log("[Heizer Mapper] aktiv", "info");

