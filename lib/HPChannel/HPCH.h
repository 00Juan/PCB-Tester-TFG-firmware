#ifndef HPCH_H
#define HPCH_H

#include <Arduino.h>
#include <Mcp320x.h>
#include <ShiftRegister74HC595.h>
#include <FastLED.h>

enum HPCHStatus {
    HPCH_STATUS_NORMAL,
    HPCH_STATUS_FAIL_OVERCURRENT,
    HPCH_STATUS_FAIL_OVERVOLTAGE,
    HPCH_STATUS_FAIL_OTHER
};

struct HPCHCalibrationData {
    // ADC linear fit: V_real = mADC_VIn * raw + bADC_VIn
    // raw is the 12-bit integer from MCP3208 (0–4095)
    float mADC_VIn;
    float bADC_VIn;
    float mADC_VOut;
    float bADC_VOut;

    // ACS725LLCTR-10AB-T current sensor parameters
    // Sensitivity in V/A (default ~0.132 V/A at 3.3V Vcc for 10AB variant)
    float acs725_sensitivity;

    // ADC reference voltage in Volts (e.g. 3.270 V)
    float adc_vref;
};

class HPCH {
private:
    uint8_t channelIndex; // Identifier (1 to N)

    MCP3208* adc;
    MCP3208::Channel adcChVIn;
    MCP3208::Channel adcChVOut;
    MCP3208::Channel adcChCurrent;

    ShiftRegister74HC595<2>* sr;
    uint8_t srPin;

    CRGB* led;

    HPCHStatus channelStatus;
    bool connected = false; // PMOS state, tracked by connect()/disconnect()

    float maxVoltageLimit;
    float maxCurrentLimit;

    HPCHCalibrationData calData;

    // Auto-calibrated zero-current ADC raw value
    uint16_t acs725_zero_adc;

    // Internal state — updated by update()
    float channelVIn;
    float channelVOut;
    float channelCurrentOut;

    // Two-point ADC calibration storage
    struct ADCCalPoint {
        uint16_t raw;
        float    voltage;
    };
    ADCCalPoint calPointVIn[2];
    ADCCalPoint calPointVOut[2];
    bool calPointCollected[2]; // true once both points [0] and [1] are stored

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------
    void     checkLimits();
    uint16_t sampleADCAverage(MCP3208::Channel ch, uint8_t numSamples = 64);
    void     fitLinearFromTwoPoints(const ADCCalPoint& p0, const ADCCalPoint& p1,
                                    float& outM, float& outB);

public:
    HPCH(uint8_t chIndex,
         MCP3208* adcPtr,
         MCP3208::Channel adcChIn,
         MCP3208::Channel adcChOut,
         MCP3208::Channel adcChCurr,
         ShiftRegister74HC595<2>* srPtr,
         uint8_t srP,
         const HPCHCalibrationData& calDataRef,
         CRGB* ledPtr);

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
    void init();
    void update(); // Must be called periodically to check limits and read sensors

    // -----------------------------------------------------------------------
    // Connection control
    // -----------------------------------------------------------------------
    void connect();    // Close the PMOS switch
    void disconnect(); // Open the PMOS switch

    // -----------------------------------------------------------------------
    // Limits & status
    // -----------------------------------------------------------------------
    void setLimits(float maxVoltage, float maxCurrent);
    HPCHStatus getStatus() const { return channelStatus; }
    void resetStatus();

    // -----------------------------------------------------------------------
    // Calibration — ADC (two-point linear fit)
    // -----------------------------------------------------------------------
    /**
     * @brief Collect one ADC calibration point for both VIn and VOut channels.
     *
     * Call this twice:
     *   - pointIndex = 0  with a low  known voltage (e.g. ~5 V)
     *   - pointIndex = 1  with a high known voltage (e.g. ~20 V)
     *
     * After both points are collected the linear coefficients
     * (mADC_VIn, bADC_VIn, mADC_VOut, bADC_VOut) are computed and stored
     * immediately.  The function averages 64 ADC samples per measurement.
     *
     * @param pointIndex  0 = low reference, 1 = high reference
     * @param knownVIn    Actual VIn voltage measured externally (Volts)
     * @param knownVOut   Actual VOut voltage measured externally (Volts)
     * @return true if calibration coefficients were computed (i.e. after
     *         point 1 is collected), false if only point 0 was stored.
     */
    bool calibrateADCPoint(uint8_t pointIndex, float knownVIn, float knownVOut);

    // -----------------------------------------------------------------------
    // Calibration — ACS725 current sensor
    // -----------------------------------------------------------------------
    /**
     * @brief Determine the zero-current ADC baseline.
     *
     * The channel is disconnected, then 200 ADC samples are averaged to find
     * the raw ADC value that corresponds to 0 A.  Stores result in
     * acs725_zero_adc.
     */
    void calibrateACS725ZeroOffset();

    /**
     * @brief Calibrate the ACS725 current sensitivity with a known current.
     *
     * The channel must be actively carrying knownCurrentAmps when this is
     * called.  The function averages 200 ADC samples, computes the voltage
     * difference from the stored zero baseline, and derives the real
     * sensitivity (V/A).  Stores result in calData.acs725_sensitivity.
     *
     * @param knownCurrentAmps  Current flowing through the channel (Amperes).
     *                          Must be > 0.
     * @return true on success, false if knownCurrentAmps <= 0 or if the ADC
     *         reading is not meaningfully different from zero.
     */
    bool calibrateACS725Sensitivity(float knownCurrentAmps);

    // -----------------------------------------------------------------------
    // Calibration helpers
    // -----------------------------------------------------------------------
    /**
     * @brief Update the calibration data struct directly (e.g. from flash/EEPROM).
     */
    void setCalibrationData(const HPCHCalibrationData& newCalData);

    /**
     * @brief Return the current calibration data (including auto-computed values).
     */
    const HPCHCalibrationData& getCalibrationData() const { return calData; }

    /**
     * @brief Return the current zero-current ADC baseline.
     */
    uint16_t getACS725ZeroADC() const { return acs725_zero_adc; }

    /**
     * @brief Print a ready-to-paste HPCHCalibrationData struct literal to Serial.
     *
     * Output format matches the hpCalData[] array in hardwareIOSetup.h so
     * the values can be copied directly.
     */
    void printCalibrationBlockForPaste() const;

    // -----------------------------------------------------------------------
    // Sensor reads
    // -----------------------------------------------------------------------
    float readVIn();
    float readVOut();
    float readCurrent();

    // State snapshot getters (values cached by update() — no bus traffic)
    uint8_t getIndex() const { return channelIndex; }
    float   getLastVIn() const { return channelVIn; }
    float   getLastVOut() const { return channelVOut; }
    float   getLastCurrent() const { return channelCurrentOut; }
    float   getMaxVoltageLimit() const { return maxVoltageLimit; }
    float   getMaxCurrentLimit() const { return maxCurrentLimit; }
    bool    isConnected() const { return connected; }

    // -----------------------------------------------------------------------
    // Debug
    // -----------------------------------------------------------------------
    void printDebugInfo() const;
};

#endif // HPCH_H
