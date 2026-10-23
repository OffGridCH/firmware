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
        data["fw"] = "0.1.0-serial-mock.4";
        data["proto"] = "1.0";
        data["state"] = state();
        data["reset_reason"] = resetReason_;
        data["mock"] = true;
        data["mock_stage"] = 4;
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
            provisioned_ = false;
            unlocked_ = false;
            session_ = false;
            resetArmed_ = false;
            lockTimeoutS_ = 15;
            for (Contact& contact : contacts_) contact = Contact{};
            for (Message& message : messages_) message = Message{};
            incomingPending_ = false;
            contactEvent_ = -1;
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
        tickMessages(nowMs);
        if (rebootPending_ && stateReason_ == nullptr && serialLink.availableTxSlots() > 0) {
            unlocked_ = false;
            session_ = false;
            resetArmed_ = false;
            resetReason_ = "software";
            for (Message& message : messages_) message = Message{};
            incomingPending_ = false;
            pairing_ = false;
            emitBoot(nowMs);
            rebootPending_ = false;
        }
        serialLink.setReceiveEnabled(stateReason_ == nullptr && !rebootPending_ &&
                         !incomingPending_ && contactEvent_ < 0);
    }

    static void handle(void* context, const char* command, JsonObjectConst arguments,
                       JsonObject response, uint64_t nowMs) {
        static_cast<MockDevice*>(context)->handleCommand(command, arguments, response, nowMs);
    }

private:
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
            for (Message& message : messages_) if (message.used && message.failure == nullptr) message.failure = "locked";
        }
        if (serialLink.availableTxSlots() == 0) return;
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
            result["est_airtime_ms"] = static_cast<uint32_t>(ceil((12.25 + 8 + ceil((8.0 * length - 36 + 44) / 36) * 5) * 4.096));
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
        output["last_rssi"] = nullptr;
        output["last_snr"] = nullptr;
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
        if (strncmp(command, "msg.", 4) == 0 || strncmp(command, "mock.", 5) == 0) {
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
            result["fw"]["version"] = "0.1.0-serial-mock.4";
            result["fw"]["build"] = __DATE__;
            result["proto"] = "1.0";
            result["state"] = state();
            result["mock"] = true;
            result["mock_stage"] = 4;
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
            result["mock_stage"] = 4;
            JsonObject radio = result.createNestedObject("radio");
            radio["running"] = unlocked_;
            radio["relay_only"] = false;
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