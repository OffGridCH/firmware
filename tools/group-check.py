#!/usr/bin/env python3
"""Three-board group acceptance for OffGrid firmware with the "groups" feature.

Board A owns the groups; B and C are members. A must be paired with B and C, and
B and C must not be paired with each other (existing pairs are reused, missing
ones are created and removed again). Boards are identified by device_id (the
12-digit MAC from sys.hello), never by port name. Uses the retained credentials
in ~/.config/offgrid/device-<device_id>.json like serial-check.py.

Steps (docs/offgrid-protocol.md section 8.5):
  1  A creates a group {A,B,C} and syncs B and C: both receipts, both join, same state everywhere
  2  A sends: B and C receive exactly once
  3  C sends: B shows from:null with C's node_id, A shows C's contact
  4  Keyless relay: group {A,C}, B only relays; A and C must be out of direct range
  5  B locked with relay_when_locked: B relays but does not deliver; group.* returns E_LOCKED
  6  A removes C (new epoch): B still receives, C no longer does
  7  B reboots: the group survives and new messages arrive
  8  C runs firmware without groups (run alone with --steps 8): sync to C fails with timeout
  9  A fills all group slots, survives a reboot, and paces back-to-back group sends
NVS entry counts (design step 9) are not exposed over serial and are not checked here.
"""

import argparse
import base64
import importlib.util
import json
from pathlib import Path
import time

LIMITS = {"max_groups": 8, "max_group_members": 16, "max_group_text_bytes": 83}


def load_serial_check():
    spec = importlib.util.spec_from_file_location("serial_check", Path(__file__).with_name("serial-check.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


check = load_serial_check()
require_equal = check.require_equal


def device_id(value):
    normalized = value.replace(":", "").replace("-", "").upper()
    if len(normalized) != 12 or any(character not in "0123456789ABCDEF" for character in normalized):
        raise argparse.ArgumentTypeError("device IDs are 12 hex digits (the board MAC)")
    return normalized


def parse_steps(value):
    steps = sorted({int(step) for step in value.split(",")})
    if not steps or steps[0] < 1 or steps[-1] > 9:
        raise argparse.ArgumentTypeError("steps are 1..9")
    if 8 in steps and steps != [8]:
        raise argparse.ArgumentTypeError("step 8 needs board C on firmware without groups; run it alone")
    return steps


class Board:
    def __init__(self, name, device, hello):
        self.name = name
        self.device = device
        self.hello = hello
        self.token = None
        self.identity = None
        self.original = None
        self.created_contacts = []

    def call(self, command, arguments=None, error=None):
        return self.device.call(command, arguments, error)


class Lab:
    def __init__(self, boards):
        self.boards = boards
        self.last_ping = time.monotonic()

    def keep_alive(self):
        # Heartbeats keep every unlocked board open while one board waits for RF events.
        if time.monotonic() - self.last_ping < 5:
            return
        for board in self.boards.values():
            if board.token is not None:
                board.call("sys.ping")
        self.last_ping = time.monotonic()

    def wait(self, board, name, predicate=lambda data: True, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.keep_alive()
            try:
                return board.device.event(name, predicate, timeout=min(3, max(0.1, deadline - time.monotonic())))["data"]
            except TimeoutError:
                continue
        raise TimeoutError(f"{board.name}: no {name} within {timeout} s")

    def absent(self, board, name, predicate=lambda data: True, seconds=20):
        try:
            data = self.wait(board, name, predicate, seconds)
        except TimeoutError:
            return
        raise RuntimeError(f"{board.name}: unexpected {name}: {data}")

    def status(self, board, reference, state, timeout=180):
        data = self.wait(board, "msg.status", lambda value: value["msg_ref"] == reference and
                         value["state"] in (state, "failed", "cancelled"), timeout)
        require_equal(data["state"], state, f"{board.name} message {reference}")
        return data


def unlock(board):
    path = Path.home() / ".config/offgrid" / f"device-{board.hello['device_id']}.json"
    credentials = check.read_credentials(path)
    require_equal(credentials["device_id"], board.hello["device_id"], f"{board.name} credential board ID")
    if "pending_token" in credentials:
        raise RuntimeError(f"{board.name}: resolve interrupted credential rotation first")
    board.token = base64.b64decode(credentials["token"], validate=True)
    check.secure_unlock(board.device, board.token)
    board.identity = board.call("id.get")


def configure(board):
    # Temporary settings only (persist false); a reboot restores the board's saved ones.
    board.call("sec.set_policy", {"lock_timeout_s": 300, "persist": False})
    board.call("radio.set", {"preset": "lab_default", "tx_power_dbm": 0, "persist": False})
    board.call("mesh.set", {"relay_enabled": False, "relay_when_locked": False, "cad_enabled": True,
                            "retry_base_ms": 3000, "persist": False})


def reboot(lab, board):
    token, board.token = board.token, None
    board.call("sys.reboot")
    lab.wait(board, "sys.boot", lambda data: data["state"] == "locked", timeout=20)
    board.token = token
    check.secure_unlock(board.device, board.token)
    configure(board)


def contacts(board):
    found, offset = [], 0
    while True:
        page = board.call("contact.list", {"offset": offset, "limit": 10})
        found += page["contacts"]
        offset += len(page["contacts"])
        if offset >= page["total"] or not page["contacts"]:
            return found


def contact_for(board, peer):
    return next((contact["contact_id"] for contact in contacts(board)
                 if contact["public_key"] == peer.identity["public_key"]), None)


def ensure_pair(first, second):
    slots = [contact_for(first, second), contact_for(second, first)]
    if all(slots):
        return slots
    if any(slots):
        raise RuntimeError(f"{first.name}/{second.name} are paired on one side only; re-pair them first")
    qrs = [board.call("pair.begin")["qr"] for board in (first, second)]
    for index, board in enumerate((first, second)):
        slots[index] = board.call("pair.complete", {"qr": qrs[1 - index]})["contact"]["contact_id"]
        board.created_contacts.append(slots[index])
    return slots


def same_group(left, right):
    keys = ("gid", "name", "epoch", "member_count")
    require_equal({key: left[key] for key in keys}, {key: right[key] for key in keys}, "group state")
    require_equal([member["node_id"] for member in left["members"]],
                  [member["node_id"] for member in right["members"]], "group member NodeIDs")


def member_group(board, gid):
    for group in board.call("group.list")["groups"]:
        if group["gid"] == gid:
            return board.call("group.get", {"group_id": group["group_id"]})["group"]
    raise RuntimeError(f"{board.name}: group {gid} missing")


def sync(lab, owner, group_id, contact_id, member, epoch):
    sent = owner.call("group.sync", {"group_id": group_id, "contact_id": contact_id})
    joined = lab.wait(member, "group.state", lambda data: data["epoch"] == epoch, timeout=180)
    lab.status(owner, sent["msg_ref"], "delivered")
    return joined


def send(lab, sender, group_id, text, hops=None):
    arguments = {"group_id": group_id, "type": "text", "text": text}
    if hops is not None:
        arguments["hops"] = hops
    sent = sender.call("group.send", arguments)
    require_equal(sent["ack_requested"], False, "group messages request no receipt")
    lab.status(sender, sent["msg_ref"], "sent")
    return sent


def receive(lab, board, gid_slot, text, node_id, timeout=120):
    data = lab.wait(board, "group.received", lambda value: value["group_id"] == gid_slot and
                    value.get("text") == text, timeout)
    require_equal(data["node_id"], node_id, f"{board.name} group sender NodeID")
    lab.absent(board, "group.received", lambda value: value.get("text") == text, seconds=10)
    return data


def run(boards, steps, prompt):
    a, b, c = boards["A"], boards["B"], boards["C"]
    lab = Lab(boards)
    owned = []
    joined = {"B": [], "C": []}
    try:
        for board in (a, b, c):
            unlock(board)
            board.original = check.read_configuration(board.device)
            configure(board)
        node = {name: board.identity["node_id"] for name, board in boards.items()}
        if contact_for(b, c) or contact_for(c, b):
            raise RuntimeError("B and C must not be paired with each other")
        a_b, b_a = ensure_pair(a, b)
        a_c, c_a = ensure_pair(a, c)

        if steps == [8]:
            if "groups" in c.hello.get("features", []):
                raise RuntimeError("step 8 needs board C on firmware without groups (main 47bb2d9)")
            c.call("group.list", error="E_UNKNOWN_CMD")
            group = a.call("group.create", {"name": "Legacy check", "members": [a_c]})["group"]
            owned.append(group["gid"])
            sent = a.call("group.sync", {"group_id": group["group_id"], "contact_id": a_c})
            failed = lab.wait(a, "msg.status", lambda data: data["msg_ref"] == sent["msg_ref"] and
                              data["state"] in ("delivered", "failed"), timeout=600)
            require_equal((failed["state"], failed.get("reason")), ("failed", "timeout"), "sync to old firmware")
            print("PASS 8: firmware without groups rejects group.*; the owner sees failed/timeout for its sync")
            return

        group = a.call("group.create", {"name": "Group check", "members": [a_b, a_c]})["group"]
        gid, slot = group["gid"], group["group_id"]
        owned.append(gid)
        if set(steps) & {1, 2, 3, 4, 5, 6, 7}:
            for contact_id, member in ((a_b, b), (a_c, c)):
                state = sync(lab, a, slot, contact_id, member, 1)
                require_equal(state["state"], "joined", f"{member.name} joins")
                joined[member.name].append(gid)
            for member in (b, c):
                same_group(member_group(member, gid), a.call("group.get", {"group_id": slot})["group"])
            print("PASS 1: create, two acknowledged syncs, identical group state on A, B and C")
        b_slot = member_group(b, gid)["group_id"] if joined["B"] else None
        c_slot = member_group(c, gid)["group_id"] if joined["C"] else None

        if 2 in steps:
            send(lab, a, slot, "Gruppe von A")
            for member, member_slot in ((b, b_slot), (c, c_slot)):
                data = receive(lab, member, member_slot, "Gruppe von A", node["A"])
                require_equal(data["member"], 0, "owner index")
            print("PASS 2: A's group text reached B and C exactly once")

        if 3 in steps:
            send(lab, c, c_slot, "Gruppe von C")
            require_equal(receive(lab, b, b_slot, "Gruppe von C", node["C"])["from"], None, "unpaired sender")
            require_equal(receive(lab, a, slot, "Gruppe von C", node["C"])["from"], a_c, "paired sender")
            print("PASS 3: C's text reached B (from null, NodeID shown) and A (contact matched)")

        if 4 in steps:
            pair = a.call("group.create", {"name": "Relay check", "members": [a_c]})["group"]
            owned.append(pair["gid"])
            b.call("mesh.set", {"relay_enabled": True, "persist": False})
            for board in (a, c):
                board.call("radio.set", {"tx_power_dbm": -9, "persist": False})
            if prompt:
                input("Step 4: place A and C out of each other's range, both within range of B, then press Enter ")
            sync(lab, a, pair["group_id"], a_c, c, 1)
            joined["C"].append(pair["gid"])
            pair_c = member_group(c, pair["gid"])["group_id"]
            before = b.call("radio.stats")["relayed"]
            send(lab, a, pair["group_id"], "Nur ueber B", hops=3)
            data = receive(lab, c, pair_c, "Nur ueber B", node["A"])
            if data["hops"] < 1:
                raise RuntimeError("C heard A directly; increase the distance and repeat step 4")
            if b.call("radio.stats")["relayed"] <= before:
                raise RuntimeError("B did not relay the group packet")
            lab.absent(b, "group.received", seconds=5)
            for board in (a, c):
                board.call("radio.set", {"tx_power_dbm": 0, "persist": False})
            if prompt:
                input("Step 4 done: put the boards back together, then press Enter ")
            print("PASS 4: B relayed a group packet without its key; C received it after one hop or more")

        if 5 in steps:
            b.call("mesh.set", {"relay_enabled": True, "relay_when_locked": True, "persist": False})
            before = b.call("radio.stats")["relayed"]
            b.call("sec.lock")
            lab.wait(b, "sec.state", lambda data: data["state"] == "locked", timeout=10)
            b.call("group.list", error="E_LOCKED")
            locked_token, b.token = b.token, None
            send(lab, a, slot, "B ist gesperrt")
            receive(lab, c, c_slot, "B ist gesperrt", node["A"])
            lab.absent(b, "group.received", seconds=5)
            b.token = locked_token
            check.secure_unlock(b.device, b.token)
            if b.call("radio.stats")["relayed"] <= before:
                raise RuntimeError("locked B did not relay the group packet")
            b.call("mesh.set", {"relay_enabled": False, "relay_when_locked": False, "persist": False})
            print("PASS 5: locked B relayed without delivering and refused group commands")

        if 6 in steps:
            index = next(member["member"] for member in a.call("group.get", {"group_id": slot})["group"]["members"]
                         if member["node_id"] == node["C"])
            changed = a.call("group.remove", {"group_id": slot, "members": [index]})["group"]
            require_equal((changed["epoch"], changed["member_count"]), (2, 2), "epoch and size after removal")
            require_equal(sync(lab, a, slot, a_b, b, 2)["state"], "updated", "B update")
            send(lab, a, slot, "Ohne C")
            receive(lab, b, b_slot, "Ohne C", node["A"])
            lab.absent(c, "group.received", lambda data: data.get("text") == "Ohne C", seconds=20)
            print("PASS 6: after removing C (epoch 2) B still receives and C does not")

        if 7 in steps:
            before = member_group(b, gid)
            reboot(lab, b)
            same_group(member_group(b, gid), before)
            send(lab, a, slot, "Nach Neustart")
            receive(lab, b, b_slot, "Nach Neustart", node["A"])
            print("PASS 7: B kept the group across a reboot and accepted new counters")

        if 9 in steps:
            while a.call("sys.status")["groups"]["used"] < LIMITS["max_groups"]:
                owned.append(a.call("group.create", {"name": "Fill", "members": [a_b]})["group"]["gid"])
            a.call("group.create", {"name": "Fill", "members": [a_b]}, error="E_STORAGE_FULL")
            names = sorted(group["gid"] for group in a.call("group.list")["groups"])
            reboot(lab, a)
            require_equal(sorted(group["gid"] for group in a.call("group.list")["groups"]), names, "groups after reboot")
            first = a.call("group.send", {"group_id": slot, "type": "text", "text": "Takt 1"})
            second = a.call("group.send", {"group_id": slot, "type": "text", "text": "Takt 2"})
            lab.status(a, first["msg_ref"], "sent")
            started = time.monotonic()
            lab.status(a, second["msg_ref"], "sent", timeout=240)
            gap = time.monotonic() - started
            if gap < 0.9 * 99 * first["est_airtime_ms"] / 1000:
                raise RuntimeError(f"second group send after {gap:.1f} s ignores the 1 % pacing")
            print(f"PASS 9: 8 groups persist across a reboot, a ninth is refused, sends paced by {gap:.1f} s")
    finally:
        for board in boards.values():
            if board.token is None:
                continue
            if board.call("sys.status")["state"] != "unlocked":
                check.secure_unlock(board.device, board.token)
            for group in board.call("group.list")["groups"]:
                if group["gid"] in owned:
                    board.call("group.delete", {"group_id": group["group_id"], "confirm": True})
            for contact_id in board.created_contacts:
                board.call("contact.delete", {"contact_id": contact_id, "confirm": True})
            board.call("pair.cancel")
            if board.original:
                for category in ("radio", "mesh"):
                    board.call(f"{category}.set", dict(board.original[category], persist=False))
                board.call("sec.set_policy", dict(board.original["policy"], persist=False))
            board.call("sec.lock")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", action="append", required=True, help="serial port; give it three times")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--board-a", type=device_id, required=True, help="device_id (MAC) of the owner")
    parser.add_argument("--board-b", type=device_id, required=True, help="device_id (MAC) of member B")
    parser.add_argument("--board-c", type=device_id, required=True, help="device_id (MAC) of member C")
    parser.add_argument("--steps", type=parse_steps, default=parse_steps("1,2,3,4,5,6,7,9"))
    parser.add_argument("--no-prompt", action="store_true", help="skip the placement prompts of step 4")
    parser.add_argument("--allow-secure-changes", action="store_true")
    parser.add_argument("--allow-radio-transmit", action="store_true")
    args = parser.parse_args()
    if len(args.port) != 3 or len(set(args.port)) != 3:
        parser.error("give three different --port values")
    if len({args.board_a, args.board_b, args.board_c}) != 3:
        parser.error("board IDs must differ")
    if not args.allow_secure_changes:
        parser.error("creates groups, contacts and settings; requires --allow-secure-changes")
    if not args.allow_radio_transmit:
        parser.error("RF tests require --allow-radio-transmit and lawful antenna/frequency/power settings")
    import serial

    roles = {args.board_a: "A", args.board_b: "B", args.board_c: "C"}
    links, boards = [], {}
    try:
        for port in args.port:
            link = serial.Serial(port=None, baudrate=args.baud, timeout=0.2, write_timeout=3)
            link.dtr = False
            link.rts = False
            link.port = port
            link.open()
            link.reset_input_buffer()
            links.append(link)
            device = check.Device(link)
            hello = device.connect()
            if hello.get("mock") is not False or hello["device_id"] not in roles:
                raise RuntimeError(f"{port}: unexpected board {hello.get('device_id')}")
            boards[roles[hello["device_id"]]] = Board(roles[hello["device_id"]], device, hello)
        for name in ("A", "B") + (() if args.steps == [8] else ("C",)):
            if "groups" not in boards[name].hello.get("features", []):
                raise RuntimeError(f"board {name} firmware has no groups feature")
            for key, value in LIMITS.items():
                require_equal(boards[name].hello["limits"].get(key), value, f"board {name} {key}")
        print("Boards:", json.dumps({name: board.hello["device_id"] for name, board in sorted(boards.items())}))
        run(boards, args.steps, not args.no_prompt)
    finally:
        for link in links:
            link.close()


if __name__ == "__main__":
    main()
