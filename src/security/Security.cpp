#include "Security.h"
#include "Security.h"

#include "CryptoEngine.h"
#include "ResetPublicKey.h"
#include "../storage/ConfigStore.h"
#include <stdio.h>
#include <string.h>

void Security::setHandlers(LoadHandler load, LockHandler lock, void* context) {
	load_ = load; lock_ = lock; context_ = context;
}

void Security::wipeIdentity() {
	CryptoEngine::zeroize(privateKey_, sizeof(privateKey_));
	CryptoEngine::zeroize(publicKey_, sizeof(publicKey_));
	CryptoEngine::zeroize(name_, sizeof(name_));
	nodeId_ = 0;
}

void Security::consumeSession() {
	session_ = false;
	CryptoEngine::zeroize(sessionPrivate_, sizeof(sessionPrivate_));
	CryptoEngine::zeroize(sessionNonce_, sizeof(sessionNonce_));
}

void Security::init(const uint8_t deviceId[6]) {
	consumeSession();
	wipeIdentity();
	memcpy(deviceId_, deviceId, sizeof(deviceId_));
	vault_.load();
	state_ = vault_.status() == VaultStore::Status::Missing ?
		DeviceState::Unprovisioned : DeviceState::Locked;
	error_ = vault_.status() == VaultStore::Status::Missing || vault_.status() == VaultStore::Status::Ready ?
		Error::None : Error::Storage;
	bool resetPending = false;
	if (ConfigStore::resetPending(resetPending) != 0 || resetPending) {
		state_ = DeviceState::Locked;
		error_ = Error::Storage;
	}
}

bool Security::beginSession(uint8_t publicKey[32], uint8_t nonce[16]) {
	consumeSession();
	const bool ok = CryptoEngine::generateX25519KeyPair(sessionPrivate_, publicKey) &&
		CryptoEngine::randomBytes(sessionNonce_, sizeof(sessionNonce_));
	if (!ok) { consumeSession(); error_ = Error::Crypto; return false; }
	memcpy(nonce, sessionNonce_, sizeof(sessionNonce_));
	session_ = true;
	error_ = Error::None;
	return true;
}

void Security::failure(uint64_t nowMs) {
	if (failures_ < 8) ++failures_;
	if (failures_ >= 3) {
		uint32_t delay = 2000u << (failures_ - 3);
		if (delay > 60000) delay = 60000;
		retryAt_ = nowMs + delay;
	}
	error_ = Error::Authentication;
}

bool Security::unwrap(const uint8_t publicKey[32], const uint8_t wrapped[60], bool wellFormed,
	bool reset, uint8_t token[32], uint64_t nowMs) {
	CryptoEngine::zeroize(token, 32);
	if (!session_) { error_ = Error::NoSession; return false; }
	if (nowMs < retryAt_) { consumeSession(); error_ = Error::RateLimited; return false; }
	uint8_t shared[32] = {}, key[32] = {}, info[23] = {};
	const char* label = reset ? "og-reset-wrap-v1" : "og-unlock-wrap-v1";
	const size_t labelLength = strlen(label);
	memcpy(info, label, labelLength);
	memcpy(info + labelLength, deviceId_, sizeof(deviceId_));
	const bool ok = wellFormed && CryptoEngine::x25519(sessionPrivate_, publicKey, shared) &&
		CryptoEngine::hkdfSha256(shared, sizeof(shared), sessionNonce_, sizeof(sessionNonce_),
			info, labelLength + sizeof(deviceId_), key, sizeof(key)) &&
		CryptoEngine::aes256GcmDecrypt(key, wrapped, deviceId_, sizeof(deviceId_),
			wrapped + 12, 32, wrapped + 44, token);
	consumeSession();
	CryptoEngine::zeroize(shared, sizeof(shared));
	CryptoEngine::zeroize(key, sizeof(key));
	if (!ok) failure(nowMs);
	return ok;
}

bool Security::derive(const uint8_t token[32], const uint8_t salt[16], uint8_t key[32]) {
	const uint8_t info[] = "og-vault-v1";
	return CryptoEngine::hkdfSha256(token, 32, salt, 16, info, sizeof(info) - 1, key, 32);
}

bool Security::loadIdentity() {
	uint8_t record[VaultStore::MaxPlaintext] = {}, digest[32] = {}, base[32] = {9};
	size_t length = 0;
	bool ok = vault_.readIdentity(record, sizeof(record), length) && length >= 34 &&
		record[32] >= 1 && record[32] <= 24 && length == size_t(33 + record[32]) &&
		memchr(record + 33, 0, record[32]) == nullptr;
	if (ok) {
		memcpy(privateKey_, record, sizeof(privateKey_));
		memcpy(name_, record + 33, record[32]);
		name_[record[32]] = '\0';
		ok = CryptoEngine::x25519(privateKey_, base, publicKey_) &&
			CryptoEngine::sha256(publicKey_, sizeof(publicKey_), digest);
		if (ok) nodeId_ = (uint16_t(digest[0]) << 8) | digest[1];
	}
	CryptoEngine::zeroize(record, sizeof(record));
	CryptoEngine::zeroize(digest, sizeof(digest));
	if (!ok) wipeIdentity();
	return ok;
}

bool Security::authenticate(Operation operation, const uint8_t publicKey[32], const uint8_t wrapped[60],
	bool wellFormed, const char* name, uint64_t nowMs) {
	uint8_t token[32] = {}, key[32] = {}, salt[16] = {}, identity[57] = {};
	if (!unwrap(publicKey, wrapped, wellFormed, operation == Operation::Reset, token, nowMs)) return false;
	bool ok = false;
	if (operation == Operation::Provision) {
		if (state_ != DeviceState::Unprovisioned) error_ = Error::State;
		else {
			uint8_t generatedPublic[32] = {}, digest[32] = {};
			ok = CryptoEngine::generateX25519KeyPair(identity, generatedPublic) &&
				CryptoEngine::sha256(generatedPublic, 32, digest) &&
				CryptoEngine::randomBytes(salt, sizeof(salt)) && derive(token, salt, key);
			char defaultName[25];
			snprintf(defaultName, sizeof(defaultName), "Offgrid-%02X%02X", digest[0], digest[1]);
			const char* selected = name ? name : defaultName;
			const size_t nameLength = strlen(selected);
			if (nameLength < 1 || nameLength > 24) ok = false;
			if (ok) {
				identity[32] = nameLength;
				memcpy(identity + 33, selected, nameLength);
				ok = vault_.create(key, salt, identity, 33 + nameLength) && loadIdentity();
			}
			CryptoEngine::zeroize(generatedPublic, sizeof(generatedPublic));
			CryptoEngine::zeroize(digest, sizeof(digest));
			if (!ok) error_ = Error::Storage;
		}
	} else if (operation == Operation::Rotate) {
		if (state_ != DeviceState::Unlocked) error_ = Error::State;
		else {
			ok = CryptoEngine::randomBytes(salt, sizeof(salt)) &&
				derive(token, salt, key) && vault_.rotate(key, salt);
			if (!ok) error_ = Error::Storage;
		}
	} else {
		ok = derive(token, vault_.salt(), key) &&
			(operation == Operation::Reset ? vault_.verifyKey(key) : vault_.open(key));
		if (!ok) failure(nowMs);
		else if (operation == Operation::Unlock) {
			ok = loadIdentity() && (load_ ? load_(context_) : vault_.contacts() == 0);
			if (!ok) error_ = Error::Storage;
		}
	}
	CryptoEngine::zeroize(token, sizeof(token));
	CryptoEngine::zeroize(key, sizeof(key));
	CryptoEngine::zeroize(identity, sizeof(identity));
	if (!ok) {
		if (operation == Operation::Unlock || operation == Operation::Provision || !vault_.isOpen()) {
			const bool wasUnlocked = state_ == DeviceState::Unlocked;
			if (lock_) lock_(context_);
			wipeIdentity();
			vault_.close();
			state_ = vault_.status() == VaultStore::Status::Missing ?
				DeviceState::Unprovisioned : DeviceState::Locked;
			if (wasUnlocked) eventReason_ = "lock";
		}
		return false;
	}
	failures_ = 0; retryAt_ = 0; error_ = Error::None;
	if (operation == Operation::Provision || operation == Operation::Unlock) {
		state_ = DeviceState::Unlocked;
		lastActivity_ = nowMs;
		eventReason_ = operation == Operation::Provision ? "provision" : "unlock";
	}
	return true;
}

bool Security::rename(const char* name) {
	const size_t length = strlen(name);
	if (state_ != DeviceState::Unlocked || length < 1 || length > 24) {
		error_ = Error::State; return false;
	}
	uint8_t record[57];
	memcpy(record, privateKey_, 32);
	record[32] = length;
	memcpy(record + 33, name, length);
	const bool ok = vault_.writeIdentity(record, 33 + length);
	CryptoEngine::zeroize(record, sizeof(record));
	if (!ok) { error_ = Error::Storage; return false; }
	memcpy(name_, name, length + 1);
	error_ = Error::None;
	return true;
}

void Security::lock(const char* reason, uint64_t) {
	consumeSession();
	if (lock_) lock_(context_);
	wipeIdentity();
	vault_.close();
	if (state_ == DeviceState::Unlocked) {
		state_ = DeviceState::Locked;
		eventReason_ = reason;
	}
}

void Security::tick(uint64_t nowMs) {
	if (state_ == DeviceState::Unlocked && timeoutS_ && nowMs - lastActivity_ >= timeoutS_ * 1000ULL)
		lock("heartbeat_timeout", nowMs);
	if (resetChallenge_ && nowMs - resetAt_ >= 30000) {
		resetChallenge_ = false;
		CryptoEngine::zeroize(resetMessage_, sizeof(resetMessage_));
	}
}

bool Security::beginResetChallenge(uint8_t message[49], uint64_t nowMs) {
	resetChallenge_ = false;
	memcpy(resetMessage_, "og-reset-v1", 11);
	memcpy(resetMessage_ + 11, deviceId_, sizeof(deviceId_));
	if (!CryptoEngine::randomBytes(resetMessage_ + 17, 32)) { error_ = Error::Crypto; return false; }
	memcpy(message, resetMessage_, sizeof(resetMessage_));
	resetAt_ = nowMs; resetChallenge_ = true;
	return true;
}

bool Security::authorizeMasterReset(const uint8_t* signature, size_t length, uint64_t nowMs) {
	const bool fresh = resetChallenge_ && nowMs - resetAt_ < 30000;
	resetChallenge_ = false;
	const bool ok = fresh &&
		CryptoEngine::verifyP256(ResetTrust::PublicKey, resetMessage_, sizeof(resetMessage_), signature, length);
	CryptoEngine::zeroize(resetMessage_, sizeof(resetMessage_));
	error_ = ok ? Error::None : Error::Authentication;
	return ok;
}