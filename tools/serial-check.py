#!/usr/bin/env python3
"""Exercise real secure OffGrid firmware over physical serial and LoRa links."""

import argparse
import base64
from collections import deque
import json
import os
from pathlib import Path
import stat
import tempfile
import time


class Device:
    def __init__(self, connection):
        self.connection = connection
        self.reference = 0
        self.events = deque(maxlen=128)
        self.buffer = bytearray()
        self.startup_ids = set()

    def connect(self, timeout=10):
        deadline = time.monotonic() + timeout
        next_probe = time.monotonic() + 0.5
        self.connection.write(b"\n")
        while time.monotonic() < deadline:
            if time.monotonic() >= next_probe:
                self.reference += 1
                self.startup_ids.add(self.reference)
                request = {"id": self.reference, "cmd": "sys.hello",
                           "args": {"app": "serial-check", "app_proto": "1.0"}}
                wire = json.dumps(request, separators=(",", ":")).encode() + b"\n"
                print(">", wire.decode().strip())
                self.connection.write(wire)
                next_probe = time.monotonic() + 0.5
            try:
                response = self.read(min(deadline, next_probe))
            except TimeoutError:
                continue
            if "evt" in response:
                self.events.append(response)
                continue
            if response.get("error", {}).get("code") == "E_PARSE" and "id" not in response:
                continue
            if response.get("id") not in self.startup_ids or not response.get("ok"):
                raise RuntimeError(f"hello failed during startup: {response}")
            return response["result"]
        raise TimeoutError("no hello within startup timeout; check port, baud and flashed image")

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
        if command in ("sys.reboot", "sys.factory_reset"):
            self.events = deque((event for event in self.events if event["evt"] != "sys.boot"), maxlen=128)
        self.reference += 1
        request = {"id": self.reference, "cmd": command}
        if arguments is not None:
            request["args"] = arguments
        wire = json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
        if len(wire) > 2048:
            raise ValueError("request exceeds serial wire limit")
        if arguments and "wrapped_token" in arguments:
            print(">", json.dumps({"id": self.reference, "cmd": command, "args": "[wrapped credential redacted]"}))
        else:
            print(">", wire.decode().strip())
        self.connection.write(wire)
        deadline = time.monotonic() + 6
        while True:
            response = self.read(deadline)
            if "evt" in response:
                self.events.append(response)
                continue
            if response.get("id") != self.reference:
                if response.get("id") in self.startup_ids:
                    continue
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
                if name.startswith("diag.radio.") and event["evt"] == "diag.radio.error":
                    raise RuntimeError(f"radio diagnostic failed: {event['data']}")
                if event["evt"] == name and predicate(event["data"]):
                    self.events.remove(event)
                    return event
            event = self.read(deadline)
            if "evt" not in event:
                raise RuntimeError(f"unsolicited response: {event}")
            self.events.append(event)




def require_equal(actual, expected, label):
    if actual != expected:
        raise RuntimeError(f"{label}: expected {expected!r}, got {actual!r}")




def exercise_ui(device):
    device.connect()
    deadline = time.monotonic() + 8
    maximum = 0
    count = 0
    startup = False
    while time.monotonic() < deadline:
        started = time.monotonic()
        status = device.call("sys.status")
        ui = status["ui"]
        if ui["oled_scene"] == "boot":
            if ui["led_pattern"] not in ("startup", "glow"):
                raise RuntimeError("OLED/LED startup synchronization failed")
            if any(pixel >> 16 or (pixel >> 8 & 255) > (pixel & 255) for pixel in ui["strip_rgb"]):
                raise RuntimeError("startup must use OLED-blue RGB values")
            startup |= ui["led_pattern"] == "startup"
        maximum = max(maximum, time.monotonic() - started)
        count += 1
        time.sleep(0.05)
    status = device.call("sys.status")
    require_equal(status["ui"]["oled_ready"], True, "connected OLED initialized")
    require_equal(status["ui"]["page"], 0, "buttonless Home page")
    require_equal(startup, True, "visible synchronized OLED/LED startup")
    require_equal(status["ui"]["led_pattern"], "glow", "post-startup idle glow")
    if status["state"] == "locked":
        require_equal(status["ui"]["oled_scene"], "locked", "post-startup large lock")
    if maximum >= 0.25:
        raise RuntimeError(f"UI responsiveness exceeded 250 ms: {maximum * 1000:.1f} ms")
    print(f"PASS: synchronized blue startup and idle lock/glow, {count} status polls; maximum RTT {maximum * 1000:.1f} ms (<250 ms).")


def exercise_framing(device):
    hello = device.connect()
    if hello.get("mock") is not False:
        raise RuntimeError("framing acceptance requires real firmware")
    requests, expected = [], []
    for _ in range(32):
        device.reference += 1
        expected.append(device.reference)
        requests.append(json.dumps({"id": device.reference, "cmd": "sys.ping"}).encode() + b"\n")
    device.connection.write(b"".join(requests))
    time.sleep(0.5)
    for reference in expected:
        while True:
            response = device.read(time.monotonic() + 6)
            if "evt" in response:
                device.events.append(response)
                continue
            if response.get("id") in device.startup_ids:
                continue
            require_equal(response["id"], reference, "ordered burst response")
            require_equal(response["ok"], True, "burst response success")
            break
    device.connection.write(b'{"id":999,"cmd":"sys.ping"} trailing\n')
    while True:
        response = device.read(time.monotonic() + 6)
        if "evt" not in response:
            require_equal(response["error"]["code"], "E_PARSE", "trailing data rejected")
            break
        device.events.append(response)
    device.connection.write(b"x" * 2048 + b"\n")
    device.event("error", lambda data: data["code"] == "E_LINE_TOO_LONG")
    device.call("sys.ping")
    device.call("sys.set_log_level", {"level": "debug"})
    device.event("log", lambda data: data["level"] == "debug")
    device.call("sys.set_log_level", {"level": "invalid"}, error="E_INVALID_ARG")
    device.call("sys.set_log_level", {"level": "off"})
    device.call("sys.set_log_level", {"level": "warn"})
    device.event("log", lambda data: data["level"] == "warn")
    print("PASS: 32-request deferred-read burst, ordered responses, malformed/oversize recovery and strict log controls.")


def exercise_led(device, credential_path):
    hello = device.connect()
    if hello.get("mock") is not False:
        raise RuntimeError("LED acceptance requires real secure firmware")
    path = credential_path or Path.home() / ".config/offgrid" / f"device-{hello['device_id']}.json"
    credentials = read_credentials(path)
    require_equal(credentials["device_id"], hello["device_id"], "credential board ID")
    secure_unlock(device, base64.b64decode(credentials["token"], validate=True), hello)
    original = device.call("led.get")
    original.pop("builtin_patterns", None)

    def expect_duty(expected):
        time.sleep(0.05)
        ui = device.call("sys.status")["ui"]
        require_equal(ui["led_ready"], True, "LED initialization")
        require_equal(ui["strip_pixels"], 4, "confirmed strip count")
        require_equal(ui["led_duty"], expected, "PWM/pixel output level")
        require_equal(ui["strip_rgb"], [expected * 0x010101] * 4, "white legacy pixel output")

    try:
        device.call("led.set", {"enabled": True, "quiet": False, "brightness": 73,
                               "state_patterns": {"unlocked": "on"}, "persist": False})
        expect_duty(73)
        device.call("led.set", {"quiet": True, "persist": False})
        device.call("led.test", {"pattern": "on"})
        expect_duty(0)
        device.call("led.set", {"quiet": False, "enabled": False, "persist": False})
        device.call("led.test", {"pattern": "on"})
        expect_duty(0)
        device.call("led.set", {"enabled": True, "state_patterns": {"unlocked": "off"},
                               "persist": False})
        device.call("led.test", {"pattern": "on", "repeat": 1})
        expect_duty(73)
        time.sleep(1.05)
        expect_duty(0)
        for pattern in ("send", "receive", "glow", "alert"):
            device.call("led.test", {"pattern": pattern})
            started = time.monotonic()
            previous, spatial, lit = None, False, False
            while time.monotonic() - started < 1.75:
                before = time.monotonic()
                ui = device.call("sys.status")["ui"]
                if time.monotonic() - before >= 0.25:
                    raise RuntimeError("animation blocked serial for 250 ms")
                pixels = ui["strip_rgb"]
                lit |= any(pixels)
                spatial |= len(set(pixels)) > 1
                for pixel in pixels:
                    red, green, blue = pixel >> 16, pixel >> 8 & 255, pixel & 255
                    if max(red, green, blue) > 73:
                        raise RuntimeError("animation exceeded brightness cap")
                    if pattern in ("send", "receive") and not red == green == blue:
                        raise RuntimeError("message wave must be white")
                    if pattern == "glow" and (red or green > blue):
                        raise RuntimeError("locked glow must be blue")
                    if pattern == "alert" and (green or blue):
                        raise RuntimeError("alert must be red")
                if previous is not None:
                    elapsed = ui["led_frame_ms"] - previous["led_frame_ms"]
                    bound = 73 * elapsed * 8 / 1000 + 2
                    for pixel, old in zip(pixels, previous["strip_rgb"]):
                        if any(abs((pixel >> shift & 255) - (old >> shift & 255)) > bound
                               for shift in (0, 8, 16)):
                            raise RuntimeError("animation changed too abruptly")
                previous = ui
                time.sleep(0.025)
            if not lit or pattern != "alert" and not spatial:
                raise RuntimeError(f"{pattern} animation did not animate")
            if pattern in ("send", "receive"):
                require_equal(previous["strip_rgb"], [0] * 4, "finite wave returns to idle")
            if pattern == "glow":
                device.call("led.test", {"pattern": "off"})
                expect_duty(0)
            device.call("led.set", {"state_patterns": {"unlocked": "off"}, "persist": False})
            time.sleep(0.3)
        device.call("pair.begin")
        time.sleep(0.4)
        ui = device.call("sys.status")["ui"]
        require_equal(ui["led_pattern"], "pairing", "active pairing pattern")
        if not all(0 < (pixel >> 8 & 255) < (pixel >> 16) < (pixel & 255) for pixel in ui["strip_rgb"]):
            raise RuntimeError("pairing must be purple")
        device.call("pair.cancel")
    finally:
        device.call("pair.cancel")
        device.call("led.set", dict(original, persist=False))
        device.call("sec.lock")
    print("PASS: smooth RGB waves/glow, brightness, continuity, purple pairing, quiet/enabled and legacy previews; visual acceptance pending.")


def write_credentials(path, values, create=False):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".offgrid-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(values, output)
            output.flush()
            os.fsync(output.fileno())
        if create:
            os.link(temporary, path)
            os.unlink(temporary)
        else:
            os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def read_credentials(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor, encoding="utf-8") as source:
        if stat.S_IMODE(os.fstat(source.fileno()).st_mode) & 0o077:
            raise RuntimeError("credential file must be private (mode 0600)")
        return json.load(source)


def wrapped_arguments(hello, token, reset=False):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF

    private = X25519PrivateKey.generate()
    public = private.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    device_id = bytes.fromhex(hello["device_id"])
    shared = private.exchange(X25519PublicKey.from_public_bytes(base64.b64decode(hello["session"]["device_eph_pub"])))
    label = b"og-reset-wrap-v1" if reset else b"og-unlock-wrap-v1"
    key = HKDF(hashes.SHA256(), 32, base64.b64decode(hello["session"]["nonce"]), label + device_id).derive(shared)
    iv = os.urandom(12)
    wrapped = iv + AESGCM(key).encrypt(iv, token, device_id)
    return {"app_eph_pub": base64.b64encode(public).decode(),
            "wrapped_token": base64.b64encode(wrapped).decode()}


def secure_unlock(device, token, hello=None):
    hello = hello or device.call("sys.hello")
    if hello.get("mock") is not False:
        raise RuntimeError("refusing real credentials on mock/unknown firmware")
    if hello["state"] == "unlocked":
        return device.call("id.get")
    operation = "sec.provision" if hello["state"] == "unprovisioned" else "sec.unlock"
    result = device.call(operation, wrapped_arguments(hello, token))
    device.event("sec.state", lambda data: data["state"] == "unlocked")
    return result


def master_reset(device, hello, key_path, test_invalid=False):
    import hashlib
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    if hello.get("mock") is not False:
        raise RuntimeError("master reset requires explicit secure firmware")
    descriptor = os.open(key_path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as source:
        if stat.S_IMODE(os.fstat(source.fileno()).st_mode) & 0o077:
            raise RuntimeError("master private key must be private (mode 0600)")
        key = serialization.load_pem_private_key(source.read(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
        raise RuntimeError("master key must be P-256")
    challenge = device.call("sys.reset_challenge")
    message = base64.b64decode(challenge["message"], validate=True)
    if len(message) != 49 or message[:17] != b"og-reset-v1" + bytes.fromhex(hello["device_id"]):
        raise RuntimeError("reset challenge has wrong action/device binding")
    public = key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    require_equal(base64.b64decode(challenge["key_id"], validate=True), hashlib.sha256(public).digest(),
                  "firmware trusts this master key")
    signature = key.sign(message, ec.ECDSA(hashes.SHA256()))
    if test_invalid:
        changed = bytearray(signature)
        changed[-1] ^= 1
        device.call("sys.factory_reset", {"confirm": "ERASE", "method": "master",
                                        "signature": base64.b64encode(changed).decode()},
                    error="E_UNLOCK_FAILED")
        device.call("sys.factory_reset", {"confirm": "ERASE", "method": "master",
                                        "signature": base64.b64encode(signature).decode()},
                    error="E_UNLOCK_FAILED")
        challenge = device.call("sys.reset_challenge")
        message = base64.b64decode(challenge["message"], validate=True)
        if len(message) != 49 or message[:17] != b"og-reset-v1" + bytes.fromhex(hello["device_id"]):
            raise RuntimeError("replacement challenge has wrong action/device binding")
        signature = key.sign(message, ec.ECDSA(hashes.SHA256()))
    started = time.monotonic()
    device.call("sys.factory_reset", {"confirm": "ERASE", "method": "master",
                                    "signature": base64.b64encode(signature).decode()})
    device.event("sys.boot", lambda data: data["state"] == "unprovisioned", timeout=15)
    if time.monotonic() - started < 1.75:
        raise RuntimeError("factory reset skipped the red animation interval")
    print("PASS: owner-signed maintenance reset completed.")


def exercise_secure(device, credential_path, master_key, erase):
    hello = device.connect()
    if hello.get("mock") is not False:
        raise RuntimeError("flash secure_dev first; never test real authentication against a mock")
    credential_path = credential_path or Path.home() / ".config/offgrid" / f"device-{hello['device_id']}.json"
    if credential_path.exists():
        credentials = read_credentials(credential_path)
        require_equal(credentials["device_id"], hello["device_id"], "credential board ID")
    else:
        if hello["state"] != "unprovisioned":
            raise RuntimeError("existing vault requires its original credential; refusing overwrite")
        credentials = {"device_id": hello["device_id"], "token": base64.b64encode(os.urandom(32)).decode()}
        write_credentials(credential_path, credentials, create=True)
    token = base64.b64decode(credentials["token"], validate=True)
    if len(token) != 32:
        raise RuntimeError("credential token has invalid length")
    if "pending_token" in credentials:
        raise RuntimeError("interrupted rotation: preserve both credentials and explicitly recover before testing")
    identity = secure_unlock(device, token, hello)
    if "public_key" in credentials and not credentials.get("reset_in_progress"):
        require_equal(identity["public_key"], credentials["public_key"], "persisted identity")
    credentials["public_key"] = identity["public_key"]
    if not credentials.get("reset_in_progress"):
        credentials["original"] = read_configuration(device)
    write_credentials(credential_path, credentials)
    device.call("sec.set_policy", {"lock_timeout_s": 120, "persist": False})
    device.call("sys.hello", {"app_proto": "1.4"})
    device.call("sys.hello", {"app_proto": "2.0"}, error="E_PROTO_MISMATCH")
    device.call("sys.factory_reset", error="E_CONFIRM_REQUIRED")
    device.call("sec.lock")
    device.event("sec.state", lambda data: data["state"] == "locked")
    for _ in range(3):
        invalid = wrapped_arguments(device.call("sys.hello"), os.urandom(32))
        device.call("sec.unlock", invalid, error="E_UNLOCK_FAILED")
        device.call("sec.unlock", invalid, error="E_NO_SESSION")
    limited = wrapped_arguments(device.call("sys.hello"), token)
    device.call("sec.unlock", limited, error="E_RATE_LIMITED")
    time.sleep(2.1)
    secure_unlock(device, token)
    device.call("sec.set_policy", {"lock_timeout_s": 120, "persist": False})
    wrong_action = wrapped_arguments(device.call("sys.hello"), token)
    device.call("sys.factory_reset", dict(wrong_action, confirm="ERASE"), error="E_UNLOCK_FAILED")
    device.call("sys.reboot")
    device.event("sys.boot", lambda data: data["state"] == "locked", timeout=15)
    require_equal(secure_unlock(device, token)["public_key"], credentials["public_key"], "identity after MCU reboot")
    replacement = os.urandom(32)
    credentials["pending_token"] = base64.b64encode(replacement).decode()
    write_credentials(credential_path, credentials)
    device.call("sec.rotate_token", wrapped_arguments(device.call("sys.hello"), replacement))
    credentials["token"] = credentials.pop("pending_token")
    write_credentials(credential_path, credentials)
    old = token
    token = replacement
    device.call("sec.lock")
    device.event("sec.state", lambda data: data["state"] == "locked")
    device.call("sec.unlock", wrapped_arguments(device.call("sys.hello"), old), error="E_UNLOCK_FAILED")
    secure_unlock(device, token)
    device.call("sec.set_policy", {"lock_timeout_s": 1, "persist": False})
    device.event("sec.state", lambda data: data["reason"] == "heartbeat_timeout", timeout=4)
    secure_unlock(device, token)
    device.call("sec.set_policy", {"lock_timeout_s": 120, "persist": False})
    if erase:
        credentials["reset_in_progress"] = True
        write_credentials(credential_path, credentials)
        started = time.monotonic()
        device.call("sys.factory_reset",
                    dict(wrapped_arguments(device.call("sys.hello"), token, reset=True), confirm="ERASE"))
        device.event("sys.boot", lambda data: data["state"] == "unprovisioned", timeout=15)
        if time.monotonic() - started < 1.75:
            raise RuntimeError("app reset skipped the red animation interval")
        secure_unlock(device, token)
        device.call("sec.lock")
        device.event("sec.state", lambda data: data["state"] == "locked")
        master_reset(device, device.call("sys.hello"), master_key, test_invalid=True)
        credentials.pop("public_key", None)
        write_credentials(credential_path, credentials)
        credentials["public_key"] = secure_unlock(device, token)["public_key"]
        write_credentials(credential_path, credentials)
    save_configuration(device, credentials["original"])
    device.call("sys.reboot")
    device.event("sys.boot", lambda data: data["state"] == "locked", timeout=15)
    require_equal(secure_unlock(device, token)["public_key"], credentials["public_key"], "final persistent identity")
    require_equal(read_configuration(device), credentials["original"], "original settings restored")
    credentials.pop("reset_in_progress", None)
    write_credentials(credential_path, credentials)
    device.call("sec.lock")
    device.event("sec.state", lambda data: data["state"] == "locked")
    print(f"PASS: real provisioning, reboot/unlock, wrong token, sessions/backoff, rotation and timeout. Credential retained privately: {credential_path}")


def exercise_contacts(device, credential_path):
    import hashlib
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey

    hello = device.connect()
    if hello.get("mock") is not False:
        raise RuntimeError("contact acceptance requires secure firmware")
    path = credential_path or Path.home() / ".config/offgrid" / f"device-{hello['device_id']}.json"
    credentials = read_credentials(path)
    require_equal(credentials["device_id"], hello["device_id"], "credential board ID")
    token = base64.b64decode(credentials["token"], validate=True)
    own = secure_unlock(device, token, hello)
    original_timeout = device.call("sec.get_policy")["lock_timeout_s"]
    device.call("sec.set_policy", {"lock_timeout_s": 120, "persist": False})
    before = device.call("contact.list")["total"]
    if before >= 32:
        raise RuntimeError("no free contact slot; no existing contacts will be overwritten")
    qr = device.call("pair.begin")["qr"]
    device.call("pair.complete", {"qr": qr}, error="E_SELF_PAIR")
    peer = X25519PrivateKey.generate().public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    peer_qr = base64.b64encode(b"OG1" + peer + os.urandom(16) + b"Check Peer").decode()
    slot = None
    try:
        contact = device.call("pair.complete", {"qr": peer_qr})["contact"]
        slot = contact["contact_id"]
        first, second = sorted((base64.b64decode(own["public_key"]), peer))
        value = int.from_bytes(hashlib.sha256(b"og1-fp" + first + second).digest()[:8], "big") % 10**12
        fingerprint = f"{value:012d}"
        expected = " ".join(fingerprint[index:index + 4] for index in (0, 4, 8))
        require_equal(contact["fingerprint"], expected, "role-independent public fingerprint")
        require_equal(contact["state"], "pending", "unconfirmed contact")
        device.call("pair.complete", {"qr": peer_qr}, error="E_NO_PAIRING")
        device.call("pair.begin")
        device.call("pair.complete", {"qr": peer_qr}, error="E_CONTACT_EXISTS")
        replacement = device.call("pair.complete", {"qr": peer_qr, "replace": True})["contact"]
        require_equal(replacement["contact_id"], slot, "replacement preserves slot")
        require_equal(replacement["tx_ctr"], 0, "new relationship counters")
        device.call("contact.rename", {"contact_id": slot, "name": "Renamed Peer"})
        device.call("sys.reboot")
        device.event("sys.boot", lambda data: data["state"] == "locked", timeout=15)
        secure_unlock(device, token)
        persisted = device.call("contact.get", {"contact_id": slot})["contact"]
        require_equal(persisted["name"], "Renamed Peer", "encrypted contact name survives reboot")
        require_equal(persisted["fingerprint"], expected, "persisted fingerprint")
        require_equal(base64.b64decode(persisted["public_key"]), peer, "persisted contact public key")
        require_equal(device.call("msg.pending")["pending"], [], "real queue starts empty after reboot")
        device.call("msg.send", {"to": slot, "type": "text", "text": "x" * 84}, error="E_TOO_LONG")
        device.call("msg.send", {"to": slot, "type": "geo", "lat": 91, "lon": 0}, error="E_INVALID_ARG")
        device.call("msg.send", {"to": slot, "type": "reaction",
                                "target": {"author": "peer", "ref": 1}, "emoji": "invalid"}, error="E_INVALID_ARG")
        device.call("msg.cancel", {"msg_ref": 4294967295}, error="E_INVALID_ARG")
        require_equal(device.call("msg.pending")["pending"], [], "rejected messages never queue or transmit")
        device.call("contact.delete", {"contact_id": slot, "confirm": False}, error="E_CONFIRM_REQUIRED")
        device.call("contact.delete", {"contact_id": slot, "confirm": True})
        device.call("contact.get", {"contact_id": slot}, error="E_NO_SUCH_CONTACT")
        slot = None
        device.call("sys.reboot")
        device.event("sys.boot", lambda data: data["state"] == "locked", timeout=15)
        secure_unlock(device, token)
        require_equal(device.call("contact.list")["total"], before, "fixture removed without touching other contacts")
    finally:
        if device.call("sys.status")["state"] != "unlocked":
            secure_unlock(device, token)
        if slot is not None:
            device.call("contact.delete", {"contact_id": slot, "confirm": True})
        device.call("sec.set_policy", {"lock_timeout_s": original_timeout, "persist": False})
        device.call("sec.lock")
    device.call("msg.pending", error="E_LOCKED")
    print("PASS: real pairing/contact persistence and no-RF message validation; two-board key agreement and delivery pending.")


def exercise_mesh(first, second):
    devices = (first, second)
    tokens, identities, originals, slots, lab = [], [], [], [None, None], []
    deleted_slot = None
    try:
        for device in devices:
            hello = device.connect()
            lab.append(hello.get("radio_diagnostics") is True)
            if hello.get("mock") is not False or hello["state"] != "locked":
                raise RuntimeError("two-board acceptance requires locked secure firmware and retained credentials")
            path = Path.home() / ".config/offgrid" / f"device-{hello['device_id']}.json"
            credentials = read_credentials(path)
            require_equal(credentials["device_id"], hello["device_id"], "credential board ID")
            if "pending_token" in credentials:
                raise RuntimeError("resolve interrupted credential rotation first")
            token = base64.b64decode(credentials["token"], validate=True)
            if len(token) != 32:
                raise RuntimeError("invalid retained token length")
            deadline = time.monotonic() + 8
            while device.call("sys.status")["ui"]["oled_scene"] == "boot":
                if time.monotonic() >= deadline:
                    raise RuntimeError("startup did not finish")
                time.sleep(0.05)
            require_equal(device.call("sys.status")["ui"]["oled_scene"], "locked", "large locked scene")
            identities.append(secure_unlock(device, token))
            tokens.append(token)
            originals.append(read_configuration(device))
            started = time.monotonic()
            scenes, maximum = set(), 0
            while time.monotonic() - started < 1.7:
                before = time.monotonic()
                scenes.add(device.call("sys.status")["ui"]["oled_scene"])
                maximum = max(maximum, time.monotonic() - before)
                time.sleep(0.04)
            if not {"unlocking", "home"}.issubset(scenes) or maximum >= 0.250:
                raise RuntimeError(f"unlock scene/latency failure: {scenes}, {maximum:.3f}s")
            device.call("sec.set_policy", {"lock_timeout_s": 120, "persist": False})
            device.call("radio.set", {"preset": "lab_default", "tx_power_dbm": 0, "persist": False})
            device.call("mesh.set", {"relay_enabled": False, "relay_when_locked": False,
                                    "cad_enabled": True, "retry_base_ms": 3000, "persist": False})
            if lab[-1]:
                device.call("dbg.rf_trace", {"enabled": True})
            else:
                device.call("diag.codec.selftest", error="E_UNKNOWN_CMD")
                device.call("diag.led.selftest", error="E_UNKNOWN_CMD")
                device.call("dbg.rf_trace", {"enabled": True}, error="E_UNKNOWN_CMD")
        if identities[0]["public_key"] == identities[1]["public_key"]:
            raise RuntimeError("boards must have distinct identities")
        qrs = [device.call("pair.begin")["qr"] for device in devices]
        for index, device in enumerate(devices):
            paired = device.call("pair.complete", {"qr": qrs[1 - index]})["contact"]
            slots[index] = paired["contact_id"]
            require_equal(paired["public_key"], identities[1 - index]["public_key"], "physical peer identity")
        require_equal(first.call("contact.get", {"contact_id": slots[0]})["contact"]["fingerprint"],
                      second.call("contact.get", {"contact_id": slots[1]})["contact"]["fingerprint"],
                      "two-board fingerprint agreement")
        payloads = [
            {"type": "text", "text": "OffGrid physical acceptance"},
            {"type": "reaction", "target": {"author": "peer", "ref": 1}, "emoji": "\U0001f44d"},
            {"type": "geo", "lat": 47.0517, "lon": 8.3075, "alt_m": -15, "age_s": 17},
        ]
        for index, sender in enumerate(devices):
            recipient = devices[1 - index]
            for payload in payloads:
                sent = sender.call("msg.send", dict(payload, to=slots[index], ack=True, hops=0))
                received = recipient.event("msg.received", lambda data: data["from"] == slots[1 - index]
                                           and data["ctr"] == sent["ctr"], timeout=90)["data"]
                time.sleep(0.35)
                for endpoint, pattern in ((sender, "send"), (recipient, "receive")):
                    ui = endpoint.call("sys.status")["ui"]
                    require_equal(ui["led_pattern"], pattern, "real message LED event")
                    if not any(ui["strip_rgb"]) or any(
                            not (pixel >> 16) == (pixel >> 8 & 255) == (pixel & 255)
                            for pixel in ui["strip_rgb"]):
                        raise RuntimeError("real message event did not produce a white wave")
                require_equal(received["type"], payload["type"], "authenticated payload type")
                if payload["type"] == "text":
                    require_equal(received["text"], payload["text"], "physical text")
                elif payload["type"] == "reaction":
                    require_equal(received["target"], {"author": "me", "ref": 1}, "reaction direction")
                    require_equal(received["emoji"], payload["emoji"], "physical emoji")
                else:
                    if abs(received["lat"] - payload["lat"]) > 0.000011 or abs(received["lon"] - payload["lon"]) > 0.000022:
                        raise RuntimeError("physical GEO quantization exceeded bounds")
                    require_equal(received["alt_m"], -15, "signed altitude")
                    require_equal(received["age_s"], 20, "quantized age")
                sender.event("msg.status", lambda data: data["msg_ref"] == sent["msg_ref"]
                             and data["state"] == "delivered", timeout=90)
                require_equal(sender.call("msg.pending")["pending"], [], "ACK removes pending work")
                time.sleep(3)
        if all(lab):
            before = second.call("radio.stats")
            second.call("diag.mesh.drop_next_ack")
            sent = first.call("msg.send", {"to": slots[0], "type": "text",
                                          "text": "Lost receipt fixture", "ack": True, "hops": 0})
            second.event("msg.received", lambda data: data["ctr"] == sent["ctr"], timeout=90)
            delivered = first.event("msg.status", lambda data: data["msg_ref"] == sent["msg_ref"]
                                    and data["state"] == "delivered", timeout=90)["data"]
            if delivered["retry"] < 1:
                raise RuntimeError("lost receipt did not cause a physical retry")
            after = second.call("radio.stats")
            require_equal(after["injected_ack_drops"], before["injected_ack_drops"] + 1, "one lost receipt")
            if after["rx_dedupe_drops"] <= before["rx_dedupe_drops"]:
                raise RuntimeError("physical duplicate did not reach replay handling")
            if any(event["evt"] == "msg.received" and event["data"]["ctr"] == sent["ctr"]
                   for event in second.events):
                raise RuntimeError("authenticated retry was delivered twice")
            first.event("rf.tx")
            second.event("rf.rx")
            baseline = first.call("radio.stats")
            queued = [first.call("msg.send", {"to": slots[0], "type": "text",
                                            "text": "Queue-bound fixture", "ack": True})
                      for _ in range(8)]
            first.call("msg.send", {"to": slots[0], "type": "text", "text": "Ninth"}, error="E_QUEUE_FULL")
            require_equal(len(first.call("msg.pending")["pending"]), 8, "exact host queue capacity")
            for message in queued:
                first.call("msg.cancel", {"msg_ref": message["msg_ref"]})
                first.event("msg.status", lambda data: data["msg_ref"] == message["msg_ref"]
                            and data["state"] == "cancelled")
            locked = [first.call("msg.send", {"to": slots[0], "type": "text", "text": "Lock fixture"})
                      for _ in range(2)]
            first.call("sec.lock")
            first.event("sec.state", lambda data: data["state"] == "locked")
            for message in locked:
                first.event("msg.status", lambda data: data["msg_ref"] == message["msg_ref"]
                            and data["state"] == "failed" and data["reason"] == "locked")
            first.call("msg.pending", error="E_LOCKED")
            secure_unlock(first, tokens[0])
            removed = [first.call("msg.send", {"to": slots[0], "type": "text", "text": "Delete fixture"})
                       for _ in range(2)]
            deleted_slot = slots[0]
            first.call("contact.delete", {"contact_id": slots[0], "confirm": True})
            slots[0] = None
            for message in removed:
                first.event("msg.status", lambda data: data["msg_ref"] == message["msg_ref"]
                            and data["state"] == "failed" and data["reason"] == "contact_deleted")
            final = first.call("radio.stats")
            require_equal(final["tx_packets"], baseline["tx_packets"], "queued fixture never reached RF")
            require_equal(final["tx_airtime_ms_total"], baseline["tx_airtime_ms_total"], "no partial fixture TX")
            print("PASS: deliberately lost physical receipt, real retry, fresh re-ACK, no duplicate delivery and RF trace.")
            print("PASS: host queue capacity exactly eight, cancel, lock and contact deletion cleanup without RF launch.")
        for index, device in enumerate(devices):
            if slots[index] is not None:
                require_equal(device.call("contact.get", {"contact_id": slots[index]})["contact"]["state"],
                              "active", "authenticated contact activation")
            device.call("sys.reboot")
            device.event("sys.boot", lambda data: data["state"] == "locked", timeout=15)
            secure_unlock(device, tokens[index])
            if slots[index] is not None:
                require_equal(device.call("contact.get", {"contact_id": slots[index]})["contact"]["state"],
                              "active", "active contact survives MCU reboot")
            elif deleted_slot is not None:
                device.call("contact.get", {"contact_id": deleted_slot}, error="E_NO_SUCH_CONTACT")
        print("PASS: two physical boards exchanged authenticated TEXT/REACTION/GEO and bound ACKs both ways; contacts persist; OLED lock/open/Home scenes and RTT verified.")
    finally:
        for index, device in enumerate(devices[:len(tokens)]):
            if device.call("sys.status")["state"] != "unlocked":
                secure_unlock(device, tokens[index])
            if slots[index] is not None:
                device.call("contact.delete", {"contact_id": slots[index], "confirm": True})
            device.call("pair.cancel")
            if index < len(originals):
                for category in ("radio", "mesh"):
                    device.call(f"{category}.set", dict(originals[index][category], persist=False))
                device.call("sec.set_policy", dict(originals[index]["policy"], persist=False))
            device.call("sec.lock")




def read_configuration(device):
    configuration = {}
    for category in ("radio", "mesh", "led"):
        values = device.call(f"{category}.get")
        for field in ("est_airtime_ms_23b", "max_text_bytes", "warnings", "builtin_patterns"):
            values.pop(field, None)
        configuration[category] = values
    configuration["policy"] = device.call("sec.get_policy")
    return configuration


def save_configuration(device, configuration):
    for category in ("radio", "mesh", "led"):
        device.call(f"{category}.set", dict(configuration[category], persist=True))
    device.call("sec.set_policy", dict(configuration["policy"], persist=True))










def send_command(device, command, arguments, credentials, allow_changes):
    hello = device.connect()
    if hello.get("mock") is not False:
        raise RuntimeError("refusing commands on mock/unknown firmware")
    if allow_changes and command not in ("sec.provision", "sec.unlock", "sys.factory_reset"):
        path = credentials or Path.home() / ".config/offgrid" / f"device-{hello['device_id']}.json"
        retained = read_credentials(path)
        require_equal(retained["device_id"], hello["device_id"], "credential board ID")
        secure_unlock(device, base64.b64decode(retained["token"], validate=True), hello)
    result = device.call(command, arguments)
    if command == "msg.send":
        terminal = device.event("msg.status", lambda data: data["msg_ref"] == result["msg_ref"] and
                                (data["state"] in ("delivered", "failed") or
                                 not result["ack_requested"] and data["state"] == "sent"), timeout=90)
        if terminal["data"]["state"] == "failed":
            device.call("sec.lock")
            raise RuntimeError(f"real RF send failed: {terminal['data']['reason']}")
    if command in ("diag.codec.selftest", "diag.vault.selftest", "diag.led.selftest"):
        require_equal(result["passed"], True, "isolated fixture acceptance")
    if allow_changes and command not in ("sec.lock", "sys.reboot", "sys.factory_reset", "sec.provision", "sec.unlock"):
        device.call("sec.lock")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--command")
    mode.add_argument("--ui-check", action="store_true")
    mode.add_argument("--framing-check", action="store_true")
    mode.add_argument("--led-check", action="store_true")
    mode.add_argument("--secure-check", action="store_true")
    mode.add_argument("--contact-check", action="store_true")
    mode.add_argument("--mesh-peer", help="second real secure board port")
    mode.add_argument("--master-reset", action="store_true")
    parser.add_argument("--args", dest="command_args", default="{}")
    parser.add_argument("--credentials", type=Path)
    parser.add_argument("--master-key", type=Path, default=Path.home() / ".config/offgrid/master-reset.pem")
    parser.add_argument("--allow-secure-changes", action="store_true")
    parser.add_argument("--allow-radio-transmit", action="store_true")
    parser.add_argument("--confirm-erase", action="store_true")
    args = parser.parse_args()
    try:
        command_args = json.loads(args.command_args)
    except json.JSONDecodeError as error:
        parser.error(f"invalid --args JSON: {error}")
    if not isinstance(command_args, dict):
        parser.error("--args must be a JSON object")
    read_only = {"sys.hello", "sys.status", "sys.ping", "radio.get", "mesh.get", "led.get"}
    if (args.command and args.command not in read_only or args.led_check or args.secure_check or
            args.contact_check or args.mesh_peer) and not args.allow_secure_changes:
        parser.error("state-changing commands/checks require --allow-secure-changes")
    if (args.mesh_peer or args.command == "msg.send") and not args.allow_radio_transmit:
        parser.error("RF tests/sends require --allow-radio-transmit and lawful antenna/frequency/power settings")
    if args.mesh_peer == args.port:
        parser.error("mesh ports must differ")
    if args.master_reset and not args.confirm_erase:
        parser.error("--master-reset requires --confirm-erase")
    if args.confirm_erase and not (args.master_reset or args.secure_check):
        parser.error("--confirm-erase applies only to maintenance reset or secure acceptance")
    import serial

    def connection(port):
        link = serial.Serial(port=None, baudrate=args.baud, timeout=0.2, write_timeout=3)
        link.dtr = False
        link.rts = False
        link.port = port
        link.open()
        link.reset_input_buffer()
        return link

    with connection(args.port) as link:
        device = Device(link)
        if args.command:
            send_command(device, args.command, command_args, args.credentials, args.allow_secure_changes)
        elif args.ui_check:
            exercise_ui(device)
        elif args.framing_check:
            exercise_framing(device)
        elif args.led_check:
            exercise_led(device, args.credentials)
        elif args.secure_check:
            exercise_secure(device, args.credentials, args.master_key, args.confirm_erase)
        elif args.contact_check:
            exercise_contacts(device, args.credentials)
        elif args.master_reset:
            master_reset(device, device.connect(), args.master_key)
        else:
            with connection(args.mesh_peer) as peer:
                exercise_mesh(device, Device(peer))


if __name__ == "__main__":
    main()