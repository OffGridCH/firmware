#pragma once

#include <U8g2lib.h>

#include "../BoardConfig.h"
#include "../Types.h"

class OledDriver {
public:
    bool init();
    void show(const UiSnapshot& snapshot);
    void onEvent(UiEvent event, uint64_t nowMs);
    void tick(uint64_t nowMs);

private:
    static constexpr uint8_t Rows = 8;
    static constexpr size_t RowBytes = 128;

    void render(uint64_t nowMs);

    U8G2_SSD1306_128X64_NONAME_F_HW_I2C display_{U8G2_R0, BoardConfig::OledReset,
        BoardConfig::OledScl, BoardConfig::OledSda};
    UiSnapshot snapshot_;
    uint8_t sent_[Rows * RowBytes] = {};
    uint8_t dirtyRows_ = 0;
    uint64_t renderedAt_ = 0;
    uint64_t bootAt_ = 0;
    bool booting_ = true;
    bool fingerprintOverlay_ = false;
    UiEvent activity_ = UiEvent::Boot;  // Boot = no activity line
};
