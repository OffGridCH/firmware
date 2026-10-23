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
#include <math.h>

#include "BoardConfig.h"
#include "host/Commands.h"
#include "host/SerialLink.h"
#include "storage/ConfigStore.h"
#include "drivers/RadioDriver.h"

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
#if OFFGRID_RADIO_DIAGNOSTICS
RadioDriver radioDriver;
#endif

class MockDevice {
public:
    void init() {
        resetConfig();
    #if OFFGRID_RADIO_DIAGNOSTICS
        radioDriver.init(radioConfig(config_["radio"].as<JsonObjectConst>()));
    #endif
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
        data["fw"] = "0.1.0-config-store.1";
        data["proto"] = "1.0";
        data["state"] = state();
        data["reset_reason"] = resetReason_;
        data["mock"] = true;
        data["mock_stage"] = 5;
        data["radio_diagnostics"] = OFFGRID_RADIO_DIAGNOSTICS != 0;
        data["config_storage"] = "nvs";
        data["config_status"] = configStore_.statusName();
        serialLink.sendJson(document);
    }

    void tick(uint64_t nowMs) {
        if ((!unlocked_ || nowMs - pairingAt_ >= 300000) && pairing_) {
            pairing_ = false;
            mbedtls_platform_zeroize(pairingSalt_, sizeof(pairingSalt_));
        }
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
            resetArmed_ = false;
            holdAt_ = 0;
            if (!configStore_.eraseAll()) {
                storageNotice_ = "configuration erase failed; factory reset cancelled";
            } else {
            provisioned_ = false;
            unlocked_ = false;
            session_ = false;
            configStore_.copyTo(config_, lockTimeoutS_);
            syncMesh();
#if OFFGRID_RADIO_DIAGNOSTICS
            radioDriver.standby();
            radioDriver.init(radioConfig(config_["radio"].as<JsonObjectConst>()));
#endif
            for (Contact& contact : contacts_) contact = Contact{};
            for (Message& message : messages_) message = Message{};
            incomingPending_ = false;
            contactEvent_ = -1;
            tracePending_ = false;
            traceEnabled_ = false;
            snprintf(name_, sizeof(name_), "Offgrid-%s", nodeId_);
            stateReason_ = "factory_reset";
            rebootPending_ = true;
            }
        }
        if (unlocked_ && lockTimeoutS_ != 0 && nowMs - lastActivity_ >= lockTimeoutS_ * 1000ULL) {
            unlocked_ = false;
            stateReason_ = "heartbeat_timeout";
#if OFFGRID_RADIO_DIAGNOSTICS
            radioDriver.standby();
#endif
        }
        if (stateReason_ != nullptr && serialLink.availableTxSlots() > 0) {
            StaticJsonDocument<384> event;
            event["evt"] = "sec.state";
            event["t"] = nowMs;
            event["data"]["state"] = state();
            event["data"]["reason"] = stateReason_;
            if (serialLink.sendJson(event)) stateReason_ = nullptr;
        }
        if (storageNotice_ != nullptr && serialLink.availableTxSlots() > 0) {
            if (serialLink.sendError("E_INTERNAL", storageNotice_, nowMs)) storageNotice_ = nullptr;
        }
        if (logPending_ && serialLink.availableTxSlots() > 0) {
            StaticJsonDocument<384> event;
            event["evt"] = "log";
            event["t"] = nowMs;
            event["data"]["level"] = logLevel_;
            event["data"]["msg"] = "mock log level updated";
            if (serialLink.sendJson(event)) logPending_ = false;
        }
        tickMessages(nowMs);
    #if OFFGRID_RADIO_DIAGNOSTICS
        tickRadio(nowMs);
    #endif
        if (rebootPending_ && stateReason_ == nullptr && serialLink.availableTxSlots() > 0) {
            unlocked_ = false;
            session_ = false;
            resetArmed_ = false;
            resetReason_ = "software";
            resetConfig();
#if OFFGRID_RADIO_DIAGNOSTICS
            radioDriver.standby();
            radioDriver.init(radioConfig(config_["radio"].as<JsonObjectConst>()));
#endif
            for (Message& message : messages_) message = Message{};
            incomingPending_ = false;
            pairing_ = false;
            tracePending_ = false;
            traceEnabled_ = false;
            emitBoot(nowMs);
            rebootPending_ = false;
        }
        serialLink.setReceiveEnabled(stateReason_ == nullptr && !rebootPending_ &&
                                     !incomingPending_ && contactEvent_ < 0 &&
                                     !tracePending_ && !logPending_ && storageNotice_ == nullptr);
    }

    static void handle(void* context, const char* command, JsonObjectConst arguments,
                       JsonObject response, uint64_t nowMs) {
        static_cast<MockDevice*>(context)->handleCommand(command, arguments, response, nowMs);
    }

private:
#if OFFGRID_RADIO_DIAGNOSTICS
    static void radioFailure(JsonObject response) {
        Commands::fail(response, radioDriver.lastError() == RadioDriver::Busy ? "E_BUSY" : "E_RADIO",
                       "radio operation failed; inspect diag.radio.status");
    }

    static void writeRadioStats(JsonObject output) {
        const RadioDriver::Stats& stats = radioDriver.stats();
        output["tx_packets"] = stats.transmitted;
        output["rx_packets"] = stats.received;
        output["rx_crc_errors"] = stats.crcErrors;
        output["rx_tag_failures"] = 0;
        output["rx_dedupe_drops"] = 0;
        output["relayed"] = 0;
        output["relay_suppressed"] = 0;
        output["acks_sent"] = 0;
        output["acks_received"] = 0;
        output["retries"] = 0;
        output["tx_airtime_ms_total"] = stats.txAirtimeMs;
        output["cad_scans"] = stats.cadScans;
        output["cad_busy"] = stats.cadBusy;
        output["failures"] = stats.failures;
        output["timeouts"] = stats.timeouts;
        output["dropped_events"] = stats.droppedEvents;
        if (stats.hasSignal) { output["last_rssi"] = stats.lastRssi; output["last_snr"] = stats.lastSnr; }
        else { output["last_rssi"] = nullptr; output["last_snr"] = nullptr; }
    }

    static int hexDigit(char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    void handleRadioDiagnostics(const char* command, JsonObjectConst args, JsonObject response,
                                uint64_t nowMs) {
        JsonObject output = response["result"].as<JsonObject>();
        if (strcmp(command, "diag.radio.status") == 0) {
            output["initialized"] = radioDriver.ready();
            output["state"] = radioDriver.stateName();
            output["last_error"] = radioDriver.lastError();
            output["config_pending"] = radioDriver.configPending();
            const RadioDriver::Config& applied = radioDriver.config();
            JsonObject settings = output.createNestedObject("applied");
            settings["freq_mhz"] = applied.frequencyMhz;
            settings["bw_khz"] = applied.bandwidthKhz;
            settings["sf"] = applied.spreadingFactor;
            settings["cr"] = applied.codingRate;
            settings["tx_power_dbm"] = applied.powerDbm;
            settings["preamble"] = applied.preamble;
            settings["sync_word"] = applied.syncWord;
            writeRadioStats(output.createNestedObject("stats"));
            output["diagnostic"] = true;
            return;
        }
        if (strcmp(command, "diag.radio.rx") == 0) {
            if (!radioDriver.startReceive()) { radioFailure(response); return; }
            output["state"] = radioDriver.stateName();
        } else if (strcmp(command, "diag.radio.stop") == 0) {
            if (!radioDriver.standby()) { radioFailure(response); return; }
            output["state"] = radioDriver.stateName();
        } else if (strcmp(command, "diag.radio.recover") == 0) {
            if (!radioDriver.init(radioConfig(config_["radio"].as<JsonObjectConst>()))) {
                radioFailure(response);
                return;
            }
            output["state"] = radioDriver.stateName();
        } else if (strcmp(command, "diag.radio.tx") == 0 || strcmp(command, "diag.radio.cad") == 0) {
            if (radioOperation_ != 0) {
                Commands::fail(response, "E_BUSY", "a radio diagnostic operation is pending");
                return;
            }
            if (nextRadioOperation_ == 0) {
                Commands::fail(response, "E_BUSY", "diagnostic reference exhausted; reset the board");
                return;
            }
            if (strcmp(command, "diag.radio.cad") == 0) {
                if (!radioDriver.startCad(nowMs)) { radioFailure(response); return; }
            } else {
                if (!Commands::stringArg(args, response, "hex", 2, 510) ||
                    !Commands::boolArg(args, response, "cad", false) ||
                    !Commands::integerArg(args, response, "backoff_slots", 0, 32, false)) return;
                const JsonString hex = args["hex"].as<JsonString>();
                if (hex.size() % 2 != 0) {
                    Commands::fail(response, "E_INVALID_ARG", "hex requires pairs of digits", "hex");
                    return;
                }
                uint8_t bytes[255];
                const size_t length = hex.size() / 2;
                for (size_t index = 0; index < length; ++index) {
                    const int high = hexDigit(hex.c_str()[index * 2]);
                    const int low = hexDigit(hex.c_str()[index * 2 + 1]);
                    if (high < 0 || low < 0) {
                        Commands::fail(response, "E_INVALID_ARG", "hex contains a non-hex digit", "hex");
                        return;
                    }
                    bytes[index] = (high << 4) | low;
                }
                if (!radioDriver.startTransmit(bytes, length, nowMs,
                    args["cad"] | config_["mesh"]["cad_enabled"].as<bool>(),
                    args["backoff_slots"] | config_["mesh"]["backoff_slots"].as<uint8_t>())) {
                    radioFailure(response);
                    return;
                }
                output["len"] = length;
                output["est_airtime_ms"] = RadioDriver::airtimeMs(radioDriver.config(), length);
            }
            radioOperation_ = nextRadioOperation_++;
            output["op_ref"] = radioOperation_;
            output["state"] = "queued";
        } else Commands::fail(response, "E_UNKNOWN_CMD", "unknown radio diagnostic command");
    }

    void tickRadio(uint64_t nowMs) {
        radioDriver.tick(nowMs);
        if (!radioEventPending_) radioEventPending_ = radioDriver.popEvent(radioEvent_);
        if (!radioEventPending_ || serialLink.availableTxSlots() == 0) return;
        StaticJsonDocument<2048> event;
        const RadioDriver::EventType type = radioEvent_.type;
        const char* name = type == RadioDriver::EventType::Received ? "diag.radio.rx" :
            type == RadioDriver::EventType::Transmitted ? "diag.radio.tx" :
            type == RadioDriver::EventType::Configured ? "diag.radio.configured" :
            type == RadioDriver::EventType::Failed ? "diag.radio.error" : "diag.radio.cad";
        event["evt"] = name;
        event["t"] = radioEvent_.timeMs;
        JsonObject data = event.createNestedObject("data");
        data["diagnostic"] = true;
        const bool operation = type != RadioDriver::EventType::Received && type != RadioDriver::EventType::Configured;
        if (operation && radioOperation_ != 0) data["op_ref"] = radioOperation_;
        if (type == RadioDriver::EventType::Received || type == RadioDriver::EventType::Transmitted) {
            char hex[511] = {};
            for (size_t index = 0; index < radioEvent_.length; ++index) {
                snprintf(hex + index * 2, 3, "%02X", radioEvent_.bytes[index]);
            }
            data["len"] = radioEvent_.length;
            data["hex"].set(hex);
            if (type == RadioDriver::EventType::Received) {
                data["rssi"] = radioEvent_.rssi;
                data["snr"] = radioEvent_.snr;
            } else data["airtime_ms"] = radioEvent_.airtimeMs;
        } else if (type == RadioDriver::EventType::Failed) {
            data["code"] = "E_RADIO";
            data["radio_error"] = radioEvent_.error;
        } else if (type == RadioDriver::EventType::CadClear || type == RadioDriver::EventType::CadBusy) {
            data["busy"] = type == RadioDriver::EventType::CadBusy;
        } else data["state"] = radioDriver.stateName();
        if (!serialLink.sendJson(event)) return;
        if (operation) radioOperation_ = 0;
        if (traceEnabled_ && (type == RadioDriver::EventType::Received || type == RadioDriver::EventType::Transmitted)) {
            event["evt"] = type == RadioDriver::EventType::Received ? "rf.rx" : "rf.tx";
            event["data"]["action"] = "diagnostic";
            if (serialLink.availableTxSlots() > 0) serialLink.sendJson(event);
        }
        radioEventPending_ = false;
    }
#endif

    static RadioDriver::Config radioConfig(JsonObjectConst values) {
        RadioDriver::Config configuration;
        configuration.frequencyMhz = values["freq_mhz"];
        configuration.bandwidthKhz = values["bw_khz"];
        configuration.spreadingFactor = values["sf"];
        configuration.codingRate = values["cr"];
        configuration.powerDbm = values["tx_power_dbm"];
        configuration.preamble = values["preamble"];
        configuration.syncWord = values["sync_word"];
        return configuration;
    }

    void resetConfig() {
                if (!configStore_.load()) {
                        storageNotice_ = "configuration load failed; using defaults with persistent writes disabled";
                }
                configStore_.copyTo(config_, lockTimeoutS_);
        syncMesh();
    }

    void syncMesh() {
        hopDefault_ = config_["mesh"]["hop_default"];
        retries_ = config_["mesh"]["retries"];
        ackText_ = config_["mesh"]["ack_default"]["text"];
        ackReaction_ = config_["mesh"]["ack_default"]["reaction"];
        ackGeo_ = config_["mesh"]["ack_default"]["geo"];
    }

    bool radioRunning() const {
#if OFFGRID_RADIO_DIAGNOSTICS
        return radioDriver.ready() && radioDriver.state() != RadioDriver::State::Standby;
#else
        return unlocked_ || (provisioned_ && config_["mesh"]["relay_when_locked"].as<bool>());
#endif
    }

    uint32_t airtime(size_t length) const {
        return RadioDriver::airtimeMs(radioConfig(config_["radio"].as<JsonObjectConst>()), length);
    }

    static bool pattern(JsonVariantConst value, JsonObject response, const char* field) {
        if (ConfigStore::validPattern(value)) return true;
        Commands::fail(response, "E_INVALID_ARG", "expected a built-in pattern or 1..16 valid steps and repeat 1..255", field);
        return false;
    }

    static void copyPattern(JsonVariant destination, JsonVariantConst source) {
        if (source.is<const char*>()) destination.set(source);
        else {
            JsonObject output = destination.to<JsonObject>();
            output["steps"].set(source["steps"]);
            output["repeat"] = source["repeat"] | 1;
        }
    }

    void handleConfig(const char* command, JsonObjectConst args, JsonObject response) {
        JsonObject result = response["result"].as<JsonObject>();
        if (strcmp(command, "dbg.rf_trace") == 0) {
            if (!requireUnlocked(response) || !Commands::boolArg(args, response, "enabled")) return;
            traceEnabled_ = args["enabled"];
            result["enabled"] = traceEnabled_;
            return;
        }
        if (strcmp(command, "radio.stats") == 0) {
            if (!requireUnlocked(response) || !Commands::boolArg(args, response, "reset", false)) return;
#if OFFGRID_RADIO_DIAGNOSTICS
            writeRadioStats(result);
            if (args["reset"] | false) radioDriver.resetStats();
            return;
#endif
            const char* counters[] = {"rx_crc_errors", "rx_tag_failures", "rx_dedupe_drops", "relay_suppressed"};
            for (const char* key : counters) result[key] = 0;
            result["tx_packets"] = txPackets_;
            result["rx_packets"] = rxPackets_;
            result["relayed"] = relayed_;
            result["acks_sent"] = acksSent_;
            result["acks_received"] = acksReceived_;
            result["retries"] = retryPackets_;
            result["tx_airtime_ms_total"] = txAirtime_;
            if (rxPackets_ > 0) { result["last_rssi"] = -92; result["last_snr"] = 7.5; }
            else { result["last_rssi"] = nullptr; result["last_snr"] = nullptr; }
            if (args["reset"] | false) {
                txPackets_ = rxPackets_ = relayed_ = acksSent_ = acksReceived_ = retryPackets_ = 0;
                txAirtime_ = 0;
            }
            return;
        }
        if (strcmp(command, "led.test") == 0) {
            if (!requireUnlocked(response) || !pattern(args["pattern"], response, "pattern") ||
                !Commands::integerArg(args, response, "repeat", 1, 255, false)) return;
            result["preview"] = true;
            result["repeat"] = args["repeat"] | 1;
            return;
        }
        const char* category = strncmp(command, "radio.", 6) == 0 ? "radio" :
            strncmp(command, "mesh.", 5) == 0 ? "mesh" : "led";
        const char* verb = strchr(command, '.');
        const bool setting = verb != nullptr && strcmp(verb, ".set") == 0;
        if (verb == nullptr || (!setting && strcmp(verb, ".get") != 0)) {
            Commands::fail(response, "E_UNKNOWN_CMD", "unknown configuration command");
            return;
        }
        if (setting && (!requireUnlocked(response) || !Commands::boolArg(args, response, "persist", false))) return;
        candidateConfig_.clear();
        candidateConfig_.set(config_[category]);
        JsonObject candidate = candidateConfig_.as<JsonObject>();
        if (setting && strcmp(category, "radio") == 0) {
            if (!Commands::stringArg(args, response, "preset", 1, 16, false)) return;
            if (args.containsKey("preset")) {
                const char* preset = args["preset"];
                if (strcmp(preset, "lab_default") && strcmp(preset, "fast") && strcmp(preset, "long_range")) {
                    Commands::fail(response, "E_INVALID_ARG", "unknown radio preset", "preset");
                    return;
                }
                candidate["freq_mhz"] = 869.525;
                candidate["bw_khz"] = strcmp(preset, "fast") == 0 ? 250 : 125;
                candidate["sf"] = strcmp(preset, "fast") == 0 ? 7 : strcmp(preset, "long_range") == 0 ? 11 : 9;
                candidate["cr"] = 5;
                candidate["tx_power_dbm"] = 14;
                candidate["preamble"] = 8;
                candidate["sync_word"] = 79;
            }
            if (!Commands::numberArg(args, response, "freq_mhz", 150, 960, false) ||
                !Commands::numberArg(args, response, "bw_khz", 62.5, 500, false) ||
                !Commands::integerArg(args, response, "sf", 5, 12, false) ||
                !Commands::integerArg(args, response, "cr", 5, 8, false) ||
                !Commands::integerArg(args, response, "tx_power_dbm", -9, 22, false) ||
                !Commands::integerArg(args, response, "preamble", 6, 64, false) ||
                !Commands::integerArg(args, response, "sync_word", 0, 255, false)) return;
            if (args.containsKey("bw_khz")) {
                const double bandwidth = args["bw_khz"];
                if (bandwidth != 62.5 && bandwidth != 125 && bandwidth != 250 && bandwidth != 500) {
                    Commands::fail(response, "E_INVALID_ARG", "unsupported bandwidth", "bw_khz");
                    return;
                }
            }
            const char* keys[] = {"freq_mhz", "bw_khz", "sf", "cr", "tx_power_dbm", "preamble", "sync_word"};
            for (const char* key : keys) if (args.containsKey(key)) candidate[key].set(args[key]);
        } else if (setting && strcmp(category, "mesh") == 0) {
            if (!Commands::integerArg(args, response, "hop_default", 0, 7, false) ||
                !Commands::integerArg(args, response, "backoff_slots", 0, 32, false) ||
                !Commands::integerArg(args, response, "retries", 0, 3, false) ||
                !Commands::integerArg(args, response, "retry_base_ms", 0, 60000, false) ||
                !Commands::integerArg(args, response, "rate_limit_per_src", 0, 255, false)) return;
            const char* flags[] = {"relay_enabled", "relay_when_locked", "cad_enabled"};
            for (const char* key : flags) if (!Commands::boolArg(args, response, key, false)) return;
            if (args.containsKey("ack_default")) {
                if (!args["ack_default"].is<JsonObjectConst>()) {
                    Commands::fail(response, "E_INVALID_ARG", "ack_default must be an object", "ack_default");
                    return;
                }
                JsonObjectConst defaults = args["ack_default"];
                const char* types[] = {"text", "reaction", "geo"};
                for (const char* key : types) {
                    if (!Commands::boolArg(defaults, response, key, false)) {
                        char field[40];
                        snprintf(field, sizeof(field), "ack_default.%s", key);
                        response["error"]["field"].set(field);
                        return;
                    }
                    if (defaults.containsKey(key)) candidate["ack_default"][key].set(defaults[key]);
                }
            }
            const char* keys[] = {"hop_default", "relay_enabled", "relay_when_locked", "cad_enabled", "backoff_slots", "retries", "retry_base_ms", "rate_limit_per_src"};
            for (const char* key : keys) if (args.containsKey(key)) candidate[key].set(args[key]);
        } else if (setting) {
            if (!Commands::boolArg(args, response, "enabled", false) ||
                !Commands::boolArg(args, response, "quiet", false) ||
                !Commands::integerArg(args, response, "brightness", 0, 255, false)) return;
            const char* groups[] = {"state_patterns", "event_patterns"};
            const char* states[] = {"unprovisioned", "locked", "unlocked"};
            const char* events[] = {"tx", "rx", "msg_received", "delivered", "failed", "error", "pairing"};
            for (size_t group = 0; group < 2; ++group) {
                if (!args.containsKey(groups[group])) continue;
                if (!args[groups[group]].is<JsonObjectConst>()) {
                    Commands::fail(response, "E_INVALID_ARG", "patterns must be an object", groups[group]);
                    return;
                }
                const char* const* keys = group == 0 ? states : events;
                for (size_t index = 0; index < (group == 0 ? 3 : 7); ++index) {
                    if (!args[groups[group]].containsKey(keys[index])) continue;
                    char field[64];
                    snprintf(field, sizeof(field), "%s.%s", groups[group], keys[index]);
                    JsonVariantConst value = args[groups[group]][keys[index]];
                    if (!pattern(value, response, field)) return;
                    copyPattern(candidate[groups[group]][keys[index]], value);
                }
            }
            const char* keys[] = {"enabled", "quiet", "brightness"};
            for (const char* key : keys) if (args.containsKey(key)) candidate[key].set(args[key]);
        }
        result.set(candidate);
        if (strcmp(category, "radio") == 0) {
            result["est_airtime_ms_23b"] = RadioDriver::airtimeMs(radioConfig(candidate), 23);
            result["max_text_bytes"] = 83;
            if (setting) {
                JsonArray warnings = result.createNestedArray("warnings");
                const double frequency = candidate["freq_mhz"];
                if (frequency < 863 || frequency > 870) warnings.add("outside_eu868");
                if (candidate["tx_power_dbm"].as<int>() > 14) warnings.add("above_eu868_power_limit");
                if (candidate["sync_word"] == 18 || candidate["sync_word"] == 52) warnings.add("reserved_sync_word");
                warnings.add("mesh_incompatible");
            }
        } else if (strcmp(category, "led") == 0) {
            JsonArray names = result.createNestedArray("builtin_patterns");
            const char* builtins[] = {"off", "on", "flash", "blink_slow", "blink_fast", "double", "heartbeat", "pulse"};
            for (const char* name : builtins) names.add(name);
        }
        if (candidateConfig_.overflowed() || measureJson(response) + 1 > BoardConfig::SerialMaxLineBytes) {
            Commands::fail(response, "E_TOO_LONG", "configuration response exceeds the serial line limit");
            return;
        }
        if (setting) {
#if OFFGRID_RADIO_DIAGNOSTICS
            if (strcmp(category, "radio") == 0 && !radioDriver.ready()) {
                Commands::fail(response, "E_RADIO", "recover the hardware before changing radio settings");
                return;
            }
#endif
            configScratch_.set(config_);
            configScratch_[category].set(candidate);
            if (configScratch_.overflowed()) {
                Commands::fail(response, "E_INTERNAL", "configuration capacity exceeded");
                return;
            }
            if ((args["persist"] | true) && !configStore_.saveCategory(category, candidate)) {
                Commands::fail(response, "E_INTERNAL", "configuration storage failed; live settings unchanged");
                return;
            }
            config_.set(configScratch_);
#if OFFGRID_RADIO_DIAGNOSTICS
            if (strcmp(category, "radio") == 0) {
                radioDriver.queueConfig(radioConfig(candidate));
                result["apply_pending"] = true;
            }
#endif
            syncMesh();
        }
    }

    struct Contact {
        bool used = false;
        bool active = false;
        uint8_t publicKey[32] = {};
        char name[25] = {};
        char nodeId[5] = {};
        char fingerprint[15] = {};
        uint32_t txCounter = 0;
        uint32_t rxCounter = 0;
    };

    struct Message {
        bool used = false;
        uint8_t contact = 0;
        char type[9] = {};
        char text[84] = {};
        char emoji[9] = {};
        bool removeReaction = false;
        bool targetOwn = false;
        uint16_t targetRef = 0;
        double lat = 0;
        double lon = 0;
        bool hasAltitude = false;
        int16_t altitude = 0;
        bool hasAge = false;
        uint16_t age = 0;
        uint32_t reference = 0;
        uint32_t counter = 0;
        uint8_t hops = 3;
        bool ack = true;
        uint8_t retry = 0;
        uint8_t nextEvent = 0;
        uint64_t created = 0;
        uint64_t due = 0;
        const char* status = "queued";
        const char* outcome = "delivered";
        const char* failure = nullptr;
    };

    size_t pendingCount() const {
        size_t count = 0;
        for (const Message& message : messages_) if (message.used) ++count;
        return count;
    }

    static size_t packetBytes(const Message& message) {
        return strcmp(message.type, "text") == 0 ? 17 + strlen(message.text) :
            strcmp(message.type, "reaction") == 0 ? 20 :
            23 + (message.hasAltitude ? 2 : 0) + (message.hasAge ? 1 : 0);
    }

    void queueTrace(const Message& message, bool transmit, bool ack = false) {
        if (!traceEnabled_) return;
        traceMessage_ = message;
        traceTransmit_ = transmit;
        traceAck_ = ack;
        tracePending_ = true;
    }

    bool readPayload(JsonObjectConst args, JsonObject response, Message& message) {
        if (!Commands::stringArg(args, response, "type", 1, 8)) return false;
        const char* type = args["type"];
        snprintf(message.type, sizeof(message.type), "%s", type);
        if (strcmp(type, "text") == 0) {
            if (!Commands::stringArg(args, response, "text", 0, 2047)) return false;
            if (args["text"].as<JsonString>().size() > 83) {
                Commands::fail(response, "E_TOO_LONG", "text exceeds 83 UTF-8 bytes", "text");
                return false;
            }
            snprintf(message.text, sizeof(message.text), "%s", args["text"].as<const char*>());
        } else if (strcmp(type, "reaction") == 0) {
            if (!args["target"].is<JsonObject>()) {
                Commands::fail(response, "E_INVALID_ARG", "target must be an object", "target");
                return false;
            }
            const JsonObjectConst target = args["target"];
            if (!Commands::stringArg(target, response, "author", 1, 4)) {
                response["error"]["field"] = "target.author";
                return false;
            }
            if (strcmp(target["author"], "me") != 0 && strcmp(target["author"], "peer") != 0) {
                Commands::fail(response, "E_INVALID_ARG", "author must be me or peer", "target.author");
                return false;
            }
            if (!Commands::integerArg(target, response, "ref", 0, 65535)) {
                response["error"]["field"] = "target.ref";
                return false;
            }
            message.targetOwn = strcmp(target["author"], "me") == 0;
            message.targetRef = target["ref"];
            if (!args.containsKey("emoji")) {
                Commands::fail(response, "E_INVALID_ARG", "emoji is required, null removes it", "emoji");
                return false;
            }
            message.removeReaction = args["emoji"].isNull();
            if (!message.removeReaction) {
                if (!Commands::stringArg(args, response, "emoji", 1, 8)) return false;
                const char* emoji = args["emoji"];
                const char* allowed[] = {"\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F",
                    "\xF0\x9F\x98\x82", "\xF0\x9F\x98\xAE", "\xF0\x9F\x98\xA2", "\xF0\x9F\x99\x8F"};
                bool found = false;
                for (const char* entry : allowed) if (strcmp(emoji, entry) == 0) found = true;
                if (!found) {
                    Commands::fail(response, "E_INVALID_ARG", "emoji is not in the protocol table", "emoji");
                    return false;
                }
                snprintf(message.emoji, sizeof(message.emoji), "%s", emoji);
            }
        } else if (strcmp(type, "geo") == 0) {
            if (!Commands::numberArg(args, response, "lat", -90, 90) ||
                !Commands::numberArg(args, response, "lon", -180, 180)) return false;
            message.lat = args["lat"];
            message.lon = args["lon"];
            message.hasAltitude = args.containsKey("alt_m") && !args["alt_m"].isNull();
            message.hasAge = args.containsKey("age_s") && !args["age_s"].isNull();
            if (message.hasAltitude && !Commands::integerArg(args, response, "alt_m", -32768, 32767)) return false;
            if (message.hasAge && !Commands::integerArg(args, response, "age_s", 0, 2550)) return false;
            message.altitude = args["alt_m"] | 0;
            message.age = ((args["age_s"] | 0) + 5) / 10 * 10;
        } else {
            Commands::fail(response, "E_INVALID_ARG", "type must be text, reaction or geo", "type");
            return false;
        }
        return true;
    }

    void writePayload(JsonObject output, const Message& message) const {
        output["type"] = message.type;
        if (strcmp(message.type, "text") == 0) output["text"] = message.text;
        else if (strcmp(message.type, "reaction") == 0) {
            output["target"]["author"] = message.targetOwn ? "me" : "peer";
            output["target"]["ref"] = message.targetRef;
            if (message.removeReaction) output["emoji"] = nullptr;
            else output["emoji"] = message.emoji;
        } else {
            output["lat"] = message.lat;
            output["lon"] = message.lon;
            if (message.hasAltitude) output["alt_m"] = message.altitude;
            else output["alt_m"] = nullptr;
            if (message.hasAge) output["age_s"] = message.age;
            else output["age_s"] = nullptr;
        }
    }

    void failMessages(size_t contact, const char* reason) {
        for (Message& message : messages_) {
            if (message.used && message.contact == contact) message.failure = reason;
        }
    }

    void tickMessages(uint64_t nowMs) {
        if (!unlocked_) {
            incomingPending_ = false;
            contactEvent_ = -1;
            tracePending_ = false;
            for (Message& message : messages_) if (message.used && message.failure == nullptr) message.failure = "locked";
        }
        if (serialLink.availableTxSlots() == 0) return;
        if (tracePending_) {
            const size_t length = traceAck_ ? 16 : packetBytes(traceMessage_);
            uint8_t wire[100] = {};
            wire[0] = (traceMessage_.hops << 5) | (traceMessage_.hops << 2) | traceMessage_.retry;
            unsigned own = 0;
            unsigned peer = 0;
            sscanf(nodeId_, "%x", &own);
            sscanf(contacts_[traceMessage_.contact].nodeId, "%x", &peer);
            const unsigned destination = traceTransmit_ ? peer : own;
            const unsigned source = traceTransmit_ ? own : peer;
            wire[1] = destination >> 8;
            wire[2] = destination;
            wire[3] = source >> 8;
            wire[4] = source;
            wire[5] = traceMessage_.counter >> 16;
            wire[6] = traceMessage_.counter >> 8;
            wire[7] = traceMessage_.counter;
            for (size_t index = 8; index < length; ++index) wire[index] = 0xa5 ^ index;
            char hex[201] = {};
            for (size_t index = 0; index < length; ++index) snprintf(hex + index * 2, 3, "%02X", wire[index]);
            StaticJsonDocument<1024> event;
            event["evt"] = traceTransmit_ ? "rf.tx" : "rf.rx";
            event["t"] = nowMs;
            JsonObject data = event.createNestedObject("data");
            data["len"] = length;
            data["hex"].set(hex);
            data["mock"] = true;
            if (traceTransmit_) data["airtime_ms"] = airtime(length);
            else {
                data["rssi"] = -92;
                data["snr"] = 7.5;
                data["action"] = traceAck_ ? "ack_matched" : "delivered";
            }
            if (serialLink.sendJson(event)) tracePending_ = false;
            return;
        }
        if (contactEvent_ >= 0) {
            StaticJsonDocument<256> event;
            event["evt"] = "contact.state";
            event["t"] = nowMs;
            event["data"]["contact_id"] = contactEvent_ + 1;
            event["data"]["state"] = "active";
            if (serialLink.sendJson(event)) contactEvent_ = -1;
            return;
        }
        if (incomingPending_) {
            StaticJsonDocument<1024> event;
            event["evt"] = "msg.received";
            event["t"] = nowMs;
            JsonObject data = event.createNestedObject("data");
            writePayload(data, incoming_);
            data["from"] = incoming_.contact + 1;
            data["ctr"] = incoming_.counter;
            data["hops"] = incoming_.hops;
            data["retry"] = incoming_.retry;
            data["rssi"] = -92;
            data["snr"] = 7.5;
            if (serialLink.sendJson(event)) {
                Contact& contact = contacts_[incoming_.contact];
                contact.rxCounter = incoming_.counter;
                ++rxPackets_;
                if (incoming_.ack) { ++acksSent_; ++txPackets_; txAirtime_ += airtime(16); }
                queueTrace(incoming_, false);
                if (!contact.active) { contact.active = true; contactEvent_ = incoming_.contact; }
                incomingPending_ = false;
            }
            return;
        }
        for (Message& message : messages_) {
            if (!message.used || (message.failure == nullptr && nowMs < message.due)) continue;
            const char* status = message.failure != nullptr ? "failed" :
                message.nextEvent == 3 ? "cancelled" : message.nextEvent == 0 ?
                (message.retry == 0 ? "sent" : "retrying") : message.nextEvent == 1 ? "relayed" : "delivered";
            if (message.nextEvent == 2 && message.failure == nullptr && strcmp(message.outcome, "timeout") == 0) {
                if (message.retry < retries_) {
                    ++message.retry;
                    message.nextEvent = 0;
                    message.due = nowMs + 400;
                    continue;
                }
                message.failure = "timeout";
                status = "failed";
            }
            if (message.nextEvent == 0 && strcmp(message.outcome, "radio_error") == 0) {
                message.failure = "radio_error";
                status = "failed";
            }
            if (message.nextEvent == 1 && message.hops == 0 && message.failure == nullptr) {
                message.nextEvent = 2;
                message.due = nowMs + 250;
                continue;
            }
            StaticJsonDocument<512> event;
            event["evt"] = "msg.status";
            event["t"] = nowMs;
            JsonObject data = event.createNestedObject("data");
            data["msg_ref"] = message.reference;
            data["to"] = message.contact + 1;
            data["state"] = status;
            data["retry"] = message.retry;
            if (message.failure != nullptr) data["reason"] = message.failure;
            if (strcmp(status, "delivered") == 0) {
                data["hops"] = message.hops == 0 ? 0 : 1;
                data["rtt_ms"] = nowMs - message.created;
            }
            if (!serialLink.sendJson(event)) return;
            if (strcmp(status, "sent") == 0 || strcmp(status, "retrying") == 0) {
                ++txPackets_;
                txAirtime_ += airtime(packetBytes(message));
                if (message.retry > 0) ++retryPackets_;
                queueTrace(message, true);
            } else if (strcmp(status, "delivered") == 0) {
                ++acksReceived_;
                ++rxPackets_;
                queueTrace(message, false, true);
            }
            if (message.failure != nullptr || message.nextEvent == 3 || message.nextEvent == 2 ||
                (!message.ack && message.nextEvent == 0)) {
                if (strcmp(status, "delivered") == 0 && !contacts_[message.contact].active) {
                    contacts_[message.contact].active = true;
                    contactEvent_ = message.contact;
                }
                message = Message{};
            } else {
                message.status = status;
                ++message.nextEvent;
                message.due = nowMs + 250;
            }
            return;
        }
    }

    void handleMessages(const char* command, JsonObjectConst args, JsonObject response,
                        uint64_t nowMs) {
        if (!requireUnlocked(response)) return;
        JsonObject result = response["result"].as<JsonObject>();
        if (strcmp(command, "mock.set_scenario") == 0) {
            if (!Commands::stringArg(args, response, "outcome", 1, 11)) return;
            const char* value = args["outcome"];
            if (strcmp(value, "delivered") && strcmp(value, "timeout") && strcmp(value, "radio_error")) {
                Commands::fail(response, "E_INVALID_ARG", "unknown mock delivery outcome", "outcome");
                return;
            }
            outcome_ = strcmp(value, "timeout") == 0 ? "timeout" : strcmp(value, "radio_error") == 0 ? "radio_error" : "delivered";
            result["outcome"] = outcome_;
        } else if (strcmp(command, "msg.pending") == 0) {
            JsonArray pending = result.createNestedArray("pending");
            for (const Message& message : messages_) {
                if (!message.used) continue;
                JsonObject item = pending.createNestedObject();
                item["msg_ref"] = message.reference;
                item["to"] = message.contact + 1;
                item["type"] = message.type;
                item["ctr"] = message.counter;
                item["state"] = message.status;
                item["retry"] = message.retry;
            }
        } else if (strcmp(command, "msg.cancel") == 0) {
            if (!Commands::integerArg(args, response, "msg_ref", 1, 4294967295LL)) return;
            for (Message& message : messages_) {
                if (message.used && message.reference == args["msg_ref"].as<uint32_t>()) {
                    message.nextEvent = 3;
                    message.failure = nullptr;
                    message.due = nowMs;
                    result["state"] = "cancelled";
                    return;
                }
            }
            Commands::fail(response, "E_INVALID_ARG", "message is not pending", "msg_ref");
        } else if (strcmp(command, "msg.send") == 0 || strcmp(command, "mock.receive") == 0) {
            const bool receiving = strcmp(command, "mock.receive") == 0;
            const char* field = receiving ? "from" : "to";
            if (!Commands::integerArg(args, response, field, 1, 32)) return;
            const size_t contact = args[field].as<size_t>() - 1;
            if (!contacts_[contact].used) {
                Commands::fail(response, "E_NO_SUCH_CONTACT", "contact slot is empty", field);
                return;
            }
            Message candidate;
            candidate.contact = contact;
            if (!readPayload(args, response, candidate) ||
                !Commands::integerArg(args, response, "hops", 0, 7, false) ||
                !Commands::boolArg(args, response, "ack", false)) return;
            candidate.hops = args["hops"] | hopDefault_;
            candidate.ack = args["ack"] | (strcmp(candidate.type, "text") == 0 ? ackText_ :
                strcmp(candidate.type, "reaction") == 0 ? ackReaction_ : ackGeo_);
            if (receiving) {
                if (!Commands::integerArg(args, response, "ctr", 1, 16777215, false) ||
                    !Commands::integerArg(args, response, "retry", 0, 3, false)) return;
                candidate.counter = args["ctr"] | (contacts_[contact].rxCounter + 1);
                if (candidate.counter > 16777215 || candidate.counter <= contacts_[contact].rxCounter) {
                    Commands::fail(response, "E_INVALID_ARG", "mock incoming counter must increase", "ctr");
                    return;
                }
                candidate.retry = args["retry"] | 0;
                incoming_ = candidate;
                incomingPending_ = true;
                result["injected"] = true;
                return;
            }
            Message* slot = nullptr;
            for (Message& message : messages_) if (!message.used) { slot = &message; break; }
            if (slot == nullptr) {
                Commands::fail(response, "E_QUEUE_FULL", "all eight mock message slots are used");
                return;
            }
            if (contacts_[contact].txCounter == 16777215 || nextReference_ == 0) {
                Commands::fail(response, "E_BUSY", "mock counter exhausted; re-pair or reset");
                return;
            }
            candidate.used = true;
            candidate.counter = ++contacts_[contact].txCounter;
            candidate.reference = nextReference_++;
            candidate.created = nowMs;
            candidate.due = nowMs + 100;
            candidate.outcome = outcome_;
            *slot = candidate;
            const size_t length = strcmp(candidate.type, "text") == 0 ? 17 + strlen(candidate.text) :
                strcmp(candidate.type, "reaction") == 0 ? 20 : 23 + (candidate.hasAltitude ? 2 : 0) + (candidate.hasAge ? 1 : 0);
            result["msg_ref"] = candidate.reference;
            result["ctr"] = candidate.counter;
            result["ack_requested"] = candidate.ack;
            result["hops"] = candidate.hops;
            result["state"] = "queued";
            result["bytes_on_air"] = length;
            result["est_airtime_ms"] = airtime(length);
        } else Commands::fail(response, "E_UNKNOWN_CMD", "unknown message/mock command");
    }

    size_t contactCount() const {
        size_t count = 0;
        for (const Contact& contact : contacts_) if (contact.used) ++count;
        return count;
    }

    void writeContact(JsonObject output, size_t index) const {
        const Contact& contact = contacts_[index];
        char encoded[45] = {};
        encode(contact.publicKey, 32, encoded, sizeof(encoded));
        output["contact_id"] = index + 1;
        output["name"] = contact.name;
        output["node_id"] = contact.nodeId;
        output["public_key"].set(encoded);
        output["state"] = contact.active ? "active" : "pending";
        output["fingerprint"] = contact.fingerprint;
        output["tx_ctr"] = contact.txCounter;
        if (contact.rxCounter != 0) output["rx_highest_ctr"] = contact.rxCounter;
        else output["rx_highest_ctr"] = nullptr;
        if (contact.active) { output["last_rssi"] = -92; output["last_snr"] = 7.5; }
        else { output["last_rssi"] = nullptr; output["last_snr"] = nullptr; }
    }

    void handleContacts(const char* command, JsonObjectConst args, JsonObject response,
                        uint64_t nowMs) {
        if (!requireUnlocked(response)) return;
        JsonObject result = response["result"].as<JsonObject>();
        if (strcmp(command, "id.get") == 0) {
            identity(result);
        } else if (strcmp(command, "id.set_name") == 0) {
            if (!Commands::stringArg(args, response, "name", 1, 24)) return;
            snprintf(name_, sizeof(name_), "%s", args["name"].as<const char*>());
            identity(result);
        } else if (strcmp(command, "pair.begin") == 0) {
            uint8_t raw[75] = {};
            memcpy(raw, "OG1", 3);
            memcpy(raw + 3, publicKey_, 32);
            esp_fill_random(pairingSalt_, sizeof(pairingSalt_));
            memcpy(raw + 35, pairingSalt_, 16);
            memcpy(raw + 51, name_, strlen(name_));
            char qr[101] = {};
            const size_t length = 51 + strlen(name_);
            encode(raw, length, qr, sizeof(qr));
            pairing_ = true;
            pairingAt_ = nowMs;
            result["qr"].set(qr);
            result["qr_bytes"] = length;
            result["expires_s"] = 300;
        } else if (strcmp(command, "pair.cancel") == 0) {
            pairing_ = false;
            mbedtls_platform_zeroize(pairingSalt_, sizeof(pairingSalt_));
            result["cancelled"] = true;
        } else if (strcmp(command, "pair.complete") == 0) {
            if (!pairing_ || nowMs - pairingAt_ >= 300000) {
                Commands::fail(response, "E_NO_PAIRING", "call pair.begin first");
                return;
            }
            if (!Commands::stringArg(args, response, "qr", 1, 100) ||
                !Commands::stringArg(args, response, "name", 1, 24, false) ||
                !Commands::boolArg(args, response, "replace", false)) return;
            uint8_t raw[75] = {};
            size_t length = 0;
            char canonical[101] = {};
            const char* qr = args["qr"];
            const bool decoded = mbedtls_base64_decode(raw, sizeof(raw), &length,
                reinterpret_cast<const unsigned char*>(qr), strlen(qr)) == 0;
            if (decoded) encode(raw, length, canonical, sizeof(canonical));
            if (!decoded || length < 51 || length > 75 || strcmp(qr, canonical) != 0 ||
                memcmp(raw, "OG1", 3) != 0 || memchr(raw + 51, 0, length - 51) != nullptr) {
                Commands::fail(response, "E_INVALID_QR", "invalid OG1 QR payload", "qr");
                return;
            }
            if (memcmp(raw + 3, publicKey_, 32) == 0) {
                Commands::fail(response, "E_SELF_PAIR", "cannot pair with own identity");
                return;
            }
            size_t slot = 32;
            for (size_t index = 0; index < 32; ++index) {
                if (contacts_[index].used && memcmp(contacts_[index].publicKey, raw + 3, 32) == 0) {
                    if (!(args["replace"] | false)) {
                        Commands::fail(response, "E_CONTACT_EXISTS", "contact already exists");
                        return;
                    }
                    slot = index;
                    break;
                }
            }
            if (slot == 32) {
                for (size_t index = 0; index < 32; ++index) {
                    if (!contacts_[index].used) { slot = index; break; }
                }
            }
            if (slot == 32) {
                Commands::fail(response, "E_STORAGE_FULL", "all mock contact slots are used");
                return;
            }
            Contact candidate;
            candidate.used = true;
            memcpy(candidate.publicKey, raw + 3, 32);
            uint8_t hash[32];
            mbedtls_sha256_ret(candidate.publicKey, 32, hash, 0);
            snprintf(candidate.nodeId, sizeof(candidate.nodeId), "%02X%02X", hash[0], hash[1]);
            if (args.containsKey("name")) snprintf(candidate.name, sizeof(candidate.name), "%s", args["name"].as<const char*>());
            else if (length > 51) memcpy(candidate.name, raw + 51, length - 51);
            else snprintf(candidate.name, sizeof(candidate.name), "Offgrid-%s", candidate.nodeId);
            uint8_t fingerprintInput[70];
            memcpy(fingerprintInput, "og1-fp", 6);
            const bool ownFirst = memcmp(publicKey_, candidate.publicKey, 32) < 0;
            memcpy(fingerprintInput + 6, ownFirst ? publicKey_ : candidate.publicKey, 32);
            memcpy(fingerprintInput + 38, ownFirst ? candidate.publicKey : publicKey_, 32);
            mbedtls_sha256_ret(fingerprintInput, sizeof(fingerprintInput), hash, 0);
            uint64_t digits = 0;
            for (size_t index = 0; index < 8; ++index) digits = (digits << 8) | hash[index];
            digits %= 1000000000000ULL;
            snprintf(candidate.fingerprint, sizeof(candidate.fingerprint), "%04u %04u %04u",
                static_cast<unsigned>(digits / 100000000),
                static_cast<unsigned>((digits / 10000) % 10000), static_cast<unsigned>(digits % 10000));
            bool collision = strcmp(candidate.nodeId, nodeId_) == 0;
            for (size_t index = 0; index < 32; ++index) {
                if (index != slot && contacts_[index].used && strcmp(contacts_[index].nodeId, candidate.nodeId) == 0) collision = true;
            }
            failMessages(slot, "contact_deleted");
            contacts_[slot] = candidate;
            writeContact(result.createNestedObject("contact"), slot);
            JsonArray warnings = result.createNestedArray("warnings");
            if (collision) warnings.add("node_id_collision");
            pairing_ = false;
            mbedtls_platform_zeroize(pairingSalt_, sizeof(pairingSalt_));
        } else if (strcmp(command, "contact.list") == 0) {
            if (!Commands::integerArg(args, response, "offset", 0, 32, false) ||
                !Commands::integerArg(args, response, "limit", 1, 10, false)) return;
            const size_t offset = args["offset"] | 0;
            const size_t limit = args["limit"] | 10;
            result["total"] = contactCount();
            result["offset"] = offset;
            JsonArray list = result.createNestedArray("contacts");
            size_t skipped = 0;
            for (size_t index = 0; index < 32 && list.size() < limit; ++index) {
                if (!contacts_[index].used) continue;
                if (skipped++ < offset) continue;
                writeContact(list.createNestedObject(), index);
                if (measureJson(response) + 1 > BoardConfig::SerialMaxLineBytes) {
                    list.remove(list.size() - 1);
                    break;
                }
            }
        } else if (strcmp(command, "contact.get") == 0 || strcmp(command, "contact.rename") == 0 ||
                   strcmp(command, "contact.delete") == 0) {
            if (!Commands::integerArg(args, response, "contact_id", 1, 32)) return;
            const size_t index = args["contact_id"].as<size_t>() - 1;
            if (!contacts_[index].used) {
                Commands::fail(response, "E_NO_SUCH_CONTACT", "contact slot is empty", "contact_id");
                return;
            }
            if (strcmp(command, "contact.rename") == 0) {
                if (!Commands::stringArg(args, response, "name", 1, 24)) return;
                snprintf(contacts_[index].name, sizeof(contacts_[index].name), "%s", args["name"].as<const char*>());
            }
            if (strcmp(command, "contact.delete") == 0) {
                if (args.containsKey("confirm") && !Commands::boolArg(args, response, "confirm")) return;
                if (!(args["confirm"] | false)) {
                    Commands::fail(response, "E_CONFIRM_REQUIRED", "confirm must be true", "confirm");
                    return;
                }
                failMessages(index, "contact_deleted");
                contacts_[index] = Contact{};
                result["deleted"] = true;
            } else writeContact(result.createNestedObject("contact"), index);
        } else Commands::fail(response, "E_UNKNOWN_CMD", "unknown identity/pairing/contact command");
    }

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
#if OFFGRID_RADIO_DIAGNOSTICS
        if (strncmp(command, "diag.radio.", 11) == 0) {
            handleRadioDiagnostics(command, args, response, nowMs);
            return;
        }
        if (strncmp(command, "msg.", 4) == 0 || strncmp(command, "mock.", 5) == 0) {
            Commands::fail(response, "E_BUSY", "mock chat is disabled in the real radio diagnostic image");
            return;
        }
#endif
        if (strncmp(command, "radio.", 6) == 0 || strncmp(command, "mesh.", 5) == 0 ||
            strncmp(command, "led.", 4) == 0 || strncmp(command, "dbg.", 4) == 0) {
            handleConfig(command, args, response);
        } else if (strncmp(command, "msg.", 4) == 0 || strncmp(command, "mock.", 5) == 0) {
            handleMessages(command, args, response, nowMs);
        } else if (strncmp(command, "id.", 3) == 0 || strncmp(command, "pair.", 5) == 0 ||
            strncmp(command, "contact.", 8) == 0) {
            handleContacts(command, args, response, nowMs);
        } else if (strcmp(command, "sys.ping") == 0) {
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
            result["fw"]["version"] = "0.1.0-config-store.1";
            result["fw"]["build"] = __DATE__;
            result["proto"] = "1.0";
            result["state"] = state();
            result["mock"] = true;
            result["mock_stage"] = 5;
            result["radio_diagnostics"] = OFFGRID_RADIO_DIAGNOSTICS != 0;
            result["config_storage"] = "nvs";
            result["config_schema"] = 1;
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
            result["mock_stage"] = 5;
            result["storage"]["backend"] = "nvs";
            result["storage"]["config_status"] = configStore_.statusName();
            result["storage"]["writable"] = configStore_.writable();
            result["storage"]["last_error"] = configStore_.lastError();
            JsonObject radio = result.createNestedObject("radio");
            radio["running"] = radioRunning();
            radio["relay_only"] = radioRunning() && !unlocked_;
#if OFFGRID_RADIO_DIAGNOSTICS
            radio["initialized"] = radioDriver.ready();
            radio["state"] = radioDriver.stateName();
            radio["last_error"] = radioDriver.lastError();
            radio["relay_only"] = false;
#endif
            JsonObject security = result.createNestedObject("security");
            security["secure_boot"] = esp_secure_boot_enabled();
            security["flash_encryption"] = esp_flash_encryption_enabled();
            if (unlocked_) {
                result["queue"]["pending"] = pendingCount();
                result["queue"]["capacity"] = 8;
                result["contacts"]["used"] = contactCount();
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
            logPending_ = strcmp(logLevel_, "off") != 0;
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
                result["contacts"] = contactCount();
                result["radio"]["running"] = true;
                result["radio"]["relay_only"] = false;
            }
            stateReason_ = provision ? "provision" : "unlock";
        } else if (strcmp(command, "sec.lock") == 0) {
            if (!requireUnlocked(response)) return;
            unlocked_ = false;
            result["state"] = state();
            stateReason_ = "lock";
#if OFFGRID_RADIO_DIAGNOSTICS
            radioDriver.standby();
#endif
        } else if (strcmp(command, "sec.get_policy") == 0 || strcmp(command, "sec.set_policy") == 0) {
            if (!requireUnlocked(response)) return;
            if (strcmp(command, "sec.set_policy") == 0) {
                if (!Commands::integerArg(args, response, "lock_timeout_s", 0, 3600, false) ||
                    !Commands::boolArg(args, response, "persist", false)) return;
                const uint32_t candidate = args["lock_timeout_s"] | lockTimeoutS_;
                if ((args["persist"] | true) && !configStore_.savePolicy(candidate)) {
                    Commands::fail(response, "E_INTERNAL", "policy storage failed; live policy unchanged");
                    return;
                }
                lockTimeoutS_ = candidate;
            }
            result["lock_timeout_s"] = lockTimeoutS_;
        } else {
            Commands::fail(response, "E_UNKNOWN_CMD", "command is unavailable in this mock stage");
        }
    }

    char deviceId_[13] = {};
    Contact contacts_[32] = {};
    Message messages_[8] = {};
    Message incoming_;
    bool incomingPending_ = false;
    int contactEvent_ = -1;
    uint32_t nextReference_ = 1;
    const char* outcome_ = "delivered";
    uint8_t retries_ = 3;
    uint8_t hopDefault_ = 3;
    bool ackText_ = true;
    bool ackReaction_ = false;
    bool ackGeo_ = false;
    bool pairing_ = false;
    uint64_t pairingAt_ = 0;
    uint8_t pairingSalt_[16] = {};
    uint8_t publicKey_[32] = {};
    char nodeId_[5] = {};
    char name_[25] = {};
    char logLevel_[6] = "off";
    bool provisioned_ = false;
    bool unlocked_ = false;
    bool session_ = false;
    uint32_t lockTimeoutS_ = 15;
    ConfigStore configStore_;
    const char* storageNotice_ = nullptr;
    StaticJsonDocument<16384> config_;
    StaticJsonDocument<8192> candidateConfig_;
    StaticJsonDocument<16384> configScratch_;
    bool logPending_ = false;
    bool traceEnabled_ = false;
    bool tracePending_ = false;
    bool traceTransmit_ = false;
    bool traceAck_ = false;
    Message traceMessage_;
    uint32_t txPackets_ = 0;
    uint32_t rxPackets_ = 0;
    uint32_t relayed_ = 0;
    uint32_t acksSent_ = 0;
    uint32_t acksReceived_ = 0;
    uint32_t retryPackets_ = 0;
    uint64_t txAirtime_ = 0;
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
#if OFFGRID_RADIO_DIAGNOSTICS
    RadioDriver::Event radioEvent_;
    bool radioEventPending_ = false;
    uint32_t radioOperation_ = 0;
    uint32_t nextRadioOperation_ = 1;
#endif
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