#include <Arduino.h>

#include "drivers/LedDriver.h"

// Bench demo harness: walks the LED driver through its whole animation
// catalogue so every state and event can be verified on the hardware.
//
// Sequence (repeats forever): boot (6.6s, mirrors the OLED boot) ->
// MeshUp + Tx/MsgReceived/Delivered -> NoNeighbors -> Unprovisioned ->
// Locked + Error/Failed -> Pairing + heartbeat -> reset-progress fill ->
// preview pattern -> back to MeshUp.

namespace {

enum Action : uint8_t {
    ActTx,
    ActMsgReceived,
    ActDelivered,
    ActError,
    ActFailed,
    ActPairing,
    ActProgress25,
    ActProgress50,
    ActProgress75,
    ActProgress100,
    ActProgressClear,
    ActPreview,
};

struct Entry {
    uint32_t atMs;  // phase-relative
    Action act;
};

const Entry kMesh[] = {
    {1000, ActTx},
    {2500, ActMsgReceived},
    {4000, ActDelivered},
};
const Entry kLocked[] = {
    {1500, ActError},
    {2500, ActFailed},
};
const Entry kPairing[] = {
    {1000, ActPairing},
};
const Entry kProgress[] = {
    {250, ActProgress25},
    {1000, ActProgress50},
    {1750, ActProgress75},
    {2500, ActProgress100},
    {2900, ActProgressClear},
};

constexpr uint8_t kPhaseLast = 7;

const Entry *phaseEntries(uint8_t phase) {
    switch (phase) {
    case 1: return kMesh;
    case 2: return nullptr;  // NoNeighbors
    case 3: return nullptr;  // Unprovisioned
    case 4: return kLocked;
    case 5: return kPairing;
    case 6: return kProgress;
    case 7: return nullptr;  // preview fired in startPhase
    default: return nullptr;
    }
}

uint8_t phaseEntryCount(uint8_t phase) {
    switch (phase) {
    case 1: return 3;
    case 4: return 2;
    case 5: return 1;
    case 6: return 5;
    default: return 0;
    }
}

uint32_t phaseDurationMs(uint8_t phase) {
    switch (phase) {
    case 1: return 5000;  // MeshUp
    case 2: return 3000;  // NoNeighbors
    case 3: return 3000;  // Unprovisioned
    case 4: return 3500;  // Locked
    case 5: return 3000;  // Pairing
    case 6: return 3300;  // reset progress
    case 7: return 1500;  // preview
    default: return 0;
    }
}

const char *phaseName(uint8_t phase) {
    switch (phase) {
    case 1: return "MeshUp";
    case 2: return "NoNeighbors";
    case 3: return "Unprovisioned";
    case 4: return "Locked";
    case 5: return "Pairing";
    case 6: return "ResetProgress";
    case 7: return "Preview";
    default: return "?";
    }
}

LedDriver ledDriver;
uint8_t phase = 0;  // 0 = wait for boot
uint32_t phaseStartMs = 0;
uint8_t actionIdx = 0;
bool bootLogged = false;

void runAction(Action act) {
    switch (act) {
    case ActTx:
        Serial.println("  trigger Tx");
        ledDriver.trigger(led::Event::Tx);
        break;
    case ActMsgReceived:
        Serial.println("  trigger MsgReceived");
        ledDriver.trigger(led::Event::MsgReceived);
        break;
    case ActDelivered:
        Serial.println("  trigger Delivered");
        ledDriver.trigger(led::Event::Delivered);
        break;
    case ActError:
        Serial.println("  trigger Error");
        ledDriver.trigger(led::Event::Error);
        break;
    case ActFailed:
        Serial.println("  trigger Failed");
        ledDriver.trigger(led::Event::Failed);
        break;
    case ActPairing:
        Serial.println("  trigger Pairing");
        ledDriver.trigger(led::Event::Pairing);
        break;
    case ActProgress25:
        Serial.println("  reset progress 25%");
        ledDriver.setResetProgress(25);
        break;
    case ActProgress50:
        Serial.println("  reset progress 50%");
        ledDriver.setResetProgress(50);
        break;
    case ActProgress75:
        Serial.println("  reset progress 75%");
        ledDriver.setResetProgress(75);
        break;
    case ActProgress100:
        Serial.println("  reset progress 100%");
        ledDriver.setResetProgress(100);
        break;
    case ActProgressClear:
        Serial.println("  reset progress done");
        ledDriver.clearResetProgress();
        break;
    case ActPreview:
        Serial.println("  preview {200,150,200} x2");
        const uint16_t steps[] = {200, 150, 200};
        ledDriver.preview(steps, 3, 2);
        break;
    }
}

void startPhase(uint8_t p) {
    phase = p;
    phaseStartMs = millis();
    actionIdx = 0;
    switch (p) {
    case 1:
    case 6:
    case 7:
        ledDriver.setMode(led::Mode::MeshUp);
        break;
    case 2:
        ledDriver.setMode(led::Mode::NoNeighbors);
        break;
    case 3:
        ledDriver.setMode(led::Mode::Unprovisioned);
        break;
    case 4:
        ledDriver.setMode(led::Mode::Locked);
        break;
    case 5:
        ledDriver.setMode(led::Mode::Pairing);
        break;
    default:
        break;
    }
    Serial.printf("demo phase: %s\n", phaseName(p));
    if (p == 7) {
        runAction(ActPreview);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.println("offgrid led demo");
    ledDriver.begin();
}

void loop() {
    ledDriver.update();

    if (phase == 0) {
        if (ledDriver.bootDone() && !bootLogged) {
            bootLogged = true;
            Serial.println("boot done");
        }
        if (ledDriver.bootDone()) {
            startPhase(1);
        }
        return;
    }

    const uint32_t t = millis() - phaseStartMs;

    const Entry *items = phaseEntries(phase);
    const uint8_t count = phaseEntryCount(phase);
    while (actionIdx < count && t >= items[actionIdx].atMs) {
        runAction(items[actionIdx].act);
        actionIdx++;
    }

    if (t >= phaseDurationMs(phase)) {
        startPhase(phase == kPhaseLast ? 1 : phase + 1);
    }
}
