#!/usr/bin/env python3
"""Exercise a flashed Offgrid serial mock over a real serial port."""

import argparse
import base64
from collections import deque
import json
import time


class Device:
    def __init__(self, connection):
        self.connection = connection
        self.reference = 0
        self.events = deque(maxlen=128)
        self.buffer = bytearray()

    def read(self, deadline):
        while time.monotonic() < deadline:
            value = self.connection.read(1)
            if not value:
                continue
            self.buffer.extend(value)
            if len(self.buffer) > 2048:
                raise RuntimeError("device output exceeded 2048 bytes")
            if value != b"\n":
                continue
            line = bytes(self.buffer)
            self.buffer.clear()
            if not line.startswith(b"{"):
                continue
            message = json.loads(line)
            print("<", json.dumps(message, ensure_ascii=False))
            return message
        raise TimeoutError("device response timed out")

    def call(self, command, arguments=None, error=None):
        self.reference += 1
        request = {"id": self.reference, "cmd": command}
        if arguments is not None:
            request["args"] = arguments
        wire = json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
        if len(wire) > 2048:
            raise ValueError("request exceeds serial wire limit")
        print(">", wire.decode().strip())
        self.connection.write(wire)
        deadline = time.monotonic() + 6
        while True:
            response = self.read(deadline)
            if "evt" in response:
                self.events.append(response)
                continue
            if response.get("id") != self.reference:
                raise RuntimeError(f"unexpected response ID: {response}")
            if error is not None:
                assert not response["ok"] and response["error"]["code"] == error, response
            else:
                assert response["ok"], response
            return response.get("result", {})

    def event(self, name, predicate=lambda data: True, timeout=8):
        deadline = time.monotonic() + timeout
        while True:
            for event in list(self.events):
                if event["evt"] == name and predicate(event["data"]):
                    self.events.remove(event)
                    return event
            event = self.read(deadline)
            if "evt" not in event:
                raise RuntimeError(f"unsolicited response: {event}")
            self.events.append(event)


def exercise(device):
    hello = device.call("sys.hello", {"app": "serial-check", "app_proto": "1.0"})
    assert hello.get("mock") is True and hello.get("mock_stage") == 5, hello
    assert len(base64.b64decode(hello["session"]["device_eph_pub"], validate=True)) == 32
    assert len(base64.b64decode(hello["session"]["nonce"], validate=True)) == 16
    device.call("sys.hello", {"app_proto": "2.0"}, error="E_PROTO_MISMATCH")
    hello = device.call("sys.hello")
    wrapper = {
        "app_eph_pub": "wLEmtaoTS4TmRiUla43olzeHYSsfODnSxgUm0hFOiG0=",
        "wrapped_token": base64.b64encode(bytes(60)).decode(),
    }
    if hello["state"] == "unprovisioned":
        device.call("sec.provision", dict(wrapper, name="Serial mock"))
        device.event("sec.state", lambda data: data["reason"] == "provision")
    elif hello["state"] == "locked":
        device.call("sec.unlock", wrapper)
        device.event("sec.state", lambda data: data["reason"] == "unlock")
    device.call("sec.set_policy", {"lock_timeout_s": 15})
    device.call("sys.ping")
    device.call("id.get")
    device.call("pair.begin")
    contact = device.call("pair.complete", {
        "qr": "T0cxwLEmtaoTS4TmRiUla43olzeHYSsfODnSxgUm0hFOiG0gISIjJCUmJygpKissLS4vQm9i",
        "replace": True,
    })["contact"]["contact_id"]
    device.call("contact.list", {"offset": 0, "limit": 10})
    device.call("contact.rename", {"contact_id": contact, "name": "Mock Bob"})
    device.call("mock.set_scenario", {"outcome": "delivered"})
    device.call("dbg.rf_trace", {"enabled": True})
    sent = device.call("msg.send", {"to": contact, "type": "text", "text": "Hello!", "ack": True})
    device.event("msg.status", lambda data: data["msg_ref"] == sent["msg_ref"] and data["state"] == "delivered")
    device.event("rf.tx")
    device.call("mock.receive", {"from": contact, "type": "geo", "lat": 47.0517, "lon": 8.3075})
    device.event("msg.received", lambda data: data["from"] == contact and data["type"] == "geo")
    device.call("msg.send", {"to": contact, "type": "text", "text": "x" * 84}, error="E_TOO_LONG")
    device.call("mock.set_scenario", {"outcome": "radio_error"})
    failed = device.call("msg.send", {"to": contact, "type": "text", "text": "Failure"})
    device.event("msg.status", lambda data: data["msg_ref"] == failed["msg_ref"] and data.get("reason") == "radio_error")
    device.call("mock.set_scenario", {"outcome": "delivered"})
    device.call("radio.set", {"preset": "fast", "persist": True})
    original = device.call("radio.get")
    device.call("radio.set", {"sf": 10, "tx_power_dbm": "bad"}, error="E_INVALID_ARG")
    assert device.call("radio.get") == original
    device.call("radio.set", {"sf": 12, "persist": False})
    device.call("mesh.set", {"ack_default": {"geo": True}})
    device.call("led.set", {"brightness": 40, "state_patterns": {"locked": {"steps": [50, 1950]}}})
    device.call("led.test", {"pattern": "heartbeat", "repeat": 2})
    device.call("sys.set_log_level", {"level": "debug"})
    device.event("log")
    device.call("sys.set_log_level", {"level": "off"})
    device.call("radio.stats", {"reset": True})
    device.call("contact.delete", {"contact_id": contact, "confirm": True})
    device.call("sys.reboot")
    device.event("sys.boot", lambda data: data["state"] == "locked")
    assert device.call("radio.get")["sf"] == original["sf"]
    device.call("id.get", error="E_LOCKED")
    device.call("sys.hello")
    device.call("sec.unlock", wrapper)
    device.event("sec.state", lambda data: data["reason"] == "unlock")
    device.call("sec.lock")
    device.event("sec.state", lambda data: data["reason"] == "lock")
    print("PASS: real-port mock API checks completed; device remains locked")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--allow-mock-changes", action="store_true",
                        help="permit synthetic provisioning, settings changes, pairing and deletion")
    args = parser.parse_args()
    if not args.allow_mock_changes:
        parser.error("--allow-mock-changes is required; this modifies a flashed mock")
    import serial

    connection = serial.Serial(port=None, baudrate=args.baud, timeout=0.2, write_timeout=3)
    connection.dtr = False
    connection.rts = False
    connection.port = args.port
    connection.open()
    with connection:
        connection.reset_input_buffer()
        exercise(Device(connection))


if __name__ == "__main__":
    main()