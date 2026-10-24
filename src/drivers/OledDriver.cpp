#include "OledDriver.h"
#include <Arduino.h>
#include <Wire.h>
#include <string.h>

static const unsigned char image_phone_on_bits[] U8X8_PROGMEM = {0x3e,0x41,0x5d,0x5d,0x5d,0x5d,0x5d,0x41,0x49,0x3e};
static const unsigned char image_phone_off_bits[] U8X8_PROGMEM = {0x3e,0x41,0x41,0x55,0x49,0x55,0x41,0x41,0x49,0x3e};
static const unsigned char image_lock_closed_bits[] U8X8_PROGMEM = {0x1c,0x22,0x22,0x7f,0x77,0x77,0x7f,0x3e};
static const unsigned char image_lock_open_bits[] U8X8_PROGMEM = {0xe0,0x00,0x10,0x01,0x10,0x01,0x1f,0x00,0x1b,0x00,0x1b,0x00,0x1f,0x00,0x0e,0x00};
static const unsigned char image_lock_unprov_bits[] U8X8_PROGMEM = {0x1c,0x22,0x22,0x7f,0x41,0x41,0x41,0x7f};
static const unsigned char image_radio_bits[] U8X8_PROGMEM = {0x82,0x00,0x29,0x01,0x45,0x01,0x55,0x01,0x29,0x01,0x92,0x00,0x10,0x00,0x38,0x00};
static const unsigned char image_contacts_bits[] U8X8_PROGMEM = {0x1c,0x3e,0x3e,0x1c,0x00,0x3e,0x7f,0x7f};
static const unsigned char image_queue_bits[] U8X8_PROGMEM = {0xff,0x01,0x83,0x01,0x45,0x01,0x29,0x01,0x11,0x01,0x01,0x01,0xff,0x01};
static const unsigned char image_rx_bits[] U8X8_PROGMEM = {0x08,0x08,0x2a,0x1c,0x08,0x41,0x7f};
static const unsigned char image_tx_bits[] U8X8_PROGMEM = {0x08,0x1c,0x2a,0x08,0x08,0x41,0x7f};
static const unsigned char image_check_bits[] U8X8_PROGMEM = {0xc0,0x60,0x33,0x1e,0x0c};
static const unsigned char image_check2_bits[] U8X8_PROGMEM = {0xc0,0x0c,0x60,0x06,0x33,0x03,0x9e,0x01,0xcc,0x00};

namespace boot {

const uint16_t T_LINE_END     = 260;
const uint16_t T_COLLAPSE_END = 420;
const uint16_t T_GROW_END     = 900;
const uint16_t T_RIPPLE_START = 950;
const uint16_t T_RIPPLE_DIAG  = 80;
const uint16_t T_RIPPLE_FRAME = 45;
const uint16_t T_SLIDE_START  = 2000;
const uint16_t T_SLIDE_END    = 2450;
const uint16_t T_TEXT_START   = 2400;
const uint16_t T_TEXT_LOCK    = 2700;
const uint16_t T_TEXT_STEP    = 75;
const uint16_t T_LINE_START   = 3200;
const uint16_t T_LINE_DONE    = 3450;
const uint16_t T_FREQ_START   = 3400;
const uint16_t T_FREQ_LOCK    = 3750;
const uint16_t T_FREQ_STEP    = 120;
const uint16_t T_FILL_START   = 4450;
const uint16_t T_END          = 6600;

const int8_t LOGO_W = 44, LOGO_H = 42;
const int8_t BUB_W = 44, BUB_H = 34, BUB_R = 11;
const int8_t CELL = 5, STEP = 7, GX = 9, GY = 4;
const int8_t LOGO_X0 = (128 - LOGO_W) / 2;
const int8_t LOGO_X1 = 3;
const int8_t LOGO_Y  = (64 - LOGO_H) / 2;
const uint8_t TEXT_X = 55;
const uint8_t WORD_Y = 31;
const uint8_t RULE_Y = 35;
const uint8_t FREQ_Y = 47;

const uint8_t DARK_ROWS[4] = { 0b1000, 0b1100, 0b1110, 0b1111 };

const uint8_t RIPPLE_DARK[]  = { 1, 2, 3, 4, 4, 3 };
const uint8_t RIPPLE_FAINT[] = { 1, 2, 3, 4, 4, 3, 5 };

static uint32_t hash(uint32_t a, uint32_t b) {
    uint32_t h = a * 374761393UL + b * 668265263UL;
    h = (h ^ (h >> 13)) * 1274126177UL;
    return h ^ (h >> 16);
}

static float prog(uint32_t t, uint16_t a, uint16_t b) {
    if (t <= a) return 0.0f;
    if (t >= b) return 1.0f;
    return (float)(t - a) / (float)(b - a);
}
static float easeOutCubic(float x)   { float u = 1.0f - x; return 1.0f - u * u * u; }
static float easeInOutCubic(float x) {
    if (x < 0.5f) return 4.0f * x * x * x;
    float u = -2.0f * x + 2.0f; return 1.0f - u * u * u / 2.0f;
}
static float easeOutBack(float x) {
    const float c1 = 1.9f, c3 = c1 + 1.0f;
    float u = x - 1.0f; return 1.0f + c3 * u * u * u + c1 * u * u;
}

static uint8_t cellState(uint32_t t, uint8_t col, uint8_t row) {
    bool dark = (DARK_ROWS[row] >> col) & 1;
    uint8_t diag = (3 - col) + row;
    uint32_t start = T_RIPPLE_START + diag * (uint32_t)T_RIPPLE_DIAG;
    if (t < start) return 0;
    uint32_t fill = T_FILL_START + diag * (uint32_t)T_RIPPLE_DIAG;
    if (!dark && t >= fill) { dark = true; start = fill; }
    uint16_t f = (t - start) / T_RIPPLE_FRAME;
    if (dark) return f < sizeof(RIPPLE_DARK)  ? RIPPLE_DARK[f]  : 3;
    else      return f < sizeof(RIPPLE_FAINT) ? RIPPLE_FAINT[f] : 6;
}

static void drawLogo(U8G2 &d, int16_t ox, int16_t oy, float s, uint32_t t) {
    int16_t cx = ox + BUB_W / 2, cy = oy + BUB_H / 2;
    int16_t w = (int16_t)(BUB_W * s + 0.5f), h = (int16_t)(BUB_H * s + 0.5f);
    if (w < 2 || h < 2) return;
    int16_t r = (int16_t)(BUB_R * s + 0.5f);
    if (r > w / 2 - 1) r = w / 2 - 1;
    if (r > h / 2 - 1) r = h / 2 - 1;

    d.setDrawColor(1);
    if (r >= 1) d.drawRBox(cx - w / 2, cy - h / 2, w, h, r);
    else        d.drawBox (cx - w / 2, cy - h / 2, w, h);

    if (s > 0.55f) {
        d.drawTriangle(cx + (int16_t)(-16 * s), cy + (int16_t)(12 * s),
                       cx + (int16_t)(-18 * s), cy + (int16_t)(24 * s),
                       cx + (int16_t)( -3 * s), cy + (int16_t)(15 * s));
    }

    d.setDrawColor(0);
    for (uint8_t row = 0; row < 4; row++) {
        for (uint8_t col = 0; col < 4; col++) {
            uint8_t st = cellState(t, col, row);
            if (!st) continue;
            int16_t x = ox + GX + col * STEP, y = oy + GY + row * STEP;
            switch (st) {
                case 1: d.drawPixel(x + 2, y + 2);              break;
                case 2: d.drawBox(x + 1, y + 1, 3, 3);          break;
                case 3: d.drawBox(x, y, CELL, CELL);            break;
                case 4: d.drawBox(x - 1, y - 1, 7, 7);          break;
                case 5: d.drawFrame(x, y, CELL, CELL);          break;
                default:
                    d.drawPixel(x + 1, y + 1); d.drawPixel(x + 3, y + 1);
                    d.drawPixel(x + 1, y + 3); d.drawPixel(x + 3, y + 3);
            }
        }
    }
    d.setDrawColor(1);
}

static uint8_t drawNoisyText(U8G2 &d, uint8_t x, uint8_t y, const char *text, uint32_t t,
                             uint32_t tStart, uint32_t tLock, uint16_t step,
                             const char *noise, uint8_t noiseLen, uint8_t seed) {
    char buf[2] = {0, 0};
    uint8_t n = 0;
    for (uint8_t i = 0; text[i]; i++) {
        buf[0] = text[i];
        uint8_t cw = d.getStrWidth(buf);
        bool fixed = (text[i] == '.');
        if (t >= tStart + i * 35UL) {
            if (!fixed && t < tLock + n * (uint32_t)step) {
                buf[0] = noise[hash(t / 45, i + seed) % noiseLen];
                uint8_t nw = d.getStrWidth(buf);
                d.drawStr(x + (cw - nw) / 2, y, buf);
            } else {
                d.drawStr(x, y, buf);
            }
        }
        if (!fixed) n++;
        x += cw;
    }
    return x;
}

static bool drawFrame(U8G2 &d, uint32_t t, uint32_t freqKhz) {
    d.setDrawColor(1);
    d.setFontMode(1);
    d.setFontPosBaseline();

    if (t < T_COLLAPSE_END) {
        int16_t len;
        if (t < T_LINE_END) len = (int16_t)(128 * easeOutCubic(prog(t, 0, T_LINE_END)));
        else                len = (int16_t)(128 * (1.0f - easeInOutCubic(prog(t, T_LINE_END, T_COLLAPSE_END))));
        if (len < 2) len = 2;
        d.drawHLine(64 - len / 2, 31, len);
        if (t >= T_LINE_END - 60) d.drawHLine(64 - len / 4, 32, len / 2);
        return true;
    }

    float s = (t < T_GROW_END) ? easeOutBack(prog(t, T_COLLAPSE_END, T_GROW_END)) : 1.0f;
    if (s < 0) s = 0;
    float sl = easeInOutCubic(prog(t, T_SLIDE_START, T_SLIDE_END));
    int16_t ox = LOGO_X0 + (int16_t)((LOGO_X1 - LOGO_X0) * sl - 0.5f);
    drawLogo(d, ox, LOGO_Y, s, t);

    d.setFont(u8g2_font_logisoso16_tr);
    if (t >= T_TEXT_START) {
        static const char NOISE[] = "ABCDEFGHJKLMNPRSTUVWXYZ0123456789#%&";
        drawNoisyText(d, TEXT_X, WORD_Y, "OFFGRID", t, T_TEXT_START, T_TEXT_LOCK, T_TEXT_STEP,
                      NOISE, sizeof(NOISE) - 1, 0);
    }
    if (t >= T_LINE_START) {
        uint8_t len = (uint8_t)(d.getStrWidth("OFFGRID") * easeOutCubic(prog(t, T_LINE_START, T_LINE_DONE)));
        if (len) d.drawHLine(TEXT_X, RULE_Y, len);
    }

    if (t >= T_FREQ_START) {
        char fs[12];
        snprintf(fs, sizeof(fs), "%lu.%03lu", (unsigned long)(freqKhz / 1000), (unsigned long)(freqKhz % 1000));
        d.setFont(u8g2_font_profont12_tr);
        uint8_t x = drawNoisyText(d, TEXT_X, FREQ_Y, fs, t, T_FREQ_START, T_FREQ_LOCK, T_FREQ_STEP,
                                  "0123456789", 10, 20);
        d.setFont(u8g2_font_4x6_tr);
        d.drawStr(x + 2, FREQ_Y, "MHz");
    }

    return t < T_END;
}

}

static void renderHome(U8G2 &d, const UiSnapshot &s, UiEvent activity) {
    char buf[32];
    const bool unlocked = s.state == DeviceState::Unlocked;

    switch (s.state) {
        case DeviceState::Locked:   d.drawXBMP(0, 0, 7, 8, image_lock_closed_bits); break;
        case DeviceState::Unlocked: d.drawXBMP(0, 0, 9, 8, image_lock_open_bits); break;
        default:                    d.drawXBMP(0, 0, 7, 8, image_lock_unprov_bits); break;
    }
    d.setFont(u8g2_font_helvB08_tf);
    d.setClipWindow(12, 0, 116, 12);
    d.drawUTF8(12, 8, s.name[0] ? s.name : s.state == DeviceState::Locked ? "Locked" : "Not set up");
    d.setMaxClipWindow();

    // ponytail: unlocked stands in for "app connected" (heartbeat timeout locks);
    // add a snapshot field once the host session is tracked outside the mock.
    d.drawXBMP(121, 0, 7, 10, unlocked ? image_phone_on_bits : image_phone_off_bits);

    d.setFont(u8g2_font_haxrcorp4089_tr);
    snprintf(buf, sizeof buf, "#%s", s.nodeId[0] ? s.nodeId : "----");
    d.drawStr(12, 18, buf);
    const char *link = unlocked ? "connected" : "disconnected";
    d.drawStr(128 - d.getStrWidth(link), 18, link);
    d.drawHLine(0, 21, 128);

    d.drawXBMP(0, 25, 9, 8, image_radio_bits);
    d.setFont(u8g2_font_helvB08_tf);
    snprintf(buf, sizeof buf, "%.3f", s.frequencyMhz);
    d.drawStr(12, 33, buf);
    int x = 12 + d.getStrWidth(buf) + 2;
    d.setFont(u8g2_font_haxrcorp4089_tr);
    d.drawStr(x, 33, "MHz");
    snprintf(buf, sizeof buf, "SF%u BW%g", s.spreadingFactor, s.bandwidthKhz);
    d.drawStr(128 - d.getStrWidth(buf), 33, buf);

    // Capacities match the mock backend's sys.status.
    d.setFont(u8g2_font_helvB08_tf);
    d.drawXBMP(1, 37, 7, 8, image_contacts_bits);
    if (unlocked) snprintf(buf, sizeof buf, "%u/32", s.contacts);
    else snprintf(buf, sizeof buf, "-");
    d.drawStr(12, 45, buf);
    d.drawXBMP(64, 38, 9, 7, image_queue_bits);
    if (unlocked) snprintf(buf, sizeof buf, "%u/8", s.pendingMessages);
    else snprintf(buf, sizeof buf, "-");
    d.drawStr(76, 45, buf);
    d.drawHLine(0, 49, 128);

    const char *text;
    bool rx = false;
    uint8_t ticks = 0;
    switch (activity) {
        case UiEvent::Tx:              text = "Sent"; ticks = 1; break;
        case UiEvent::Delivered:       text = "Delivered"; ticks = 2; break;
        case UiEvent::Failed:          text = "Send failed"; break;
        case UiEvent::Rx:              text = "Received"; rx = true; break;
        case UiEvent::MessageReceived: text = "New message"; rx = true; break;
        default: return;
    }
    d.drawXBMP(1, 53, 7, 7, rx ? image_rx_bits : image_tx_bits);
    d.setFont(u8g2_font_haxrcorp4089_tr);
    d.drawStr(12, 60, text);
    if (ticks >= 2) d.drawXBMP(116, 54, 12, 5, image_check2_bits);
    else if (ticks == 1) d.drawXBMP(120, 54, 8, 5, image_check_bits);
}

static void renderRadio(U8G2 &d, const UiSnapshot &s) {
    char buf[32];
    d.drawXBMP(0, 0, 9, 8, image_radio_bits);
    d.setFont(u8g2_font_helvB08_tf);
    d.drawStr(12, 8, "Radio");
    d.setFont(u8g2_font_haxrcorp4089_tr);
    const char *state = s.radioError ? "error" : s.relayOnly ? "relay only" :
        s.radioRunning ? "running" : "standby";
    d.drawStr(128 - d.getStrWidth(state), 8, state);
    d.drawStr(12, 18, s.radioDiagnostics ? "diagnostic" : "mock");
    if (s.radioError) {
        snprintf(buf, sizeof buf, "err %ld", (long)s.radioError);
        d.drawStr(128 - d.getStrWidth(buf), 18, buf);
    }
    d.drawHLine(0, 21, 128);

    d.setFont(u8g2_font_helvB08_tf);
    snprintf(buf, sizeof buf, "%.3f", s.frequencyMhz);
    d.drawStr(0, 33, buf);
    int x = d.getStrWidth(buf) + 2;
    d.setFont(u8g2_font_haxrcorp4089_tr);
    d.drawStr(x, 33, "MHz");
    snprintf(buf, sizeof buf, "SF%u BW%g", s.spreadingFactor, s.bandwidthKhz);
    d.drawStr(128 - d.getStrWidth(buf), 33, buf);

    d.setFont(u8g2_font_helvB08_tf);
    d.drawXBMP(1, 38, 7, 7, image_tx_bits);
    snprintf(buf, sizeof buf, "%lu", (unsigned long)s.txPackets);
    d.drawStr(12, 45, buf);
    d.drawXBMP(64, 38, 7, 7, image_rx_bits);
    snprintf(buf, sizeof buf, "%lu", (unsigned long)s.rxPackets);
    d.drawStr(76, 45, buf);
    d.drawHLine(0, 49, 128);

    d.setFont(u8g2_font_haxrcorp4089_tr);
    if (s.hasSignal) snprintf(buf, sizeof buf, "RSSI %.0f dBm   SNR %.1f dB", s.rssi, s.snr);
    else snprintf(buf, sizeof buf, "no signal yet");
    d.drawStr(0, 60, buf);
}

static void renderPairing(U8G2 &d, const UiSnapshot &s) {
    char buf[32];
    d.drawXBMP(1, 0, 7, 8, image_contacts_bits);
    d.setFont(u8g2_font_helvB08_tf);
    d.drawStr(12, 8, "Pairing");
    d.drawHLine(0, 11, 128);
    d.setFont(u8g2_font_haxrcorp4089_tr);
    if (s.pairingActive) {
        d.drawStr(0, 24, "Scan the QR code in the app");
        snprintf(buf, sizeof buf, "%u:%02u", s.pairingRemainingS / 60, s.pairingRemainingS % 60);
        d.setFont(u8g2_font_logisoso16_tr);
        d.drawStr((128 - d.getStrWidth(buf)) / 2, 50, buf);
        d.setFont(u8g2_font_haxrcorp4089_tr);
        d.drawStr(0, 62, "remaining");
    } else if (s.fingerprint[0]) {
        d.drawStr(0, 24, "Compare on both devices:");
        d.setFont(u8g2_font_profont12_tr);
        d.drawStr((128 - d.getStrWidth(s.fingerprint)) / 2, 42, s.fingerprint);
        d.setFont(u8g2_font_haxrcorp4089_tr);
        d.drawStr(0, 60, "Returns home automatically");
    } else {
        d.drawStr(0, 30, s.state == DeviceState::Unlocked ? "Start pairing in the app" : "Unlock to pair");
    }
}

bool OledDriver::init() {
    pinMode(BoardConfig::VextControl, OUTPUT);
    digitalWrite(BoardConfig::VextControl, LOW);
    delay(100);
    display_.setBusClock(400000);
    display_.begin();
    Wire.setTimeOut(20);
    Wire.beginTransmission(0x3C);
    if (Wire.endTransmission() != 0) return false;
    display_.setFontMode(1);
    display_.setBitmapMode(1);
    return true;
}

void OledDriver::show(const UiSnapshot& snapshot) {
    if (snapshot.fingerprint[0] && strcmp(snapshot.fingerprint, snapshot_.fingerprint) != 0) {
        fingerprintUntil_ = snapshot.uptimeMs + 15000;
    }
    if (snapshot.state != DeviceState::Unlocked) {
        activity_ = UiEvent::Boot;
        fingerprintUntil_ = 0;
    }
    snapshot_ = snapshot;
}

void OledDriver::onEvent(UiEvent event, uint64_t nowMs) {
    switch (event) {
        case UiEvent::Boot:
            bootAt_ = nowMs;
            booting_ = true;
            break;
        case UiEvent::Pairing:
            booting_ = false;
            fingerprintUntil_ = 0;
            break;
        case UiEvent::Error:
            booting_ = false;
            errorUntil_ = nowMs + 3000;
            break;
        default:
            activity_ = event;
            activityUntil_ = nowMs + 5000;
    }
}

// Renders into RAM at most every FrameMs, but only after the previous frame is
// fully sent, then pushes one changed tile row per tick.
// ponytail: one 128-byte row (~3-4 ms at 400 kHz) per tick; measure on device
// and send half rows if the loop budget needs it.
void OledDriver::tick(uint64_t nowMs) {
    constexpr uint64_t FrameMs = 40;
    uint8_t *buffer = display_.getBufferPtr();
    if (dirtyRows_ == 0 && nowMs - renderedAt_ >= FrameMs) {
        renderedAt_ = nowMs;
        render(nowMs);
        for (uint8_t row = 0; row < Rows; ++row) {
            if (memcmp(buffer + row * RowBytes, sent_ + row * RowBytes, RowBytes) != 0) {
                dirtyRows_ |= 1 << row;
            }
        }
    }
    if (dirtyRows_ == 0) return;
    const uint8_t row = __builtin_ctz(dirtyRows_);
    display_.updateDisplayArea(0, row, 16, 1);
    memcpy(sent_ + row * RowBytes, buffer + row * RowBytes, RowBytes);
    dirtyRows_ &= ~(1 << row);
}

void OledDriver::render(uint64_t nowMs) {
    display_.clearBuffer();
    if (nowMs < errorUntil_) {
        display_.setFont(u8g2_font_helvB08_tf);
        display_.drawStr(0, 20, "Operation failed");
        display_.setFont(u8g2_font_haxrcorp4089_tr);
        display_.drawStr(0, 38, "See app for details");
    } else if (booting_) {
        booting_ = boot::drawFrame(display_, nowMs - bootAt_,
            static_cast<uint32_t>(snapshot_.frequencyMhz * 1000 + 0.5f));
    } else if (snapshot_.pairingActive || (nowMs < fingerprintUntil_ && snapshot_.fingerprint[0]) ||
               snapshot_.page == UiPage::Pairing) {
        renderPairing(display_, snapshot_);
    } else if (snapshot_.page == UiPage::Radio) {
        renderRadio(display_, snapshot_);
    } else {
        renderHome(display_, snapshot_, nowMs < activityUntil_ ? activity_ : UiEvent::Boot);
    }
}
