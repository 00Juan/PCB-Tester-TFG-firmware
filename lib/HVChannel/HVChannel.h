#ifndef HV_CHANNEL_H
#define HV_CHANNEL_H

#include <Arduino.h>
#include <Mcp320x.h>
#include <ShiftRegister74HC595.h>
#include <FastLED.h>

// ============================================================================
// Maximum number of calibration segments used by the piecewise linear model.
// Increase if you need finer granularity over a wider voltage range.
// ============================================================================
constexpr uint8_t HV_CAL_MAX_POINTS = 16;

// ============================================================================
// Status flags
// ============================================================================
enum HVChannelStatus {
    HV_STATUS_NORMAL,
    HV_STATUS_FAIL_OVERVOLTAGE,
    HV_STATUS_FAIL_OTHER
};

// ============================================================================
// Calibration data
//
// The HV voltage divider has a significant dead-zone at the low end (roughly
// 0–30 V) where the ADC output stays flat and therefore cannot distinguish
// between voltages.  A simple two-point linear fit is not sufficient.
//
// Instead the calibration stores N (raw, voltage) anchor points that define
// a piecewise-linear (PWL) transfer function:
//
//   V_real = PWL(raw_adc)
//
// Points must be stored in ascending order of raw_adc.
// The first point represents the end of the dead-zone, i.e. the first raw
// value where the ADC output starts to move meaningfully.
//
// Points with raw == 0 AND voltage == 0 are treated as "not set" and are
// ignored during look-up.
// ============================================================================
struct HVCalPoint {
    uint16_t raw;     // 12-bit ADC raw value (0–4095)
    float    voltage; // Real-world voltage at that raw value (Volts)
};

struct HVChannelCalibrationData {
    // Dead-zone threshold: any raw value at or below this is reported as 0 V.
    // Typically corresponds to the raw reading when the input is at ~30 V.
    uint16_t deadZoneRaw;

    // Piecewise-linear anchor points (sorted by raw, ascending).
    // Fill from index 0; unused slots should have raw=0, voltage=0.
    HVCalPoint points[HV_CAL_MAX_POINTS];
    uint8_t    numPoints; // How many valid points are stored (2 … HV_CAL_MAX_POINTS)

    // ADC reference voltage used for any raw-to-voltage helpers (Volts).
    float adc_vref;
};

// ============================================================================
// HVChannel class
// ============================================================================
class HVChannel {
public:
    // -----------------------------------------------------------------------
    // Constructor
    // -----------------------------------------------------------------------
    /**
     * @param chIndex       User-facing channel identifier (1-based)
     * @param adcPtr        Pointer to the shared MCP3208 object
     * @param adcCh         Which MCP3208 channel to read the HV voltage from
     * @param srPtr         Pointer to the shift-register controlling the relay
     * @param srP           Bit position inside the shift register (0-based)
     * @param calDataRef    Initial calibration data (may be identity / defaults)
     * @param ledPtr        Optional pointer to a FastLED CRGB pixel for status
     */
    HVChannel(uint8_t chIndex,
              MCP3208* adcPtr,
              MCP3208::Channel adcCh,
              ShiftRegister74HC595<2>* srPtr,
              uint8_t srP,
              const HVChannelCalibrationData& calDataRef,
              CRGB* ledPtr = nullptr);

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
    /** Initialise GPIO, disconnect relay, set LED green. */
    void init();

    /**
     * Read sensors, check limits.  Call this periodically (e.g. every loop
     * iteration or every 100 ms).
     */
    void update();

    // -----------------------------------------------------------------------
    // Connection control
    // -----------------------------------------------------------------------
    /** Energise the relay (close).  No-op if channel is in a fault state. */
    void connect();

    /** De-energise the relay (open). */
    void disconnect();

    // -----------------------------------------------------------------------
    // Limits & status
    // -----------------------------------------------------------------------
    /**
     * @param maxVoltage  Trip threshold in Volts (overvoltage protection).
     */
    void setLimits(float maxVoltage);
    HVChannelStatus getStatus() const { return channelStatus; }

    /**
     * Clear fault, disconnect relay, restore green LED.
     * The user must call connect() again explicitly after resetting.
     */
    void resetStatus();

    // -----------------------------------------------------------------------
    // Sensor reads
    // -----------------------------------------------------------------------
    /**
     * @brief Read HV voltage using the stored piecewise-linear calibration.
     *
     * Returns 0.0 if the raw ADC reading is inside the dead-zone.
     */
    float readVoltage();

    /**
     * @brief Return the raw 12-bit ADC value without any conversion.
     *
     * Useful during calibration to observe what the ADC actually reports.
     */
    uint16_t readRaw();

    // State snapshot getters (values cached by update() — no bus traffic)
    uint8_t getIndex() const { return channelIndex; }
    float   getLastVoltage() const { return channelVoltage; }
    float   getMaxVoltageLimit() const { return maxVoltageLimit; }
    bool    isConnected() const { return connected; }
    uint8_t getCalPointCount() const { return calData.numPoints; }
    bool    isCalibrationValid() const { return calData.numPoints >= 2; }

    // -----------------------------------------------------------------------
    // Calibration — interactive multi-point PWL
    // -----------------------------------------------------------------------

    /**
     * @brief Reset the calibration table and the dead-zone threshold.
     *
     * Call this before starting a new calibration session.
     */
    void calibrationReset();

    /**
     * @brief Record the dead-zone boundary.
     *
     * Apply a known voltage just above the dead-zone (e.g. 31–35 V), then
     * call this function with that voltage.  The function samples the ADC,
     * stores the measured raw value as deadZoneRaw and also saves the first
     * calibration point.
     *
     * @param knownVoltage  Voltage currently applied to the HV input (Volts).
     *                      Must be the lowest voltage at which the ADC reading
     *                      starts to change (end of dead-zone).
     */
    void calibrateDeadZone(float knownVoltage);

    /**
     * @brief Add one calibration anchor point.
     *
     * Apply a stable, known voltage to the HV input, then call this function.
     * The ADC is averaged over numSamples readings and the (raw, voltage) pair
     * is appended to the calibration table in sorted order.
     *
     * At least 2 points (plus the dead-zone boundary) are needed for a valid
     * piecewise-linear fit.
     *
     * @param knownVoltage  Voltage currently applied to the HV input (Volts).
     * @param numSamples    Number of ADC samples to average (default 64).
     * @return true  if the point was stored successfully.
     * @return false if the table is already full (HV_CAL_MAX_POINTS reached).
     */
    bool calibrateAddPoint(float knownVoltage, uint8_t numSamples = 64);

    /**
     * @brief Finalise calibration and sort the point table.
     *
     * Call once after all calibrateAddPoint() calls.  Sorts the internal
     * table by raw value (ascending) so that the PWL look-up works correctly.
     *
     * @return true  if at least 2 points are stored (calibration is valid).
     * @return false if fewer than 2 points were collected.
     */
    bool calibrateFinish();

    // -----------------------------------------------------------------------
    // Calibration data management
    // -----------------------------------------------------------------------
    /** Replace the entire calibration data struct (e.g. loaded from flash). */
    void setCalibrationData(const HVChannelCalibrationData& newCalData);

    /** Return a const reference to the current calibration data. */
    const HVChannelCalibrationData& getCalibrationData() const { return calData; }

    /**
     * @brief Print a ready-to-paste HVChannelCalibrationData initialiser to
     *        Serial so it can be copied into hardwareIOSetup.h.
     */
    void printCalibrationBlockForPaste() const;

    // -----------------------------------------------------------------------
    // Debug
    // -----------------------------------------------------------------------
    void printDebugInfo() const;

private:
    // -----------------------------------------------------------------------
    // Fields
    // -----------------------------------------------------------------------
    uint8_t  channelIndex;
    MCP3208* adc;
    MCP3208::Channel adcChannel;
    ShiftRegister74HC595<2>* sr;
    uint8_t  srPin;
    CRGB*    led;

    HVChannelStatus channelStatus;
    bool     connected = false; // relay state, tracked by connect()/disconnect()
    float    maxVoltageLimit;

    HVChannelCalibrationData calData;

    // Last readings updated by update()
    float    channelVoltage;

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------
    void     checkLimits();
    uint16_t sampleADCAverage(uint8_t numSamples);

    /**
     * @brief Evaluate the piecewise-linear function at a given raw ADC value.
     *
     * Returns 0.0 if raw <= deadZoneRaw or if fewer than 2 points are stored.
     * Clamps to the outermost point values if raw is outside the calibrated
     * range.
     */
    float    pwlLookup(uint16_t raw) const;

    /** Insertion-sort the calibration point table by raw (ascending). */
    void     sortCalibrationPoints();
};

#endif // HV_CHANNEL_H
