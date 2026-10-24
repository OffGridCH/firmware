#pragma once

#include <stddef.h>
#include <stdint.h>

class PacketCodec {
public:
	static constexpr size_t HeaderBytes = 8;
	static constexpr size_t TagBytes = 8;
	static constexpr size_t MaxTextBytes = 83;
	static constexpr size_t MaxPacketBytes = 100;
	enum class Error { None, Format, UnsupportedCompression, Authentication, Crypto };
	enum class Type : uint8_t { Text, Reaction, Geo };
	struct Header {
		uint8_t hopStart = 3;
		uint8_t hopLeft = 3;
		uint8_t retry = 0;
		uint16_t destination = 0;
		uint16_t source = 0;
		uint32_t counter = 0;
	};
	struct Payload {
		Type type = Type::Text;
		bool ack = false;
		char text[MaxTextBytes + 1] = {};
		uint16_t reference = 0;
		bool targetOwn = false;
		uint8_t emoji = 0;
		double latitude = 0;
		double longitude = 0;
		bool hasAltitude = false;
		int16_t altitude = 0;
		bool hasAge = false;
		uint16_t ageSeconds = 0;
	};
	struct Packet {
		uint8_t bytes[MaxPacketBytes] = {};
		size_t length = 0;
	};
	static Error inspect(const uint8_t* bytes, size_t length, Header& header);
	static Error validate(const Payload& payload, size_t& frameLength);
	static Error encode(const Header& header, const Payload& payload,
		const uint8_t key[16], Packet& packet);
	static Error decode(const uint8_t* bytes, size_t length, const uint8_t key[16], Payload& payload);
	static Error encodeAck(const Header& header, uint32_t originalCounter,
		const uint8_t key[16], Packet& packet);
	static Error verifyAck(const uint8_t* bytes, size_t length, uint32_t originalCounter,
		const uint8_t key[16]);
#if OFFGRID_RADIO_DIAGNOSTICS
	static bool selfTest();
#endif

private:
	static Error serialize(const Payload& payload, uint8_t* bytes, size_t& length);
	static bool valid(const Header& header);
	static void writeHeader(const Header& header, uint8_t* bytes);
	static void cryptoInputs(const uint8_t* bytes, uint8_t nonce[13], uint8_t ad[11],
		uint32_t originalCounter = 0);
};