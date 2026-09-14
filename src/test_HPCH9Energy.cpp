/**
 * @file    test_HPCH9Energy.cpp
 * @brief   Voltage / current / power / energy datalogger for HP Channel CH9.
 *
 * Build environment: test_HPCH9Energy  (platformio.ini)
 * Baud rate        : 115200
 *
 * CH9 is HPCH1 (hpChannels[0]): ADC2 SINGLE_0 = VIn, SINGLE_1 = VOut,
 * SINGLE_2 = ACS725 current, PMOS switch on shift-register bit 8.
 *
 * Only the INPUT side of the channel is recorded: VIn (upstream of the PMOS
 * switch), the ACS725 current, and the power and energy derived from them.
 * VOut is still read by HPCH::update() for the channel's own limit checking,
 * but it is not logged.
 *
 * ── What it does ───────────────────────────────────────────────────────────
 *
 *   On boot the channel is OPEN and the ACS725 zero-current baseline is
 *   calibrated (no current must be flowing).  The run is then driven by three
 *   ENTER presses:
 *
 *     1st ENTER → logging starts immediately with the channel still OPEN.
 *                 VIn and the current are sampled every 10 ms.  The instant
 *                 of the keypress is recorded (ms since boot).
 *     2nd ENTER → the channel CLOSES so current can flow to the DUT.  The
 *                 instant is recorded and it is from here that peak current,
 *                 power and energy are computed.
 *     3rd ENTER → logging stops, the channel OPENS, the instant is recorded
 *                 and the whole run is dumped as CSV plus a summary.
 *
 *   The samples taken between the 1st and 2nd press are kept in the CSV as
 *   an open-channel baseline (negative t_close_ms); they are excluded from
 *   every derived figure.
 *
 * Peak current, peak power and total energy are derived from the 10 ms
 * samples (transients shorter than one sample period are not captured):
 *
 *   P = VIn * I          E = ∫ P dt   (trapezoidal, over the real timestamps,
 *                                      starting at the first closed sample)
 *
 * ── Number format ──────────────────────────────────────────────────────────
 *   Every number is printed with ',' as the decimal separator, so the CSV
 *   uses ';' as the field separator (European spreadsheet convention):
 *       pd.read_csv("run.csv", sep=';', decimal=',', comment='#')
 *
 * ── Serial commands ────────────────────────────────────────────────────────
 *
 *   ENTER  – advance the run: start → close channel → stop & dump
 *            (the encoder button does the same)
 *   A      – abort a run in progress (channel opens, samples are discarded)
 *   D      – dump the last run again (CSV + summary)
 *   C      – clear the stored run
 *   S      – status (phase, sample count, buffer usage)
 *   Z      – re-run the ACS725 zero-current calibration (channel must be open)
 *   H      – help
 *
 * ── Capturing the CSV ──────────────────────────────────────────────────────
 *   Log the serial monitor to a file and keep the block between
 *   "=== CSV BEGIN ===" and "=== CSV END ===", e.g.
 *       pio device monitor -e test_HPCH9Energy | tee run.log
 *   The summary is embedded at the top of the CSV as '#' comment lines.
 * ───────────────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include "hardwareIOSetup.h"

// ============================================================================
// Configuration
// ============================================================================

/// CH9 == HPCH1 == hpChannels[0]
static HPCH& ch9 = hpChannels[0];

/// Logging period — one sample every 10 ms.
constexpr uint32_t kSamplePeriodMs = 10;

/// Live status line cadence while logging (every 100 samples ≈ 1 s).
constexpr uint16_t kStatusEverySamples = 100;

/// Protection limits are set far above the channel hardware range so that
/// HPCH::checkLimits() never interferes with a measurement run.
constexpr float kVoltageLimit = 60.0f;
constexpr float kCurrentLimit = 20.0f;

/// CSV field separator — ';' because ',' is the decimal separator.
constexpr char kCsvSep = ';';

/// Buffer capacities attempted at boot, largest first.
/// 20000 samples * 12 B = 240 KB ≈ 3.3 min at 10 ms.
static const uint32_t kCapacityLadder[] = {20000, 16000, 12000, 8000, 6000, 3000, 1200};

// ============================================================================
// Number formatting — ',' as the decimal separator
// ============================================================================

/**
 * @brief Format a number with ',' as the decimal separator.
 *
 * Returns a pointer into a small rotating pool of buffers so that several
 * calls can appear as %s arguments of the same printf().  Each call owns its
 * own slot, so the unspecified evaluation order of the arguments is harmless.
 */
static const char* num(double v, uint8_t decimals = 4) {
    constexpr uint8_t kSlots = 12;
    static char pool[kSlots][24];
    static uint8_t slot = 0;

    char* buf = pool[slot];
    slot = (slot + 1) % kSlots;

    snprintf(buf, sizeof(pool[0]), "%.*f", decimals, v);
    for (char* p = buf; *p; ++p) {
        if (*p == '.') { *p = ','; break; }
    }
    return buf;
}

// ============================================================================
// Sample storage
// ============================================================================

struct Sample {
    uint32_t tMs;  ///< ms since the START keypress (1st ENTER)
    float    vIn;  ///< V, supply side of the PMOS switch
    float    i;    ///< A, ACS725 reading
};

static Sample*  gSamples  = nullptr;
static uint32_t gCapacity = 0;
static uint32_t gCount    = 0;

/// Run phase, advanced by each ENTER press.
enum Phase : uint8_t {
    PHASE_IDLE,       ///< not logging
    PHASE_LOG_OPEN,   ///< logging, channel still open (baseline)
    PHASE_LOG_CLOSED  ///< logging, channel closed (analysis window)
};
static Phase gPhase = PHASE_IDLE;

/// Event timestamps, ms since boot, recorded as each ENTER arrives.
static uint32_t gStartEventMs = 0; ///< 1st ENTER — logging started
static uint32_t gCloseEventMs = 0; ///< 2nd ENTER — channel closed
static uint32_t gStopEventMs  = 0; ///< 3rd ENTER — logging stopped

/// Index of the first sample stored at or after the channel closed.
constexpr uint32_t kNoClose = 0xFFFFFFFFUL;
static uint32_t gCloseIndex = kNoClose;

static bool gHaveRun    = false; ///< a completed run is held in the buffer
static bool gBufferFull = false; ///< last run ended because the buffer filled

static uint32_t gNextSampleMs = 0;

// ============================================================================
// Derived results
// ============================================================================

/// Every figure below covers the closed-channel window only.
struct RunSummary {
    bool     valid;        ///< false if the channel never closed
    float    durationS;    ///< closed-channel duration
    uint32_t samples;      ///< samples inside the analysis window
    uint32_t baselineSamples;

    float  iPeak;   float iPeakAtS;
    float  iMin;
    float  iMean;

    float  pPeak;   float pPeakAtS;
    float  pMean;

    double eJ;

    float  vInMin, vInMax, vInMean;
};

/**
 * @brief Recompute every derived figure from the closed-channel samples.
 *
 * Samples logged before the 2nd ENTER are skipped entirely.  The energy is a
 * trapezoidal integral over the real timestamps, so a missed or jittered
 * sample period is accounted for instead of assumed.
 */
static RunSummary computeSummary() {
    RunSummary s = {};
    if (gCloseIndex == kNoClose || gCloseIndex >= gCount) return s;

    s.valid           = true;
    s.samples         = gCount - gCloseIndex;
    s.baselineSamples = gCloseIndex;

    const Sample& first = gSamples[gCloseIndex];
    s.iPeak  = s.iMin  = first.i;
    s.vInMin = s.vInMax = first.vIn;

    double sumI = 0.0, sumP = 0.0, sumVIn = 0.0;

    for (uint32_t k = gCloseIndex; k < gCount; k++) {
        const Sample& sm = gSamples[k];
        // Time axis of the analysis window: 0 at the first closed sample.
        const float tS = (sm.tMs - first.tMs) * 0.001f;
        const float p  = sm.vIn * sm.i;

        if (sm.i > s.iPeak) { s.iPeak = sm.i; s.iPeakAtS = tS; }
        if (sm.i < s.iMin)  { s.iMin  = sm.i; }
        if (p    > s.pPeak) { s.pPeak = p;    s.pPeakAtS = tS; }

        if (sm.vIn < s.vInMin) s.vInMin = sm.vIn;
        if (sm.vIn > s.vInMax) s.vInMax = sm.vIn;

        sumI += sm.i; sumP += p; sumVIn += sm.vIn;

        // Trapezoidal energy increment against the previous sample
        if (k > gCloseIndex) {
            const Sample& pv = gSamples[k - 1];
            const double dt = (sm.tMs - pv.tMs) * 0.001;
            s.eJ += 0.5 * ((double)pv.vIn * pv.i + p) * dt;
        }
    }

    s.durationS = (gSamples[gCount - 1].tMs - first.tMs) * 0.001f;
    s.iMean     = sumI   / s.samples;
    s.pMean     = sumP   / s.samples;
    s.vInMean   = sumVIn / s.samples;
    return s;
}

// ============================================================================
// Output
// ============================================================================

static void printHelp() {
    Serial.println("\n╔══════════════════════════════════════════════╗");
    Serial.println(  "║   CH9 (HPCH1) energy logger — command menu   ║");
    Serial.println(  "╠══════════════════════════════════════════════╣");
    Serial.println(  "║  ENTER  1st: start logging (channel OPEN)    ║");
    Serial.println(  "║         2nd: CLOSE channel, start computing  ║");
    Serial.println(  "║         3rd: stop, open channel, dump CSV    ║");
    Serial.println(  "║  A      abort run, discard samples           ║");
    Serial.println(  "║  D      dump last run again (CSV + summary)  ║");
    Serial.println(  "║  C      clear stored run                     ║");
    Serial.println(  "║  S      status                               ║");
    Serial.println(  "║  Z      ACS725 zero calibration (open only)  ║");
    Serial.println(  "║  H      this help                            ║");
    Serial.println(  "╚══════════════════════════════════════════════╝");
    Serial.printf("Sample period %lu ms | buffer %lu samples (~%s min)\n",
                  (unsigned long)kSamplePeriodMs,
                  (unsigned long)gCapacity,
                  num((gCapacity * kSamplePeriodMs) / 60000.0, 1));
    Serial.println("Logged quantities: VIn, I, P = VIn*I, E = integral of P dt\n");
}

static void printSummaryHuman(const RunSummary& s) {
    Serial.println("\n================ RUN SUMMARY — CH9 (HPCH1) ================");
    Serial.printf("start keypress   : %lu ms since boot\n", (unsigned long)gStartEventMs);
    Serial.printf("close keypress   : %lu ms since boot  (+%lu ms)\n",
                  (unsigned long)gCloseEventMs,
                  (unsigned long)(gCloseEventMs - gStartEventMs));
    Serial.printf("stop  keypress   : %lu ms since boot  (+%lu ms)\n",
                  (unsigned long)gStopEventMs,
                  (unsigned long)(gStopEventMs - gStartEventMs));

    if (!s.valid) {
        Serial.println("-----------------------------------------------------------");
        Serial.println("[!] Channel never closed — no figures to compute.");
        Serial.println("===========================================================\n");
        return;
    }

    Serial.printf("baseline (open)  : %lu samples\n", (unsigned long)s.baselineSamples);
    Serial.printf("closed window    : %s s  (%lu samples @ %lu ms)\n",
                  num(s.durationS, 3), (unsigned long)s.samples,
                  (unsigned long)kSamplePeriodMs);
    if (gBufferFull) {
        Serial.println("[!] run ended because the sample buffer filled up");
    }
    Serial.println("------- all figures below cover the closed window only -----");
    Serial.printf("VIn min/mean/max : %s / %s / %s V\n",
                  num(s.vInMin), num(s.vInMean), num(s.vInMax));
    Serial.printf("I   min/mean/peak: %s / %s / %s A   (peak @ %s s)\n",
                  num(s.iMin), num(s.iMean), num(s.iPeak), num(s.iPeakAtS, 3));
    Serial.printf("P   mean/peak    : %s / %s W   (peak @ %s s)\n",
                  num(s.pMean), num(s.pPeak), num(s.pPeakAtS, 3));
    Serial.println("-----------------------------------------------------------");
    Serial.printf("E   total        : %s J  (%s Wh)\n",
                  num(s.eJ), num(s.eJ / 3600.0, 6));
    Serial.println("===========================================================\n");
}

/**
 * @brief Dump the run as a single CSV block.
 *
 * All samples are written, including the open-channel baseline (the rows
 * with a negative t_close_ms, which are excluded from every derived figure).
 * The summary is written as '#' comment lines above the table so the whole
 * block is still one valid CSV file.  Fields are separated by ';' because
 * ',' is used as the decimal separator.
 */
static void dumpCsv(const RunSummary& s) {
    if (gCount == 0) {
        Serial.println("No samples stored — nothing to dump.");
        return;
    }

    Serial.printf("Dumping %lu rows at %d baud — this takes roughly %lu s.\n",
                  (unsigned long)gCount, 115200,
                  (unsigned long)((gCount * 50UL) / 11000UL) + 1);

    const bool haveClose = (gCloseIndex != kNoClose && gCloseIndex < gCount);
    const uint32_t closeTMs = haveClose ? gSamples[gCloseIndex].tMs : 0;
    const char sp = kCsvSep;

    Serial.println("=== CSV BEGIN ===");
    Serial.println("# PCB-Tester CH9 (HPCH1) input-side voltage/current/power/energy log");
    Serial.println("# decimal separator ',' — field separator ';'");
    Serial.println("# t_ms is referenced to the 1st ENTER, t_close_ms to the channel closing");
    Serial.println("# t_close_ms < 0 marks the open-channel baseline, >= 0 the analysed window");
    Serial.printf ("# sample_period_ms%c%lu%cms\n",          sp, (unsigned long)kSamplePeriodMs, sp);
    Serial.printf ("# start_keypress_since_boot%c%lu%cms\n", sp, (unsigned long)gStartEventMs, sp);
    Serial.printf ("# close_keypress_since_boot%c%lu%cms\n", sp, (unsigned long)gCloseEventMs, sp);
    Serial.printf ("# stop_keypress_since_boot%c%lu%cms\n",  sp, (unsigned long)gStopEventMs, sp);
    Serial.printf ("# close_keypress_after_start%c%lu%cms\n", sp,
                   (unsigned long)(gCloseEventMs - gStartEventMs), sp);
    Serial.printf ("# stop_keypress_after_start%c%lu%cms\n", sp,
                   (unsigned long)(gStopEventMs - gStartEventMs), sp);
    Serial.printf ("# total_samples%c%lu%ccount\n",          sp, (unsigned long)gCount, sp);
    Serial.printf ("# baseline_samples%c%lu%ccount\n",       sp, (unsigned long)s.baselineSamples, sp);
    Serial.printf ("# buffer_full%c%s%cbool\n",              sp, gBufferFull ? "true" : "false", sp);

    if (!s.valid) {
        Serial.printf("# channel_closed%cfalse%cbool\n", sp, sp);
    } else {
        Serial.printf("# channel_closed%ctrue%cbool\n",  sp, sp);
        Serial.printf("# closed_duration%c%s%cs\n",      sp, num(s.durationS, 3), sp);
        Serial.printf("# closed_samples%c%lu%ccount\n",  sp, (unsigned long)s.samples, sp);
        Serial.printf("# vin_min%c%s%cV\n",              sp, num(s.vInMin), sp);
        Serial.printf("# vin_mean%c%s%cV\n",             sp, num(s.vInMean), sp);
        Serial.printf("# vin_max%c%s%cV\n",              sp, num(s.vInMax), sp);
        Serial.printf("# i_min%c%s%cA\n",                sp, num(s.iMin), sp);
        Serial.printf("# i_mean%c%s%cA\n",               sp, num(s.iMean), sp);
        Serial.printf("# i_peak%c%s%cA\n",               sp, num(s.iPeak), sp);
        Serial.printf("# i_peak_at%c%s%cs\n",            sp, num(s.iPeakAtS, 3), sp);
        Serial.printf("# p_mean%c%s%cW\n",               sp, num(s.pMean), sp);
        Serial.printf("# p_peak%c%s%cW\n",               sp, num(s.pPeak), sp);
        Serial.printf("# p_peak_at%c%s%cs\n",            sp, num(s.pPeakAtS, 3), sp);
        Serial.printf("# e_total%c%s%cJ\n",              sp, num(s.eJ), sp);
        Serial.printf("# e_total_wh%c%s%cWh\n",          sp, num(s.eJ / 3600.0, 6), sp);
    }

    Serial.printf("t_ms%ct_close_ms%cvin_V%ci_A%cp_W%ce_J\n",
                  sp, sp, sp, sp, sp);

    // The cumulative energy is re-integrated here with the same trapezoidal
    // rule used by computeSummary(), so the last row matches the summary.
    // It stays at zero until the channel closes.
    double eJ = 0.0;
    for (uint32_t k = 0; k < gCount; k++) {
        const Sample& sm = gSamples[k];
        const bool closed = haveClose && k >= gCloseIndex;
        const double p = (double)sm.vIn * sm.i;

        if (closed && k > gCloseIndex) {
            const Sample& pv = gSamples[k - 1];
            const double dt = (sm.tMs - pv.tMs) * 0.001;
            eJ += 0.5 * ((double)pv.vIn * pv.i + p) * dt;
        }

        Serial.printf("%lu%c", (unsigned long)sm.tMs, sp);
        if (haveClose) {
            Serial.printf("%ld%c", (long)((int32_t)sm.tMs - (int32_t)closeTMs), sp);
        } else {
            Serial.print(sp); // no close event — leave the field empty (NaN)
        }
        Serial.printf("%s%c%s%c%s%c%s\n",
                      num(sm.vIn), sp, num(sm.i), sp,
                      num(p), sp, num(eJ));
    }
    Serial.println("=== CSV END ===");
    Serial.flush();
}

// ============================================================================
// Run control
// ============================================================================

/// 1st ENTER — begin logging with the channel still open.
static void startLogging() {
    if (gCapacity == 0) {
        Serial.println("[ERR] No sample buffer available — cannot start.");
        return;
    }
    if (ch9.getStatus() != HPCH_STATUS_NORMAL) {
        Serial.println("[ERR] CH9 is in a fault state — clearing it before start.");
        ch9.resetStatus();
    }

    gCount      = 0;
    gCloseIndex = kNoClose;
    gHaveRun    = false;
    gBufferFull = false;

    gStartEventMs = millis();
    gCloseEventMs = 0;
    gStopEventMs  = 0;
    gPhase        = PHASE_LOG_OPEN;
    gNextSampleMs = millis();

    Serial.printf("\n>>> [1] START at %lu ms since boot — logging every %lu ms, "
                  "CH9 still OPEN.\n",
                  (unsigned long)gStartEventMs, (unsigned long)kSamplePeriodMs);
    Serial.println(">>> Press ENTER again to CLOSE the channel.\n");
}

/// 2nd ENTER — close the channel; the analysis window starts here.
static void closeChannel() {
    // Record the user input instant first, then close the channel, so the
    // timestamp reflects the command and not the switching delay.
    gCloseEventMs = millis();
    ch9.connect();
    gCloseIndex = gCount; // the next sample stored is the first closed one
    gPhase      = PHASE_LOG_CLOSED;

    Serial.printf("\n>>> [2] CLOSE at %lu ms since boot (+%lu ms) — CH9 CLOSED, "
                  "computing from here.\n",
                  (unsigned long)gCloseEventMs,
                  (unsigned long)(gCloseEventMs - gStartEventMs));
    Serial.println(">>> Press ENTER again to stop and export.\n");
}

/// 3rd ENTER (or a full buffer) — stop, open the channel and export.
static void stopRun(bool bufferFull) {
    gStopEventMs = millis();
    ch9.disconnect();
    gPhase      = PHASE_IDLE;
    gHaveRun    = gCount > 0;
    gBufferFull = bufferFull;

    Serial.printf("\n>>> [3] STOP at %lu ms since boot (+%lu ms) — CH9 OPEN. "
                  "%lu samples captured.\n",
                  (unsigned long)gStopEventMs,
                  (unsigned long)(gStopEventMs - gStartEventMs),
                  (unsigned long)gCount);
    if (bufferFull) {
        Serial.println(">>> Sample buffer filled — run was terminated automatically.");
    }

    if (gCount == 0) {
        Serial.println(">>> No samples captured (run shorter than one sample period).");
        return;
    }

    RunSummary s = computeSummary();
    printSummaryHuman(s);
    dumpCsv(s);
}

/// Abort without exporting.
static void abortRun() {
    if (gPhase == PHASE_IDLE) {
        Serial.println("No run in progress.");
        return;
    }
    ch9.disconnect();
    gPhase      = PHASE_IDLE;
    gCount      = 0;
    gCloseIndex = kNoClose;
    gHaveRun    = false;
    Serial.println("\n>>> Run ABORTED — CH9 OPEN, samples discarded.\n");
}

/// Take one sample and append it to the buffer.
static void takeSample() {
    ch9.update(); // reads VIn, VOut, I and evaluates the (disabled) limits

    Sample& sm = gSamples[gCount];
    sm.tMs = millis() - gStartEventMs;
    sm.vIn = ch9.getLastVIn();
    sm.i   = ch9.getLastCurrent();
    gCount++;

    if (gCount % kStatusEverySamples == 0) {
        Serial.printf("[%s s][%s] VIn=%s V  I=%s A  P=%s W  (%lu/%lu samples)\n",
                      num(sm.tMs * 0.001, 2),
                      gPhase == PHASE_LOG_CLOSED ? "closed" : " open ",
                      num(sm.vIn), num(sm.i), num((double)sm.vIn * sm.i),
                      (unsigned long)gCount, (unsigned long)gCapacity);
    }
}

// ============================================================================
// Commands
// ============================================================================

static void runStatus() {
    const char* phaseStr = "idle";
    if (gPhase == PHASE_LOG_OPEN)   phaseStr = "LOGGING — channel open (baseline)";
    if (gPhase == PHASE_LOG_CLOSED) phaseStr = "LOGGING — channel closed";

    Serial.printf("\nPhase     : %s\n", phaseStr);
    Serial.printf("CH9 status: %s\n",
                  ch9.getStatus() == HPCH_STATUS_NORMAL ? "NORMAL" : "FAULT");
    Serial.printf("Samples   : %lu / %lu (%s%% of buffer)\n",
                  (unsigned long)gCount, (unsigned long)gCapacity,
                  num(gCapacity ? (100.0 * gCount) / gCapacity : 0.0, 1));
    if (gCloseIndex != kNoClose) {
        Serial.printf("Baseline  : %lu samples before the channel closed\n",
                      (unsigned long)gCloseIndex);
    }
    if (gPhase == PHASE_IDLE && !gHaveRun) {
        ch9.update();
        Serial.printf("Live      : VIn=%s V  I=%s A\n",
                      num(ch9.getLastVIn()), num(ch9.getLastCurrent()));
    }
    Serial.println();
}

static void runZeroCalibration() {
    if (gPhase != PHASE_IDLE) {
        Serial.println("[ERR] Stop the run before calibrating.");
        return;
    }
    Serial.println("\nACS725 zero-current calibration — no current must be flowing.");
    ch9.calibrateACS725ZeroOffset();
    Serial.println("Done.\n");
}

static void processCommand(char cmd) {
    switch (cmd) {
        case 'a': case 'A': abortRun(); break;
        case 'd': case 'D':
            if (gHaveRun) {
                RunSummary s = computeSummary();
                printSummaryHuman(s);
                dumpCsv(s);
            } else {
                Serial.println("No stored run to dump.");
            }
            break;
        case 'c': case 'C':
            if (gPhase != PHASE_IDLE) {
                Serial.println("[ERR] Stop the run before clearing.");
            } else {
                gCount = 0; gCloseIndex = kNoClose;
                gHaveRun = false; gBufferFull = false;
                Serial.println("Stored run cleared.");
            }
            break;
        case 's': case 'S': runStatus();          break;
        case 'z': case 'Z': runZeroCalibration(); break;
        case 'h': case 'H': case '?': printHelp();break;
        default:
            Serial.printf("Unknown command '%c'. Press H for help.\n", cmd);
            break;
    }
}

/// One ENTER press advances the run by one step.
static void onEnterPressed() {
    switch (gPhase) {
        case PHASE_IDLE:       startLogging();  break;
        case PHASE_LOG_OPEN:   closeChannel();  break;
        case PHASE_LOG_CLOSED: stopRun(false);  break;
    }
}

/// ENTER (from either CR or LF) advances the run; other characters are commands.
static void handleSerial() {
    static char lastChar = 0;

    // A line-buffered terminal delivers "D\n" in one go: the newline that
    // terminates a command letter must not also advance the run.  Character-
    // at-a-time terminals deliver the letter and a later ENTER in separate
    // drains, so a bare ENTER still advances.
    bool sawCommandChar = false;

    while (Serial.available()) {
        char c = Serial.read();

        if (c == '\r' || c == '\n') {
            // Swallow the LF of a CRLF pair so one keypress advances once.
            bool isCrLfTail = (c == '\n' && lastChar == '\r');
            if (!isCrLfTail && !sawCommandChar) {
                onEnterPressed();
            }
        } else {
            processCommand(c);
            sawCommandChar = true;
        }
        lastChar = c;
    }
}

/// The encoder button is an alternative to ENTER for hands-on use.
static void handleEncoderButton() {
    static bool lastBtnState = HIGH;
    bool btnState = digitalRead(pinEncoderSw);

    if (lastBtnState == HIGH && btnState == LOW) {
        delay(20); // simple debounce
        if (digitalRead(pinEncoderSw) == LOW) {
            onEnterPressed();
        }
    }
    lastBtnState = btnState;
}

// ============================================================================
// setup()
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(2000); // allow the serial monitor to attach

    Serial.println("\n=================================================");
    Serial.println("  CH9 (HPCH1) voltage / current / energy logger  ");
    Serial.println("=================================================");

    // ── Hardware init ───────────────────────────────────────────────────────
    Wire.begin(pinI2cSda, pinI2cScl);
    SPI.begin(pinSpiClk, pinSpiMiso, pinSpiMosi);

    pinMode(pinSrOe, OUTPUT);
    digitalWrite(pinSrOe, LOW);
    sr.setAllLow();

    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.clear();
    FastLED.show();

    if (!initializeMCP3208()) {
        Serial.println("[WARN] MCP3208 init reported issues — continuing anyway.");
    }

    pinMode(pinEncoderSw, INPUT_PULLUP);

    // ── Sample buffer ───────────────────────────────────────────────────────
    for (uint8_t k = 0; k < sizeof(kCapacityLadder) / sizeof(kCapacityLadder[0]); k++) {
        gSamples = (Sample*)malloc(kCapacityLadder[k] * sizeof(Sample));
        if (gSamples) {
            gCapacity = kCapacityLadder[k];
            break;
        }
    }
    if (!gSamples) {
        Serial.println("[ERR] Could not allocate any sample buffer!");
    } else {
        Serial.printf("\nSample buffer: %lu samples (%lu KB) — up to %s min at %lu ms.\n",
                      (unsigned long)gCapacity,
                      (unsigned long)((gCapacity * sizeof(Sample)) / 1024),
                      num((gCapacity * kSamplePeriodMs) / 60000.0, 1),
                      (unsigned long)kSamplePeriodMs);
    }

    // ── CH9 ─────────────────────────────────────────────────────────────────
    ch9.init();
    // Limits parked well above the hardware range: protection must not cut a
    // measurement run short.
    ch9.setLimits(kVoltageLimit, kCurrentLimit);
    Serial.printf("CH9 (HPCH1) initialised — limits parked at %s V / %s A.\n",
                  num(kVoltageLimit, 1), num(kCurrentLimit, 1));

    Serial.println("\nACS725 zero-offset calibration (channel open, no current)...");
    ch9.calibrateACS725ZeroOffset();

    printHelp();
    Serial.println("Ready — press ENTER to start logging (CH9 stays open).\n");
}

// ============================================================================
// loop()
// ============================================================================

void loop() {
    handleSerial();
    handleEncoderButton();

    if (gPhase == PHASE_IDLE) return;

    if ((int32_t)(millis() - gNextSampleMs) >= 0) {
        gNextSampleMs += kSamplePeriodMs;

        // If the loop fell behind (e.g. a long print), resync instead of
        // bursting out the backlog of missed slots.
        if ((int32_t)(millis() - gNextSampleMs) >= (int32_t)kSamplePeriodMs) {
            gNextSampleMs = millis() + kSamplePeriodMs;
        }

        takeSample();

        if (gCount >= gCapacity) {
            stopRun(true);
        }
    }
}
