#include <Arduino.h>
#include <esp_flash_encrypt.h>
#include <esp_secure_boot.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform_util.h>

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
    pinMode(0, INPUT_PULLUP);
    rawButton_ = digitalRead(0) == LOW;
    stableButton_ = rawButton_;
    makePublicKey(publicKey_);
    uint8_t digest[32];
    mbedtls_sha256_ret(publicKey_, sizeof(publicKey_), digest, 0);
    snprintf(nodeId_, sizeof(nodeId_), "%02X%02X", digest[0], digest[1]);
    snprintf(name_, sizeof(name_), "Offgrid-%s", nodeId_);
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
        data["fw"] = "0.1.0-serial-mock.2";
        data["proto"] = "1.0";
        data["state"] = state();
        data["reset_reason"] = resetReason_;
        data["mock"] = true;
        data["mock_stage"] = 2;
        serialLink.sendJson(document);
    }

    void tick(uint64_t nowMs) {
        const bool pressed = digitalRead(0) == LOW;
        if (pressed != rawButton_) {
            rawButton_ = pressed;
            buttonChangedAt_ = nowMs;
        }
        if (rawButton_ != stableButton_ && nowMs - buttonChangedAt_ >= 30) {
            stableButton_ = rawButton_;
            if (stableButton_ && resetArmed_ && resetSawRelease_) holdAt_ = nowMs;
            if (!stableButton_) {
                resetSawRelease_ = true;
                holdAt_ = 0;
            }
        }
        if (resetArmed_ && nowMs - resetArmedAt_ >= 10000) resetArmed_ = false;
        if (resetArmed_ && stableButton_ && holdAt_ != 0 && nowMs - holdAt_ >= 3000) {
            provisioned_ = false;
            unlocked_ = false;
            session_ = false;
            resetArmed_ = false;
            lockTimeoutS_ = 15;
            snprintf(name_, sizeof(name_), "Offgrid-%s", nodeId_);
            stateReason_ = "factory_reset";
            rebootPending_ = true;
        }
        if (unlocked_ && lockTimeoutS_ != 0 && nowMs - lastActivity_ >= lockTimeoutS_ * 1000ULL) {
            unlocked_ = false;
            stateReason_ = "heartbeat_timeout";
        }
        if (stateReason_ != nullptr && serialLink.availableTxSlots() > 0) {
            StaticJsonDocument<384> event;
            event["evt"] = "sec.state";
            event["t"] = nowMs;
            event["data"]["state"] = state();
            event["data"]["reason"] = stateReason_;
            if (serialLink.sendJson(event)) stateReason_ = nullptr;
        }
        if (rebootPending_ && stateReason_ == nullptr && serialLink.availableTxSlots() > 0) {
            unlocked_ = false;
            session_ = false;
            resetArmed_ = false;
            resetReason_ = "software";
            emitBoot(nowMs);
            rebootPending_ = false;
        }
        serialLink.setReceiveEnabled(stateReason_ == nullptr && !rebootPending_);
    }

    static void handle(void* context, const char* command, JsonObjectConst arguments,
                       JsonObject response, uint64_t nowMs) {
        static_cast<MockDevice*>(context)->handleCommand(command, arguments, response, nowMs);
    }

private:
    static int rng(void*, unsigned char* output, size_t length) {
        esp_fill_random(output, length);
        return 0;
    }

    static bool makePublicKey(uint8_t output[32]) {
        mbedtls_ecp_keypair key;
        mbedtls_ecp_keypair_init(&key);
        const bool success = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_CURVE25519, &key, rng, nullptr) == 0 &&
            mbedtls_mpi_write_binary_le(&key.Q.X, output, 32) == 0;
        mbedtls_ecp_keypair_free(&key);
        return success;
    }

    static void encode(const uint8_t* input, size_t length, char* output, size_t capacity) {
        size_t written = 0;
        if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(output), capacity,
                                  &written, input, length) == 0) output[written] = '\0';
    }

    const char* state() const {
        return !provisioned_ ? "unprovisioned" : unlocked_ ? "unlocked" : "locked";
    }

    bool requireUnlocked(JsonObject response) const {
        if (unlocked_) return true;
        Commands::fail(response, provisioned_ ? "E_LOCKED" : "E_NOT_PROVISIONED",
                       "command requires an unlocked device");
        return false;
    }

    void identity(JsonObject output) const {
        char encoded[45] = {};
        encode(publicKey_, 32, encoded, sizeof(encoded));
        output["node_id"] = nodeId_;
        output["public_key"].set(encoded);
        output["name"] = name_;
    }

    void handleCommand(const char* command, JsonObjectConst args, JsonObject response,
                       uint64_t nowMs) {
        JsonObject result = response["result"].as<JsonObject>();
        lastActivity_ = nowMs;
        if (strcmp(command, "sys.ping") == 0) {
            result["uptime_ms"] = nowMs;
        } else if (strcmp(command, "sys.hello") == 0) {
            if (!Commands::stringArg(args, response, "app", 0, 128, false) ||
                !Commands::stringArg(args, response, "app_proto", 3, 24, false)) return;
            if (args.containsKey("app_proto")) {
                const char* version = args["app_proto"];
                const char* dot = strchr(version, '.');
                if (dot == nullptr || dot == version || dot[1] == '\0' ||
                    strspn(version, "0123456789") != static_cast<size_t>(dot - version) ||
                    strspn(dot + 1, "0123456789") != strlen(dot + 1)) {
                    Commands::fail(response, "E_INVALID_ARG", "expected major.minor", "app_proto");
                    return;
                }
                while (version < dot && *version == '0') ++version;
                if (dot - version != 1 || *version != '1') {
                    Commands::fail(response, "E_PROTO_MISMATCH", "incompatible major version");
                    return;
                }
            }
            uint8_t ephemeral[32];
            uint8_t nonce[16];
            if (!makePublicKey(ephemeral)) {
                Commands::fail(response, "E_INTERNAL", "mock session generation failed");
                return;
            }
            esp_fill_random(nonce, sizeof(nonce));
            char encodedPublic[45] = {};
            char encodedNonce[25] = {};
            encode(ephemeral, 32, encodedPublic, sizeof(encodedPublic));
            encode(nonce, 16, encodedNonce, sizeof(encodedNonce));
            session_ = true;
            result["device"] = "heltec_wifi_lora_32_v3";
            result["chip"] = "ESP32-S3";
            result["device_id"] = deviceId_;
            result["fw"]["version"] = "0.1.0-serial-mock.2";
            result["fw"]["build"] = __DATE__;
            result["proto"] = "1.0";
            result["state"] = state();
            result["mock"] = true;
            result["mock_stage"] = 2;
            result["authentication"] = "bypassed-envelope-shape-only";
            result["session"]["device_eph_pub"].set(encodedPublic);
            result["session"]["nonce"].set(encodedNonce);
            result["limits"]["max_line"] = 2048;
            result["limits"]["max_contacts"] = 32;
            result["limits"]["max_text_bytes"] = 83;
            result["limits"]["pending_queue"] = 8;
        } else if (strcmp(command, "sys.status") == 0) {
            result["state"] = state();
            result["uptime_ms"] = nowMs;
            result["reset_reason"] = resetReason_;
            result["heap_free"] = ESP.getFreeHeap();
            result["battery_mv"] = nullptr;
            result["mock"] = true;
            result["mock_stage"] = 2;
            JsonObject radio = result.createNestedObject("radio");
            radio["running"] = unlocked_;
            radio["relay_only"] = false;
            JsonObject security = result.createNestedObject("security");
            security["secure_boot"] = esp_secure_boot_enabled();
            security["flash_encryption"] = esp_flash_encryption_enabled();
            if (unlocked_) {
                result["queue"]["pending"] = 0;
                result["queue"]["capacity"] = 8;
                result["contacts"]["used"] = 0;
                result["contacts"]["capacity"] = 32;
            }
        } else if (strcmp(command, "sys.reboot") == 0) {
            rebootPending_ = true;
        } else if (strcmp(command, "sys.factory_reset") == 0) {
            if (!args["confirm"].is<const char*>() || strcmp(args["confirm"], "ERASE") != 0) {
                Commands::fail(response, "E_CONFIRM_REQUIRED", "confirm must be ERASE", "confirm");
                return;
            }
            resetArmed_ = true;
            resetArmedAt_ = nowMs;
            resetSawRelease_ = !rawButton_ && !stableButton_;
            holdAt_ = 0;
            result["armed"] = true;
            result["expires_s"] = 10;
            result["action"] = "hold PRG button for 3 s";
        } else if (strcmp(command, "sys.set_log_level") == 0) {
            if (!Commands::stringArg(args, response, "level", 1, 5)) return;
            const char* level = args["level"];
            if (strcmp(level, "off") && strcmp(level, "error") && strcmp(level, "warn") &&
                strcmp(level, "info") && strcmp(level, "debug")) {
                Commands::fail(response, "E_INVALID_ARG", "unknown log level", "level");
                return;
            }
            snprintf(logLevel_, sizeof(logLevel_), "%s", level);
            result["level"] = logLevel_;
        } else if (strcmp(command, "sec.provision") == 0 || strcmp(command, "sec.unlock") == 0 ||
                   strcmp(command, "sec.rotate_token") == 0) {
            const bool provision = strcmp(command, "sec.provision") == 0;
            const bool rotate = strcmp(command, "sec.rotate_token") == 0;
            if (provision && provisioned_) {
                Commands::fail(response, "E_ALREADY_PROVISIONED", "device is already provisioned");
                return;
            }
            if (!provision && !provisioned_) {
                Commands::fail(response, "E_NOT_PROVISIONED", "device has no mock vault");
                return;
            }
            if (!provision && !rotate && unlocked_) {
                Commands::fail(response, "E_ALREADY_UNLOCKED", "device is already unlocked");
                return;
            }
            if (rotate && !requireUnlocked(response)) return;
            if (!session_) {
                Commands::fail(response, "E_NO_SESSION", "call sys.hello for a fresh session");
                return;
            }
            session_ = false;
            if (!Commands::base64Arg(args, response, "app_eph_pub", 32) ||
                !Commands::base64Arg(args, response, "wrapped_token", 60) ||
                (provision && !Commands::stringArg(args, response, "name", 1, 24, false))) return;
            if (rotate) {
                result["rotated"] = true;
                return;
            }
            if (provision && args.containsKey("name")) snprintf(name_, sizeof(name_), "%s", args["name"].as<const char*>());
            provisioned_ = true;
            unlocked_ = true;
            identity(result);
            result["state"] = state();
            if (!provision) {
                result["contacts"] = 0;
                result["radio"]["running"] = true;
                result["radio"]["relay_only"] = false;
            }
            stateReason_ = provision ? "provision" : "unlock";
        } else if (strcmp(command, "sec.lock") == 0) {
            if (!requireUnlocked(response)) return;
            unlocked_ = false;
            result["state"] = state();
            stateReason_ = "lock";
        } else if (strcmp(command, "sec.get_policy") == 0 || strcmp(command, "sec.set_policy") == 0) {
            if (!requireUnlocked(response)) return;
            if (strcmp(command, "sec.set_policy") == 0) {
                if (!Commands::integerArg(args, response, "lock_timeout_s", 0, 3600, false) ||
                    !Commands::boolArg(args, response, "persist", false)) return;
                if (args.containsKey("lock_timeout_s")) lockTimeoutS_ = args["lock_timeout_s"];
            }
            result["lock_timeout_s"] = lockTimeoutS_;
        } else {
            Commands::fail(response, "E_UNKNOWN_CMD", "command is unavailable in this mock stage");
        }
    }

    char deviceId_[13] = {};
    uint8_t publicKey_[32] = {};
    char nodeId_[5] = {};
    char name_[25] = {};
    char logLevel_[6] = "off";
    bool provisioned_ = false;
    bool unlocked_ = false;
    bool session_ = false;
    uint32_t lockTimeoutS_ = 15;
    uint64_t lastActivity_ = 0;
    const char* stateReason_ = nullptr;
    bool rebootPending_ = false;
    bool resetArmed_ = false;
    bool resetSawRelease_ = false;
    bool rawButton_ = false;
    bool stableButton_ = false;
    uint64_t buttonChangedAt_ = 0;
    uint64_t resetArmedAt_ = 0;
    uint64_t holdAt_ = 0;
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
    mockDevice.tick(nowMs);
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