#include <Arduino.h>
#include <esp_flash_encrypt.h>
#include <esp_secure_boot.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>

#include "BoardConfig.h"
#include "host/Commands.h"
#include "host/SerialLink.h"

#if !defined(OFFGRID_MOCK_DEVICE) || OFFGRID_MOCK_DEVICE != 1
#error "This entry point is a serial development mock, not production firmware."
#endif

namespace {
#if OFFGRID_SERIAL_NATIVE_USB
auto& hostPort = Serial;
bool wasConnected = false;
#else
auto& hostPort = Serial0;
#endif

SerialLink serialLink(hostPort);

class MockDevice {
public:
    void init() {
        const uint64_t mac = ESP.getEfuseMac();
        for (size_t index = 0; index < 6; ++index) {
            snprintf(deviceId_ + index * 2, 3, "%02X",
                     static_cast<unsigned>((mac >> (index * 8)) & 0xff));
        }
        switch (esp_reset_reason()) {
            case ESP_RST_POWERON: resetReason_ = "power_on"; break;
            case ESP_RST_SW: resetReason_ = "software"; break;
            case ESP_RST_BROWNOUT: resetReason_ = "brownout"; break;
            case ESP_RST_INT_WDT:
            case ESP_RST_TASK_WDT:
            case ESP_RST_WDT: resetReason_ = "watchdog"; break;
            default: resetReason_ = "other"; break;
        }
    }

    void emitBoot(uint64_t nowMs) {
        StaticJsonDocument<512> document;
        document["evt"] = "sys.boot";
        document["t"] = nowMs;
        JsonObject data = document.createNestedObject("data");
        data["device_id"] = deviceId_;
        data["fw"] = "0.1.0-serial-mock.1";
        data["proto"] = "1.0";
        data["state"] = "unprovisioned";
        data["reset_reason"] = resetReason_;
        data["mock"] = true;
        data["mock_stage"] = 1;
        serialLink.sendJson(document);
    }

    static void handle(void* context, const char* command, JsonObjectConst arguments,
                       JsonObject response, uint64_t nowMs) {
        static_cast<MockDevice*>(context)->handleCommand(command, arguments, response, nowMs);
    }

private:
    void handleCommand(const char* command, JsonObjectConst, JsonObject response,
                       uint64_t nowMs) {
        JsonObject result = response["result"].as<JsonObject>();
        if (strcmp(command, "sys.ping") == 0) {
            result["uptime_ms"] = nowMs;
        } else if (strcmp(command, "sys.status") == 0) {
            result["state"] = "unprovisioned";
            result["uptime_ms"] = nowMs;
            result["reset_reason"] = resetReason_;
            result["heap_free"] = ESP.getFreeHeap();
            result["battery_mv"] = nullptr;
            result["mock"] = true;
            result["mock_stage"] = 1;
            JsonObject radio = result.createNestedObject("radio");
            radio["running"] = false;
            radio["relay_only"] = false;
            JsonObject security = result.createNestedObject("security");
            security["secure_boot"] = esp_secure_boot_enabled();
            security["flash_encryption"] = esp_flash_encryption_enabled();
        } else {
            Commands::fail(response, "E_UNKNOWN_CMD", "command is unavailable in serial mock stage 1");
        }
    }

    char deviceId_[13] = {};
    const char* resetReason_ = "other";
};

MockDevice mockDevice;
Commands commands(serialLink, MockDevice::handle, &mockDevice);
}

void setup() {
#if !OFFGRID_SERIAL_NATIVE_USB
    hostPort.setRxBufferSize(4096);
    hostPort.setTxBufferSize(4096);
#endif
    hostPort.begin(BoardConfig::SerialBaud);
    serialLink.init();
    commands.init();
    mockDevice.init();
#if !OFFGRID_SERIAL_NATIVE_USB
    mockDevice.emitBoot(static_cast<uint64_t>(esp_timer_get_time()) / 1000);
#endif
}

void loop() {
    const uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000;
#if OFFGRID_SERIAL_NATIVE_USB
    if (!hostPort) {
        wasConnected = false;
        return;
    }
    if (!wasConnected) {
        serialLink.init();
        mockDevice.emitBoot(nowMs);
        wasConnected = true;
    }
#endif
    serialLink.tick(nowMs);
}