#pragma once

#include <stddef.h>
#include <stdint.h>

class CryptoEngine {
public:
	static constexpr size_t Sha256Bytes = 32;
	static constexpr size_t X25519Bytes = 32;
	static constexpr size_t Aes128KeyBytes = 16;
	static constexpr size_t Aes256KeyBytes = 32;
	static constexpr size_t CcmNonceBytes = 13;
	static constexpr size_t CcmTagBytes = 8;
	static constexpr size_t GcmNonceBytes = 12;
	static constexpr size_t GcmTagBytes = 16;

	static bool randomBytes(uint8_t* output, size_t length);
	static bool sha256(const uint8_t* input, size_t length,
				   uint8_t output[Sha256Bytes]);
	static bool hmacSha256(const uint8_t* key, size_t keyLength,
					   const uint8_t* input, size_t inputLength,
					   uint8_t output[Sha256Bytes]);
	static bool hkdfSha256(const uint8_t* inputKey, size_t inputKeyLength,
						const uint8_t* salt, size_t saltLength,
						const uint8_t* info, size_t infoLength,
						uint8_t* output, size_t outputLength);
	static bool generateX25519KeyPair(uint8_t privateKey[X25519Bytes],
								   uint8_t publicKey[X25519Bytes]);
	static bool x25519(const uint8_t privateKey[X25519Bytes],
					const uint8_t peerPublicKey[X25519Bytes],
					uint8_t sharedSecret[X25519Bytes]);
	static bool aes128CcmEncrypt(const uint8_t key[Aes128KeyBytes],
							 const uint8_t nonce[CcmNonceBytes],
							 const uint8_t* associatedData, size_t associatedDataLength,
							 const uint8_t* plaintext, size_t plaintextLength,
							 uint8_t* ciphertext, uint8_t tag[CcmTagBytes]);
	static bool aes128CcmDecrypt(const uint8_t key[Aes128KeyBytes],
							 const uint8_t nonce[CcmNonceBytes],
							 const uint8_t* associatedData, size_t associatedDataLength,
							 const uint8_t* ciphertext, size_t ciphertextLength,
							 const uint8_t tag[CcmTagBytes], uint8_t* plaintext);
	static bool aes256GcmEncrypt(const uint8_t key[Aes256KeyBytes],
							 const uint8_t nonce[GcmNonceBytes],
							 const uint8_t* associatedData, size_t associatedDataLength,
							 const uint8_t* plaintext, size_t plaintextLength,
							 uint8_t* ciphertext, uint8_t tag[GcmTagBytes]);
	static bool aes256GcmDecrypt(const uint8_t key[Aes256KeyBytes],
							 const uint8_t nonce[GcmNonceBytes],
							 const uint8_t* associatedData, size_t associatedDataLength,
							 const uint8_t* ciphertext, size_t ciphertextLength,
							 const uint8_t tag[GcmTagBytes], uint8_t* plaintext);
	static void zeroize(void* data, size_t length);
	static bool verifyP256(const uint8_t publicKey[65], const uint8_t* message,
		size_t length, const uint8_t* signature, size_t signatureLength);
};