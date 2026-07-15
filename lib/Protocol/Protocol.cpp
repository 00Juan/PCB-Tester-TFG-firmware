#include "Protocol.h"
#include <ArduinoJson.h>

// ============================================================================
// Helpers
// ============================================================================

static float round3(float v) { return roundf(v * 1000.0f) / 1000.0f; }
static float round4(float v) { return roundf(v * 10000.0f) / 10000.0f; }

static const char* lvlpModeStr(LVLPMode m) {
    switch (m) {
        case MODE_HIGH_IMPEDANCE: return "HZ";
        case MODE_VOLTAGE_SOURCE: return "VS";
        case MODE_CURRENT_SOURCE: return "CS";
        case MODE_RESISTIVE_LOAD: return "RL";
        case MODE_PWM_GENERATOR:  return "PWM";
    }
    return "?";
}

static bool lvlpModeFromStr(const char* s, LVLPMode& out) {
    if (!s) return false;
    if (strcmp(s, "HZ") == 0)  { out = MODE_HIGH_IMPEDANCE; return true; }
    if (strcmp(s, "VS") == 0)  { out = MODE_VOLTAGE_SOURCE; return true; }
    if (strcmp(s, "CS") == 0)  { out = MODE_CURRENT_SOURCE; return true; }
    if (strcmp(s, "RL") == 0)  { out = MODE_RESISTIVE_LOAD; return true; }
    if (strcmp(s, "PWM") == 0) { out = MODE_PWM_GENERATOR;  return true; }
    return false;
}

static const char* lvlpFaultStr(LVLPStatus s) {
    switch (s) {
        case STATUS_NORMAL:           return "NONE";
        case STATUS_FAIL_OVERCURRENT: return "OVERCURRENT";
        case STATUS_FAIL_OVERVOLTAGE: return "OVERVOLTAGE";
        default:                      return "OTHER";
    }
}

static const char* hpFaultStr(HPCHStatus s) {
    switch (s) {
        case HPCH_STATUS_NORMAL:           return "NONE";
        case HPCH_STATUS_FAIL_OVERCURRENT: return "OVERCURRENT";
        case HPCH_STATUS_FAIL_OVERVOLTAGE: return "OVERVOLTAGE";
        default:                           return "OTHER";
    }
}

static const char* hvFaultStr(HVChannelStatus s) {
    switch (s) {
        case HV_STATUS_NORMAL:           return "NONE";
        case HV_STATUS_FAIL_OVERVOLTAGE: return "OVERVOLTAGE";
        default:                         return "OTHER";
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void TesterProtocol::begin(LVLPChannel* lvlp, uint8_t lvlpCount,
                           HPCH* hp, uint8_t hpCount,
                           HVChannel* hv, uint8_t hvCount,
                           Stream& io) {
    lvlp_ = lvlp;  nLvlp_ = min<uint8_t>(lvlpCount, 8);
    hp_   = hp;    nHp_   = min<uint8_t>(hpCount, 2);
    hv_   = hv;    nHv_   = min<uint8_t>(hvCount, 1);
    io_ = &io;
    rxLen_ = 0;
    for (uint8_t i = 0; i < nLvlp_; i++) prevLvlpStatus_[i] = (uint8_t)lvlp_[i].getStatus();
    for (uint8_t i = 0; i < nHp_;   i++) prevHpStatus_[i]   = (uint8_t)hp_[i].getStatus();
    for (uint8_t i = 0; i < nHv_;   i++) prevHvStatus_[i]   = (uint8_t)hv_[i].getStatus();
}

void TesterProtocol::setTelemetryRateHz(uint8_t hz) {
    if (hz > 50) hz = 50;
    telemHz_ = hz;
    telemPeriodMs_ = (hz == 0) ? 0 : (1000UL / hz);
}

// ============================================================================
// Serial RX
// ============================================================================

void TesterProtocol::service() {
    while (io_ && io_->available()) {
        char c = (char)io_->read();
        if (c == '\n') {
            rxBuf_[rxLen_] = '\0';
            if (rxLen_ > 0) handleLine(rxBuf_);
            rxLen_ = 0;
        } else if (c != '\r') {
            if (rxLen_ < RX_BUF_SIZE - 1) {
                rxBuf_[rxLen_++] = c;
            } else {
                rxLen_ = 0; // overflow: drop the line
                log("warn", "rx line too long, dropped");
            }
        }
    }
}

// ============================================================================
// TX helpers
// ============================================================================

static void sendDoc(Stream* io, JsonDocument& doc) {
    if (!io) return;
    serializeJson(doc, *io);
    io->print('\n');
}

void TesterProtocol::log(const char* level, const char* msg) {
    JsonDocument doc;
    doc["type"] = "log";
    doc["lvl"] = level;
    doc["msg"] = msg;
    sendDoc(io_, doc);
}

// ============================================================================
// Command dispatch
// ============================================================================

void TesterProtocol::handleLine(char* line) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line);

    JsonDocument res;
    res["type"] = "ack";
    long id = doc["id"] | -1L;
    if (id >= 0) res["id"] = id;

    if (err) {
        res["ok"] = false;
        res["err"] = "E_PARSE";
        res["msg"] = err.c_str();
        sendDoc(io_, res);
        return;
    }

    const char* cmd = doc["cmd"] | (const char*)nullptr;
    if (!cmd) {
        res["ok"] = false;
        res["err"] = "E_CMD";
        res["msg"] = "missing cmd";
        sendDoc(io_, res);
        return;
    }

    // ---- hello -------------------------------------------------------------
    if (strcmp(cmd, "hello") == 0) {
        res["ok"] = true;
        res["name"] = "PCB-Tester";
        res["fw"] = FW_VERSION;
        res["proto"] = PROTO_VERSION;
        res["lvlp"] = nLvlp_;
        res["hp"] = nHp_;
        res["hv"] = nHv_;
        res["hv_cal"] = (nHv_ > 0) ? hv_[0].isCalibrationValid() : false;
        res["telem_hz"] = telemHz_;
        res["estop"] = estopLatched_;
        sendDoc(io_, res);
        return;
    }

    // ---- estop ---------------------------------------------------------------
    if (strcmp(cmd, "estop") == 0) {
        triggerEstop("cmd");
        res["ok"] = true;
        sendDoc(io_, res);
        return;
    }

    // ---- telem.rate ----------------------------------------------------------
    if (strcmp(cmd, "telem.rate") == 0) {
        if (!doc["hz"].is<int>()) {
            res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "missing hz";
            sendDoc(io_, res);
            return;
        }
        int hz = doc["hz"];
        if (hz < 0 || hz > 50) {
            res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "hz must be 0-50";
            sendDoc(io_, res);
            return;
        }
        setTelemetryRateHz((uint8_t)hz);
        res["ok"] = true;
        res["telem_hz"] = telemHz_;
        sendDoc(io_, res);
        return;
    }

    // ---- ch.* commands (all need a valid "ch") -------------------------------
    if (strncmp(cmd, "ch.", 3) == 0) {
        int ch = doc["ch"] | 0;
        const uint8_t chMax = nLvlp_ + nHp_ + nHv_;
        if (ch < 1 || ch > chMax) {
            res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "ch out of range";
            sendDoc(io_, res);
            return;
        }
        const bool isLvlp = ch <= nLvlp_;
        const bool isHp   = !isLvlp && ch <= nLvlp_ + nHp_;
        LVLPChannel* lc = isLvlp ? &lvlp_[ch - 1] : nullptr;
        HPCH*        hc = isHp   ? &hp_[ch - 1 - nLvlp_] : nullptr;
        HVChannel*   vc = (!isLvlp && !isHp) ? &hv_[ch - 1 - nLvlp_ - nHp_] : nullptr;

        if (strcmp(cmd, "ch.set") == 0) {
            if (!lc) {
                res["ok"] = false; res["err"] = "E_ARG";
                res["msg"] = "ch.set only valid for LVLP channels (1-8)";
                sendDoc(io_, res);
                return;
            }
            LVLPMode mode;
            if (!lvlpModeFromStr(doc["mode"] | (const char*)nullptr, mode)) {
                res["ok"] = false; res["err"] = "E_ARG";
                res["msg"] = "mode must be HZ|VS|CS|RL|PWM";
                sendDoc(io_, res);
                return;
            }
            if (mode == MODE_VOLTAGE_SOURCE && !doc["v"].is<float>()) {
                res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "VS needs v";
                sendDoc(io_, res);
                return;
            }
            if ((mode == MODE_CURRENT_SOURCE || mode == MODE_RESISTIVE_LOAD)
                && !doc["i"].is<float>()) {
                res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "CS/RL needs i";
                sendDoc(io_, res);
                return;
            }
            if (mode == MODE_PWM_GENERATOR
                && (!doc["duty"].is<int>() || !doc["freq"].is<int>())) {
                res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "PWM needs duty and freq";
                sendDoc(io_, res);
                return;
            }
            if (!lc->setMode(mode)) {
                res["ok"] = false; res["err"] = "E_STATE";
                res["msg"] = (lc->getStatus() != STATUS_NORMAL)
                                 ? "channel faulted, send ch.reset first"
                                 : "mode not supported on this channel";
                sendDoc(io_, res);
                return;
            }
            if (mode == MODE_VOLTAGE_SOURCE) {
                lc->setOutputVoltage(doc["v"].as<float>());
            } else if (mode == MODE_CURRENT_SOURCE || mode == MODE_RESISTIVE_LOAD) {
                lc->setOutputCurrent(doc["i"].as<float>());
            } else if (mode == MODE_PWM_GENERATOR) {
                if (!lc->setPwm(doc["duty"].as<uint16_t>(), doc["freq"].as<uint32_t>())) {
                    lc->setMode(MODE_HIGH_IMPEDANCE);
                    res["ok"] = false; res["err"] = "E_ARG";
                    res["msg"] = "invalid duty/freq for LEDC";
                    sendDoc(io_, res);
                    return;
                }
            } else { // HZ: park the DAC at 0 V as well
                lc->setOutputVoltage(0.0f);
            }
            res["ok"] = true;
            sendDoc(io_, res);
            return;
        }

        if (strcmp(cmd, "ch.connect") == 0 || strcmp(cmd, "ch.disconnect") == 0) {
            const bool wantConnect = (cmd[3] == 'c');
            if (lc) {
                res["ok"] = false; res["err"] = "E_ARG";
                res["msg"] = "LVLP connection is managed via ch.set mode";
                sendDoc(io_, res);
                return;
            }
            if (wantConnect) {
                if (hc) hc->connect(); else vc->connect();
                const bool now = hc ? hc->isConnected() : vc->isConnected();
                if (!now) {
                    res["ok"] = false; res["err"] = "E_STATE";
                    res["msg"] = "channel faulted, send ch.reset first";
                    sendDoc(io_, res);
                    return;
                }
            } else {
                if (hc) hc->disconnect(); else vc->disconnect();
            }
            res["ok"] = true;
            sendDoc(io_, res);
            return;
        }

        if (strcmp(cmd, "ch.limits") == 0) {
            if (!doc["vmax"].is<float>()) {
                res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "missing vmax";
                sendDoc(io_, res);
                return;
            }
            float vmax = doc["vmax"].as<float>();
            if (vc) {
                vc->setLimits(vmax);
            } else {
                if (!doc["imax"].is<float>()) {
                    res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "missing imax";
                    sendDoc(io_, res);
                    return;
                }
                float imax = doc["imax"].as<float>();
                if (lc) lc->setLimits(vmax, imax); else hc->setLimits(vmax, imax);
            }
            res["ok"] = true;
            sendDoc(io_, res);
            return;
        }

        if (strcmp(cmd, "ch.reset") == 0) {
            if (lc) lc->resetStatus(); else if (hc) hc->resetStatus(); else vc->resetStatus();
            res["ok"] = true;
            sendDoc(io_, res);
            return;
        }
    }

    res["ok"] = false;
    res["err"] = "E_CMD";
    res["msg"] = "unknown cmd";
    sendDoc(io_, res);
}

// ============================================================================
// Telemetry
// ============================================================================

void TesterProtocol::emitTelemetryIfDue() {
    if (telemPeriodMs_ == 0) return;
    uint32_t now = millis();
    if (now - lastTelemMs_ < telemPeriodMs_) return;
    lastTelemMs_ = now;

    JsonDocument doc;
    doc["type"] = "telem";
    doc["t"] = now;
    doc["estop"] = estopLatched_;

    JsonArray la = doc["lvlp"].to<JsonArray>();
    for (uint8_t i = 0; i < nLvlp_; i++) {
        LVLPChannel& c = lvlp_[i];
        JsonObject o = la.add<JsonObject>();
        o["ch"] = c.getIndex();
        o["mode"] = lvlpModeStr(c.getMode());
        o["st"] = (uint8_t)c.getStatus();
        o["conn"] = c.isConnected();
        o["vt"] = round3(c.getTargetVoltage());
        o["it"] = round4(c.getTargetCurrent());
        o["v"] = round3(c.getLastVoltage());
        o["i"] = round4(c.getLastCurrent());
        if (c.getMode() == MODE_PWM_GENERATOR) {
            o["duty"] = c.getPwmDuty();
            o["freq"] = c.getPwmFreq();
        }
    }

    JsonArray ha = doc["hp"].to<JsonArray>();
    for (uint8_t i = 0; i < nHp_; i++) {
        HPCH& c = hp_[i];
        JsonObject o = ha.add<JsonObject>();
        o["ch"] = nLvlp_ + c.getIndex();
        o["st"] = (uint8_t)c.getStatus();
        o["conn"] = c.isConnected();
        o["vin"] = round3(c.getLastVIn());
        o["vout"] = round3(c.getLastVOut());
        o["i"] = round4(c.getLastCurrent());
    }

    JsonArray va = doc["hv"].to<JsonArray>();
    for (uint8_t i = 0; i < nHv_; i++) {
        HVChannel& c = hv_[i];
        JsonObject o = va.add<JsonObject>();
        o["ch"] = nLvlp_ + nHp_ + c.getIndex();
        o["st"] = (uint8_t)c.getStatus();
        o["conn"] = c.isConnected();
        o["v"] = round3(c.getLastVoltage());
    }

    sendDoc(io_, doc);
}

// ============================================================================
// Fault events
// ============================================================================

void TesterProtocol::pollFaultEvents() {
    for (uint8_t i = 0; i < nLvlp_; i++) {
        uint8_t st = (uint8_t)lvlp_[i].getStatus();
        if (st != prevLvlpStatus_[i] && st != (uint8_t)STATUS_NORMAL) {
            JsonDocument doc;
            doc["type"] = "fault";
            doc["ch"] = lvlp_[i].getIndex();
            doc["code"] = lvlpFaultStr(lvlp_[i].getStatus());
            doc["v"] = round3(lvlp_[i].getLastVoltage());
            doc["i"] = round4(lvlp_[i].getLastCurrent());
            sendDoc(io_, doc);
        }
        prevLvlpStatus_[i] = st;
    }
    for (uint8_t i = 0; i < nHp_; i++) {
        uint8_t st = (uint8_t)hp_[i].getStatus();
        if (st != prevHpStatus_[i] && st != (uint8_t)HPCH_STATUS_NORMAL) {
            JsonDocument doc;
            doc["type"] = "fault";
            doc["ch"] = nLvlp_ + hp_[i].getIndex();
            doc["code"] = hpFaultStr(hp_[i].getStatus());
            doc["vin"] = round3(hp_[i].getLastVIn());
            doc["vout"] = round3(hp_[i].getLastVOut());
            doc["i"] = round4(hp_[i].getLastCurrent());
            sendDoc(io_, doc);
        }
        prevHpStatus_[i] = st;
    }
    for (uint8_t i = 0; i < nHv_; i++) {
        uint8_t st = (uint8_t)hv_[i].getStatus();
        if (st != prevHvStatus_[i] && st != (uint8_t)HV_STATUS_NORMAL) {
            JsonDocument doc;
            doc["type"] = "fault";
            doc["ch"] = nLvlp_ + nHp_ + hv_[i].getIndex();
            doc["code"] = hvFaultStr(hv_[i].getStatus());
            doc["v"] = round3(hv_[i].getLastVoltage());
            sendDoc(io_, doc);
        }
        prevHvStatus_[i] = st;
    }
}

// ============================================================================
// E-stop
// ============================================================================

void TesterProtocol::triggerEstop(const char* source) {
    // MODE_HIGH_IMPEDANCE is always accepted by setMode(), even when faulted.
    for (uint8_t i = 0; i < nLvlp_; i++) {
        lvlp_[i].setMode(MODE_HIGH_IMPEDANCE);
        lvlp_[i].setOutputVoltage(0.0f);
    }
    for (uint8_t i = 0; i < nHp_; i++) hp_[i].disconnect();
    for (uint8_t i = 0; i < nHv_; i++) hv_[i].disconnect();
    estopLatched_ = true;

    JsonDocument doc;
    doc["type"] = "estop";
    doc["src"] = source;
    sendDoc(io_, doc);
}
