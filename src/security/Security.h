#pragma once

#include "../Types.h"
#include "../storage/VaultStore.h"

class Security {
public:
	enum class Error { None, State, NoSession, Authentication, RateLimited, Storage, Crypto };
	enum class Operation { Provision, Unlock, Rotate, Reset };
	using LoadHandler = bool (*)(void* context);
	using LockHandler = void (*)(void* context);

	explicit Security(VaultStore& vault) : vault_(vault) {}
	void init(const uint8_t deviceId[6]);
	void setHandlers(LoadHandler load, LockHandler lock, void* context);
	bool beginSession(uint8_t publicKey[32], uint8_t nonce[16]);
	void consumeSession();
	bool authenticate(Operation operation, const uint8_t publicKey[32], const uint8_t wrapped[60],
		bool wellFormed, const char* name, uint64_t nowMs);
	void lock(const char* reason, uint64_t nowMs);
	void tick(uint64_t nowMs);
	void activity(uint64_t nowMs) { lastActivity_ = nowMs; }
	void setTimeout(uint32_t seconds) { timeoutS_ = seconds; }
	uint32_t timeout() const { return timeoutS_; }
	bool rename(const char* name);
	bool beginResetChallenge(uint8_t message[49], uint64_t nowMs);
	bool authorizeMasterReset(const uint8_t* signature, size_t length, uint64_t nowMs);
	const char* eventReason() const { return eventReason_; }
	void clearEvent() { eventReason_ = nullptr; }
	DeviceState state() const { return state_; }
	Error error() const { return error_; }
	const uint8_t* publicKey() const { return publicKey_; }
	const char* name() const { return name_; }
	uint16_t nodeId() const { return nodeId_; }

private:
	friend class Pairing;
	bool unwrap(const uint8_t publicKey[32], const uint8_t wrapped[60], bool wellFormed,
		bool reset, uint8_t token[32], uint64_t nowMs);
	bool loadIdentity();
	void failure(uint64_t nowMs);
	void wipeIdentity();
	static bool derive(const uint8_t token[32], const uint8_t salt[16], uint8_t key[32]);

	VaultStore& vault_;
	LoadHandler load_ = nullptr;
	LockHandler lock_ = nullptr;
	void* context_ = nullptr;
	DeviceState state_ = DeviceState::Unprovisioned;
	Error error_ = Error::None;
	uint8_t deviceId_[6] = {};
	uint8_t privateKey_[32] = {};
	uint8_t publicKey_[32] = {};
	char name_[25] = {};
	uint16_t nodeId_ = 0;
	uint8_t sessionPrivate_[32] = {};
	uint8_t sessionNonce_[16] = {};
	bool session_ = false;
	uint8_t resetMessage_[49] = {};
	uint64_t resetAt_ = 0;
	bool resetChallenge_ = false;
	uint8_t failures_ = 0;
	uint64_t retryAt_ = 0;
	uint32_t timeoutS_ = 15;
	uint64_t lastActivity_ = 0;
	const char* eventReason_ = nullptr;
};