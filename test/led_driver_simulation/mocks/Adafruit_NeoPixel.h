// In-memory Adafruit_NeoPixel mock for native (host) tests.
#pragma once

#include <cstdint>

#define NEO_GRB 0x00
#define NEO_KHZ800 0x20

class Adafruit_NeoPixel {
public:
    Adafruit_NeoPixel(uint16_t n, int8_t pin, uint8_t type)
        : n_(n), pin_(pin), type_(type) {}

    void begin() { begin_ = true; }

    void setPixelColor(uint16_t index, uint32_t color) {
        if (index < n_) pixels_[index] = color;
    }

    uint32_t getPixelColor(uint16_t index) const {
        return index < n_ ? pixels_[index] : 0;
    }

    void show() {}

    uint16_t numPixels() const { return n_; }
    int8_t pin() const { return pin_; }
    bool isBegun() const { return begin_; }

    static uint32_t Color(uint8_t r, uint8_t g, uint8_t b) {
        return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
               static_cast<uint32_t>(b);
    }

private:
    uint16_t n_;
    int8_t pin_;
    uint8_t type_;
    uint32_t pixels_[16] = {0};
    bool begin_ = false;
};
