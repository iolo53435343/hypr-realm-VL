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
        self.path = root / ".realm-control.sock"
        self.requests = []
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
                        connection.sendall(
                            self._frame(
                                {
                                    "request_id": request_id,
                                    "ok": True,
                                    "result": {
                                        "action": "queued",
                                        "capture_id": 41,
                                        "realm": {"name": "codex"},
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
                                        0x00,
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

                    connection.sendall(
                        self._frame(
                            {
                                "request_id": request_id,
                                "ok": True,
                                "result": {
                                    "action": method,
                                    "realm": {
                                        "id": 7,
                                        "name": "codex",
                                        "state": "running",
                                    },
                                },
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

    def test_realm_bound_tools_and_capture(self):
        self.initialize()

        listed = self.call("tools/list", {}, 2)["result"]["tools"]
        names = {tool["name"] for tool in listed}
        self.assertIn("capture_realm", names)
        self.assertIn("type_text", names)
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
        clicked = self.tool("click", {"button": "left"}, 6)
        self.assertFalse(clicked["isError"])
        pressed = self.tool("press_key", {"keycode": 30}, 7)
        self.assertFalse(pressed["isError"])
        typed = self.tool("type_text", {"text": "hello realm"}, 8)
        self.assertFalse(typed["isError"])

        denied = self.tool("scroll", {"axis": "vertical", "steps": 13}, 9)
        self.assertTrue(denied["isError"])
        self.assertIn("capability_denied", denied["content"][0]["text"])

        captured = self.tool("capture_realm", {}, 10)
        self.assertFalse(captured["isError"])
        image = captured["content"][0]
        self.assertEqual(image["type"], "image")
        self.assertEqual(image["mimeType"], "image/png")
        self.assertTrue(base64.b64decode(image["data"]).startswith(b"\x89PNG\r\n\x1a\n"))

        self.assertTrue(self.control.requests)
        self.assertTrue(all(request["params"]["realm"] == "codex" for request in self.control.requests))
        methods = [request["method"] for request in self.control.requests]
        self.assertEqual(methods.count("pointer.click"), 1)
        self.assertEqual(methods.count("keyboard.press"), 1)


class RealmMCPServerSocketSafetyTest(unittest.TestCase):
    def test_rejects_insecure_control_socket_permissions(self):
        with tempfile.TemporaryDirectory(prefix="realm-mcp-insecure.") as temporary_directory:
            root = pathlib.Path(temporary_directory)
            os.chmod(root, 0o700)
            socket_path = root / ".realm-control.sock"
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
