#pragma once

#include "Security.h"
#include "Contacts.h"

class Pairing {
public:
	enum class Error { None, NoPairing, InvalidQr, Self, Exists, Full, Storage, Crypto };
	Pairing(Security& security, Contacts& contacts) : security_(security), contacts_(contacts) {}
	bool begin(uint8_t qr[75], size_t& length, uint64_t nowMs);
	void cancel();
	void tick(uint64_t nowMs);
	bool complete(const uint8_t* qr, size_t length, const char* name, bool replace, uint64_t nowMs,
		uint8_t& slot, bool& collision);
	bool active() const { return active_; }
	uint16_t remaining(uint64_t nowMs) const;
	const char* fingerprint() const { return fingerprint_; }
	void clearFingerprint() { fingerprint_[0] = '\0'; }
	Error error() const { return error_; }

private:
	Security& security_;
	Contacts& contacts_;
	uint8_t salt_[16] = {};
	uint64_t beganAt_ = 0;
	bool active_ = false;
	char fingerprint_[15] = {};
	Error error_ = Error::None;
};