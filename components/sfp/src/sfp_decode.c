// SFF-8472 decoding: identity (A0h), thresholds and live diagnostics (A2h).
// Pure functions on byte buffers so they are unit tested on the host against
// EEPROM dumps. Internally calibrated values need no floating point.
#include <string.h>

#include "sfp_internal.h"

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

uint8_t sfp_sum8(const uint8_t *p, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; ++i) s = (uint8_t)(s + p[i]);
    return s;
}

static void copy_str(char *dst, const uint8_t *src, int len) {
    int end = len;
    while (end > 0 && (src[end - 1] == ' ' || src[end - 1] == 0)) --end;  // strip padding
    for (int i = 0; i < end; ++i) dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
    dst[end] = 0;
}

sfp_err_t sfp_decode_identity(const uint8_t a0[96], sfp_identity_t *id) {
    // A slave holding SDA low reads back as all zeros, and all zeros passes
    // both checksums (plain sums, 0 == 0), so the identifier has to say
    // SFP/SFP+ as well. Found by the stuck-bus fault injection test.
    if (a0[0] != 0x03) return SFP_ERR_CHECKSUM;
    if (sfp_sum8(a0, 63) != a0[63]) return SFP_ERR_CHECKSUM;
    if (sfp_sum8(a0 + 64, 31) != a0[95]) return SFP_ERR_CHECKSUM;
    memset(id, 0, sizeof *id);
    id->identifier = a0[0];
    id->compliance_10g = a0[3];
    id->bitrate_100mbps = a0[12];
    copy_str(id->vendor, a0 + 20, 16);
    copy_str(id->part, a0 + 40, 16);
    copy_str(id->serial, a0 + 68, 16);
    id->wavelength_nm = be16(a0 + 60);
    id->ddm = (a0[92] >> 6) & 1;
    id->ext_cal = (a0[92] >> 4) & 1;
    id->addr_change = (a0[92] >> 2) & 1;
    return SFP_OK;
}

// log2(x) in 1/1024 units for x >= 1: position of the top bit plus a 16-entry
// table for the fraction, linear in between. Worst error about 0.01 dB over
// 0.1 uW to 6.5 mW.
static int32_t log2_q16(uint32_t x) {
    // frac[i] = round(65536 * log2(1 + i/64)). 64 segments: a straight line
    // between table points sits under the log curve by at most 0.0001 dB,
    // where 16 segments left every reading biased low.
    static const uint32_t frac[65] = {
        0, 1466, 2909, 4331, 5732, 7112, 8473, 9814, 11136,
        12440, 13727, 14996, 16248, 17484, 18704, 19909, 21098, 22272,
        23433, 24579, 25711, 26830, 27936, 29029, 30109, 31178, 32234,
        33279, 34312, 35334, 36346, 37346, 38336, 39316, 40286, 41246,
        42196, 43137, 44068, 44990, 45904, 46809, 47705, 48593, 49472,
        50344, 51207, 52063, 52911, 53751, 54584, 55410, 56229, 57040,
        57845, 58643, 59434, 60219, 60997, 61769, 62534, 63294, 64047,
        64794, 65536,
    };
    int msb = 31;
    while (msb > 0 && !(x & (1u << msb))) --msb;
    // 16 bits below the top bit: table index (6 bits) + interpolation (10 bits).
    // Keeping only 8 and rounding down made every reading about 0.01 dB low.
    uint32_t m = msb >= 16 ? (x >> (msb - 16)) & 0xFFFF : (x << (16 - msb)) & 0xFFFF;
    uint32_t idx = m >> 10, t = m & 0x3FF;
    uint32_t f = frac[idx] + (((frac[idx + 1] - frac[idx]) * t + 512) >> 10);
    return msb * 65536 + (int32_t)f;
}

int16_t sfp_power_to_cdbm(uint16_t power_01uw) {
    if (power_01uw == 0) return SFP_NO_LIGHT;
    // dBm = 10 log10(P / 1 mW) with P in 0.1 uW: 10 log10(x) - 40.
    // 10 log10(x) = 3.0103 * log2(x); in hundredths of a dB with log2 in Q16:
    //   cdbm = log2_q16(x) * 301.03 / 65536 - 4000   (rounded to nearest)
    int64_t l = log2_q16(power_01uw);
    int32_t cdbm = (int32_t)((l * 30103 + 3276800) / 6553600) - 4000;
    return (int16_t)cdbm;
}

static void limits(const uint8_t *p, bool is_signed, sfp_limits_t *l) {
    int32_t v[4];
    for (int i = 0; i < 4; ++i) {
        uint16_t raw = be16(p + 2 * i);
        v[i] = is_signed ? (int16_t)raw : (int32_t)raw;
    }
    l->high_alarm = v[0];
    l->low_alarm = v[1];
    l->high_warn = v[2];
    l->low_warn = v[3];
}

void sfp_decode_thresholds(const uint8_t a2[40], sfp_thresholds_t *th) {
    limits(a2 + 0, true, &th->temp);
    limits(a2 + 8, false, &th->vcc);
    limits(a2 + 16, false, &th->bias);
    limits(a2 + 24, false, &th->tx_power);
    limits(a2 + 32, false, &th->rx_power);
}

static float be_float(const uint8_t *p) {
    uint32_t u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

void sfp_decode_ext_cal(const uint8_t c[36], sfp_ext_cal_t *cal) {
    for (int i = 0; i < 5; ++i) cal->rx_pwr[i] = be_float(c + 4 * i);
    cal->bias_slope = be16(c + 20);
    cal->bias_off = (int16_t)be16(c + 22);
    cal->tx_slope = be16(c + 24);
    cal->tx_off = (int16_t)be16(c + 26);
    cal->temp_slope = be16(c + 28);
    cal->temp_off = (int16_t)be16(c + 30);
    cal->vcc_slope = be16(c + 32);
    cal->vcc_off = (int16_t)be16(c + 34);
}

// value = slope * raw + offset, slope unsigned 8.8 fixed point (SFF-8472 9.2).
static int32_t linear(int32_t raw, uint16_t slope, int16_t off) {
    return ((raw * (int32_t)slope + 128) >> 8) + off;
}

static uint16_t clamp_u16(int32_t v) { return v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : (uint16_t)v; }

void sfp_decode_ddm(const uint8_t live[22], const sfp_ext_cal_t *cal, sfp_ddm_t *d,
                    uint32_t *alarm_flags, uint8_t *status) {
    int32_t temp = (int16_t)be16(live + 0);
    int32_t vcc = be16(live + 2);
    int32_t bias = be16(live + 4);
    int32_t tx = be16(live + 6);
    int32_t rx = be16(live + 8);
    if (cal) {
        temp = linear(temp, cal->temp_slope, cal->temp_off);
        vcc = linear(vcc, cal->vcc_slope, cal->vcc_off);
        bias = linear(bias, cal->bias_slope, cal->bias_off);
        tx = linear(tx, cal->tx_slope, cal->tx_off);
        // Rx power is a 4th-order polynomial with float coefficients; the
        // only floating point in the SDK, and only for externally calibrated
        // modules (rare on 10G parts).
        float p = (float)rx;
        float r = cal->rx_pwr[0] * p * p * p * p + cal->rx_pwr[1] * p * p * p +
                  cal->rx_pwr[2] * p * p + cal->rx_pwr[3] * p + cal->rx_pwr[4];
        rx = (int32_t)(r + 0.5f);
    }
    d->temp_c256 = (int16_t)(temp < INT16_MIN ? INT16_MIN : temp > INT16_MAX ? INT16_MAX : temp);
    d->vcc_100uv = clamp_u16(vcc);
    d->bias_2ua = clamp_u16(bias);
    d->tx_01uw = clamp_u16(tx);
    d->rx_01uw = clamp_u16(rx);
    d->tx_cdbm = sfp_power_to_cdbm(d->tx_01uw);
    d->rx_cdbm = sfp_power_to_cdbm(d->rx_01uw);
    // byte 110 status/control, 112-113 alarm flags, 116-117 warning flags
    if (status) *status = live[14];
    if (alarm_flags) {
        uint32_t alarms = ((uint32_t)live[16] << 8) | live[17];
        uint32_t warns = ((uint32_t)live[20] << 8) | live[21];
        *alarm_flags = (warns << 16) | alarms;
    }
}
