#ifndef TESTER_PROTOCOL_H
#define TESTER_PROTOCOL_H

#include <Arduino.h>
#include "LVLPChannel.h"
#include "HPCH.h"
#include "HVChannel.h"

// ============================================================================
// TesterProtocol — NDJSON serial protocol for the PC GUI
//
// Transport: newline-delimited JSON over USB-CDC.
//
// PC → ESP32 commands (each carries a client-chosen "id"):
//   {"id":1,"cmd":"hello"}
//   {"id":2,"cmd":"ch.set","ch":1,"mode":"VS","v":3.14}
//   {"id":3,"cmd":"ch.set","ch":1,"mode":"CS","i":0.05}
//   {"id":4,"cmd":"ch.set","ch":1,"mode":"PWM","duty":128,"freq":1000}
//   {"id":5,"cmd":"ch.set","ch":1,"mode":"HZ"}
//   {"id":6,"cmd":"ch.connect","ch":9}        // HP/HV relays only (ch 9-11)
//   {"id":7,"cmd":"ch.disconnect","ch":9}
//   {"id":8,"cmd":"ch.limits","ch":1,"vmax":5.0,"imax":0.3}
//   {"id":9,"cmd":"ch.reset","ch":1}          // clear latched fault
//   {"id":10,"cmd":"telem.rate","hz":10}      // 0 = telemetry off
//   {"id":11,"cmd":"estop"}
//   {"id":12,"cmd":"estop.clear"}             // dismiss the latch indicator
//
// ESP32 → PC: every command is answered exactly once with
//   {"type":"ack","id":N,"ok":true, ...}   or
//   {"type":"ack","id":N,"ok":false,"err":"E_ARG","msg":"..."}
// plus unsolicited events (no id): "telem", "fault", "estop", "log".
//
// Channel numbering: 1-8 = LVLP, 9-10 = HPCH, 11 = HV.
// ============================================================================

class TesterProtocol {
public:
    static constexpr const char* FW_VERSION = "0.1.0";
    static constexpr uint8_t PROTO_VERSION = 1;

    void begin(LVLPChannel* lvlp, uint8_t lvlpCount,
               HPCH* hp, uint8_t hpCount,
               HVChannel* hv, uint8_t hvCount,
               Stream& io);

    /// Drain the serial RX buffer; dispatch any complete command lines.
    void service();

    /// Emit a telemetry frame if the configured period has elapsed.
    /// Reads only values cached by the channels' update() — no bus traffic.
    void emitTelemetryIfDue();

    /// Compare channel statuses against the previous poll and emit one
    /// "fault" event per channel that has newly entered a fault state.
    /// Call after the channels' update() pass.
    void pollFaultEvents();

    /// Open all relays and drive every LVLP channel to high impedance + 0 V.
    /// Emits an "estop" event with the given source ("cmd", "button", ...).
    void triggerEstop(const char* source);

    void setTelemetryRateHz(uint8_t hz);   // clamped to 0-50
    uint8_t getTelemetryRateHz() const { return telemHz_; }
    bool isEstopLatched() const { return estopLatched_; }
    /// Forget the E-stop latch indicator (channels still need ch.reset).
    void clearEstopLatch() { estopLatched_ = false; }

    void log(const char* level, const char* msg);

private:
    static constexpr size_t RX_BUF_SIZE = 768;

    LVLPChannel* lvlp_ = nullptr;
    HPCH*        hp_   = nullptr;
    HVChannel*   hv_   = nullptr;
    uint8_t nLvlp_ = 0, nHp_ = 0, nHv_ = 0;
    Stream* io_ = nullptr;

    char   rxBuf_[RX_BUF_SIZE];
    size_t rxLen_ = 0;

    uint8_t  telemHz_ = 10;
    uint32_t telemPeriodMs_ = 100;
    uint32_t lastTelemMs_ = 0;

    bool estopLatched_ = false;

    // Previous statuses for fault edge detection
    uint8_t prevLvlpStatus_[8] = {0};
    uint8_t prevHpStatus_[2] = {0};
    uint8_t prevHvStatus_[1] = {0};

    void handleLine(char* line);
};

#endif // TESTER_PROTOCOL_H
