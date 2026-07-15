// Testbench protocol commands (tb.*) and the JSON <-> TestCase codec.
//
// Campaigns are loaded one test per line to bound the RX buffer:
//   tb.clear -> N x tb.add -> tb.run
// tb.run acks "started" BEFORE the (blocking) run: the outer JsonDocument
// points into the shared RX buffer that the runner's comms hook will reuse,
// so nothing may touch it after runAll() begins. Progress/results stream as
// tb_progress / tb_result events, finished by one tb_done.

#include "Protocol.h"

// ---------------------------------------------------------------------------
// Type name table
// ---------------------------------------------------------------------------

struct TbTypeMap { const char* s; TestType t; };
static const TbTypeMap kTbTypes[] = {
    {"voltage_threshold",  TEST_VOLTAGE_THRESHOLD},
    {"voltage_accuracy",   TEST_VOLTAGE_ACCURACY},
    {"voltage_ripple",     TEST_VOLTAGE_RIPPLE},
    {"current_consumption",TEST_CURRENT_CONSUMPTION},
    {"current_inrush",     TEST_CURRENT_INRUSH},
    {"power_sequence",     TEST_POWER_SEQUENCE},
    {"pwm_integrity",      TEST_PWM_INTEGRITY},
    {"short_circuit",      TEST_SHORT_CIRCUIT_PROTECTION},
    {"load_regulation",    TEST_LOAD_REGULATION},
    {"cross_isolation",    TEST_CROSS_CHANNEL_ISOLATION},
    {"static_voltage",     TEST_STATIC_VOLTAGE},
};

const char* tbTypeStr(TestType t) {
    for (const auto& m : kTbTypes) if (m.t == t) return m.s;
    return "?";
}

static const char* tbOutcomeStr(TestOutcome o) {
    switch (o) {
        case OUTCOME_PASS:    return "PASS";
        case OUTCOME_FAIL:    return "FAIL";
        case OUTCOME_TIMEOUT: return "TIMEOUT";
        case OUTCOME_SKIP:    return "SKIP";
        default:              return "ERROR";
    }
}

// ---------------------------------------------------------------------------
// JSON -> TestCase
// ---------------------------------------------------------------------------

static bool decodeSetupSteps(JsonArrayConst arr, TestCase& out,
                             char* errBuf, size_t errLen) {
    uint8_t n = 0;
    for (JsonVariantConst v : arr) {
        if (n >= DUT_MAX_SETUP_STEPS) {
            snprintf(errBuf, errLen, "too many setup steps (max %u)",
                     DUT_MAX_SETUP_STEPS);
            return false;
        }
        JsonObjectConst s = v.as<JsonObjectConst>();
        const char* kind = s["step"] | (const char*)nullptr;
        SetupStep& st = out.setup[n];
        st = SetupStep{};
        if (!kind) {
            snprintf(errBuf, errLen, "setup step %u: missing \"step\"", n);
            return false;
        }
        if (strcmp(kind, "vs") == 0) {
            st.kind = STEP_VS; st.ch = s["ch"] | 0; st.value = s["v"] | 0.0f;
        } else if (strcmp(kind, "cs") == 0) {
            st.kind = STEP_CS; st.ch = s["ch"] | 0; st.value = s["i"] | 0.0f;
        } else if (strcmp(kind, "hz") == 0) {
            st.kind = STEP_HZ; st.ch = s["ch"] | 0;
        } else if (strcmp(kind, "pwm") == 0) {
            st.kind = STEP_PWM; st.ch = s["ch"] | 0;
            st.duty = s["duty"] | 0; st.ms = s["freq"] | 1000;
            st.value = s["v"] | 0.0f;
        } else if (strcmp(kind, "wait") == 0) {
            st.kind = STEP_WAIT; st.ms = s["ms"] | 0;
        } else if (strcmp(kind, "sr") == 0) {
            st.kind = STEP_SR_BIT; st.ch = s["bit"] | 0;
            st.value = (s["on"] | false) ? 1.0f : 0.0f;
        } else {
            snprintf(errBuf, errLen, "setup step %u: unknown kind \"%s\"", n, kind);
            return false;
        }
        n++;
    }
    out.setupCount = n;
    return true;
}

bool TesterProtocol::decodeTestCase(JsonObjectConst t, TestCase& out,
                                    char* errBuf, size_t errLen) {
    out = TestCase{};

    const char* name = t["name"] | (const char*)nullptr;
    if (!name || !name[0]) {
        snprintf(errBuf, errLen, "missing test name");
        return false;
    }
    strlcpy(out.nameBuf, name, sizeof(out.nameBuf));
    out.name = out.nameBuf;

    const char* type = t["type"] | (const char*)nullptr;
    bool found = false;
    for (const auto& m : kTbTypes) {
        if (type && strcmp(type, m.s) == 0) { out.type = m.t; found = true; break; }
    }
    if (!found) {
        snprintf(errBuf, errLen, "unknown test type \"%s\"", type ? type : "");
        return false;
    }

    if (t["setup"].is<JsonArrayConst>()) {
        if (!decodeSetupSteps(t["setup"].as<JsonArrayConst>(), out, errBuf, errLen))
            return false;
    }

    JsonObjectConst p = t["params"].as<JsonObjectConst>();
    switch (out.type) {
        case TEST_VOLTAGE_THRESHOLD: {
            auto& d = out.voltageThreshold;
            d.driveChannelMask = p["drive_mask"] | 0;
            d.driveVoltage     = p["drive_v"] | 0.0f;
            d.senseChannelMask = p["sense_mask"] | 0;
            d.thresholdVoltage = p["threshold_v"] | 0.0f;
            d.timeoutMs        = p["timeout_ms"] | 1000;
            break;
        }
        case TEST_VOLTAGE_ACCURACY: {
            auto& d = out.voltageAccuracy;
            d.channelMask    = p["mask"] | 0;
            d.targetVoltage  = p["target_v"] | 0.0f;
            d.toleranceVolts = p["tolerance_v"] | 0.05f;
            d.settleMs       = p["settle_ms"] | 100;
            d.numSamples     = p["samples"] | 8;
            break;
        }
        case TEST_VOLTAGE_RIPPLE: {
            auto& d = out.voltageRipple;
            d.channelMask    = p["mask"] | 0;
            d.driveVoltage   = p["drive_v"] | 0.0f;
            d.windowMs       = p["window_ms"] | 200;
            d.numSamples     = p["samples"] | 32;
            d.maxRippleVolts = p["max_ripple_v"] | 0.1f;
            break;
        }
        case TEST_CURRENT_CONSUMPTION: {
            auto& d = out.currentConsumption;
            d.channelMask  = p["mask"] | 0;
            d.driveVoltage = p["drive_v"] | 0.0f;
            d.minCurrentA  = p["min_a"] | 0.0f;
            d.maxCurrentA  = p["max_a"] | 0.5f;
            d.settleMs     = p["settle_ms"] | 100;
            d.numSamples   = p["samples"] | 8;
            break;
        }
        case TEST_CURRENT_INRUSH: {
            auto& d = out.currentInrush;
            d.channelMask      = p["mask"] | 0;
            d.driveVoltage     = p["drive_v"] | 0.0f;
            d.windowMs         = p["window_ms"] | 500;
            d.sampleIntervalMs = p["interval_ms"] | 2;
            d.maxInrushA       = p["max_a"] | 0.5f;
            d.settleThresholdA = p["settle_band_a"] | 0.01f;
            break;
        }
        case TEST_POWER_SEQUENCE: {
            auto& d = out.powerSequence;
            JsonArrayConst steps = p["steps"].as<JsonArrayConst>();
            uint8_t n = 0;
            for (JsonVariantConst sv : steps) {
                if (n >= 8) break;
                JsonObjectConst so = sv.as<JsonObjectConst>();
                d.stepChannelMask[n]   = so["mask"] | 0;
                d.stepTargetVoltage[n] = so["target_v"] | 0.0f;
                d.stepDelayMs[n]       = so["delay_ms"] | 100;
                n++;
            }
            d.stepCount = n;
            d.stepToleranceVolts = p["tolerance_v"] | 0.2f;
            if (n == 0) {
                snprintf(errBuf, errLen, "power_sequence needs params.steps[]");
                return false;
            }
            break;
        }
        case TEST_PWM_INTEGRITY: {
            auto& d = out.pwmIntegrity;
            d.driveChannelMask = p["drive_mask"] | 0;
            d.senseChannelMask = p["sense_mask"] | 0;
            d.driveVoltage     = p["drive_v"] | 3.3f;
            d.dutyCycle        = p["duty"] | 128;
            d.frequency        = p["freq"] | 1000;
            d.settleMs         = p["settle_ms"] | 200;
            d.toleranceVolts   = p["tolerance_v"] | 0.2f;
            break;
        }
        case TEST_SHORT_CIRCUIT_PROTECTION: {
            auto& d = out.shortCircuit;
            d.channelMask       = p["mask"] | 0;
            d.driveVoltage      = p["drive_v"] | 0.0f;
            d.shortCurrentLimitA= p["short_limit_a"] | 0.05f;
            d.timeoutMs         = p["timeout_ms"] | 1000;
            break;
        }
        case TEST_LOAD_REGULATION: {
            auto& d = out.loadRegulation;
            d.driveChannelMask = p["drive_mask"] | 0;
            d.loadChannelMask  = p["load_mask"] | 0;
            d.driveVoltage     = p["drive_v"] | 0.0f;
            d.loadCurrentA     = p["load_a"] | 0.0f;
            d.settleMs         = p["settle_ms"] | 200;
            d.maxDropVolts     = p["max_drop_v"] | 0.2f;
            break;
        }
        case TEST_CROSS_CHANNEL_ISOLATION: {
            auto& d = out.crossChannelIsolation;
            d.driveChannelMask = p["drive_mask"] | 0;
            d.senseChannelMask = p["sense_mask"] | 0;
            d.driveVoltage     = p["drive_v"] | 0.0f;
            d.settleMs         = p["settle_ms"] | 100;
            d.maxCouplingVolts = p["max_coupling_v"] | 0.2f;
            break;
        }
        case TEST_STATIC_VOLTAGE: {
            auto& d = out.staticVoltage;
            d.senseChannelMask = p["sense_mask"] | 0;
            d.expectedVoltage  = p["expected_v"] | 0.0f;
            d.toleranceVolts   = p["tolerance_v"] | 0.2f;
            d.settleMs         = p["settle_ms"] | 100;
            break;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static void tbSendDoc(Stream* io, JsonDocument& doc) {
    if (!io) return;
    serializeJson(doc, *io);
    io->print('\n');
}

void TesterProtocol::emitTbProgress(uint8_t idx, uint8_t total,
                                    const char* name, uint32_t elapsedMs) {
    JsonDocument doc;
    doc["type"] = "tb_progress";
    doc["test"] = idx;
    doc["of"] = total;
    doc["name"] = name;
    doc["elapsed_ms"] = elapsedMs;
    tbSendDoc(io_, doc);
}

void TesterProtocol::emitTbResult(uint8_t idx, const TestResult& r) {
    JsonDocument doc;
    doc["type"] = "tb_result";
    doc["test"] = idx;
    doc["name"] = r.testName;
    doc["outcome"] = tbOutcomeStr(r.outcome);
    doc["measured"] = r.measuredValue;
    doc["expected"] = r.expectedValue;
    doc["ms"] = r.elapsedMs;
    doc["detail"] = r.details;
    tbSendDoc(io_, doc);
}

// ---------------------------------------------------------------------------
// Command handler
// ---------------------------------------------------------------------------

void TesterProtocol::handleTb(long id, const char* cmd, JsonDocument& doc) {
    JsonDocument res;
    res["type"] = "ack";
    if (id >= 0) res["id"] = id;

    if (!runner_) {
        res["ok"] = false; res["err"] = "E_STATE"; res["msg"] = "no test runner";
        tbSendDoc(io_, res);
        return;
    }

    if (strcmp(cmd, "tb.status") == 0) {
        res["ok"] = true;
        res["running"] = runner_->isRunning();
        if (runner_->isRunning()) {
            res["test"] = runner_->currentIndex();
            res["of"] = runner_->getTestCount();
            res["name"] = runner_->currentName();
            res["elapsed_ms"] = runner_->currentElapsedMs();
        }
        tbSendDoc(io_, res);
        return;
    }

    if (strcmp(cmd, "tb.abort") == 0) {
        res["ok"] = true;
        res["running"] = runner_->isRunning();
        if (runner_->isRunning()) runner_->requestAbort();
        tbSendDoc(io_, res);
        return;
    }

    if (strcmp(cmd, "tb.clear") == 0) {
        runner_->clearTests();
        res["ok"] = true;
        tbSendDoc(io_, res);
        return;
    }

    if (strcmp(cmd, "tb.add") == 0) {
        if (!doc["test"].is<JsonObjectConst>()) {
            res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = "missing test object";
            tbSendDoc(io_, res);
            return;
        }
        static TestCase tc; // ~300 B: keep off the stack
        char err[64];
        if (!decodeTestCase(doc["test"].as<JsonObjectConst>(), tc, err, sizeof(err))) {
            res["ok"] = false; res["err"] = "E_ARG"; res["msg"] = err;
            tbSendDoc(io_, res);
            return;
        }
        if (!runner_->addTest(tc)) {
            res["ok"] = false; res["err"] = "E_STATE"; res["msg"] = "test queue full";
            tbSendDoc(io_, res);
            return;
        }
        res["ok"] = true;
        res["count"] = runner_->getTestCount();
        tbSendDoc(io_, res);
        return;
    }

    if (strcmp(cmd, "tb.list") == 0) {
        res["ok"] = true;
        JsonArray arr = res["tests"].to<JsonArray>();
        for (uint8_t i = 0; i < runner_->getTestCount(); i++) {
            JsonObject o = arr.add<JsonObject>();
            o["name"] = runner_->getTest(i).name;
            o["type"] = tbTypeStr(runner_->getTest(i).type);
        }
        tbSendDoc(io_, res);
        return;
    }

    if (strcmp(cmd, "tb.run") == 0) {
        if (runner_->getTestCount() == 0) {
            res["ok"] = false; res["err"] = "E_STATE"; res["msg"] = "no tests loaded";
            tbSendDoc(io_, res);
            return;
        }
        // Ack BEFORE the blocking run; do not touch `doc` after this point
        // (the comms hook reuses the RX buffer it points into).
        res["ok"] = true;
        res["started"] = true;
        res["tests"] = runner_->getTestCount();
        tbSendDoc(io_, res);

        busy_ = true;
        runner_->runAll();
        busy_ = false;

        uint8_t pass = 0, fail = 0;
        for (uint8_t i = 0; i < runner_->getTestCount(); i++) {
            if (runner_->getResult(i).outcome == OUTCOME_PASS) pass++;
            else if (runner_->getResult(i).outcome != OUTCOME_SKIP) fail++;
        }
        JsonDocument done;
        done["type"] = "tb_done";
        done["total"] = runner_->getTestCount();
        done["pass"] = pass;
        done["fail"] = fail;
        done["aborted"] = runner_->wasAborted();
        tbSendDoc(io_, done);
        return;
    }

    res["ok"] = false;
    res["err"] = "E_CMD";
    res["msg"] = "unknown cmd";
    tbSendDoc(io_, res);
}