#include "Pairing.h"
#include "Pairing.h"

#include "CryptoEngine.h"
#include <stdio.h>
#include <string.h>

void Pairing::cancel() {
	active_ = false;
	saltValid_ = false;
	CryptoEngine::zeroize(salt_, sizeof(salt_));
	CryptoEngine::zeroize(used_, sizeof(used_));
	usedCount_ = 0;
}

void Pairing::tick(uint64_t nowMs) {
	if (security_.state() != DeviceState::Unlocked || (saltValid_ && nowMs - beganAt_ >= 300000)) cancel();
	if (security_.state() != DeviceState::Unlocked) clearFingerprint();
}

uint16_t Pairing::remaining(uint64_t nowMs) const {
	return active_ && nowMs - beganAt_ < 300000 ? (300000 - (nowMs - beganAt_) + 999) / 1000 : 0;
}

bool Pairing::begin(uint8_t qr[75], size_t& length, uint64_t nowMs) {
	tick(nowMs); clearFingerprint(); length = 0;
	// One salt per 5-minute window. Both boards derive the contact keys from both salts, so the QR a
	// peer scanned must stay the QR this board pairs with. The app calls pair.begin whenever its
	// pairing screen opens, also after it scanned the peer and before the peer scans back; a fresh
	// salt there gave the two boards different keys and every packet failed its tag (lab field test
	// of 2026-10-24). A new salt starts only when less than a minute of the window is left.
	if (!saltValid_ || nowMs - beganAt_ >= 240000) {
		if (security_.state() != DeviceState::Unlocked || !CryptoEngine::randomBytes(salt_, sizeof(salt_))) {
			cancel(); error_ = Error::Crypto; return false;
		}
		beganAt_ = nowMs; saltValid_ = true;
		CryptoEngine::zeroize(used_, sizeof(used_));
		usedCount_ = 0;
	}
	memcpy(qr, "OG1", 3);
	memcpy(qr + 3, security_.publicKey(), 32);
	memcpy(qr + 35, salt_, 16);
	length = 51 + strlen(security_.name());
	memcpy(qr + 51, security_.name(), length - 51);
	active_ = true; error_ = Error::None;
	return true;
}

bool Pairing::complete(const uint8_t* qr, size_t length, const char* name, bool replace, uint64_t nowMs,
	uint8_t& slot, bool& collision) {
	tick(nowMs); slot = 0; collision = false;
	if (!saltValid_) { error_ = Error::NoPairing; return false; }
	if (!qr || length < 51 || length > 75 || memcmp(qr, "OG1", 3) ||
		memchr(qr + 51, 0, length - 51) ||
		!validUtf8(qr + 51, length - 51)) { error_ = Error::InvalidQr; return false; }
	const uint8_t* own = security_.publicKey();
	const uint8_t* peer = qr + 3;
	if (memcmp(own, peer, 32) == 0) { error_ = Error::Self; return false; }
	uint8_t peerQr[32];
	if (!CryptoEngine::sha256(qr + 3, 48, peerQr)) { error_ = Error::Crypto; return false; }
	for (size_t index = 0; index < usedCount_; ++index)
		if (memcmp(used_[index], peerQr, 32) == 0) { error_ = Error::NoPairing; return false; }
	if (usedCount_ == 4) { error_ = Error::NoPairing; return false; }
	slot = contacts_.findPublicKey(peer);
	if (slot && !replace) { error_ = Error::Exists; return false; }
	if (!slot) slot = contacts_.freeSlot();
	if (!slot) { error_ = Error::Full; return false; }
	char selected[25] = {};
	if (name) {
		if (strlen(name) < 1 || strlen(name) > 24) { error_ = Error::InvalidQr; return false; }
		strcpy(selected, name);
	} else if (length > 51) memcpy(selected, qr + 51, length - 51);
	else strcpy(selected, "Offgrid");
	struct Secrets {
		uint8_t shared[32] = {}, salt[32] = {}, info[67] = {}, outgoing[16] = {}, incoming[16] = {};
		~Secrets() { CryptoEngine::zeroize(this, sizeof(*this)); }
	} secrets;
	const bool ownFirst = memcmp(own, peer, 32) < 0;
	memcpy(secrets.salt, ownFirst ? salt_ : qr + 35, 16);
	memcpy(secrets.salt + 16, ownFirst ? qr + 35 : salt_, 16);
	memcpy(secrets.info, "og1", 3);
	memcpy(secrets.info + 3, own, 32); memcpy(secrets.info + 35, peer, 32);
	bool ok = CryptoEngine::x25519(security_.privateKey_, peer, secrets.shared) &&
		CryptoEngine::hkdfSha256(secrets.shared, 32, secrets.salt, 32, secrets.info, 67, secrets.outgoing, 16);
	memcpy(secrets.info + 3, peer, 32); memcpy(secrets.info + 35, own, 32);
	ok = ok && CryptoEngine::hkdfSha256(secrets.shared, 32, secrets.salt, 32, secrets.info, 67, secrets.incoming, 16);
	uint8_t fingerprintInput[70], digest[32];
	memcpy(fingerprintInput, "og1-fp", 6);
	memcpy(fingerprintInput + 6, ownFirst ? own : peer, 32);
	memcpy(fingerprintInput + 38, ownFirst ? peer : own, 32);
	ok = ok && CryptoEngine::sha256(fingerprintInput, sizeof(fingerprintInput), digest);
	if (!ok) { error_ = Error::Crypto; return false; }
	uint64_t digits = 0;
	for (size_t index = 0; index < 8; ++index) digits = (digits << 8) | digest[index];
	digits %= 1000000000000ULL;
	char fingerprint[15];
	snprintf(fingerprint, sizeof(fingerprint), "%04u %04u %04u",
		unsigned(digits / 100000000), unsigned((digits / 10000) % 10000), unsigned(digits % 10000));
	uint8_t nodeHash[32];
	if (!CryptoEngine::sha256(peer, 32, nodeHash)) { error_ = Error::Crypto; return false; }
	const uint16_t node = (uint16_t(nodeHash[0]) << 8) | nodeHash[1];
	collision = node == security_.nodeId();
	for (uint8_t id = 1; id <= Contacts::Capacity; ++id)
		if (id != slot && contacts_.view(id) && contacts_.view(id)->nodeId == node) collision = true;
	if (!contacts_.insert(slot, peer, selected, fingerprint, secrets.outgoing, secrets.incoming)) {
		error_ = Error::Storage; return false;
	}
	strcpy(fingerprint_, fingerprint);
	memcpy(used_[usedCount_++], peerQr, 32);
	// The session ends on the display, the salt stays for the rest of its window (see begin()).
	active_ = false; error_ = Error::None;
	return true;
}