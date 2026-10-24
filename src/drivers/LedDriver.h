#pragma once

#include <Adafruit_NeoPixel.h>
#include <cstdint>

namespace led {
// 0xRRGGBB palette, matched to the blue SSD1306 display.
constexpr uint32_t kBlue  = 0x2A55FF; // base color, everything mesh/radio
constexpr uint32_t kWhite = 0xCCE4FF; // attention (msg received, boot lock-in)
constexpr uint32_t kAmber = 0xFF9600; // "I need setup"
constexpr uint32_t kCyan  = 0x18B8FF; // pairing / delivered
constexpr uint32_t kRed   = 0xFF2E2E; // failure / error

// Long-running device states. "Disconnected" on LoRa means "no RF
// neighbours" (LoRa has no persistent connection to lose).
enum class Mode : uint8_t {
    Unprovisioned, // amber breathing — setup me
    Locked,        // dim blue soft blink — private, RF off
    NoNeighbors,   // dim slow breathing — RF up, nobody around
    MeshUp,        // blue ripple chase around the ring
    Pairing,       // cyan heartbeat — QR pairing in progress
};

// One-shot events, played once on top of the mode, which resumes after.
enum class Event : uint8_t {
    None,
    Tx,          // blue flash — packet out
    Rx,          // blue flash — packet in
    MsgReceived, // white double flash — message for me
    Delivered,   // cyan flash — ACK seen
    Failed,      // red triple blink
    Error,       // red fast blink
    Pairing,     // cyan heartbeat
};
}

class LedDriver {
public:
    // Ring order: [L0..L3 top->bottom, R0..R3 top->bottom], L = strip 1.
    static constexpr uint8_t RingSize = 8;
    // Mirrors OledDriver boot::T_END so both animations end together.
    static constexpr uint32_t BootDurationMs = 6600;

    LedDriver();

    void begin(); // init strips, start the boot sequence
    void update(); // non-blocking, call every loop

    void setMode(led::Mode mode);
    led::Mode mode() const { return mode_; }

    void trigger(led::Event event);
    bool eventActive() const { return ev_.event != led::Event::None; }

    // led.set configuration (link spec 7.8)
    void setEnabled(bool enabled);
    void setQuiet(bool quiet);
    void setBrightness(uint8_t brightness); // 0..255, default 128

    // Reset-hold progress: percent 1..100 fills the ring, clear when done.
    void setResetProgress(uint8_t percent);
    void clearResetProgress();

    // led.test preview: play a custom step pattern (on,off,on,... ms).
    void preview(const uint16_t *steps, uint8_t stepCount, uint8_t repeat = 1);

    bool bootDone() const { return !bootActive_; }

    // Test hook: last rendered pixel for a ring index (0xRRGGBB).
    uint32_t pixelAt(uint8_t ringIndex) const { return frame_[ringIndex]; }

private:
    struct EventState {
        led::Event event = led::Event::None;
        const uint16_t *steps = nullptr;
        uint8_t stepCount = 0;
        uint8_t stepIndex = 0; // even = on, odd = off
        uint8_t repeatLeft = 0;
        uint32_t stepEndsAt = 0;
        uint32_t color = 0;
    };

    static constexpr uint8_t Strip1Pin = 45; // left side, index 0 = top
    static constexpr uint8_t Strip2Pin = 46; // right side, index 0 = top
    static constexpr uint8_t StripLeds = RingSize / 2;
    static constexpr uint8_t MaxPreviewSteps = 16; // link spec limit

    Adafruit_NeoPixel strip1_;
    Adafruit_NeoPixel strip2_;

    led::Mode mode_ = led::Mode::Unprovisioned;
    bool bootActive_ = false;
    uint32_t bootStartedAt_ = 0;

    EventState ev_;
    uint16_t previewSteps_[MaxPreviewSteps];
    uint8_t previewStepCount_ = 0;

    bool progressActive_ = false;
    uint8_t progressPct_ = 0;

    bool enabled_ = true;
    bool quiet_ = false;
    uint8_t brightness_ = 128;

    uint8_t gamma_[256];
    uint32_t frame_[RingSize];

    void buildGamma();
    void writeStrips();
    void advanceEvent(uint32_t now);
    void startEvent(led::Event event, const uint16_t *steps, uint8_t stepCount,
                    uint8_t repeat, uint32_t color, uint32_t now);
    void renderBoot(uint32_t t, uint8_t v[RingSize], uint32_t &color);
    void renderMode(uint32_t now, uint8_t v[RingSize], uint32_t &color);
    uint8_t scale(uint8_t v) const;

    static float prog(uint32_t t, uint32_t a, uint32_t b);
    static float easeOutCubic(float x);
    static float easeInOutCubic(float x);
    static float easeOutBack(float x);
    static uint8_t triPulse(uint32_t t, uint32_t start, uint32_t rise, uint32_t fall);
};
