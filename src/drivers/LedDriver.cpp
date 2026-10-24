#include "LedDriver.h"

#include <Arduino.h>

#include <cmath>

namespace {
// Boot timeline, mirrors OledDriver::boot (feature/oled-driver) beat for beat.
constexpr uint32_t B_LINE_END     = 260;
constexpr uint32_t B_COLLAPSE_END = 420;
constexpr uint32_t B_GROW_END     = 900;
constexpr uint32_t B_RIPPLE_A     = 950;
constexpr uint32_t B_RIPPLE_B     = 1350;
constexpr uint32_t B_RIPPLE_ROW   = 80;
constexpr uint32_t B_SLIDE_START  = 2000;
constexpr uint32_t B_SLIDE_END    = 2450;
constexpr uint32_t B_TEXT_FLASH   = 2700; // "OFFGRID" lock-in
constexpr uint32_t B_FREQ_FLASH   = 3750; // frequency lock-in
constexpr uint32_t B_FILL_START   = 4450;
constexpr uint32_t B_FILL_STEP    = 150;
constexpr uint32_t B_FADE_START   = 6200;

// line: strip pairs light outward from the middle, index order (1,1),(0,0),(2,2),(3,3)
constexpr uint8_t kLinePair[4] = {1, 0, 2, 3};
constexpr uint32_t kLinePairGap = 70;
constexpr uint32_t kLineRamp = 50;

// Event step tables: on,off,on,off... (link spec 7.8 named patterns)
constexpr uint16_t kStepsFlash[]  = {60};
constexpr uint16_t kStepsDouble[] = {80, 80, 80, 760};
constexpr uint16_t kStepsBlink100[] = {100, 100};
constexpr uint16_t kStepsHeart[]  = {100, 150, 100, 650};

constexpr uint32_t kFlashMs = 90;

constexpr float kTwoPi = 6.28318530718f;
}

LedDriver::LedDriver()
    : strip1_(StripLeds, Strip1Pin, NEO_GRB + NEO_KHZ800),
      strip2_(StripLeds, Strip2Pin, NEO_GRB + NEO_KHZ800) {
    buildGamma();
}

void LedDriver::buildGamma() {
    for (int i = 0; i < 256; i++) {
        const float f = i / 255.0f;
        gamma_[i] = static_cast<uint8_t>(255.0f * f * f + 0.5f);
    }
}

void LedDriver::begin() {
    strip1_.begin();
    strip2_.begin();

    bootActive_ = true;
    bootStartedAt_ = millis();

    for (uint8_t i = 0; i < RingSize; i++) frame_[i] = 0;
    writeStrips();
}

void LedDriver::setMode(led::Mode mode) {
    mode_ = mode;
}

void LedDriver::setEnabled(bool enabled) {
    enabled_ = enabled;
}

void LedDriver::setQuiet(bool quiet) {
    quiet_ = quiet;
}

void LedDriver::setBrightness(uint8_t brightness) {
    brightness_ = brightness;
}

void LedDriver::setResetProgress(uint8_t percent) {
    if (percent == 0) return;
    if (percent > 100) percent = 100;
    progressPct_ = percent;
    progressActive_ = true;
}

void LedDriver::clearResetProgress() {
    progressActive_ = false;
    progressPct_ = 0;
}

void LedDriver::trigger(led::Event event) {
    const uint32_t now = millis();
    switch (event) {
    case led::Event::Tx:
        startEvent(event, kStepsFlash, 1, 1, led::kBlue, now);
        break;
    case led::Event::Rx:
        startEvent(event, kStepsFlash, 1, 1, led::kBlue, now);
        break;
    case led::Event::MsgReceived:
        startEvent(event, kStepsDouble, 4, 1, led::kWhite, now);
        break;
    case led::Event::Delivered:
        startEvent(event, kStepsFlash, 1, 1, led::kCyan, now);
        break;
    case led::Event::Failed:
        startEvent(event, kStepsBlink100, 2, 3, led::kRed, now);
        break;
    case led::Event::Error:
        startEvent(event, kStepsBlink100, 2, 2, led::kRed, now);
        break;
    case led::Event::Pairing:
        startEvent(event, kStepsHeart, 4, 1, led::kCyan, now);
        break;
    case led::Event::None:
        break;
    }
}

void LedDriver::preview(const uint16_t *steps, uint8_t stepCount, uint8_t repeat) {
    if (stepCount == 0 || stepCount > MaxPreviewSteps) return;
    for (uint8_t i = 0; i < stepCount; i++) previewSteps_[i] = steps[i];
    previewStepCount_ = stepCount;
    startEvent(led::Event::None, previewSteps_, stepCount, repeat, led::kBlue, millis());
}

void LedDriver::startEvent(led::Event event, const uint16_t *steps, uint8_t stepCount,
                           uint8_t repeat, uint32_t color, uint32_t now) {
    ev_.event = (event != led::Event::None) ? event : ev_.event; // preview keeps its own
    if (event == led::Event::None) ev_.event = led::Event::Pairing; // dummy tag, steps drive it
    ev_.steps = steps;
    ev_.stepCount = stepCount;
    ev_.stepIndex = 0;
    ev_.repeatLeft = repeat;
    ev_.stepEndsAt = now + steps[0];
    ev_.color = color;
}

void LedDriver::advanceEvent(uint32_t now) {
    if (ev_.event == led::Event::None) return;

    while (now >= ev_.stepEndsAt) {
        if (ev_.stepIndex + 1 < ev_.stepCount) {
            ev_.stepIndex++;
            ev_.stepEndsAt += ev_.steps[ev_.stepIndex];
            return;
        }
        ev_.repeatLeft--;
        if (ev_.repeatLeft == 0) {
            ev_.event = led::Event::None;
            ev_.stepCount = 0;
            return;
        }
        ev_.stepIndex = 0;
        ev_.stepEndsAt += ev_.steps[0];
    }
}

void LedDriver::update() {
    const uint32_t now = millis();
    advanceEvent(now);

    uint8_t v[RingSize] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t color = led::kBlue;

    if (enabled_ && !quiet_) {
        if (bootActive_) {
            const uint32_t t = now - bootStartedAt_;
            if (t >= BootDurationMs) {
                bootActive_ = false;
            } else {
                renderBoot(t, v, color);
            }
        } else if (progressActive_ && progressPct_ > 0) {
            color = led::kWhite;
            const uint8_t lit = static_cast<uint8_t>((progressPct_ * RingSize + 99) / 100);
            for (uint8_t i = 0; i < lit; i++) v[i] = 220;
        } else if (ev_.event != led::Event::None) {
            color = ev_.color;
            if ((ev_.stepIndex % 2) == 0) {
                for (uint8_t i = 0; i < RingSize; i++) v[i] = 255;
            }
        } else {
            renderMode(now, v, color);
        }
    }

    for (uint8_t i = 0; i < RingSize; i++) {
        const uint8_t s = scale(v[i]);
        const uint8_t r = static_cast<uint8_t>((color >> 16) & 0xFF);
        const uint8_t g = static_cast<uint8_t>((color >> 8) & 0xFF);
        const uint8_t b = static_cast<uint8_t>(color & 0xFF);
        frame_[i] = (static_cast<uint32_t>(s) * r >> 8) << 16
                  | (static_cast<uint32_t>(s) * g >> 8) << 8
                  | (static_cast<uint32_t>(s) * b >> 8);
    }
    writeStrips();
}

void LedDriver::writeStrips() {
    for (uint8_t i = 0; i < StripLeds; i++) {
        const uint32_t a = frame_[i];
        const uint32_t b = frame_[i + StripLeds];
        strip1_.setPixelColor(i, strip1_.Color(static_cast<uint8_t>(a >> 16),
                                               static_cast<uint8_t>(a >> 8),
                                               static_cast<uint8_t>(a)));
        strip2_.setPixelColor(i, strip2_.Color(static_cast<uint8_t>(b >> 16),
                                               static_cast<uint8_t>(b >> 8),
                                               static_cast<uint8_t>(b)));
    }
    strip1_.show();
    strip2_.show();
}

uint8_t LedDriver::scale(uint8_t v) const {
    const uint8_t x = static_cast<uint8_t>((static_cast<uint32_t>(v) * brightness_) >> 8);
    return gamma_[x];
}

void LedDriver::renderBoot(uint32_t t, uint8_t v[RingSize], uint32_t &color) {
    color = led::kBlue;
    for (uint8_t i = 0; i < RingSize; i++) v[i] = 0;

    if (t < B_COLLAPSE_END) {
        // line: pairs grow outward, then the whole line collapses
        for (uint8_t p = 0; p < 4; p++) {
            const uint8_t idx = kLinePair[p];
            const uint32_t start = static_cast<uint32_t>(p) * kLinePairGap;
            uint8_t lv = static_cast<uint8_t>(255 * easeOutCubic(prog(t, start, start + kLineRamp)));
            if (t >= B_LINE_END) {
                lv = static_cast<uint8_t>(lv * (1.0f - easeInOutCubic(prog(t, B_LINE_END, B_COLLAPSE_END))));
            }
            v[idx] = lv;
            v[idx + StripLeds] = lv;
        }
    } else if (t < B_GROW_END) {
        // logo scale-in with overshoot
        float s = easeOutBack(prog(t, B_COLLAPSE_END, B_GROW_END));
        if (s < 0.0f) s = 0.0f;
        if (s > 1.0f) s = 1.0f;
        for (uint8_t i = 0; i < RingSize; i++) v[i] = static_cast<uint8_t>(255 * s);
    } else if (t < B_SLIDE_START) {
        // two ripple sweeps, top row first (matches the OLED diagonal wave)
        for (uint32_t s0 : {B_RIPPLE_A, B_RIPPLE_B}) {
            for (uint8_t row = 0; row < 4; row++) {
                const uint8_t lv = triPulse(t, s0 + row * B_RIPPLE_ROW, 60, 60);
                if (lv > v[row]) v[row] = lv;
                if (lv > v[row + StripLeds]) v[row + StripLeds] = lv;
            }
        }
    } else if (t < B_SLIDE_END) {
        // slide: one bright spot chases around the ring
        const uint8_t step = static_cast<uint8_t>((t - B_SLIDE_START) / 56);
        for (uint8_t i = 0; i < RingSize; i++) {
            const uint8_t d = static_cast<uint8_t>((i + RingSize - step) % RingSize);
            v[i] = (d == 0) ? 255 : (d == RingSize - 1) ? 90 : (d == 1 ? 60 : 0);
        }
    } else if (t >= B_TEXT_FLASH && t < B_TEXT_FLASH + kFlashMs) {
        color = led::kWhite;
        for (uint8_t i = 0; i < RingSize; i++) v[i] = 255;
    } else if (t >= B_FREQ_FLASH && t < B_FREQ_FLASH + kFlashMs) {
        color = led::kWhite;
        for (uint8_t i = 0; i < RingSize; i++) v[i] = 255;
    } else if (t >= B_FILL_START && t < B_FADE_START) {
        // staircase fill, mirrors the OLED dark-cell fill: right side bottom-up, then left
        for (uint8_t k = 0; k < RingSize; k++) {
            const uint8_t idx = static_cast<uint8_t>(RingSize - 1 - k);
            const uint32_t start = B_FILL_START + static_cast<uint32_t>(k) * B_FILL_STEP;
            v[idx] = static_cast<uint8_t>(255 * easeOutCubic(prog(t, start, start + 80)));
        }
    } else if (t < BootDurationMs) {
        for (uint8_t i = 0; i < RingSize; i++) {
            v[i] = static_cast<uint8_t>(255 * (1.0f - easeInOutCubic(prog(t, B_FADE_START, BootDurationMs))));
        }
    }
}

void LedDriver::renderMode(uint32_t now, uint8_t v[RingSize], uint32_t &color) {
    switch (mode_) {
    case led::Mode::Unprovisioned: {
        color = led::kAmber;
        const float s = 0.5f + 0.5f * sinf(kTwoPi * static_cast<float>(now) / 1800.0f);
        const uint8_t lv = static_cast<uint8_t>(40 + 170 * s);
        for (uint8_t i = 0; i < RingSize; i++) v[i] = lv;
        break;
    }
    case led::Mode::Locked: {
        color = led::kBlue;
        const uint32_t t = now % 2000;
        uint8_t lv;
        if (t < 950) {
            uint32_t f = t < 120 ? t : (950 - t < 120 ? 950 - t : 120);
            lv = static_cast<uint8_t>(80 * static_cast<float>(f) / 120.0f);
        } else {
            lv = 6;
        }
        for (uint8_t i = 0; i < RingSize; i++) v[i] = lv;
        break;
    }
    case led::Mode::NoNeighbors: {
        color = led::kBlue;
        const float s = 0.5f + 0.5f * sinf(kTwoPi * static_cast<float>(now) / 3000.0f);
        const uint8_t lv = static_cast<uint8_t>(15 + 55 * s);
        for (uint8_t i = 0; i < RingSize; i++) v[i] = lv;
        break;
    }
    case led::Mode::MeshUp: {
        color = led::kBlue;
        const uint8_t step = static_cast<uint8_t>((now / 45) % RingSize);
        for (uint8_t i = 0; i < RingSize; i++) {
            const uint8_t d = static_cast<uint8_t>((i + RingSize - step) % RingSize);
            v[i] = (d == 0) ? 255 : (d == 1) ? 100 : (d == RingSize - 1 ? 60 : 0);
        }
        break;
    }
    case led::Mode::Pairing: {
        color = led::kCyan;
        const uint32_t t = now % 1000;
        uint8_t lv = 0;
        if (t < 100) lv = static_cast<uint8_t>(255 * t / 100);
        else if (t < 250) lv = static_cast<uint8_t>(255 * (250 - t) / 150);
        else if (t < 350) lv = static_cast<uint8_t>(120 * (t - 250) / 100);
        else if (t < 500) lv = static_cast<uint8_t>(120 * (500 - t) / 150);
        for (uint8_t i = 0; i < RingSize; i++) v[i] = lv;
        break;
    }
    }
}

float LedDriver::prog(uint32_t t, uint32_t a, uint32_t b) {
    if (t <= a) return 0.0f;
    if (t >= b) return 1.0f;
    return static_cast<float>(t - a) / static_cast<float>(b - a);
}

float LedDriver::easeOutCubic(float x) {
    const float u = 1.0f - x;
    return 1.0f - u * u * u;
}

float LedDriver::easeInOutCubic(float x) {
    if (x < 0.5f) return 4.0f * x * x * x;
    const float u = -2.0f * x + 2.0f;
    return 1.0f - u * u * u / 2.0f;
}

float LedDriver::easeOutBack(float x) {
    const float c1 = 1.9f, c3 = c1 + 1.0f;
    const float u = x - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

uint8_t LedDriver::triPulse(uint32_t t, uint32_t start, uint32_t rise, uint32_t fall) {
    if (t < start) return 0;
    const uint32_t e = t - start;
    if (e < rise) return static_cast<uint8_t>(255 * static_cast<float>(e) / rise);
    if (e < rise + fall) return static_cast<uint8_t>(255 * static_cast<float>(rise + fall - e) / fall);
    return 0;
}
