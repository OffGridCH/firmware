#include "PacketCodec.h"

#include <math.h>
#include <string.h>
#include "../Types.h"
#include "../security/CryptoEngine.h"

namespace {
uint32_t read24(const uint8_t* bytes) {
	return (uint32_t(bytes[0]) << 16) | (uint32_t(bytes[1]) << 8) | bytes[2];
}

void write24(uint8_t* bytes, uint32_t value) {
	bytes[0] = value >> 16;
	bytes[1] = value >> 8;
	bytes[2] = value;
}

struct Plaintext {
	uint8_t bytes[PacketCodec::MaxPacketBytes - PacketCodec::HeaderBytes - PacketCodec::TagBytes] = {};
	~Plaintext() { CryptoEngine::zeroize(bytes, sizeof(bytes)); }
};

uint32_t coordinate(double degrees, double offset, double range) {
	const double rounded = floor((degrees + offset) / range * 16777216.0 + 0.5);
	return rounded >= 16777215.0 ? 0xffffff : uint32_t(rounded);
}
}

bool PacketCodec::valid(const Header& header) {
	return header.hopStart <= 7 && header.hopLeft <= header.hopStart &&
		header.retry <= 3 && header.counter > 0 && header.counter <= 0xffffff;
}

void PacketCodec::writeHeader(const Header& header, uint8_t* bytes) {
	bytes[0] = (header.hopStart << 5) | (header.hopLeft << 2) | header.retry;
	bytes[1] = header.destination >> 8;
	bytes[2] = header.destination;
	bytes[3] = header.source >> 8;
	bytes[4] = header.source;
	write24(bytes + 5, header.counter);
}

PacketCodec::Error PacketCodec::inspect(const uint8_t* bytes, size_t length, Header& header) {
	header = {};
	if (!bytes || length < HeaderBytes + TagBytes || length > MaxPacketBytes) return Error::Format;
	Header candidate;
	candidate.hopStart = bytes[0] >> 5;
	candidate.hopLeft = (bytes[0] >> 2) & 7;
	candidate.retry = bytes[0] & 3;
	candidate.destination = (uint16_t(bytes[1]) << 8) | bytes[2];
	candidate.source = (uint16_t(bytes[3]) << 8) | bytes[4];
	candidate.counter = read24(bytes + 5);
	if (!valid(candidate) || (length == 16 && candidate.retry != 0)) return Error::Format;
	header = candidate;
	return Error::None;
}

void PacketCodec::cryptoInputs(const uint8_t* bytes, uint8_t nonce[13], uint8_t ad[11],
	uint32_t originalCounter) {
	memcpy(nonce, bytes + 3, 2);
	memcpy(nonce + 2, bytes + 1, 2);
	memcpy(nonce + 4, bytes + 5, 3);
	nonce[7] = bytes[0] & 3;
	memset(nonce + 8, 0, 5);
	memcpy(ad, bytes, HeaderBytes);
	ad[0] &= 0xe3;
	write24(ad + HeaderBytes, originalCounter);
}

PacketCodec::Error PacketCodec::serialize(const Payload& payload, uint8_t* bytes, size_t& length) {
	Plaintext plain;
	length = 0;
	const uint8_t ack = payload.ack ? 8 : 0;
	switch (payload.type) {
	case Type::Text: {
		const size_t textLength = strnlen(payload.text, sizeof(payload.text));
		if (textLength > MaxTextBytes || !validUtf8(reinterpret_cast<const uint8_t*>(payload.text), textLength))
			return Error::Format;
		plain.bytes[0] = ack;
		memcpy(plain.bytes + 1, payload.text, textLength);
		length = 1 + textLength;
		break;
	}
	case Type::Reaction:
		if (payload.emoji > 6) return Error::Format;
		plain.bytes[0] = 0x10 | ack | (payload.targetOwn ? 4 : 0);
		plain.bytes[1] = payload.reference >> 8;
		plain.bytes[2] = payload.reference;
		plain.bytes[3] = payload.emoji;
		length = 4;
		break;
	case Type::Geo:
		if (!isfinite(payload.latitude) || !isfinite(payload.longitude) ||
			payload.latitude < -90 || payload.latitude > 90 ||
			payload.longitude < -180 || payload.longitude > 180 ||
			(payload.hasAge && payload.ageSeconds > 2550)) return Error::Format;
		plain.bytes[0] = 0x20 | ack | (payload.hasAltitude ? 2 : 0) | (payload.hasAge ? 1 : 0);
		write24(plain.bytes + 1, coordinate(payload.latitude, 90, 180));
		write24(plain.bytes + 4, coordinate(payload.longitude, 180, 360));
		length = 7;
		if (payload.hasAltitude) {
			plain.bytes[length++] = uint16_t(payload.altitude) >> 8;
			plain.bytes[length++] = uint16_t(payload.altitude);
		}
		if (payload.hasAge) plain.bytes[length++] = (payload.ageSeconds + 5) / 10;
		break;
	default: return Error::Format;
	}
	memcpy(bytes, plain.bytes, length);
	return Error::None;
}

PacketCodec::Error PacketCodec::validate(const Payload& payload, size_t& frameLength) {
	frameLength = 0;
	Plaintext plain;
	size_t length = 0;
	const Error error = serialize(payload, plain.bytes, length);
	if (error == Error::None) frameLength = HeaderBytes + length + TagBytes;
	return error;
}

PacketCodec::Error PacketCodec::encode(const Header& header, const Payload& payload,
	const uint8_t key[16], Packet& packet) {
	packet = {};
	if (!key || !valid(header)) return Error::Format;
	Plaintext plain;
	size_t length = 0;
	const Error error = serialize(payload, plain.bytes, length);
	if (error != Error::None) return error;
	writeHeader(header, packet.bytes);
	uint8_t nonce[13], ad[11];
	cryptoInputs(packet.bytes, nonce, ad);
	if (!CryptoEngine::aes128CcmEncrypt(key, nonce, ad, HeaderBytes, plain.bytes, length,
		packet.bytes + HeaderBytes, packet.bytes + HeaderBytes + length)) {
		packet = {};
		return Error::Crypto;
	}
	packet.length = HeaderBytes + length + TagBytes;
	return Error::None;
}

PacketCodec::Error PacketCodec::decode(const uint8_t* bytes, size_t length,
	const uint8_t key[16], Payload& payload) {
	payload = {};
	Header header;
	if (!key || inspect(bytes, length, header) != Error::None || length == 16) return Error::Format;
	const size_t plainLength = length - HeaderBytes - TagBytes;
	Plaintext plain;
	uint8_t nonce[13], ad[11];
	cryptoInputs(bytes, nonce, ad);
	if (!CryptoEngine::aes128CcmDecrypt(key, nonce, ad, HeaderBytes, bytes + HeaderBytes,
		plainLength, bytes + length - TagBytes, plain.bytes)) return Error::Authentication;
	const uint8_t type = plain.bytes[0] >> 4;
	const uint8_t flags = plain.bytes[0] & 15;
	Payload candidate;
	candidate.ack = flags & 8;
	switch (type) {
	case 0:
		if (flags & 3) return Error::Format;
		if (flags & 4) return Error::UnsupportedCompression;
		if (memchr(plain.bytes + 1, 0, plainLength - 1) ||
			!validUtf8(plain.bytes + 1, plainLength - 1)) return Error::Format;
		memcpy(candidate.text, plain.bytes + 1, plainLength - 1);
		break;
	case 1:
		if ((flags & 3) || plainLength != 4 || plain.bytes[3] > 6) return Error::Format;
		candidate.type = Type::Reaction;
		candidate.reference = (uint16_t(plain.bytes[1]) << 8) | plain.bytes[2];
		candidate.targetOwn = flags & 4;
		candidate.emoji = plain.bytes[3];
		break;
	case 2: {
		const size_t expected = 7 + ((flags & 2) ? 2 : 0) + ((flags & 1) ? 1 : 0);
		if ((flags & 4) || plainLength != expected) return Error::Format;
		candidate.type = Type::Geo;
		candidate.latitude = double(read24(plain.bytes + 1)) / 16777216.0 * 180 - 90;
		candidate.longitude = double(read24(plain.bytes + 4)) / 16777216.0 * 360 - 180;
		size_t offset = 7;
		candidate.hasAltitude = flags & 2;
		candidate.hasAge = flags & 1;
		if (candidate.hasAltitude) {
			const uint16_t raw = (uint16_t(plain.bytes[offset]) << 8) | plain.bytes[offset + 1];
			candidate.altitude = raw & 0x8000 ? int32_t(raw) - 65536 : raw;
			offset += 2;
		}
		if (candidate.hasAge) candidate.ageSeconds = uint16_t(plain.bytes[offset]) * 10;
		break;
	}
	default: return Error::Format;
	}
	payload = candidate;
	CryptoEngine::zeroize(&candidate, sizeof(candidate));
	return Error::None;
}

PacketCodec::Error PacketCodec::encodeAck(const Header& header, uint32_t originalCounter,
	const uint8_t key[16], Packet& packet) {
	packet = {};
	if (!key || !valid(header) || header.retry || !originalCounter || originalCounter > 0xffffff)
		return Error::Format;
	writeHeader(header, packet.bytes);
	uint8_t nonce[13], ad[11];
	cryptoInputs(packet.bytes, nonce, ad, originalCounter);
	if (!CryptoEngine::aes128CcmEncrypt(key, nonce, ad, sizeof(ad), nullptr, 0,
		nullptr, packet.bytes + HeaderBytes)) {
		packet = {};
		return Error::Crypto;
	}
	packet.length = 16;
	return Error::None;
}

PacketCodec::Error PacketCodec::verifyAck(const uint8_t* bytes, size_t length, uint32_t originalCounter,
	const uint8_t key[16]) {
	Header header;
	if (!key || length != 16 || inspect(bytes, length, header) != Error::None ||
		!originalCounter || originalCounter > 0xffffff) return Error::Format;
	uint8_t nonce[13], ad[11];
	cryptoInputs(bytes, nonce, ad, originalCounter);
	return CryptoEngine::aes128CcmDecrypt(key, nonce, ad, sizeof(ad), nullptr, 0,
		bytes + HeaderBytes, nullptr) ? Error::None : Error::Authentication;
}

#if OFFGRID_RADIO_DIAGNOSTICS
bool PacketCodec::selfTest() {
	auto fromHex = [](const char* hex, uint8_t* bytes, size_t length) {
		auto digit = [](char value) { return value <= '9' ? value - '0' : value - 'a' + 10; };
		for (size_t index = 0; index < length; ++index)
			bytes[index] = (digit(hex[index * 2]) << 4) | digit(hex[index * 2 + 1]);
	};
	uint8_t alice[16], bob[16];
	fromHex("b6c3836e8bfc1d784b0688a5963e1497", alice, 16);
	fromHex("869a272db66553bf737a40f9fb8929ca", bob, 16);
	Packet packet, expected;
	Header header;
	header.source = 0xe3f0;
	header.destination = 0xe841;
	header.counter = 1;
	Payload payload, decoded;
	payload.ack = true;
	strcpy(payload.text, "Hello!");
	auto matches = [&](const char* hex, size_t length) {
		fromHex(hex, expected.bytes, length);
		return packet.length == length && memcmp(packet.bytes, expected.bytes, length) == 0;
	};
	if (encode(header, payload, alice, packet) != Error::None ||
		!matches("6ce841e3f00000015d1d3fd92a3e04da794024db76adbc", 23) ||
		decode(packet.bytes, packet.length, alice, decoded) != Error::None ||
		strcmp(decoded.text, "Hello!") || !decoded.ack) return false;
	packet.bytes[0] = 0x68;
	if (decode(packet.bytes, packet.length, alice, decoded) != Error::None) return false;
	for (size_t index = 0; index < packet.length; ++index) {
		packet.bytes[index] ^= index == 0 ? 1 : 0x80;
		const Error error = decode(packet.bytes, packet.length, alice, decoded);
		packet.bytes[index] ^= index == 0 ? 1 : 0x80;
		if (error == Error::None || decoded.text[0]) return false;
	}
	header.source = 0xe841;
	header.destination = 0xe3f0;
	header.hopStart = header.hopLeft = 2;
	if (encodeAck(header, 1, bob, packet) != Error::None ||
		!matches("48e3f0e8410000017f15646b08384b1d", 16) ||
		verifyAck(packet.bytes, packet.length, 7, bob) != Error::Authentication ||
		verifyAck(packet.bytes, packet.length, 1, bob) != Error::None) return false;
	packet.bytes[0] |= 1;
	if (verifyAck(packet.bytes, packet.length, 1, bob) != Error::Format) return false;
	header.hopStart = header.hopLeft = 3;
	header.counter = 2;
	payload = {};
	payload.type = Type::Reaction;
	payload.reference = payload.emoji = 1;
	if (encode(header, payload, bob, packet) != Error::None ||
		!matches("6ce3f0e841000002080240450920b20d483bbe72", 20) ||
		decode(packet.bytes, packet.length, bob, decoded) != Error::None ||
		decoded.type != Type::Reaction || decoded.reference != 1 || decoded.emoji != 1 ||
		decoded.targetOwn) return false;
	header.source = 0xe3f0;
	header.destination = 0xe841;
	payload = {};
	payload.type = Type::Geo;
	payload.latitude = 47.0517;
	payload.longitude = 8.3075;
	if (encode(header, payload, alice, packet) != Error::None ||
		!matches("6ce841e3f0000002fd277016a3aa4e6fadd2e9abc5d993", 23) ||
		decode(packet.bytes, packet.length, alice, decoded) != Error::None ||
		fabs(decoded.latitude - payload.latitude) > 0.000011 ||
		fabs(decoded.longitude - payload.longitude) > 0.000022) return false;
	payload.latitude = 90;
	payload.longitude = 180;
	payload.hasAltitude = payload.hasAge = true;
	payload.altitude = -32768;
	payload.ageSeconds = 2546;
	if (encode(header, payload, alice, packet) != Error::None || packet.length != 26 ||
		decode(packet.bytes, packet.length, alice, decoded) != Error::None ||
		decoded.altitude != -32768 || decoded.ageSeconds != 2550 ||
		decoded.latitude >= 90 || decoded.latitude < 89.99998 ||
		decoded.longitude >= 180 || decoded.longitude < 179.99997) return false;
	payload.latitude = -90;
	payload.longitude = -180;
	payload.altitude = 32767;
	payload.ageSeconds = 4;
	if (encode(header, payload, alice, packet) != Error::None ||
		decode(packet.bytes, packet.length, alice, decoded) != Error::None ||
		decoded.latitude != -90 || decoded.longitude != -180 ||
		decoded.altitude != 32767 || decoded.ageSeconds != 0) return false;
	payload.latitude = NAN;
	if (encode(header, payload, alice, packet) != Error::Format || packet.length) return false;
	payload = {};
	memset(payload.text, 'x', MaxTextBytes);
	header.retry = 3;
	if (encode(header, payload, alice, packet) != Error::None || packet.length != 100 ||
		decode(packet.bytes, packet.length, alice, decoded) != Error::None ||
		strcmp(decoded.text, payload.text)) return false;
	payload.text[MaxTextBytes] = 'x';
	if (encode(header, payload, alice, packet) != Error::Format || packet.length) return false;
	payload = {};
	payload.text[0] = char(0xc0);
	if (encode(header, payload, alice, packet) != Error::Format) return false;
	payload = {};
	header.retry = 0;
	if (encode(header, payload, alice, packet) != Error::None || packet.length != 17 ||
		decode(packet.bytes, packet.length, alice, decoded) != Error::None || decoded.text[0]) return false;
	uint8_t nonce[13], ad[11];
	cryptoInputs(packet.bytes, nonce, ad);
	const uint8_t invalid[] = {4, 1, 0x30, 0x14, 0x24};
	for (uint8_t inner : invalid) {
		if (!CryptoEngine::aes128CcmEncrypt(alice, nonce, ad, 8, &inner, 1,
			packet.bytes + 8, packet.bytes + 9)) return false;
		const Error result = decode(packet.bytes, packet.length, alice, decoded);
		if (result != (inner == 4 ? Error::UnsupportedCompression : Error::Format)) return false;
	}
	header.counter = 0;
	if (encode(header, payload, alice, packet) != Error::Format) return false;
	header.counter = 0x1000000;
	if (encode(header, payload, alice, packet) != Error::Format) return false;
	header.counter = 1;
	header.hopStart = 0;
	if (encode(header, payload, alice, packet) != Error::Format) return false;
	header.hopLeft = 0;
	header.counter = 0xffffff;
	if (encode(header, payload, alice, packet) != Error::None ||
		decode(packet.bytes, packet.length, bob, decoded) != Error::Authentication ||
		decode(packet.bytes, packet.length, alice, decoded) != Error::None) return false;
	Header parsed;
	if (inspect(nullptr, 16, parsed) != Error::Format ||
		inspect(packet.bytes, 15, parsed) != Error::Format ||
		inspect(packet.bytes, 101, parsed) != Error::Format) return false;
	write24(packet.bytes + 5, 0);
	if (inspect(packet.bytes, packet.length, parsed) != Error::Format) return false;
	header.hopStart = header.hopLeft = 7;
	for (uint8_t retry = 0; retry <= 3; ++retry) {
		header.retry = retry;
		if (encode(header, payload, alice, packet) != Error::None ||
			decode(packet.bytes, packet.length, alice, decoded) != Error::None ||
			inspect(packet.bytes, packet.length, parsed) != Error::None ||
			parsed.retry != retry || parsed.counter != 0xffffff) return false;
	}
	header.retry = 0;
	if (encodeAck(header, 0, alice, packet) != Error::Format ||
		encodeAck(header, 0x1000000, alice, packet) != Error::Format) return false;
	payload.type = Type::Reaction;
	payload.emoji = 7;
	if (encode(header, payload, alice, packet) != Error::Format) return false;
	payload = {};
	payload.type = Type::Geo;
	payload.latitude = 90.0001;
	if (encode(header, payload, alice, packet) != Error::Format) return false;
	payload.latitude = 0;
	payload.longitude = INFINITY;
	if (encode(header, payload, alice, packet) != Error::Format) return false;
	payload.longitude = 0;
	payload.hasAge = true;
	payload.ageSeconds = 2551;
	if (encode(header, payload, alice, packet) != Error::Format) return false;
	return true;
}
#endif