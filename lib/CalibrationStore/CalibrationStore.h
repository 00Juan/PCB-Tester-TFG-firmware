#ifndef CALIBRATION_STORE_H
#define CALIBRATION_STORE_H

#include <Arduino.h>
#include "LVLPChannel.h"
#include "HPCH.h"
#include "HVChannel.h"

// ============================================================================
// CalibrationStore — persists channel calibration in ESP32 NVS.
//
// Layout: namespace "pcbtcal", one blob per channel:
//   keys "lvlp0".."lvlp7", "hp0".."hp1", "hv0"
// Each blob = 1 version byte + the raw calibration struct (HP blobs also
// carry the ACS725 zero-current ADC baseline).
//
// The hardcoded literals in hardwareIOSetup.h remain the factory defaults:
// load() calls simply fail (return false) until the first save(), and the
// caller keeps the defaults in that case.
// ============================================================================

class CalibrationStore {
public:
    static constexpr uint8_t BLOB_VERSION = 1;

    bool saveLVLP(uint8_t idx, const ChannelCalibrationData& d);
    bool loadLVLP(uint8_t idx, ChannelCalibrationData& out);

    bool saveHP(uint8_t idx, const HPCHCalibrationData& d, uint16_t zeroAdc);
    bool loadHP(uint8_t idx, HPCHCalibrationData& out, uint16_t& zeroAdcOut);

    bool saveHV(uint8_t idx, const HVChannelCalibrationData& d);
    bool loadHV(uint8_t idx, HVChannelCalibrationData& out);

    /// Wipe every stored calibration (factory defaults apply on next boot).
    bool eraseAll();

private:
    bool saveBlob(const char* key, const void* data, size_t len);
    bool loadBlob(const char* key, void* out, size_t len);
};

#endif // CALIBRATION_STORE_H
