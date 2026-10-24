// CryptoEngine for host tests: the subset the codec, contacts, groups and mesh use, on OpenSSL.
// An independent AES-CCM/HKDF implementation also cross-checks the mbedTLS results in the spec vectors.
#include "../../src/security/CryptoEngine.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <string.h>
#include <deque>

// Bytes queued here are returned by randomBytes before OpenSSL is used, so tests can force keys.
std::deque<uint8_t> testRandom;

void CryptoEngine::init() {}

bool CryptoEngine::randomBytes(uint8_t* output, size_t length) {
	for (size_t index = 0; index < length; ++index) {
		if (testRandom.empty()) return RAND_bytes(output + index, int(length - index)) == 1;
		output[index] = testRandom.front();
		testRandom.pop_front();
	}
	return true;
}

bool CryptoEngine::sha256(const uint8_t* input, size_t length, uint8_t output[Sha256Bytes]) {
	unsigned int written = 0;
	return EVP_Digest(input, length, output, &written, EVP_sha256(), nullptr) == 1 && written == Sha256Bytes;
}

bool CryptoEngine::hmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* input, size_t inputLength,
	uint8_t output[Sha256Bytes]) {
	unsigned int written = 0;
	return HMAC(EVP_sha256(), key, int(keyLength), input, inputLength, output, &written) && written == Sha256Bytes;
}

bool CryptoEngine::hkdfSha256(const uint8_t* inputKey, size_t inputKeyLength, const uint8_t* salt,
	size_t saltLength, const uint8_t* info, size_t infoLength, uint8_t* output, size_t outputLength) {
	uint8_t zero[Sha256Bytes] = {}, prk[Sha256Bytes], block[Sha256Bytes], input[Sha256Bytes + 256 + 1];
	if (infoLength > 256 || outputLength > 255 * Sha256Bytes ||
		!hmacSha256(saltLength ? salt : zero, saltLength ? saltLength : sizeof(zero), inputKey, inputKeyLength, prk))
		return false;
	size_t previous = 0;
	for (uint8_t counter = 1; outputLength; ++counter) {
		memcpy(input, block, previous);
		memcpy(input + previous, info, infoLength);
		input[previous + infoLength] = counter;
		if (!hmacSha256(prk, sizeof(prk), input, previous + infoLength + 1, block)) return false;
		const size_t take = outputLength < Sha256Bytes ? outputLength : Sha256Bytes;
		memcpy(output, block, take);
		output += take; outputLength -= take; previous = Sha256Bytes;
	}
	return true;
}

namespace {
bool ccm(bool encrypt, const uint8_t key[16], const uint8_t nonce[13], const uint8_t* ad, size_t adLength,
	const uint8_t* input, size_t length, uint8_t* output, uint8_t tag[8]) {
	EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
	uint8_t scratch[1];
	int written = 0;
	bool ok = context &&
		EVP_CipherInit_ex(context, EVP_aes_128_ccm(), nullptr, nullptr, nullptr, encrypt) == 1 &&
		EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_SET_IVLEN, 13, nullptr) == 1 &&
		EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_SET_TAG, 8, encrypt ? nullptr : tag) == 1 &&
		EVP_CipherInit_ex(context, nullptr, nullptr, key, nonce, encrypt) == 1 &&
		EVP_CipherUpdate(context, nullptr, &written, nullptr, int(length)) == 1 &&
		(!adLength || EVP_CipherUpdate(context, nullptr, &written, ad, int(adLength)) == 1) &&
		EVP_CipherUpdate(context, length ? output : scratch, &written, length ? input : scratch, int(length)) == 1;
	if (ok && encrypt) ok = EVP_CipherFinal_ex(context, scratch, &written) == 1 &&
		EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_GET_TAG, 8, tag) == 1;
	EVP_CIPHER_CTX_free(context);
	return ok;
}
}

bool CryptoEngine::aes128CcmEncrypt(const uint8_t key[Aes128KeyBytes], const uint8_t nonce[CcmNonceBytes],
	const uint8_t* associatedData, size_t associatedDataLength, const uint8_t* plaintext, size_t plaintextLength,
	uint8_t* ciphertext, uint8_t tag[CcmTagBytes]) {
	return ccm(true, key, nonce, associatedData, associatedDataLength, plaintext, plaintextLength, ciphertext, tag);
}

bool CryptoEngine::aes128CcmDecrypt(const uint8_t key[Aes128KeyBytes], const uint8_t nonce[CcmNonceBytes],
	const uint8_t* associatedData, size_t associatedDataLength, const uint8_t* ciphertext, size_t ciphertextLength,
	const uint8_t tag[CcmTagBytes], uint8_t* plaintext) {
	uint8_t copy[CcmTagBytes];
	memcpy(copy, tag, sizeof(copy));
	return ccm(false, key, nonce, associatedData, associatedDataLength, ciphertext, ciphertextLength, plaintext, copy);
}

void CryptoEngine::zeroize(void* data, size_t length) {
	volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
	while (length--) *bytes++ = 0;
}
