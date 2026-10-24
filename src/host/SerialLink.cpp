#include "SerialLink.h"

SerialLink::SerialLink(Stream& port) : port_(port) {
}

void SerialLink::init() {
	rxLength_ = 0;
	discarding_ = false;
	txHead_ = 0;
	txCount_ = 1;
	tx_[0].bytes[0] = '\n';
	tx_[0].length = 1;
	tx_[0].offset = 0;
}

void SerialLink::setLineHandler(LineHandler handler, void* context) {
	handler_ = handler;
	context_ = context;
}

void SerialLink::tick(uint64_t nowMs) {
	drainTx();
	receive(nowMs);
}

void SerialLink::setReceiveEnabled(bool enabled) {
	receiveEnabled_ = enabled;
}

size_t SerialLink::availableTxSlots() const {
	return BoardConfig::SerialTxSlots - txCount_;
}

bool SerialLink::sendJson(const JsonDocument& document) {
	if (document.overflowed() || txCount_ == BoardConfig::SerialTxSlots) {
		return false;
	}

	const size_t length = measureJson(document);
	if (length >= BoardConfig::SerialMaxLineBytes) {
		return false;
	}

	TxLine& slot = tx_[(txHead_ + txCount_) % BoardConfig::SerialTxSlots];
	const size_t written = serializeJson(document, slot.bytes, sizeof(slot.bytes));
	if (written != length) {
		return false;
	}
	slot.bytes[length] = '\n';
	slot.length = length + 1;
	slot.offset = 0;
	++txCount_;
	return true;
}

bool SerialLink::sendError(const char* code, const char* message, uint64_t nowMs) {
	StaticJsonDocument<384> document;
	document["evt"] = "error";
	document["t"] = nowMs;
	JsonObject data = document.createNestedObject("data");
	data["code"] = code;
	data["message"] = message;
	return sendJson(document);
}

void SerialLink::drainTx() {
	if (txCount_ == 0) {
		return;
	}

	const int available = port_.availableForWrite();
	if (available <= 0) {
		return;
	}

	TxLine& slot = tx_[txHead_];
	size_t count = slot.length - slot.offset;
	if (count > static_cast<size_t>(available)) {
		count = static_cast<size_t>(available);
	}
	if (count > BoardConfig::SerialTxBytesPerTick) {
		count = BoardConfig::SerialTxBytesPerTick;
	}

	const size_t written = port_.write(
		reinterpret_cast<const uint8_t*>(slot.bytes + slot.offset), count);
	slot.offset += written;
	if (slot.offset == slot.length) {
		txHead_ = (txHead_ + 1) % BoardConfig::SerialTxSlots;
		--txCount_;
	}
}

void SerialLink::receive(uint64_t nowMs) {
	if (!receiveEnabled_ || handler_ == nullptr || BoardConfig::SerialTxSlots - txCount_ < 2) {
		return;
	}

	for (size_t processed = 0; processed < BoardConfig::SerialRxBytesPerTick;
		 ++processed) {
		if (port_.available() <= 0) {
			return;
		}
		const int value = port_.read();
		if (value < 0) {
			return;
		}
		if (value == '\n') {
			if (discarding_) {
				sendError("E_LINE_TOO_LONG", "line exceeded 2048 bytes and was dropped", nowMs);
			} else {
				if (rxLength_ > 0 && rx_[rxLength_ - 1] == '\r') {
					--rxLength_;
				}
				rx_[rxLength_] = '\0';
				handler_(context_, rx_, rxLength_, nowMs);
			}
			rxLength_ = 0;
			discarding_ = false;
			return;
		}
		if (!discarding_) {
			if (rxLength_ == sizeof(rx_) - 1) {
				discarding_ = true;
			} else {
				rx_[rxLength_++] = static_cast<char>(value);
			}
		}
	}
}