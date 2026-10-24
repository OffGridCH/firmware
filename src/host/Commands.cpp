#include "Commands.h"

#include <string.h>
#include <mbedtls/base64.h>
#include <math.h>
#include <esp_flash_encrypt.h>
#include <esp_secure_boot.h>
#include <esp_system.h>
#include "../security/CryptoEngine.h"
#include "../security/ResetPublicKey.h"

namespace {
class RequestReader {
public:
	RequestReader(const char* bytes, size_t length) : bytes_(bytes), length_(length) {
	}

	int read() {
		return offset_ < length_ ? static_cast<unsigned char>(bytes_[offset_++]) : -1;
	}

	size_t readBytes(char* buffer, size_t count) {
		if (count > length_ - offset_) {
			count = length_ - offset_;
		}
		memcpy(buffer, bytes_ + offset_, count);
		offset_ += count;
		return count;
	}

	bool onlyWhitespaceRemains() {
		while (offset_ < length_) {
			const char value = bytes_[offset_++];
			if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
				return false;
			}
		}
		return true;
	}

private:
	const char* bytes_;
	size_t length_;
	size_t offset_ = 0;
};
}

Commands::Commands(SerialLink& link, StateHandler handler, void* context)
	: link_(link), handler_(handler), context_(context) {
}

void Commands::init() {
	link_.setLineHandler(onLine, this);
}

LedPattern Commands::ledPattern(JsonVariantConst value) {
	LedPattern pattern;
	if (value.is<const char*>()) {
		const char* names[] = {"off", "on", "flash", "blink_slow", "blink_fast", "double", "heartbeat", "pulse"};
		for (size_t index = 0; index < 8; ++index) {
			if (strcmp(value.as<const char*>(), names[index]) == 0) {
				pattern.kind = static_cast<LedPatternKind>(index);
				break;
			}
		}
	} else {
		pattern.kind = LedPatternKind::Custom;
		for (JsonVariantConst step : value["steps"].as<JsonArrayConst>()) {
			if (pattern.stepCount == 16) break;
			pattern.stepsMs[pattern.stepCount++] = step.as<uint16_t>();
		}
		pattern.repeat = value["repeat"] | 1;
	}
	return pattern;
}

#if !OFFGRID_MOCK_DEVICE
namespace {
const char* stateName(const Security& security) {
	return security.state() == DeviceState::Unprovisioned ? "unprovisioned" :
		security.state() == DeviceState::Unlocked ? "unlocked" : "locked";
}

bool encode(JsonObject output, const char* field, const uint8_t* input, size_t length) {
	unsigned char bytes[173];
	size_t written = 0;
	return mbedtls_base64_encode(bytes, sizeof(bytes), &written, input, length) == 0 &&
		output[field].set(reinterpret_cast<char*>(bytes));
}

bool decode(JsonObjectConst args, JsonObject response, const char* field, uint8_t* output, size_t bytes) {
	if (!Commands::base64Arg(args, response, field, bytes)) return false;
	size_t written = 0;
	const JsonString text = args[field];
	return mbedtls_base64_decode(output, bytes, &written,
		reinterpret_cast<const uint8_t*>(text.c_str()), text.size()) == 0 && written == bytes;
}

bool radioRunning(const RadioDriver& radio) {
	return radio.ready() && radio.state() != RadioDriver::State::Standby && radio.state() != RadioDriver::State::Off;
}

const char* const emojiTable[] = {nullptr, "\xf0\x9f\x91\x8d", "\xe2\x9d\xa4\xef\xb8\x8f",
	"\xf0\x9f\x98\x82", "\xf0\x9f\x98\xae", "\xf0\x9f\x98\xa2", "\xf0\x9f\x99\x8f"};

void writePayload(JsonObject output, const PacketCodec::Payload& payload, bool receiving) {
	output["type"] = MeshEngine::typeName(payload.type);
	switch (payload.type) {
	case PacketCodec::Type::Text: output["text"].set(payload.text); break;
	case PacketCodec::Type::Reaction:
		output["target"]["author"] = (receiving ? !payload.targetOwn : payload.targetOwn) ? "me" : "peer";
		output["target"]["ref"] = payload.reference;
		output["emoji"] = emojiTable[payload.emoji];
		break;
	case PacketCodec::Type::Geo:
		output["lat"] = payload.latitude; output["lon"] = payload.longitude;
		if (payload.hasAltitude) output["alt_m"] = payload.altitude;
		else output["alt_m"] = nullptr;
		if (payload.hasAge) output["age_s"] = payload.ageSeconds;
		else output["age_s"] = nullptr;
		break;
	}
}
}

void Commands::initSecure(ConfigStore& store, Security& security) {
	uint32_t timeout = 15;
	store.copyTo(live_, timeout);
	security.setTimeout(timeout);
	const uint64_t mac = ESP.getEfuseMac();
	for (size_t index = 0; index < 6; ++index)
		snprintf(deviceId_ + index * 2, 3, "%02X", unsigned((mac >> (index * 8)) & 255));
}

RadioDriver::Config Commands::secureRadioConfig() const {
	const JsonObjectConst value = live_["radio"];
	RadioDriver::Config config;
	config.frequencyMhz = value["freq_mhz"];
	config.bandwidthKhz = value["bw_khz"];
	config.spreadingFactor = value["sf"];
	config.codingRate = value["cr"];
	config.powerDbm = value["tx_power_dbm"];
	config.preamble = value["preamble"];
	config.syncWord = value["sync_word"];
	return config;
}

bool Commands::identity(JsonObject output, const Security& security) {
	char node[5];
	snprintf(node, sizeof(node), "%04X", security.nodeId());
	output["node_id"].set(node);
	output["name"] = security.name();
	return encode(output, "public_key", security.publicKey(), 32);
}

void Commands::securityFailure(JsonObject response, const Security& security) {
	switch (security.error()) {
		case Security::Error::NoSession: fail(response, "E_NO_SESSION", "call sys.hello for a fresh session"); break;
		case Security::Error::RateLimited: fail(response, "E_RATE_LIMITED", "authentication backoff active"); break;
		case Security::Error::Authentication: fail(response, "E_UNLOCK_FAILED", "unlock failed"); break;
		case Security::Error::Storage: fail(response, "E_INTERNAL", "vault storage unavailable or invalid"); break;
		default: fail(response, "E_INTERNAL", "security operation failed"); break;
	}
}

void Commands::emitSecureBoot(const Security& security, uint64_t nowMs) {
	StaticJsonDocument<512> event;
	event["evt"] = "sys.boot";
	event["t"] = nowMs;
	event["data"]["device_id"] = deviceId_;
	event["data"]["fw"] = "0.2.0-secure-dev";
	event["data"]["proto"] = "1.0";
	event["data"]["state"] = stateName(security);
	event["data"]["mock"] = false;
	event["data"]["reset_reason"] = esp_reset_reason() == ESP_RST_SW ? "software" : "power_on";
	link_.sendJson(event);
}

Commands::Action Commands::handleSecure(const char* command, JsonObjectConst args, JsonObject response,
	uint64_t nowMs, Security& security, VaultStore& vault, ConfigStore& store, RadioDriver& radio,
	Contacts& contacts, Pairing& pairing, MeshEngine& mesh) {
	security.activity(nowMs);
	JsonObject result = response["result"];
	const bool unlocked = security.state() == DeviceState::Unlocked;
	if (strcmp(command, "sys.hello") == 0) {
		if (!stringArg(args, response, "app", 0, 128, false) ||
			!stringArg(args, response, "app_proto", 3, 24, false)) return Action::None;
		if (args.containsKey("app_proto")) {
			const char* version = args["app_proto"];
			const char* dot = strchr(version, '.');
			if (!dot || dot == version || !dot[1] ||
				strspn(version, "0123456789") != size_t(dot - version) ||
				strspn(dot + 1, "0123456789") != strlen(dot + 1)) {
				fail(response, "E_INVALID_ARG", "expected major.minor", "app_proto");
				return Action::None;
			}
			while (version < dot && *version == '0') ++version;
			if (dot - version != 1 || *version != '1') {
				fail(response, "E_PROTO_MISMATCH", "supported host protocol major is 1");
				return Action::None;
			}
		}
		uint8_t publicKey[32], nonce[16];
		if (!security.beginSession(publicKey, nonce)) { securityFailure(response, security); return Action::None; }
		result["device"] = "heltec_wifi_lora_32_v3";
		result["chip"] = "ESP32-S3";
		result["device_id"] = deviceId_;
		result["fw"]["version"] = "0.2.0-secure-dev";
		result["fw"]["build"] = __DATE__;
		result["proto"] = "1.0";
		result["state"] = stateName(security);
		result["mock"] = false;
		result["authentication"] = "x25519-aes256gcm";
		result["config_storage"] = "nvs";
		JsonObject session = result.createNestedObject("session");
		if (!encode(session, "device_eph_pub", publicKey, sizeof(publicKey)) ||
			!encode(session, "nonce", nonce, sizeof(nonce))) {
			security.consumeSession();
			fail(response, "E_INTERNAL", "session encoding failed"); return Action::None;
		}
		result["limits"]["max_line"] = 2048;
		result["limits"]["max_contacts"] = 32;
		result["limits"]["max_text_bytes"] = 83;
		result["limits"]["pending_queue"] = 8;
		result["recovery"]["master"] = "ECDSA-P256-SHA256";
		result["recovery"]["app_wrap_label"] = "og-reset-wrap-v1";
	} else if (strcmp(command, "sys.ping") == 0) {
		result["uptime_ms"] = nowMs;
	} else if (strcmp(command, "sys.status") == 0) {
		result["state"] = stateName(security);
		result["uptime_ms"] = nowMs;
		result["heap_free"] = ESP.getFreeHeap();
		result["battery_mv"] = nullptr;
		result["mock"] = false;
		result["radio"]["running"] = radioRunning(radio);
		result["radio"]["relay_only"] = mesh.radioWanted() && !unlocked;
		result["radio"]["initialized"] = radio.ready();
		result["radio"]["last_error"] = radio.lastError();
		result["storage"]["config_status"] = store.statusName();
		result["storage"]["writable"] = store.writable();
		result["storage"]["vault_status"] = vault.statusName();
		result["storage"]["generation"] = vault.generation();
		result["security"]["secure_boot"] = esp_secure_boot_enabled();
		result["security"]["flash_encryption"] = esp_flash_encryption_enabled();
		if (unlocked) {
			result["contacts"]["used"] = __builtin_popcount(vault.contacts());
			result["contacts"]["capacity"] = 32;
			result["queue"]["pending"] = mesh.count();
			result["queue"]["capacity"] = 8;
			result["queue"]["dropped_events"] = mesh.stats().droppedEvents;
			result["queue"]["dropped_acks"] = mesh.stats().droppedAcks;
		}
	} else if (strcmp(command, "sys.reboot") == 0) {
		return Action::Reboot;
	} else if (strcmp(command, "sys.reset_challenge") == 0) {
		uint8_t message[49], digest[32];
		if (!security.beginResetChallenge(message, nowMs) ||
			!CryptoEngine::sha256(ResetTrust::PublicKey, sizeof(ResetTrust::PublicKey), digest)) {
			fail(response, "E_INTERNAL", "reset challenge generation failed"); return Action::None;
		}
		if (!encode(result, "message", message, sizeof(message)) ||
			!encode(result, "key_id", digest, sizeof(digest))) {
			fail(response, "E_INTERNAL", "challenge encoding failed"); return Action::None;
		}
		result["expires_s"] = 30;
		result["algorithm"] = "ECDSA-P256-SHA256";
	} else if (strcmp(command, "sys.factory_reset") == 0) {
		if (!args["confirm"].is<const char*>() || strcmp(args["confirm"], "ERASE") != 0) {
			fail(response, "E_CONFIRM_REQUIRED", "confirm must be ERASE", "confirm"); return Action::None;
		}
		if (args.containsKey("method") &&
			(!args["method"].is<const char*>() ||
			(strcmp(args["method"], "app") != 0 && strcmp(args["method"], "master") != 0))) {
			fail(response, "E_INVALID_ARG", "method must be app or master", "method"); return Action::None;
		}
		bool authorized = false;
		if (args["method"].is<const char*>() && strcmp(args["method"], "master") == 0) {
			uint8_t signature[72] = {};
			size_t length = 0;
			bool shape = stringArg(args, response, "signature", 12, 96) &&
				mbedtls_base64_decode(signature, sizeof(signature), &length,
					reinterpret_cast<const uint8_t*>(args["signature"].as<const char*>()),
					strlen(args["signature"])) == 0;
			if (shape) shape = base64Arg(args, response, "signature", length);
			authorized = security.authorizeMasterReset(signature, shape ? length : 0, nowMs);
		} else {
			uint8_t publicKey[32] = {}, wrapped[60] = {};
			const bool shape = decode(args, response, "app_eph_pub", publicKey, 32) &&
				decode(args, response, "wrapped_token", wrapped, 60);
			authorized = security.authenticate(Security::Operation::Reset, publicKey, wrapped, shape, nullptr, nowMs);
		}
		if (!authorized) { securityFailure(response, security); return Action::None; }
		if (ConfigStore::beginReset() != 0) {
			fail(response, "E_INTERNAL", "reset authorization could not be committed"); return Action::None;
		}
		security.lock("factory_reset", nowMs);
		response["ok"] = true;
		result = response["result"].to<JsonObject>();
		result["pending"] = true;
		return Action::Reset;
	} else if (strcmp(command, "sec.provision") == 0 || strcmp(command, "sec.unlock") == 0 ||
		strcmp(command, "sec.rotate_token") == 0) {
		const bool provision = strcmp(command, "sec.provision") == 0;
		const bool rotate = strcmp(command, "sec.rotate_token") == 0;
		if ((provision && security.state() != DeviceState::Unprovisioned) ||
			(!provision && !rotate && security.state() != DeviceState::Locked) || (rotate && !unlocked)) {
			security.consumeSession();
			fail(response, provision ? "E_ALREADY_PROVISIONED" : unlocked ? "E_ALREADY_UNLOCKED" :
				security.state() == DeviceState::Unprovisioned ? "E_NOT_PROVISIONED" : "E_LOCKED",
				"command not allowed in current state"); return Action::None;
		}
		if (provision && !stringArg(args, response, "name", 1, 24, false)) {
			security.consumeSession(); return Action::None;
		}
		uint8_t publicKey[32] = {}, wrapped[60] = {};
		const bool shape = decode(args, response, "app_eph_pub", publicKey, 32) &&
			decode(args, response, "wrapped_token", wrapped, 60);
		if (!security.authenticate(provision ? Security::Operation::Provision :
			rotate ? Security::Operation::Rotate : Security::Operation::Unlock,
			publicKey, wrapped, shape, args["name"] | static_cast<const char*>(nullptr), nowMs)) {
			securityFailure(response, security); return Action::None;
		}
		response["ok"] = true;
		result = response["result"].to<JsonObject>();
		result["state"] = stateName(security);
		if (!identity(result, security)) {
			fail(response, "E_INTERNAL", "identity encoding failed"); return Action::None;
		}
		if (!rotate) {
			if (radio.ready()) radio.startReceive();
			result["contacts"] = __builtin_popcount(vault.contacts());
			result["radio"]["running"] = radioRunning(radio);
			result["radio"]["relay_only"] = false;
			result["radio"]["last_error"] = radio.lastError();
		}
	} else if (strcmp(command, "sec.lock") == 0 || strcmp(command, "sec.get_policy") == 0 ||
		strcmp(command, "sec.set_policy") == 0 || strncmp(command, "id.", 3) == 0) {
		if (!unlocked) {
			fail(response, security.state() == DeviceState::Unprovisioned ? "E_NOT_PROVISIONED" : "E_LOCKED",
				"unlock required"); return Action::None;
		}
		if (strcmp(command, "sec.lock") == 0) {
			security.lock("lock", nowMs);
			result["state"] = "locked";
		} else if (strcmp(command, "sec.get_policy") == 0 || strcmp(command, "sec.set_policy") == 0) {
			if (strcmp(command, "sec.set_policy") == 0) {
				if (!integerArg(args, response, "lock_timeout_s", 0, 3600, false) ||
					!boolArg(args, response, "persist", false)) return Action::None;
				const uint32_t timeout = args["lock_timeout_s"] | security.timeout();
				if ((args["persist"] | true) && !store.savePolicy(timeout)) {
					fail(response, "E_INTERNAL", "policy storage failed; live policy unchanged"); return Action::None;
				}
				security.setTimeout(timeout);
			}
			result["lock_timeout_s"] = security.timeout();
		} else if (strcmp(command, "id.get") == 0 || strcmp(command, "id.set_name") == 0) {
			if (strcmp(command, "id.set_name") == 0 &&
				(!stringArg(args, response, "name", 1, 24) || !security.rename(args["name"]))) {
				if (response["ok"].as<bool>()) securityFailure(response, security);
				return Action::None;
			}
			if (!identity(result, security)) fail(response, "E_INTERNAL", "identity encoding failed");
		} else fail(response, "E_UNKNOWN_CMD", "unknown identity command");
	} else if (strncmp(command, "radio.", 6) == 0 || strncmp(command, "mesh.", 5) == 0 ||
		strncmp(command, "led.", 4) == 0) {
		const char* verb = strchr(command, '.');
		if (!unlocked && strcmp(verb, ".get") != 0) {
			fail(response, security.state() == DeviceState::Unprovisioned ? "E_NOT_PROVISIONED" : "E_LOCKED",
				"unlock required");
		} else {
			const MeshEngine::Stats stats = mesh.stats();
			secureConfig(command, args, response, store, radio);
			if (response["ok"].as<bool>() && strcmp(command, "radio.stats") == 0) {
				result["rx_tag_failures"] = stats.tagFailures;
				result["rx_dedupe_drops"] = stats.replayDrops;
				result["acks_sent"] = stats.acksSent; result["acks_received"] = stats.acksReceived;
				result["retries"] = stats.retries; result["dropped_events"] = stats.droppedEvents;
				result["dropped_acks"] = stats.droppedAcks;
				result["rx_malformed"] = stats.malformed;
				result["relayed"] = stats.relayed; result["relay_suppressed"] = stats.relaySuppressed;
				result["relay_dropped"] = stats.relayDropped; result["rate_limit_drops"] = stats.rateDrops;
				result["tx_duty_cycle_max_percent"] = 1;
				if (args["reset"] | false) mesh.resetStats();
			}
			mesh.configure(secureMeshConfig());
		}
	} else if (strncmp(command, "pair.", 5) == 0 || strncmp(command, "contact.", 8) == 0) {
		if (!unlocked) fail(response, security.state() == DeviceState::Unprovisioned ? "E_NOT_PROVISIONED" : "E_LOCKED",
			"unlock required");
		else secureContacts(command, args, response, nowMs, security, contacts, pairing);
	} else if (strncmp(command, "msg.", 4) == 0) {
		if (!unlocked) fail(response, "E_LOCKED", "unlock required");
		else secureMessages(command, args, response, nowMs, mesh, radio);
	} else fail(response, "E_UNKNOWN_CMD", "unknown command");
	return Action::None;
}

MeshEngine::Config Commands::secureMeshConfig() const {
	MeshEngine::Config config;
	config.retries = live_["mesh"]["retries"];
	config.retryBaseMs = live_["mesh"]["retry_base_ms"];
	config.relayEnabled = live_["mesh"]["relay_enabled"];
	config.relayWhenLocked = live_["mesh"]["relay_when_locked"];
	config.backoffSlots = live_["mesh"]["backoff_slots"];
	config.ratePerSource = live_["mesh"]["rate_limit_per_src"];
	return config;
}

void Commands::secureMessages(const char* command, JsonObjectConst args, JsonObject response,
	uint64_t nowMs, MeshEngine& mesh, const RadioDriver& radio) {
	JsonObject result = response["result"];
	if (strcmp(command, "msg.pending") == 0) {
		JsonArray pending = result.createNestedArray("pending");
		for (size_t index = 0; index < MeshEngine::Capacity; ++index) {
			const MeshEngine::Pending* message = mesh.pending(index);
			if (!message) continue;
			JsonObject item = pending.createNestedObject();
			item["msg_ref"] = message->reference; item["to"] = message->contact;
			item["type"] = MeshEngine::typeName(message->type); item["ctr"] = message->counter;
			item["state"] = MeshEngine::stateName(message->state); item["retry"] = message->retry;
		}
		return;
	}
	if (strcmp(command, "msg.cancel") == 0) {
		if (!integerArg(args, response, "msg_ref", 1, UINT32_MAX)) return;
		if (!mesh.cancel(args["msg_ref"], nowMs)) {
			fail(response, "E_INVALID_ARG", "message is not pending", "msg_ref"); return;
		}
		result["state"] = "cancelled";
		return;
	}
	if (strcmp(command, "msg.send") != 0) { fail(response, "E_UNKNOWN_CMD", "unknown message command"); return; }
	if (!integerArg(args, response, "to", 1, 32) || !stringArg(args, response, "type", 1, 8) ||
		!integerArg(args, response, "hops", 0, 7, false) || !boolArg(args, response, "ack", false)) return;
	PacketCodec::Payload payload;
	const char* type = args["type"];
	if (strcmp(type, "text") == 0) {
		if (!stringArg(args, response, "text", 0, 2047)) return;
		const JsonString text = args["text"];
		if (text.size() > PacketCodec::MaxTextBytes) {
			fail(response, "E_TOO_LONG", "text exceeds 83 UTF-8 bytes", "text"); return;
		}
		memcpy(payload.text, text.c_str(), text.size());
	} else if (strcmp(type, "reaction") == 0) {
		payload.type = PacketCodec::Type::Reaction;
		const JsonObjectConst target = args["target"];
		if (target.isNull() || !stringArg(target, response, "author", 2, 4) ||
			!integerArg(target, response, "ref", 0, 65535)) {
			if (target.isNull()) fail(response, "E_INVALID_ARG", "target object required", "target");
			return;
		}
		const char* author = target["author"];
		if (strcmp(author, "me") && strcmp(author, "peer")) {
			fail(response, "E_INVALID_ARG", "author must be me or peer", "target.author"); return;
		}
		payload.targetOwn = strcmp(author, "me") == 0;
		payload.reference = target["ref"];
		if (!args.containsKey("emoji")) { fail(response, "E_INVALID_ARG", "emoji required, null removes", "emoji"); return; }
		if (!args["emoji"].isNull()) {
			if (!stringArg(args, response, "emoji", 1, 8)) return;
			for (uint8_t index = 1; index <= 6; ++index)
				if (strcmp(args["emoji"], emojiTable[index]) == 0) payload.emoji = index;
			if (!payload.emoji) { fail(response, "E_INVALID_ARG", "unsupported emoji", "emoji"); return; }
		}
	} else if (strcmp(type, "geo") == 0) {
		payload.type = PacketCodec::Type::Geo;
		if (!numberArg(args, response, "lat", -90, 90) || !numberArg(args, response, "lon", -180, 180)) return;
		payload.latitude = args["lat"]; payload.longitude = args["lon"];
		payload.hasAltitude = args.containsKey("alt_m") && !args["alt_m"].isNull();
		payload.hasAge = args.containsKey("age_s") && !args["age_s"].isNull();
		if (payload.hasAltitude && !integerArg(args, response, "alt_m", -32768, 32767)) return;
		if (payload.hasAge && !integerArg(args, response, "age_s", 0, 2550)) return;
		if (payload.hasAltitude) payload.altitude = args["alt_m"];
		if (payload.hasAge) payload.ageSeconds = args["age_s"];
	} else { fail(response, "E_INVALID_ARG", "type must be text, reaction or geo", "type"); return; }
	payload.ack = args["ack"] | live_["mesh"]["ack_default"][type].as<bool>();
	MeshEngine::Pending accepted;
	size_t bytes = 0;
	if (!radio.ready()) {
		CryptoEngine::zeroize(&payload, sizeof(payload));
		fail(response, "E_RADIO", "radio unavailable"); return;
	}
	const MeshEngine::Error error = mesh.enqueue(args["to"], payload,
		args["hops"] | live_["mesh"]["hop_default"].as<uint8_t>(), nowMs, accepted, bytes);
	CryptoEngine::zeroize(&payload, sizeof(payload));
	if (error != MeshEngine::Error::None) {
		const char* code = error == MeshEngine::Error::NoContact ? "E_NO_SUCH_CONTACT" :
			error == MeshEngine::Error::Full ? "E_QUEUE_FULL" : error == MeshEngine::Error::Locked ? "E_LOCKED" :
			error == MeshEngine::Error::Storage ? "E_INTERNAL" :
			error == MeshEngine::Error::CounterExhausted ? "E_BUSY" : "E_INVALID_ARG";
		fail(response, code, error == MeshEngine::Error::CounterExhausted ? "counter exhausted; re-pair" : "message enqueue failed");
		return;
	}
	result["msg_ref"] = accepted.reference; result["ctr"] = accepted.counter;
	result["ack_requested"] = args["ack"] | live_["mesh"]["ack_default"][type].as<bool>();
	result["hops"] = args["hops"] | live_["mesh"]["hop_default"].as<uint8_t>();
	result["state"] = "queued"; result["bytes_on_air"] = bytes;
	result["est_airtime_ms"] = RadioDriver::airtimeMs(radio.config(), bytes);
}

bool Commands::emitMeshEvent(const MeshEngine::Event& value) {
	StaticJsonDocument<768> event;
	event["t"] = value.timeMs;
	JsonObject data = event.createNestedObject("data");
	if (value.type == MeshEngine::EventType::ContactActive) {
		event["evt"] = "contact.state"; data["contact_id"] = value.message.contact; data["state"] = "active";
	} else if (value.type == MeshEngine::EventType::Received) {
		event["evt"] = "msg.received";
		writePayload(data, value.payload, true);
		data["from"] = value.message.contact; data["ctr"] = value.message.counter; data["retry"] = value.message.retry;
		data["hops"] = value.hops; data["rssi"] = value.rssi; data["snr"] = value.snr;
	} else {
		event["evt"] = "msg.status";
		data["msg_ref"] = value.message.reference; data["to"] = value.message.contact;
		data["state"] = MeshEngine::stateName(value.message.state); data["retry"] = value.message.retry;
		if (value.reason != MeshEngine::Reason::None) data["reason"] = MeshEngine::reasonName(value.reason);
		if (value.message.state == MeshEngine::State::Delivered) { data["hops"] = value.hops; data["rtt_ms"] = value.rttMs; }
	}
	return link_.sendJson(event);
}

void Commands::secureConfig(const char* command, JsonObjectConst args, JsonObject response,
	ConfigStore& store, RadioDriver& radio) {
	JsonObject result = response["result"];
	if (strcmp(command, "led.test") == 0) {
		if (!ConfigStore::validPattern(args["pattern"]) || !integerArg(args, response, "repeat", 1, 255, false)) {
			fail(response, "E_INVALID_ARG", "invalid LED preview", "pattern"); return;
		}
		preview_ = ledPattern(args["pattern"]); preview_.repeat = args["repeat"] | 1;
		previewPending_ = true; result["preview"] = true; result["repeat"] = preview_.repeat;
		return;
	}
	if (strcmp(command, "radio.stats") == 0) {
		if (!boolArg(args, response, "reset", false)) return;
		result["tx_packets"] = radio.stats().transmitted;
		result["rx_packets"] = radio.stats().received;
		result["rx_crc_errors"] = radio.stats().crcErrors;
		result["radio_dropped_events"] = radio.stats().droppedEvents;
		result["tx_airtime_ms_total"] = radio.stats().txAirtimeMs;
		result["last_rssi"] = radio.stats().lastRssi;
		result["last_snr"] = radio.stats().lastSnr;
		if (args["reset"] | false) radio.resetStats();
		return;
	}
	const char* category = strncmp(command, "radio.", 6) == 0 ? "radio" :
		strncmp(command, "mesh.", 5) == 0 ? "mesh" : "led";
	const char* verb = strchr(command, '.');
	const bool setting = strcmp(verb, ".set") == 0;
	if (!setting && strcmp(verb, ".get") != 0) { fail(response, "E_UNKNOWN_CMD", "unknown configuration command"); return; }
	if (!boolArg(args, response, "persist", false)) return;
	candidate_.set(live_[category]);
	JsonObject candidate = candidate_.as<JsonObject>();
	if (setting) {
		if (strcmp(category, "radio") == 0 && args.containsKey("preset")) {
			if (!stringArg(args, response, "preset", 1, 16)) return;
			const char* preset = args["preset"];
			if (strcmp(preset, "lab_default") && strcmp(preset, "fast") && strcmp(preset, "long_range")) {
				fail(response, "E_INVALID_ARG", "unknown radio preset", "preset"); return;
			}
			candidate["freq_mhz"] = 869.525; candidate["bw_khz"] = strcmp(preset, "fast") == 0 ? 250 : 125;
			candidate["sf"] = strcmp(preset, "fast") == 0 ? 7 : strcmp(preset, "long_range") == 0 ? 11 : 9;
			candidate["cr"] = 5; candidate["tx_power_dbm"] = 14; candidate["preamble"] = 8; candidate["sync_word"] = 79;
		}
		for (JsonPair pair : candidate) {
			const char* key = pair.key().c_str();
			if (!args.containsKey(key)) continue;
			if (strcmp(key, "state_patterns") == 0 || strcmp(key, "event_patterns") == 0 || strcmp(key, "ack_default") == 0) {
				if (!args[key].is<JsonObjectConst>()) { fail(response, "E_INVALID_ARG", "expected object", key); return; }
				for (JsonPair nested : pair.value().as<JsonObject>())
					if (args[key].containsKey(nested.key().c_str())) nested.value().set(args[key][nested.key().c_str()]);
			} else pair.value().set(args[key]);
		}
		if (candidate_.overflowed() || !store.validCategory(category, candidate)) {
			fail(response, "E_INVALID_ARG", "invalid configuration candidate", category); return;
		}
		if (strcmp(category, "radio") == 0 && !radio.ready()) {
			fail(response, "E_RADIO", "radio unavailable; live settings unchanged"); return;
		}
	}
	result.set(candidate);
	if (strcmp(category, "radio") == 0) {
		RadioDriver::Config config = secureRadioConfig();
		config.frequencyMhz = candidate["freq_mhz"]; config.bandwidthKhz = candidate["bw_khz"];
		config.spreadingFactor = candidate["sf"]; config.codingRate = candidate["cr"];
		config.powerDbm = candidate["tx_power_dbm"]; config.preamble = candidate["preamble"]; config.syncWord = candidate["sync_word"];
		result["est_airtime_ms_23b"] = RadioDriver::airtimeMs(config, 23); result["max_text_bytes"] = 83;
		if (setting) {
			JsonArray warnings = result.createNestedArray("warnings");
			if (config.frequencyMhz < 863 || config.frequencyMhz > 870) warnings.add("outside_eu868");
			if (config.powerDbm > 14) warnings.add("above_eu868_power_limit");
			if (config.syncWord == 18 || config.syncWord == 52) warnings.add("reserved_sync_word");
			warnings.add("mesh_incompatible");
		}
	} else if (strcmp(category, "led") == 0) {
		JsonArray patterns = result.createNestedArray("builtin_patterns");
		const char* names[] = {"off", "on", "flash", "blink_slow", "blink_fast", "double", "heartbeat", "pulse"};
		for (const char* name : names) patterns.add(name);
	}
	if (measureJson(response) >= BoardConfig::SerialMaxLineBytes) { fail(response, "E_TOO_LONG", "configuration response too long"); return; }
	if (setting) {
		if ((args["persist"] | true) && !store.saveCategory(category, candidate)) {
			fail(response, "E_INTERNAL", "configuration storage failed; live settings unchanged"); return;
		}
		live_[category].set(candidate);
		if (strcmp(category, "led") == 0) ledDirty_ = true;
		if (strcmp(category, "radio") == 0) {
			if (!radio.queueConfig(secureRadioConfig())) fail(response, "E_RADIO", "radio configuration application failed");
			else result["apply_pending"] = radio.configPending();
		}
	}
}

void Commands::secureSnapshot(UiSnapshot& snapshot, uint64_t nowMs, const Security& security, const RadioDriver& radio,
	const Contacts& contacts, const Pairing& pairing) {
	snapshot = UiSnapshot{};
	snapshot.mock = false; snapshot.uptimeMs = nowMs; snapshot.state = security.state();
	memcpy(snapshot.deviceId, deviceId_, sizeof(deviceId_));
	if (security.state() == DeviceState::Unlocked) {
		snprintf(snapshot.nodeId, sizeof(snapshot.nodeId), "%04X", security.nodeId());
		strncpy(snapshot.name, security.name(), sizeof(snapshot.name) - 1);
		snapshot.contacts = contacts.count();
		snapshot.pairingActive = pairing.active();
		snapshot.pairingRemainingS = pairing.remaining(nowMs);
		strncpy(snapshot.fingerprint, pairing.fingerprint(), sizeof(snapshot.fingerprint) - 1);
	}
	snapshot.radioRunning = radioRunning(radio); snapshot.radioError = radio.lastError();
	snapshot.frequencyMhz = live_["radio"]["freq_mhz"]; snapshot.bandwidthKhz = live_["radio"]["bw_khz"];
	snapshot.spreadingFactor = live_["radio"]["sf"];
	snapshot.txPackets = radio.stats().transmitted; snapshot.rxPackets = radio.stats().received;
	snapshot.hasSignal = radio.stats().hasSignal; snapshot.rssi = radio.stats().lastRssi; snapshot.snr = radio.stats().lastSnr;
}

bool Commands::takeSecureLedConfig(LedConfig& config) {
	if (!ledDirty_) return false;
	const JsonObjectConst value = live_["led"];
	config.enabled = value["enabled"]; config.quiet = value["quiet"]; config.brightness = value["brightness"];
	const char* states[] = {"unprovisioned", "locked", "unlocked"};
	const char* events[] = {"tx", "rx", "msg_received", "delivered", "failed", "error", "pairing"};
	for (size_t index = 0; index < 3; ++index) config.statePatterns[index] = ledPattern(value["state_patterns"][states[index]]);
	for (size_t index = 0; index < 7; ++index) config.eventPatterns[index] = ledPattern(value["event_patterns"][events[index]]);
	ledDirty_ = false;
	return true;
}

bool Commands::takeSecureLedPreview(LedPattern& pattern) {
	if (!previewPending_) return false;
	pattern = preview_; previewPending_ = false;
	return true;
}

bool Commands::contactView(JsonObject output, uint8_t slot, const Contacts::View& view) {
	char node[5];
	snprintf(node, sizeof(node), "%04X", view.nodeId);
	output["contact_id"] = slot; output["name"] = view.name; output["node_id"].set(node);
	output["state"] = view.active ? "active" : "pending"; output["fingerprint"] = view.fingerprint;
	output["tx_ctr"] = view.txCounter;
	if (view.rxHighest) output["rx_highest_ctr"] = view.rxHighest;
	else output["rx_highest_ctr"] = nullptr;
	if (view.hasSignal) { output["last_rssi"] = view.rssi; output["last_snr"] = view.snr; }
	else { output["last_rssi"] = nullptr; output["last_snr"] = nullptr; }
	return encode(output, "public_key", view.publicKey, 32);
}

void Commands::secureContacts(const char* command, JsonObjectConst args, JsonObject response,
	uint64_t nowMs, Security& security, Contacts& contacts, Pairing& pairing) {
	JsonObject result = response["result"];
	if (strcmp(command, "pair.begin") == 0) {
		uint8_t qr[75]; size_t length = 0;
		if (!pairing.begin(qr, length, nowMs) || !encode(result, "qr", qr, length)) {
			pairing.cancel(); fail(response, "E_INTERNAL", "pairing initialization failed"); return;
		}
		result["qr_bytes"] = length; result["expires_s"] = 300;
		return;
	}
	if (strcmp(command, "pair.cancel") == 0) { pairing.cancel(); result["cancelled"] = true; return; }
	if (strcmp(command, "pair.complete") == 0) {
		if (!stringArg(args, response, "qr", 68, 100) || !stringArg(args, response, "name", 1, 24, false) ||
			!boolArg(args, response, "replace", false)) return;
		uint8_t qr[75]; size_t length = 0;
		if (mbedtls_base64_decode(qr, sizeof(qr), &length,
			reinterpret_cast<const uint8_t*>(args["qr"].as<const char*>()), strlen(args["qr"])) != 0 ||
			!base64Arg(args, response, "qr", length)) {
			fail(response, "E_INVALID_QR", "invalid QR encoding", "qr"); return;
		}
		uint8_t slot = 0; bool collision = false;
		if (!pairing.complete(qr, length, args["name"] | static_cast<const char*>(nullptr),
			args["replace"] | false, nowMs, slot, collision)) {
			const Pairing::Error error = pairing.error();
			const char* code = error == Pairing::Error::NoPairing ? "E_NO_PAIRING" :
				error == Pairing::Error::Self ? "E_SELF_PAIR" : error == Pairing::Error::Exists ? "E_CONTACT_EXISTS" :
				error == Pairing::Error::Full ? "E_STORAGE_FULL" : error == Pairing::Error::Storage ? "E_INTERNAL" : "E_INVALID_QR";
			if (error == Pairing::Error::Storage) security.lock("lock", nowMs);
			fail(response, code, "pairing could not be completed"); return;
		}
		if (!contactView(result.createNestedObject("contact"), slot, *contacts.view(slot))) {
			fail(response, "E_INTERNAL", "contact encoding failed"); return;
		}
		JsonArray warnings = result.createNestedArray("warnings");
		if (collision) warnings.add("node_id_collision");
		return;
	}
	if (strcmp(command, "contact.list") == 0) {
		if (!integerArg(args, response, "offset", 0, 32, false) ||
			!integerArg(args, response, "limit", 1, 10, false)) return;
		const uint8_t offset = args["offset"] | 0, limit = args["limit"] | 10;
		result["total"] = contacts.count(); result["offset"] = offset;
		JsonArray values = result.createNestedArray("contacts");
		uint8_t seen = 0;
		for (uint8_t slot = 1; slot <= 32 && values.size() < limit; ++slot) {
			const Contacts::View* view = contacts.view(slot);
			if (!view || seen++ < offset) continue;
			if (!contactView(values.createNestedObject(), slot, *view)) {
				fail(response, "E_INTERNAL", "contact encoding failed"); return;
			}
			if (measureJson(response) >= BoardConfig::SerialMaxLineBytes) { values.remove(values.size() - 1); break; }
		}
		return;
	}
	if (strcmp(command, "contact.get") && strcmp(command, "contact.rename") && strcmp(command, "contact.delete")) {
		fail(response, "E_UNKNOWN_CMD", "unknown contact command"); return;
	}
	if (!integerArg(args, response, "contact_id", 1, 32)) return;
	const uint8_t slot = args["contact_id"];
	if (!contacts.view(slot)) { fail(response, "E_NO_SUCH_CONTACT", "contact does not exist", "contact_id"); return; }
	if (strcmp(command, "contact.delete") == 0) {
		if (!args["confirm"].is<bool>() || !args["confirm"].as<bool>()) {
			fail(response, "E_CONFIRM_REQUIRED", "confirm must be true", "confirm"); return;
		}
		if (!contacts.remove(slot)) {
			security.lock("lock", nowMs); fail(response, "E_INTERNAL", "contact deletion storage failed"); return;
		}
		result["deleted"] = true;
	} else {
		if (strcmp(command, "contact.rename") == 0) {
			if (!stringArg(args, response, "name", 1, 24)) return;
			if (!contacts.rename(slot, args["name"])) {
				security.lock("lock", nowMs); fail(response, "E_INTERNAL", "contact rename storage failed"); return;
			}
		}
		if (!contactView(result.createNestedObject("contact"), slot, *contacts.view(slot)))
			fail(response, "E_INTERNAL", "contact encoding failed");
	}
}
#endif

bool Commands::stringArg(JsonObjectConst args, JsonObject response, const char* field,
						 size_t minimum, size_t maximum, bool required) {
	if (!required && !args.containsKey(field)) return true;
	const JsonVariantConst value = args[field];
	const JsonString text = value.as<JsonString>();
	if (!value.is<const char*>() || text.size() < minimum || text.size() > maximum ||
		memchr(text.c_str(), '\0', text.size()) != nullptr ||
		!validUtf8(reinterpret_cast<const uint8_t*>(text.c_str()), text.size())) {
		fail(response, "E_INVALID_ARG", "expected a string within the allowed byte length", field);
		return false;
	}
	return true;
}

bool Commands::integerArg(JsonObjectConst args, JsonObject response, const char* field,
						  int64_t minimum, int64_t maximum, bool required) {
	if (!required && !args.containsKey(field)) return true;
	const JsonVariantConst value = args[field];
	if (!value.is<int64_t>() || value.as<int64_t>() < minimum || value.as<int64_t>() > maximum) {
		fail(response, "E_INVALID_ARG", "expected an integer within the allowed range", field);
		return false;
	}
	return true;
}

bool Commands::boolArg(JsonObjectConst args, JsonObject response, const char* field,
					   bool required) {
	if (!required && !args.containsKey(field)) return true;
	if (!args[field].is<bool>()) {
		fail(response, "E_INVALID_ARG", "expected a boolean", field);
		return false;
	}
	return true;
}

bool Commands::base64Arg(JsonObjectConst args, JsonObject response, const char* field,
						 size_t decodedBytes) {
	if (!stringArg(args, response, field, 1, 172)) return false;
	const JsonString text = args[field].as<JsonString>();
	uint8_t decoded[128];
	unsigned char canonical[173];
	size_t actual = 0;
	size_t encoded = 0;
	if (mbedtls_base64_decode(decoded, sizeof(decoded), &actual,
		 reinterpret_cast<const unsigned char*>(text.c_str()), text.size()) != 0 ||
		actual != decodedBytes ||
		mbedtls_base64_encode(canonical, sizeof(canonical), &encoded, decoded, actual) != 0 ||
		encoded != text.size() || memcmp(canonical, text.c_str(), encoded) != 0) {
		fail(response, "E_INVALID_ARG", "expected canonical Base64 with the specified decoded length", field);
		return false;
	}
	return true;
}

bool Commands::numberArg(JsonObjectConst args, JsonObject response, const char* field,
						 double minimum, double maximum, bool required) {
	if (!required && !args.containsKey(field)) return true;
	const JsonVariantConst value = args[field];
	const double number = value.as<double>();
	if (!value.is<double>() || !isfinite(number) || number < minimum || number > maximum) {
		fail(response, "E_INVALID_ARG", "expected a finite number within the allowed range", field);
		return false;
	}
	return true;
}

void Commands::fail(JsonObject response, const char* code, const char* message,
					const char* field) {
	response.remove("result");
	response["ok"] = false;
	JsonObject error = response.createNestedObject("error");
	error["code"] = code;
	error["message"] = message;
	if (field != nullptr) {
		error["field"].set(const_cast<char*>(field));
	}
}

void Commands::onLine(void* context, const char* line, size_t length, uint64_t nowMs) {
	static_cast<Commands*>(context)->processLine(line, length, nowMs);
}

void Commands::processLine(const char* line, size_t length, uint64_t nowMs) {
	response_.clear();
	JsonObject response = response_.to<JsonObject>();
	RequestReader reader(line, length);
	const DeserializationError error = deserializeJson(
		request_, reader, DeserializationOption::NestingLimit(BoardConfig::JsonNestingLimit));

	if (error || !reader.onlyWhitespaceRemains()) {
		fail(response, "E_PARSE", "line is not a supported JSON document");
	} else if (!request_.is<JsonObject>()) {
		fail(response, "E_INVALID_ARG", "request must be an object", "request");
	} else if (!request_["id"].is<int64_t>()) {
		fail(response, "E_INVALID_ARG", "id must be a signed 64-bit integer", "id");
	} else {
		response["id"] = request_["id"];
		if (!request_["cmd"].is<const char*>()) {
			fail(response, "E_INVALID_ARG", "cmd must be a string", "cmd");
		} else {
			const JsonString command = request_["cmd"].as<JsonString>();
			if (command.size() == 0 || memchr(command.c_str(), '\0', command.size()) != nullptr) {
				fail(response, "E_INVALID_ARG", "cmd must be a nonempty command name", "cmd");
			} else if (request_.containsKey("args") && !request_["args"].is<JsonObject>()) {
				fail(response, "E_INVALID_ARG", "args must be an object", "args");
			} else if (handler_ == nullptr) {
				fail(response, "E_INTERNAL", "device handler is unavailable");
			} else {
				response["ok"] = true;
				response.createNestedObject("result");
				handler_(context_, command.c_str(), request_["args"].as<JsonObjectConst>(),
						 response, nowMs);
			}
		}
	}

	if (response_.overflowed() || measureJson(response_) >= BoardConfig::SerialMaxLineBytes) {
		response_.clear();
		response = response_.to<JsonObject>();
		if (request_.is<JsonObject>() && request_["id"].is<int64_t>() && !error) {
			response["id"] = request_["id"];
		}
		fail(response, "E_INTERNAL", "response exceeds the configured capacity");
	}
	link_.sendJson(response_);
	request_.clear();
}