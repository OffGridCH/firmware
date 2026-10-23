#pragma once

#include <U8g2lib.h>
#include "../BoardConfig.h"

enum class LockState : uint8_t { Unprov, Locked, Unlocked };
enum class EventKind : uint8_t { None, Rx, Tx };

struct HomeView {
    LockState lock;
    uint16_t nodeId;
    char name[25];
    bool phoneConnected;
    float freqMhz;
    uint8_t sf;
    uint16_t bwKhz;
    uint8_t contacts, contactsMax;
    uint8_t queue, queueMax;
    EventKind event;
    char eventText[24];
    uint8_t eventTicks;
};

class OledDriver {
public:
    void begin();
    void drawHome(const HomeView &v);

private:
    U8G2_SSD1306_128X64_NONAME_F_HW_I2C display{U8G2_R0, OLED_RST, OLED_SCL, OLED_SDA};
};
