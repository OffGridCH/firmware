#pragma once

#include <ArduinoJson.h>

#include "SerialLink.h"
#include "../Types.h"
#include "../security/Security.h"
#include "../security/Contacts.h"
#include "../security/Pairing.h"
#include "../storage/ConfigStore.h"
#include "../drivers/RadioDriver.h"
#include "../mesh/MeshEngine.h"

class Commands {
public:
	using StateHandler = void (*)(void* context, const char* command,
								  JsonObjectConst arguments, JsonObject response,
								  uint64_t nowMs);

	Commands(SerialLink& link, StateHandler handler, void* context);
	void init();
	static void fail(JsonObject response, const char* code, const char* message,
					 const char* field = nullptr);
	static bool stringArg(JsonObjectConst args, JsonObject response, const char* field,
						  size_t minimum, size_t maximum, bool required = true);
	static bool integerArg(JsonObjectConst args, JsonObject response, const char* field,
						   int64_t minimum, int64_t maximum, bool required = true);
	static bool boolArg(JsonObjectConst args, JsonObject response, const char* field,
						bool required = true);
	static bool base64Arg(JsonObjectConst args, JsonObject response, const char* field,
						  size_t decodedBytes);
	static bool numberArg(JsonObjectConst args, JsonObject response, const char* field,
						 double minimum, double maximum, bool required = true);
	static LedPattern ledPattern(JsonVariantConst value);

	enum class Action { None, Reboot, Reset };
	void initSecure(ConfigStore& store, Security& security);
	Action handleSecure(const char* command, JsonObjectConst args, JsonObject response,
		uint64_t nowMs, Security& security, VaultStore& vault, ConfigStore& store, RadioDriver& radio,
		Contacts& contacts, Pairing& pairing, MeshEngine& mesh);
	void secureSnapshot(UiSnapshot& snapshot, uint64_t nowMs, const Security& security, const RadioDriver& radio,
		const Contacts& contacts, const Pairing& pairing);
	bool takeSecureLedConfig(LedConfig& config);
	bool takeSecureLedPreview(LedPattern& pattern);
	void emitSecureBoot(const Security& security, uint64_t nowMs);
	RadioDriver::Config secureRadioConfig() const;
	bool lockedRelay() const { return live_["mesh"]["relay_when_locked"].as<bool>(); }
	MeshEngine::Config secureMeshConfig() const;
	bool cadEnabled() const { return live_["mesh"]["cad_enabled"].as<bool>(); }
	uint8_t backoffSlots() const { return live_["mesh"]["backoff_slots"].as<uint8_t>(); }
	bool emitMeshEvent(const MeshEngine::Event& event);
	void clearDebug();
	bool emitNotices(uint64_t nowMs);
	void traceRadio(const RadioDriver::Event& event, bool unlocked, const char* action);
	uint32_t traceDrops() const { return traceDrops_; }

private:
	static void onLine(void* context, const char* line, size_t length, uint64_t nowMs);
	void processLine(const char* line, size_t length, uint64_t nowMs);

	SerialLink& link_;
	StateHandler handler_;
	void* context_;
	StaticJsonDocument<BoardConfig::RequestDocumentBytes> request_;
	StaticJsonDocument<BoardConfig::ResponseDocumentBytes> response_;
	void secureConfig(const char* command, JsonObjectConst args, JsonObject response,
		ConfigStore& store, RadioDriver& radio);
	static void securityFailure(JsonObject response, const Security& security);
	static bool identity(JsonObject output, const Security& security);
	static bool contactView(JsonObject output, uint8_t slot, const Contacts::View& view);
	void secureContacts(const char* command, JsonObjectConst args, JsonObject response,
		uint64_t nowMs, Security& security, Contacts& contacts, Pairing& pairing);
	void secureMessages(const char* command, JsonObjectConst args, JsonObject response,
		uint64_t nowMs, MeshEngine& mesh, const RadioDriver& radio);
	StaticJsonDocument<16384> live_;
	StaticJsonDocument<8192> candidate_;
	char deviceId_[13] = {};
	bool ledDirty_ = true;
	bool previewPending_ = false;
	LedPattern preview_;
	const char* logLevel_ = "warn";
	bool logPending_ = false;
	uint32_t traceDrops_ = 0;
#if OFFGRID_RADIO_DIAGNOSTICS
	bool traceEnabled_ = false;
#endif
};