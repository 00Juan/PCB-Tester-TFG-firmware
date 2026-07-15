#include "CalibrationStore.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "pcbtcal";

// ---------------------------------------------------------------------------
// Blob helpers: 1 version byte + payload, written atomically per key
// ---------------------------------------------------------------------------

bool CalibrationStore::saveBlob(const char* key, const void* data, size_t len) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return false;
    uint8_t buf[1 + 256];
    if (len > sizeof(buf) - 1) { prefs.end(); return false; }
    buf[0] = BLOB_VERSION;
    memcpy(buf + 1, data, len);
    size_t written = prefs.putBytes(key, buf, len + 1);
    prefs.end();
    return written == len + 1;
}

bool CalibrationStore::loadBlob(const char* key, void* out, size_t len) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) return false;
    uint8_t buf[1 + 256];
    if (len > sizeof(buf) - 1) { prefs.end(); return false; }
    size_t stored = prefs.getBytesLength(key);
    bool ok = false;
    if (stored == len + 1 && prefs.getBytes(key, buf, stored) == stored
        && buf[0] == BLOB_VERSION) {
        memcpy(out, buf + 1, len);
        ok = true;
    }
    prefs.end();
    return ok;
}

// ---------------------------------------------------------------------------
// Per-type entry points
// ---------------------------------------------------------------------------

bool CalibrationStore::saveLVLP(uint8_t idx, const ChannelCalibrationData& d) {
    if (idx >= 8) return false;
    char key[8];
    snprintf(key, sizeof(key), "lvlp%u", idx);
    return saveBlob(key, &d, sizeof(d));
}

bool CalibrationStore::loadLVLP(uint8_t idx, ChannelCalibrationData& out) {
    if (idx >= 8) return false;
    char key[8];
    snprintf(key, sizeof(key), "lvlp%u", idx);
    return loadBlob(key, &out, sizeof(out));
}

// HP blob = calibration struct + ACS725 zero baseline (kept outside the
// struct by HPCH, but meaningless to persist separately).
struct HPBlob {
    HPCHCalibrationData cal;
    uint16_t zeroAdc;
};

bool CalibrationStore::saveHP(uint8_t idx, const HPCHCalibrationData& d,
                              uint16_t zeroAdc) {
    if (idx >= 2) return false;
    char key[8];
    snprintf(key, sizeof(key), "hp%u", idx);
    HPBlob blob;
    blob.cal = d;
    blob.zeroAdc = zeroAdc;
    return saveBlob(key, &blob, sizeof(blob));
}

bool CalibrationStore::loadHP(uint8_t idx, HPCHCalibrationData& out,
                              uint16_t& zeroAdcOut) {
    if (idx >= 2) return false;
    char key[8];
    snprintf(key, sizeof(key), "hp%u", idx);
    HPBlob blob;
    if (!loadBlob(key, &blob, sizeof(blob))) return false;
    out = blob.cal;
    zeroAdcOut = blob.zeroAdc;
    return true;
}

bool CalibrationStore::saveHV(uint8_t idx, const HVChannelCalibrationData& d) {
    if (idx >= 1) return false;
    return saveBlob("hv0", &d, sizeof(d));
}

bool CalibrationStore::loadHV(uint8_t idx, HVChannelCalibrationData& out) {
    if (idx >= 1) return false;
    return loadBlob("hv0", &out, sizeof(out));
}

bool CalibrationStore::eraseAll() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return false;
    bool ok = prefs.clear();
    prefs.end();
    return ok;
}
