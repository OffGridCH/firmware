#include "CryptoEngine.h"

#include <cstring>
#include <esp_system.h>
#include <bootloader_random.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ccm.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

namespace {
bool entropyReady = false;

int espRandom(void*, unsigned char* output, size_t length) {
	return CryptoEngine::randomBytes(output, length) ? 0 : MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}

bool validBuffer(const uint8_t* buffer, size_t length) {
	return buffer != nullptr || length == 0;
}

const unsigned char* nonNull(const uint8_t* buffer) {
	static const unsigned char empty = 0;
	return buffer != nullptr ? buffer : &empty;
}

unsigned char* nonNull(uint8_t* buffer) {
	static unsigned char empty = 0;
	return buffer != nullptr ? buffer : &empty;
}

bool isAllZero(const uint8_t* value, size_t length) {
	uint8_t combined = 0;
	for (size_t index = 0; index < length; ++index) {
		combined |= value[index];
	}
	return combined == 0;
}
}

bool CryptoEngine::verifyP256(const uint8_t publicKey[65], const uint8_t* message,
	size_t length, const uint8_t* signature, size_t signatureLength) {
	if (!publicKey || !message || !signature || signatureLength < 8 || signatureLength > 72) return false;
	uint8_t digest[32];
	mbedtls_ecdsa_context context;
	mbedtls_ecdsa_init(&context);
	const bool ok = sha256(message, length, digest) &&
		mbedtls_ecp_group_load(&context.grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
		mbedtls_ecp_point_read_binary(&context.grp, &context.Q, publicKey, 65) == 0 &&
		mbedtls_ecp_check_pubkey(&context.grp, &context.Q) == 0 &&
		mbedtls_ecdsa_read_signature(&context, digest, sizeof(digest), signature, signatureLength) == 0;
	mbedtls_ecdsa_free(&context);
	zeroize(digest, sizeof(digest));
	return ok;
}

void CryptoEngine::init() {
	// External SX1262 does not enable ESP32 Wi-Fi/BLE entropy. No ADC is used.
	if (!entropyReady) bootloader_random_enable();
	entropyReady = true;
}

bool CryptoEngine::randomBytes(uint8_t* output, size_t length) {
	if (!validBuffer(output, length)) {
		return false;
	}
	if (!entropyReady) {
		zeroize(output, length);
		return false;
	}
	if (length != 0) {
		esp_fill_random(output, length);
	}
	return true;
}

bool CryptoEngine::sha256(const uint8_t* input, size_t length,
						 uint8_t output[Sha256Bytes]) {
	if (output == nullptr || !validBuffer(input, length)) {
		return false;
	}
	return mbedtls_sha256_ret(nonNull(input), length, output, 0) == 0;
}

bool CryptoEngine::hmacSha256(const uint8_t* key, size_t keyLength,
						  const uint8_t* input, size_t inputLength,
						  uint8_t output[Sha256Bytes]) {
	if (!validBuffer(key, keyLength) || !validBuffer(input, inputLength) ||
		output == nullptr) {
		return false;
	}
	const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	return info != nullptr &&
		mbedtls_md_hmac(info, nonNull(key), keyLength, nonNull(input), inputLength,
					output) == 0;
}

bool CryptoEngine::hkdfSha256(const uint8_t* inputKey, size_t inputKeyLength,
						   const uint8_t* salt, size_t saltLength,
						   const uint8_t* info, size_t infoLength,
						   uint8_t* output, size_t outputLength) {
	if (!validBuffer(inputKey, inputKeyLength) || !validBuffer(salt, saltLength) ||
		!validBuffer(info, infoLength) || !validBuffer(output, outputLength) ||
		outputLength > 255 * Sha256Bytes) {
		return false;
	}
	if (outputLength == 0) {
		return true;
	}
	const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	if (mdInfo == nullptr) {
		return false;
	}
	uint8_t emptySalt[Sha256Bytes] = {};
	uint8_t pseudoRandomKey[Sha256Bytes] = {};
	uint8_t previousBlock[Sha256Bytes] = {};
	const uint8_t* extractSalt = saltLength == 0 ? emptySalt : salt;
	const size_t extractSaltLength = saltLength == 0 ? sizeof(emptySalt) : saltLength;
	bool success = mbedtls_md_hmac(mdInfo, extractSalt, extractSaltLength,
		nonNull(inputKey), inputKeyLength, pseudoRandomKey) == 0;
	mbedtls_md_context_t context;
	mbedtls_md_init(&context);
	if (success) {
		success = mbedtls_md_setup(&context, mdInfo, 1) == 0;
	}
	size_t written = 0;
	size_t previousLength = 0;
	for (uint16_t block = 1; success && written < outputLength; ++block) {
		const uint8_t blockNumber = static_cast<uint8_t>(block);
		success = mbedtls_md_hmac_starts(&context, pseudoRandomKey,
										 sizeof(pseudoRandomKey)) == 0;
		if (success && previousLength != 0) {
			success = mbedtls_md_hmac_update(&context, previousBlock,
											 previousLength) == 0;
		}
		if (success && infoLength != 0) {
			success = mbedtls_md_hmac_update(&context, info, infoLength) == 0;
		}
		if (success) {
			success = mbedtls_md_hmac_update(&context, &blockNumber, 1) == 0;
		}
		if (success) {
			success = mbedtls_md_hmac_finish(&context, previousBlock) == 0;
		}
		if (success) {
			previousLength = sizeof(previousBlock);
			const size_t remaining = outputLength - written;
			const size_t copyLength = remaining < previousLength ? remaining : previousLength;
			memcpy(output + written, previousBlock, copyLength);
			written += copyLength;
		}
	}
	mbedtls_md_free(&context);
	if (!success) {
		zeroize(output, outputLength);
	}
	zeroize(emptySalt, sizeof(emptySalt));
	zeroize(pseudoRandomKey, sizeof(pseudoRandomKey));
	zeroize(previousBlock, sizeof(previousBlock));
	return success;
}

bool CryptoEngine::generateX25519KeyPair(uint8_t privateKey[X25519Bytes],
										 uint8_t publicKey[X25519Bytes]) {
	if (privateKey == nullptr || publicKey == nullptr) {
		return false;
	}
	mbedtls_ecp_keypair keypair;
	mbedtls_ecp_keypair_init(&keypair);
	const bool success =
		mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_CURVE25519, &keypair, espRandom, nullptr) == 0 &&
		mbedtls_ecp_write_key(&keypair, privateKey, X25519Bytes) == 0 &&
		mbedtls_mpi_write_binary_le(&keypair.Q.X, publicKey, X25519Bytes) == 0;
	mbedtls_ecp_keypair_free(&keypair);
	if (!success) {
		zeroize(privateKey, X25519Bytes);
		zeroize(publicKey, X25519Bytes);
	}
	return success;
}

bool CryptoEngine::x25519(const uint8_t privateKey[X25519Bytes],
						  const uint8_t peerPublicKey[X25519Bytes],
						  uint8_t sharedSecret[X25519Bytes]) {
	if (privateKey == nullptr || peerPublicKey == nullptr || sharedSecret == nullptr) {
		return false;
	}
	mbedtls_ecp_keypair keypair;
	mbedtls_ecp_point peerPoint;
	mbedtls_mpi shared;
	mbedtls_ecp_keypair_init(&keypair);
	mbedtls_ecp_point_init(&peerPoint);
	mbedtls_mpi_init(&shared);
	const bool success =
		mbedtls_ecp_read_key(MBEDTLS_ECP_DP_CURVE25519, &keypair, privateKey,
							 X25519Bytes) == 0 &&
		mbedtls_ecp_point_read_binary(&keypair.grp, &peerPoint, peerPublicKey,
									  X25519Bytes) == 0 &&
		mbedtls_ecdh_compute_shared(&keypair.grp, &shared, &peerPoint, &keypair.d,
									 espRandom, nullptr) == 0 &&
		mbedtls_mpi_write_binary_le(&shared, sharedSecret, X25519Bytes) == 0 &&
		!isAllZero(sharedSecret, X25519Bytes);
	mbedtls_mpi_free(&shared);
	mbedtls_ecp_point_free(&peerPoint);
	mbedtls_ecp_keypair_free(&keypair);
	if (!success) {
		zeroize(sharedSecret, X25519Bytes);
	}
	return success;
}

bool CryptoEngine::aes128CcmEncrypt(const uint8_t key[Aes128KeyBytes],
									const uint8_t nonce[CcmNonceBytes],
									const uint8_t* associatedData,
									size_t associatedDataLength,
									const uint8_t* plaintext,
									size_t plaintextLength,
									uint8_t* ciphertext,
									uint8_t tag[CcmTagBytes]) {
	if (key == nullptr || nonce == nullptr || tag == nullptr ||
		!validBuffer(associatedData, associatedDataLength) ||
		!validBuffer(plaintext, plaintextLength) ||
		!validBuffer(ciphertext, plaintextLength)) {
		return false;
	}
	mbedtls_ccm_context context;
	mbedtls_ccm_init(&context);
	const bool success =
		mbedtls_ccm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
		mbedtls_ccm_encrypt_and_tag(&context, plaintextLength, nonce, CcmNonceBytes,
									 nonNull(associatedData), associatedDataLength,
									 nonNull(plaintext), nonNull(ciphertext), tag,
									 CcmTagBytes) == 0;
	mbedtls_ccm_free(&context);
	if (!success) {
		zeroize(ciphertext, plaintextLength);
		zeroize(tag, CcmTagBytes);
	}
	return success;
}

bool CryptoEngine::aes128CcmDecrypt(const uint8_t key[Aes128KeyBytes],
									const uint8_t nonce[CcmNonceBytes],
									const uint8_t* associatedData,
									size_t associatedDataLength,
									const uint8_t* ciphertext,
									size_t ciphertextLength,
									const uint8_t tag[CcmTagBytes],
									uint8_t* plaintext) {
	if (key == nullptr || nonce == nullptr || tag == nullptr ||
		!validBuffer(associatedData, associatedDataLength) ||
		!validBuffer(ciphertext, ciphertextLength) ||
		!validBuffer(plaintext, ciphertextLength)) {
		return false;
	}
	mbedtls_ccm_context context;
	mbedtls_ccm_init(&context);
	const bool success =
		mbedtls_ccm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
		mbedtls_ccm_auth_decrypt(&context, ciphertextLength, nonce, CcmNonceBytes,
									 nonNull(associatedData), associatedDataLength,
									 nonNull(ciphertext), nonNull(plaintext), tag,
									 CcmTagBytes) == 0;
	mbedtls_ccm_free(&context);
	if (!success) {
		zeroize(plaintext, ciphertextLength);
	}
	return success;
}

bool CryptoEngine::aes256GcmEncrypt(const uint8_t key[Aes256KeyBytes],
									const uint8_t nonce[GcmNonceBytes],
									const uint8_t* associatedData,
									size_t associatedDataLength,
									const uint8_t* plaintext,
									size_t plaintextLength,
									uint8_t* ciphertext,
									uint8_t tag[GcmTagBytes]) {
	if (key == nullptr || nonce == nullptr || tag == nullptr ||
		!validBuffer(associatedData, associatedDataLength) ||
		!validBuffer(plaintext, plaintextLength) ||
		!validBuffer(ciphertext, plaintextLength)) {
		return false;
	}
	mbedtls_gcm_context context;
	mbedtls_gcm_init(&context);
	const bool success =
		mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
		mbedtls_gcm_crypt_and_tag(&context, MBEDTLS_GCM_ENCRYPT, plaintextLength,
								  nonce, GcmNonceBytes, nonNull(associatedData),
								  associatedDataLength, nonNull(plaintext),
								  nonNull(ciphertext), GcmTagBytes, tag) == 0;
	mbedtls_gcm_free(&context);
	if (!success) {
		zeroize(ciphertext, plaintextLength);
		zeroize(tag, GcmTagBytes);
	}
	return success;
}

bool CryptoEngine::aes256GcmDecrypt(const uint8_t key[Aes256KeyBytes],
									const uint8_t nonce[GcmNonceBytes],
									const uint8_t* associatedData,
									size_t associatedDataLength,
									const uint8_t* ciphertext,
									size_t ciphertextLength,
									const uint8_t tag[GcmTagBytes],
									uint8_t* plaintext) {
	if (key == nullptr || nonce == nullptr || tag == nullptr ||
		!validBuffer(associatedData, associatedDataLength) ||
		!validBuffer(ciphertext, ciphertextLength) ||
		!validBuffer(plaintext, ciphertextLength) ||
		(ciphertextLength != 0 && plaintext == ciphertext)) {
		return false;
	}
	mbedtls_gcm_context context;
	mbedtls_gcm_init(&context);
	const bool success =
		mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
		mbedtls_gcm_auth_decrypt(&context, ciphertextLength, nonce, GcmNonceBytes,
									 nonNull(associatedData), associatedDataLength, tag,
									 GcmTagBytes, nonNull(ciphertext),
									 nonNull(plaintext)) == 0;
	mbedtls_gcm_free(&context);
	if (!success) {
		zeroize(plaintext, ciphertextLength);
	}
	return success;
}

void CryptoEngine::zeroize(void* data, size_t length) {
	if (data != nullptr && length != 0) {
		mbedtls_platform_zeroize(data, length);
	}
}