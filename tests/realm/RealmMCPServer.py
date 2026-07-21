#!/usr/bin/env python3

import array
import base64
import json
import os
import pathlib
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest


class FakeRealmControlServer:
    def __init__(self, root: pathlib.Path):
        self.path = root / ".realm.sock"
        self.requests = []
        self.capture_counter = 0
        self.realm_states = {"codex": "running"}
        self.error = None
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.bind(str(self.path))
        os.chmod(self.path, 0o600)
        self.socket.listen(1)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    @staticmethod
    def _read_exact(connection, size):
        chunks = []
        while size:
            chunk = connection.recv(size)
            if not chunk:
                return None
            chunks.append(chunk)
            size -= len(chunk)
        return b"".join(chunks)

    @staticmethod
    def _frame(payload):
        encoded = json.dumps(payload, separators=(",", ":")).encode()
        return struct.pack(">I", len(encoded)) + encoded

    def _run(self):
        try:
            connection, _ = self.socket.accept()
            with connection:
                while True:
                    header = self._read_exact(connection, 4)
                    if header is None:
                        return
                    payload = self._read_exact(connection, struct.unpack(">I", header)[0])
                    if payload is None:
                        return
                    request = json.loads(payload)
                    self.requests.append(request)
                    request_id = request["request_id"]
                    method = request["method"]
                    realm_name = request.get("params", {}).get("realm", "codex")

                    if method == "realm.list":
                        connection.sendall(
                            self._frame(
                                {
                                    "request_id": request_id,
                                    "ok": True,
                                    "result": {
                                        "realms": [
                                            {"id": index + 1, "name": name, "state": state}
                                            for index, (name, state) in enumerate(self.realm_states.items())
                                        ]
                                    },
                                }
                            )
                        )
                        continue

                    if method == "realm.create":
                        self.realm_states[realm_name] = "stopped"
                    elif method == "realm.start":
                        self.realm_states[realm_name] = "running"
                    elif method == "realm.stop":
                        self.realm_states[realm_name] = "stopped"
                    elif method == "realm.destroy":
                        self.realm_states.pop(realm_name, None)

                    if method == "pointer.scroll" and request["params"]["steps"] == 13:
                        connection.sendall(
                            self._frame(
                                {
                                    "request_id": request_id,
                                    "ok": False,
                                    "error": {
                                        "code": "capability_denied",
                                        "message": "pointer capability is not granted",
                                    },
                                }
                            )
                        )
                        continue

                    if method in ("realm.capture", "realm.capture_region"):
                        self.capture_counter += 1
                        connection.sendall(
                            self._frame(
                                {
                                    "request_id": request_id,
                                    "ok": True,
                                    "result": {
                                        "action": "queued",
                                        "capture_id": 41,
                                        "realm": {"name": realm_name},
                                    },
                                }
                            )
                        )
                        descriptor = os.memfd_create("realm-mcp-test", os.MFD_CLOEXEC)
                        try:
                            os.write(
                                descriptor,
                                bytes(
                                    [
                                        self.capture_counter & 0xFF,
                                        0x00,
                                        0xFF,
                                        0x00,
                                        0x00,
                                        0xFF,
                                        0x00,
                                        0x00,
                                        0xFF,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0xFF,
                                        0xFF,
                                        0xFF,
                                        0x00,
                                    ]
                                ),
                            )
                            event = self._frame(
                                {
                                    "event": "realm.capture.ready",
                                    "capture_id": 41,
                                    "realm_id": 7,
                                    "frame": {
                                        "transport": "scm_rights",
                                        "fd_count": 1,
                                        "format": 1,
                                        "format_name": "xrgb8888",
                                        "width": 2,
                                        "height": 2,
                                        "stride": 8,
                                        "byte_size": 16,
                                        "y_inverted": False,
                                    },
                                }
                            )
                            sent = connection.sendmsg(
                                [event],
                                [
                                    (
                                        socket.SOL_SOCKET,
                                        socket.SCM_RIGHTS,
                                        array.array("i", [descriptor]),
                                    )
                                ],
                            )
                            if sent < len(event):
                                connection.sendall(event[sent:])
                        finally:
                            os.close(descriptor)
                        continue

                    if method in (
                        "pointer.move",
                        "pointer.click",
                        "pointer.point_and_click",
                        "pointer.scroll",
                        "keyboard.press",
                        "keyboard.shortcut",
                        "keyboard.type",
                    ):
                        connection.sendall(
                            self._frame(
                                {
                                    "request_id": request_id,
                                    "ok": True,
                                    "result": {
                                        "action": "queued",
                                        "sequence": len(self.requests),
                                        "realm": {"name": realm_name},
                                    },
                                }
                            )
                        )
                        connection.sendall(
                            self._frame(
                                {
                                    "event": "realm.input.applied",
                                    "sequence": len(self.requests),
                                    "realm_id": 7,
                                }
                            )
                        )
                        continue

                    state = self.realm_states.get(realm_name, "stopped")
                    result = {
                        "action": method,
                        "realm": {
                            "id": 7,
                            "name": realm_name,
                            "state": state,
                        },
                    }
                    if method == "realm.open":
                        result.update(
                            {
                                "action": "opened",
                                "application": request["params"]["application"],
                                "pid": 1234,
                            }
                        )
                    elif method == "realm.place":
                        result.update(
                            {
                                "action": "placed",
                                "workspace": request["params"]["workspace"],
                            }
                        )
                    connection.sendall(
                        self._frame(
                            {
                                "request_id": request_id,
                                "ok": True,
                                "result": result,
                            }
                        )
                    )
        except Exception as error:  # pragma: no cover - surfaced by the test
            self.error = error

    def close(self):
        self.socket.close()
        self.thread.join(timeout=2)
        if self.error:
            raise self.error


class RealmMCPServerTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory(prefix="realm-mcp.")
        root = pathlib.Path(self.temporary_directory.name)
        os.chmod(root, 0o700)
        self.control = FakeRealmControlServer(root)
        self.process = subprocess.Popen(
            [sys.argv[1], "--realm", "codex", "--socket", str(self.control.path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def tearDown(self):
        if self.process.stdin:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)
        stderr = self.process.stderr.read()
        self.process.stdout.close()
        self.process.stderr.close()
        self.control.close()
        self.temporary_directory.cleanup()
        self.assertEqual(self.process.returncode, 0, stderr)

    def call(self, method, params=None, request_id=1):
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        self.process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        readable, _, _ = select.select([self.process.stdout], [], [], 3)
        self.assertTrue(readable, "MCP server did not respond")
        response = json.loads(self.process.stdout.readline())
        self.assertEqual(response["id"], request_id)
        return response

    def tool(self, name, arguments=None, request_id=2):
        params = {"name": name}
        if arguments is not None:
            params["arguments"] = arguments
        return self.call("tools/call", params, request_id)["result"]

    def initialize(self):
        response = self.call(
            "initialize",
            {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {"name": "isolated-test", "version": "1"},
            },
        )
        self.assertEqual(response["result"]["protocolVersion"], "2025-11-25")
        self.assertEqual(response["result"]["serverInfo"]["name"], "hyprland-realm")
        self.assertIn("capture_realm", response["result"]["instructions"])
        self.assertIn("point_and_click", response["result"]["instructions"])

    def test_realm_bound_tools_and_capture(self):
        self.initialize()

        listed = self.call("tools/list", {}, 2)["result"]["tools"]
        names = {tool["name"] for tool in listed}
        self.assertIn("capture_realm", names)
        self.assertIn("point_and_click", names)
        self.assertIn("press_shortcut", names)
        self.assertIn("wait", names)
        self.assertIn("type_text", names)
        self.assertNotIn("open_application", names)
        for tool in listed:
            self.assertNotIn("realm", tool["inputSchema"].get("properties", {}))

        info = self.tool("realm_info", {}, 3)
        self.assertFalse(info["isError"])
        self.assertEqual(info["structuredContent"]["realm"]["name"], "codex")

        before = len(self.control.requests)
        rejected = self.tool("move_pointer", {"realm": "other", "x": 1, "y": 2}, 4)
        self.assertTrue(rejected["isError"])
        self.assertEqual(len(self.control.requests), before)

        moved = self.tool("move_pointer", {"x": 12, "y": 34}, 5)
        self.assertFalse(moved["isError"])
        self.assertEqual(moved["structuredContent"]["action"], "applied")
        self.assertIn("elapsed_ms", moved["structuredContent"])
        pointed = self.tool("point_and_click", {"x": 20, "y": 30}, "5-point")
        self.assertFalse(pointed["isError"])
        clicked = self.tool("click", {"button": "left"}, 6)
        self.assertFalse(clicked["isError"])
        pressed = self.tool("press_key", {"keycode": 30}, 7)
        self.assertFalse(pressed["isError"])
        named = self.tool("press_key", {"key": "enter"}, "7-named")
        self.assertFalse(named["isError"])
        shortcut = self.tool("press_shortcut", {"modifiers": ["ctrl"], "key": "t"}, "7-shortcut")
        self.assertFalse(shortcut["isError"])
        typed = self.tool("type_text", {"text": "hello realm"}, 8)
        self.assertFalse(typed["isError"])
        waited = self.tool("wait", {"duration_ms": 0}, "8-wait")
        self.assertFalse(waited["isError"])

        denied = self.tool("scroll", {"axis": "vertical", "steps": 13}, 9)
        self.assertTrue(denied["isError"])
        self.assertIn("capability_denied", denied["content"][0]["text"])

        captured = self.tool("capture_realm", {}, 10)
        self.assertFalse(captured["isError"])
        image = captured["content"][0]
        self.assertEqual(image["type"], "image")
        self.assertEqual(image["mimeType"], "image/png")
        png = base64.b64decode(image["data"])
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(struct.unpack(">II", png[16:24]), (2, 2))
        self.assertEqual(captured["structuredContent"]["source_width"], 2)
        self.assertEqual(captured["structuredContent"]["source_height"], 2)

        native_pointed = self.tool("point_and_click", {"x": 1, "y": 1}, "10-native-point")
        self.assertFalse(native_pointed["isError"])

        region = self.tool("capture_realm", {"x": 1, "y": 1, "width": 1, "height": 1}, 11)
        self.assertFalse(region["isError"])
        region_png = base64.b64decode(region["content"][0]["data"])
        self.assertEqual(struct.unpack(">II", region_png[16:24]), (1, 1))

        out_of_bounds = self.tool("move_pointer", {"x": 2, "y": 0}, "11-bounds")
        self.assertTrue(out_of_bounds["isError"])
        self.assertIn("2x2", out_of_bounds["content"][0]["text"])

        changed = self.tool(
            "capture_realm",
            {
                "wait_for_change": True,
                "timeout_ms": 1000,
                "poll_interval_ms": 250,
            },
            12,
        )
        self.assertFalse(changed["isError"])
        self.assertTrue(changed["structuredContent"]["waited_for_change"])
        self.assertEqual(changed["structuredContent"]["coordinate_width"], 2)
        self.assertGreaterEqual(changed["structuredContent"]["elapsed_ms"], 250)

        self.assertTrue(self.control.requests)
        self.assertTrue(all(request["params"]["realm"] == "codex" for request in self.control.requests))
        methods = [request["method"] for request in self.control.requests]
        self.assertEqual(methods.count("pointer.click"), 1)
        self.assertEqual(methods.count("pointer.point_and_click"), 2)
        self.assertEqual(methods.count("keyboard.press"), 2)
        self.assertEqual(methods.count("keyboard.shortcut"), 1)
        self.assertEqual(methods.count("realm.capture"), 3)
        self.assertNotIn("realm.capture_region", methods)
        move = next(request for request in self.control.requests if request["method"] == "pointer.move")
        self.assertEqual((move["params"]["width"], move["params"]["height"]), (1280, 720))
        points = [request for request in self.control.requests if request["method"] == "pointer.point_and_click"]
        self.assertEqual((points[-1]["params"]["width"], points[-1]["params"]["height"]), (2, 2))


class RealmMCPOrchestratorTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory(prefix="realm-mcp-orchestrator.")
        root = pathlib.Path(self.temporary_directory.name)
        os.chmod(root, 0o700)
        self.control = FakeRealmControlServer(root)
        self.process = subprocess.Popen(
            [sys.argv[1], "--socket", str(self.control.path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def tearDown(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)
        stderr = self.process.stderr.read()
        self.process.stdout.close()
        self.process.stderr.close()
        self.control.close()
        self.temporary_directory.cleanup()
        self.assertEqual(self.process.returncode, 0, stderr)

    def call(self, method, params=None, request_id=1):
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        self.process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        readable, _, _ = select.select([self.process.stdout], [], [], 3)
        self.assertTrue(readable, "MCP orchestrator did not respond")
        return json.loads(self.process.stdout.readline())

    def tool(self, name, arguments=None, request_id=2):
        params = {"name": name}
        if arguments is not None:
            params["arguments"] = arguments
        return self.call("tools/call", params, request_id)["result"]

    def test_launches_and_routes_multiple_temporary_realms(self):
        initialized = self.call(
            "initialize",
            {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {"name": "orchestrator-test", "version": "1"},
            },
        )
        self.assertIn("demo orchestrator", initialized["result"]["instructions"])

        tools = self.call("tools/list", {}, 2)["result"]["tools"]
        names = {tool["name"] for tool in tools}
        self.assertIn("launch_realm", names)
        self.assertIn("launch_realms", names)
        self.assertIn("open_application", names)
        self.assertIn("finish_realm", names)
        self.assertNotIn("realm_create", names)
        capture_schema = next(tool for tool in tools if tool["name"] == "capture_realm")["inputSchema"]
        self.assertIn("realm", capture_schema["required"])
        self.assertEqual(capture_schema["properties"]["poll_interval_ms"]["minimum"], 100)

        launched = self.tool(
            "launch_realm",
            {"name": "demo-one", "application": "kitty", "workspace": 5},
            3,
        )
        self.assertFalse(launched["isError"])
        self.assertTrue(launched["structuredContent"]["temporary"])
        self.assertEqual(launched["structuredContent"]["name"], "demo-one")
        self.assertEqual(launched["structuredContent"]["workspace"], 5)

        launched_many = self.tool(
            "launch_realms",
            {
                "realms": [
                    {"name": "demo-two", "application": "brave", "workspace": 4},
                    {"name": "demo-three", "application": "kitty", "workspace": 6},
                ]
            },
            4,
        )
        self.assertFalse(launched_many["isError"])
        self.assertEqual(len(launched_many["structuredContent"]["realms"]), 2)

        captured = self.tool("capture_realm", {"realm": "demo-two"}, 5)
        self.assertFalse(captured["isError"])
        self.assertEqual(captured["structuredContent"]["realm"], "demo-two")
        pointed = self.tool(
            "point_and_click",
            {"realm": "demo-two", "x": 1, "y": 1},
            6,
        )
        self.assertFalse(pointed["isError"])

        opened = self.tool(
            "open_application",
            {"realm": "demo-one", "application": "brave"},
            7,
        )
        self.assertFalse(opened["isError"])
        finished = self.tool("finish_realm", {"realm": "demo-one"}, 8)
        self.assertFalse(finished["isError"])
        self.assertEqual(finished["structuredContent"]["action"], "finished")

        demo_requests = [
            request for request in self.control.requests if request.get("params", {}).get("realm") == "demo-one"
        ]
        methods = [request["method"] for request in demo_requests]
        self.assertIn("realm.place", methods)
        self.assertEqual(methods.count("realm.grant"), 3)
        self.assertIn("realm.open", methods)
        self.assertIn("realm.destroy", methods)


class RealmMCPServerSocketSafetyTest(unittest.TestCase):
    def test_discovers_short_control_socket_name(self):
        with tempfile.TemporaryDirectory(prefix="realm-mcp-discovery.") as temporary_directory:
            root = pathlib.Path(temporary_directory)
            signature = "test-instance"
            instance = root / "hypr" / signature
            instance.mkdir(parents=True, mode=0o700)
            control = FakeRealmControlServer(instance)
            environment = os.environ.copy()
            environment["XDG_RUNTIME_DIR"] = str(root)
            environment["HYPRLAND_INSTANCE_SIGNATURE"] = signature
            try:
                result = subprocess.run(
                    [sys.argv[1], "--realm", "codex"],
                    input="",
                    capture_output=True,
                    text=True,
                    timeout=3,
                    check=False,
                    env=environment,
                )
            finally:
                control.close()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(control.path.name, ".realm.sock")

    def test_rejects_insecure_control_socket_permissions(self):
        with tempfile.TemporaryDirectory(prefix="realm-mcp-insecure.") as temporary_directory:
            root = pathlib.Path(temporary_directory)
            os.chmod(root, 0o700)
            socket_path = root / ".realm.sock"
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                listener.bind(str(socket_path))
                os.chmod(socket_path, 0o666)
                listener.listen(1)
                result = subprocess.run(
                    [sys.argv[1], "--realm", "codex", "--socket", str(socket_path)],
                    input="",
                    capture_output=True,
                    text=True,
                    timeout=3,
                    check=False,
                )
            finally:
                listener.close()

        self.assertEqual(result.returncode, 1)
        self.assertIn("accessible by group or other users", result.stderr)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: RealmMCPServer.py MCP_SERVER_BINARY")
    unittest.main(argv=[sys.argv[0]])
