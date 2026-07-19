#include "Protocol.h"
#include "CalibrationStore.h"

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

TesterProtocol* TesterProtocol::instance_ = nullptr;

void TesterProtocol::setTestRunner(DUTTestRunner* runner) {
    runner_ = runner;
    instance_ = this;
    if (runner_) {
        runner_->setCommsHook(&TesterProtocol::tbCommsHookThunk);
        runner_->setProgressCallback(&TesterProtocol::tbProgressThunk);
        runner_->setResultCallback(&TesterProtocol::tbResultThunk);
    }
}

void TesterProtocol::tbCommsHookThunk() {
    if (instance_) instance_->service();
}

void TesterProtocol::tbProgressThunk(uint8_t idx, uint8_t total,
                                     const char* name, uint32_t elapsedMs) {
    if (instance_) instance_->emitTbProgress(idx, total, name, elapsedMs);
}

void TesterProtocol::tbResultThunk(uint8_t idx, const TestResult& r) {
    if (instance_) instance_->emitTbResult(idx, r);
}

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

    // While a testbench campaign runs (handleLine re-entered via the runner's
    // comms hook) only safety/status commands are served.
    if (busy_
        && strcmp(cmd, "hello") != 0 && strcmp(cmd, "estop") != 0
        && strcmp(cmd, "tb.abort") != 0 && strcmp(cmd, "tb.status") != 0) {
        res["ok"] = false;
        res["err"] = "E_BUSY";
        res["msg"] = "testbench running";
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
        res["oc_debounce_ms"] = LVLPChannel::getOverCurrentDebounceMs();
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

    // ---- estop.clear (channels still need individual ch.reset) ----------------
    if (strcmp(cmd, "estop.clear") == 0) {
        clearEstopLatch();
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

    // ---- oc.debounce: over-current debounce window (ms), all LVLP channels ----
    if (strcmp(cmd, "oc.debounce") == 0) {
        if (!doc["ms"].is<int>()) {
            res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "missing ms";
            sendDoc(io_, res);
            return;
        }
        int ms = doc["ms"];
        if (ms < 0 || ms > 5000) {
            res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "ms must be 0-5000";
            sendDoc(io_, res);
            return;
        }
        LVLPChannel::setOverCurrentDebounceMs((uint32_t)ms);
        res["ok"] = true;
        res["oc_debounce_ms"] = LVLPChannel::getOverCurrentDebounceMs();
        sendDoc(io_, res);
        return;
    }

    // ---- tb.* (testbench) ------------------------------------------------------
    if (strncmp(cmd, "tb.", 3) == 0) {
        handleTb(id, cmd, doc);
        return;
    }

    // ---- cal.* ----------------------------------------------------------------
    if (strncmp(cmd, "cal.", 4) == 0) {
        if (strcmp(cmd, "cal.save") == 0) { handleCalSave(id); return; }
        if (strcmp(cmd, "cal.load") == 0) { handleCalLoad(id); return; }
        if (strcmp(cmd, "cal.get") == 0 || strcmp(cmd, "cal.set") == 0) {
            int ch = doc["ch"] | 0;
            if (ch < 1 || ch > nLvlp_ + nHp_ + nHv_) {
                res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "ch out of range";
                sendDoc(io_, res);
                return;
            }
            if (cmd[4] == 'g') {
                handleCalGet(id, (uint8_t)ch);
            } else {
                if (!doc["cal"].is<JsonObjectConst>()) {
                    res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "missing cal object";
                    sendDoc(io_, res);
                    return;
                }
                handleCalSet(id, (uint8_t)ch, doc["cal"].as<JsonObjectConst>());
            }
            return;
        }
        res["ok"] = false; res["err"] = "E_CMD"; res["msg"] = "unknown cmd";
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
            if (mode == MODE_PWM_GENERATOR) {
                if (!doc["duty"].is<int>() || !doc["freq"].is<int>()) {
                    res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "PWM needs duty and freq";
                    sendDoc(io_, res);
                    return;
                }
                // Optional "res" (LEDC bits) and "v" (high-level amplitude);
                // validate duty against the effective resolution up front.
                int pwmRes = doc["res"] | (int)lc->getPwmResolution();
                if (pwmRes < 1 || pwmRes > 14) {
                    res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "res must be 1-14 bits";
                    sendDoc(io_, res);
                    return;
                }
                uint32_t maxDuty = (1UL << pwmRes) - 1;
                if (doc["duty"].as<uint32_t>() > maxDuty) {
                    res["ok"] = false; res["err"] = "E_ARG";
                    res["msg"] = "duty exceeds resolution max";
                    sendDoc(io_, res);
                    return;
                }
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
                if (doc["res"].is<int>()) {
                    lc->setPwmResolution((uint8_t)doc["res"].as<int>());
                }
                if (doc["v"].is<float>()) {
                    // The DAC/op-amp chain sets the PWM high level
                    lc->setOutputVoltage(doc["v"].as<float>());
                }
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

        if (strcmp(cmd, "ch.capture") == 0) {
            int n = doc["n"] | 128;
            int dt = doc["dt_ms"] | 2;
            if (n < 2 || n > (int)CAPTURE_MAX_SAMPLES || dt < 1 || dt > 100
                || (long)n * dt > 5000) {
                res["ok"] = false; res["err"] = "E_ARG";
                res["msg"] = "need n 2-512, dt_ms 1-100, n*dt <= 5000 ms";
                sendDoc(io_, res);
                return;
            }
            handleCapture(id, (uint8_t)ch, (uint16_t)n, (uint16_t)dt);
            return;
        }

        if (strcmp(cmd, "ch.settle") == 0) {
            if (!lc) {
                res["ok"] = false; res["err"] = "E_ARG";
                res["msg"] = "ch.settle only valid for LVLP channels (1-8)";
                sendDoc(io_, res);
                return;
            }
            if (!doc["to"].is<float>()) {
                res["ok"] = false; res["err"] = "E_ARG";
                res["msg"] = "need to (target volts)";
                sendDoc(io_, res);
                return;
            }
            if (lc->getStatus() != STATUS_NORMAL) {
                res["ok"] = false; res["err"] = "E_STATE";
                res["msg"] = "channel faulted, send ch.reset first";
                sendDoc(io_, res);
                return;
            }
            float fromV   = doc["from"] | 0.0f;
            float toV     = doc["to"].as<float>();
            int   n       = doc["n"] | 300;
            long  dtUs    = doc["dt_us"] | 333;
            int   settleMs = doc["settle_ms"] | 500;
            if (n < 2 || n > (int)CAPTURE_MAX_SAMPLES || dtUs < 50 || dtUs > 100000
                || (long)n * dtUs > 2000000L || settleMs < 0 || settleMs > 2000) {
                res["ok"] = false; res["err"] = "E_ARG";
                res["msg"] = "need n 2-512, dt_us 50-100000, "
                             "n*dt_us <= 2e6 us, settle_ms 0-2000";
                sendDoc(io_, res);
                return;
            }
            handleSettle(id, (uint8_t)ch, fromV, toV,
                         (uint16_t)n, (uint32_t)dtUs, (uint16_t)settleMs);
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
            o["res"] = c.getPwmResolution();
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
// Burst capture (blocking; telemetry pauses for n*dt ms)
// ============================================================================

void TesterProtocol::handleCapture(long id, uint8_t ch, uint16_t n, uint16_t dtMs) {
    static float buf[CAPTURE_MAX_SAMPLES];

    // Ack immediately — the capture itself can take up to 5 s and the client
    // waits for the "capture" event, not the ack.
    {
        JsonDocument res;
        res["type"] = "ack";
        if (id >= 0) res["id"] = id;
        res["ok"] = true;
        res["n"] = n;
        res["dt_ms"] = dtMs;
        sendDoc(io_, res);
    }

    const bool isLvlp = ch <= nLvlp_;
    const bool isHp = !isLvlp && ch <= nLvlp_ + nHp_;

    uint32_t next = millis();
    for (uint16_t k = 0; k < n; k++) {
        if (isLvlp)      buf[k] = lvlp_[ch - 1].readVoltage();
        else if (isHp)   buf[k] = hp_[ch - 1 - nLvlp_].readVOut();
        else             buf[k] = hv_[ch - 1 - nLvlp_ - nHp_].readVoltage();
        next += dtMs;
        while ((int32_t)(millis() - next) < 0) delayMicroseconds(100);
    }

    JsonDocument doc;
    doc["type"] = "capture";
    doc["kind"] = "scope";
    doc["ch"] = ch;
    doc["dt_ms"] = dtMs;
    doc["unit"] = "V";
    JsonArray arr = doc["samples"].to<JsonArray>();
    for (uint16_t k = 0; k < n; k++) arr.add(round3(buf[k]));
    sendDoc(io_, doc);
}

// ----------------------------------------------------------------------------
// ch.settle — LVLP output step-response capture.
//
// Drives the channel to `fromV`, waits `settleMs`, then steps to `toV` and
// samples the output voltage at `dtUs` spacing (busy-waited for even timing).
// The result lets the PC reconstruct the apparent current
// (vcmd - vout)/rshunt over time and size the over-current debounce window.
// The channel is parked in high impedance afterwards.
// ----------------------------------------------------------------------------
void TesterProtocol::handleSettle(long id, uint8_t ch, float fromV, float toV,
                                  uint16_t n, uint32_t dtUs, uint16_t settleMs) {
    static float buf[CAPTURE_MAX_SAMPLES];
    LVLPChannel& c = lvlp_[ch - 1];

    // Ack immediately — the settle delay + capture window can take up to ~2.5 s
    // and the client waits for the "capture" event, not the ack.
    {
        JsonDocument res;
        res["type"] = "ack";
        if (id >= 0) res["id"] = id;
        res["ok"] = true;
        res["n"] = n;
        res["dt_us"] = dtUs;
        sendDoc(io_, res);
    }

    // Establish the starting point and let it fully settle.
    c.setMode(MODE_VOLTAGE_SOURCE);
    c.setOutputVoltage(fromV);
    delay(settleMs);

    // t0 is captured BEFORE the DAC write, matching the live firmware where the
    // command returns and update() samples some time later. vCmd is the
    // feed-forward voltage the current calc jumps to the instant of the step.
    uint32_t t0 = micros();
    c.setOutputVoltage(toV);
    float vCmd = c.calculateExpectedOutputVoltage(c.dacValueAttribute);

    uint32_t nextT = 0;
    for (uint16_t k = 0; k < n; k++) {
        while ((uint32_t)(micros() - t0) < nextT) {
            // busy-wait for even spacing
        }
        buf[k] = c.readVoltage();
        nextT += dtUs;
    }
    uint32_t elapsedUs = micros() - t0;

    // Park the channel safely before streaming the (large) result back.
    c.setMode(MODE_HIGH_IMPEDANCE);
    c.setOutputVoltage(0.0f);

    JsonDocument doc;
    doc["type"] = "capture";
    doc["kind"] = "settling";
    doc["ch"] = ch;
    doc["dt_us"] = dtUs;
    doc["t_total_us"] = elapsedUs;
    doc["from"] = round3(fromV);
    doc["to"] = round3(toV);
    doc["vcmd"] = round3(vCmd);
    doc["rshunt"] = round3(c.getShuntResistance());
    doc["imax"] = round4(c.getMaxCurrentLimit());
    doc["unit"] = "V";
    JsonArray arr = doc["samples"].to<JsonArray>();
    for (uint16_t k = 0; k < n; k++) arr.add(round4(buf[k]));
    sendDoc(io_, doc);
}

// ============================================================================
// Calibration commands
// ============================================================================

void TesterProtocol::handleCalGet(long id, uint8_t ch) {
    JsonDocument res;
    res["type"] = "ack";
    if (id >= 0) res["id"] = id;
    res["ok"] = true;
    res["ch"] = ch;
    JsonObject cal = res["cal"].to<JsonObject>();

    if (ch <= nLvlp_) {
        const ChannelCalibrationData& d = lvlp_[ch - 1].getCalibrationData();
        cal["K1"] = d.K1;       cal["K2"] = d.K2;    cal["offset"] = d.offset;
        cal["mADC"] = d.mADC;   cal["bADC"] = d.bADC;
        cal["mDAC"] = d.mDAC;   cal["bDAC"] = d.bDAC;
    } else if (ch <= nLvlp_ + nHp_) {
        HPCH& c = hp_[ch - 1 - nLvlp_];
        const HPCHCalibrationData& d = c.getCalibrationData();
        cal["mADC_VIn"] = d.mADC_VIn;   cal["bADC_VIn"] = d.bADC_VIn;
        cal["mADC_VOut"] = d.mADC_VOut; cal["bADC_VOut"] = d.bADC_VOut;
        cal["sens"] = d.acs725_sensitivity;
        cal["vref"] = d.adc_vref;
        cal["zero_adc"] = c.getACS725ZeroADC();
    } else {
        const HVChannelCalibrationData& d =
            hv_[ch - 1 - nLvlp_ - nHp_].getCalibrationData();
        cal["deadzone"] = d.deadZoneRaw;
        cal["vref"] = d.adc_vref;
        JsonArray pts = cal["points"].to<JsonArray>();
        for (uint8_t k = 0; k < d.numPoints; k++) {
            JsonArray p = pts.add<JsonArray>();
            p.add(d.points[k].raw);
            p.add(d.points[k].voltage);
        }
    }
    sendDoc(io_, res);
}

void TesterProtocol::handleCalSet(long id, uint8_t ch, JsonObjectConst cal) {
    // Merge semantics: fields absent from the request keep their current value.
    if (ch <= nLvlp_) {
        LVLPChannel& c = lvlp_[ch - 1];
        ChannelCalibrationData d = c.getCalibrationData();
        d.K1 = cal["K1"] | d.K1;         d.K2 = cal["K2"] | d.K2;
        d.offset = cal["offset"] | d.offset;
        d.mADC = cal["mADC"] | d.mADC;   d.bADC = cal["bADC"] | d.bADC;
        d.mDAC = cal["mDAC"] | d.mDAC;   d.bDAC = cal["bDAC"] | d.bDAC;
        c.setCalibrationData(d);
    } else if (ch <= nLvlp_ + nHp_) {
        HPCH& c = hp_[ch - 1 - nLvlp_];
        HPCHCalibrationData d = c.getCalibrationData();
        d.mADC_VIn = cal["mADC_VIn"] | d.mADC_VIn;
        d.bADC_VIn = cal["bADC_VIn"] | d.bADC_VIn;
        d.mADC_VOut = cal["mADC_VOut"] | d.mADC_VOut;
        d.bADC_VOut = cal["bADC_VOut"] | d.bADC_VOut;
        d.acs725_sensitivity = cal["sens"] | d.acs725_sensitivity;
        d.adc_vref = cal["vref"] | d.adc_vref;
        c.setCalibrationData(d);
        if (cal["zero_adc"].is<int>()) c.setACS725ZeroADC(cal["zero_adc"].as<uint16_t>());
    } else {
        HVChannel& c = hv_[ch - 1 - nLvlp_ - nHp_];
        HVChannelCalibrationData d = c.getCalibrationData();
        d.deadZoneRaw = cal["deadzone"] | d.deadZoneRaw;
        d.adc_vref = cal["vref"] | d.adc_vref;
        if (cal["points"].is<JsonArrayConst>()) {
            uint8_t np = 0;
            for (JsonVariantConst pv : cal["points"].as<JsonArrayConst>()) {
                if (np >= HV_CAL_MAX_POINTS) break;
                JsonArrayConst pair = pv.as<JsonArrayConst>();
                if (pair.size() < 2) continue;
                d.points[np].raw = pair[0].as<uint16_t>();
                d.points[np].voltage = pair[1].as<float>();
                np++;
            }
            for (uint8_t k = np; k < HV_CAL_MAX_POINTS; k++) d.points[k] = {0, 0.0f};
            d.numPoints = np;
            // PWL lookup requires ascending raw order
            for (uint8_t a = 1; a < np; a++) {
                HVCalPoint key = d.points[a];
                int8_t b = a - 1;
                while (b >= 0 && d.points[b].raw > key.raw) {
                    d.points[b + 1] = d.points[b];
                    b--;
                }
                d.points[b + 1] = key;
            }
        }
        c.setCalibrationData(d);
    }

    JsonDocument res;
    res["type"] = "ack";
    if (id >= 0) res["id"] = id;
    res["ok"] = true;
    sendDoc(io_, res);
}

void TesterProtocol::handleCalSave(long id) {
    JsonDocument res;
    res["type"] = "ack";
    if (id >= 0) res["id"] = id;
    if (!calStore_) {
        res["ok"] = false; res["err"] = "E_STATE"; res["msg"] = "no calibration storage";
        sendDoc(io_, res);
        return;
    }
    bool ok = true;
    for (uint8_t i = 0; i < nLvlp_; i++)
        ok &= calStore_->saveLVLP(i, lvlp_[i].getCalibrationData());
    for (uint8_t i = 0; i < nHp_; i++)
        ok &= calStore_->saveHP(i, hp_[i].getCalibrationData(),
                                hp_[i].getACS725ZeroADC());
    for (uint8_t i = 0; i < nHv_; i++)
        ok &= calStore_->saveHV(i, hv_[i].getCalibrationData());
    res["ok"] = ok;
    if (!ok) { res["err"] = "E_STATE"; res["msg"] = "NVS write failed"; }
    sendDoc(io_, res);
}

void TesterProtocol::handleCalLoad(long id) {
    JsonDocument res;
    res["type"] = "ack";
    if (id >= 0) res["id"] = id;
    if (!calStore_) {
        res["ok"] = false; res["err"] = "E_STATE"; res["msg"] = "no calibration storage";
        sendDoc(io_, res);
        return;
    }
    uint8_t loaded = 0;
    ChannelCalibrationData ld;
    for (uint8_t i = 0; i < nLvlp_; i++)
        if (calStore_->loadLVLP(i, ld)) { lvlp_[i].setCalibrationData(ld); loaded++; }
    HPCHCalibrationData hd;
    uint16_t zero;
    for (uint8_t i = 0; i < nHp_; i++)
        if (calStore_->loadHP(i, hd, zero)) {
            hp_[i].setCalibrationData(hd);
            hp_[i].setACS725ZeroADC(zero);
            loaded++;
        }
    HVChannelCalibrationData vd;
    for (uint8_t i = 0; i < nHv_; i++)
        if (calStore_->loadHV(i, vd)) { hv_[i].setCalibrationData(vd); loaded++; }
    res["ok"] = true;
    res["loaded"] = loaded;
    sendDoc(io_, res);
}

// ============================================================================
// E-stop
// ============================================================================

void TesterProtocol::triggerEstop(const char* source) {
    if (runner_) runner_->requestAbort(); // stop any running campaign first
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
