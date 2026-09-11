#!/usr/bin/env python3
"""Smoke test a built CLIProxyAPI native plugin.

Reproduces what internal/pluginhost/loader_unix.go does, without needing the
server:

  1. dlopen the library (CDLL)
  2. dlsym "cliproxy_plugin_init" and call it with a host API table
  3. verify abi_version and that the returned function table is complete
  4. drive real ABI calls through the table

Usage:
    python3 scripts/smoke-test.py path/to/plugin.so
    python3 scripts/smoke-test.py dist/key-model-restriction_1.0.0_linux_amd64.zip

Exit status is non-zero if any check fails.
"""

from __future__ import annotations

import base64
import ctypes
import hashlib
import json
import sys
import tempfile
import zipfile
from pathlib import Path

PLUGIN_ID = "key-model-restriction"
ABI_VERSION = 1
CALLER_SCOPE_PREFIX = b"cli-proxy-api:caller-scope:v1\x00"


class Buffer(ctypes.Structure):
    _fields_ = [("ptr", ctypes.c_void_p), ("len", ctypes.c_size_t)]


CallFn = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(Buffer),
)
HostCallFn = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.POINTER(Buffer),
)
HostFreeFn = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_size_t)
FreeFn = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_size_t)
ShutdownFn = ctypes.CFUNCTYPE(None)


class HostApi(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("host_ctx", ctypes.c_void_p),
        ("call", HostCallFn),
        ("free_buffer", HostFreeFn),
    ]


class PluginApi(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("call", CallFn),
        ("free_buffer", FreeFn),
        ("shutdown", ShutdownFn),
    ]


def caller_scope(api_key: str) -> str:
    return hashlib.sha256(CALLER_SCOPE_PREFIX + api_key.encode()).hexdigest()


def resolve_library(argument: str) -> Path:
    """Accept either the built library or the plugin store zip containing it."""
    path = Path(argument)
    if not path.is_file():
        raise SystemExit(f"library not found: {path}")
    if path.suffix != ".zip":
        return path

    target = f"{PLUGIN_ID}.so"
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if target not in names:
            raise SystemExit(f"{path.name}: {target} not at zip root (found: {names})")
        destination = Path(tempfile.mkdtemp(prefix="plugin-smoke-"))
        archive.extract(target, destination)
    return destination / target


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    library = resolve_library(argv[1])
    print(f"loading {library}")

    lib = ctypes.CDLL(str(library))
    try:
        init = lib.cliproxy_plugin_init
    except AttributeError:
        print("FAIL: library does not export cliproxy_plugin_init", file=sys.stderr)
        return 1

    init.restype = ctypes.c_int
    init.argtypes = [ctypes.POINTER(HostApi), ctypes.POINTER(PluginApi)]

    host_logs: list[dict] = []

    @HostCallFn
    def host_call(_host_ctx, method, request, request_len, response):
        method_name = method.decode() if method else ""
        raw_request = ctypes.string_at(request, request_len) if request and request_len else b""
        if method_name == "host.log":
            host_logs.append(json.loads(raw_request))
        response.contents.ptr = None
        response.contents.len = 0
        return 0

    host = HostApi(ABI_VERSION, None, host_call, HostFreeFn(lambda _ptr, _length: None))
    api = PluginApi()
    rc = init(ctypes.byref(host), ctypes.byref(api))
    if rc != 0:
        print(f"FAIL: cliproxy_plugin_init returned {rc}", file=sys.stderr)
        return 1
    if api.abi_version != ABI_VERSION:
        print(f"FAIL: abi_version={api.abi_version}, host requires {ABI_VERSION}", file=sys.stderr)
        return 1
    if not api.call or not api.free_buffer:
        print("FAIL: plugin function table is incomplete", file=sys.stderr)
        return 1
    print(f"init ok: abi_version={api.abi_version}, function table complete")

    def call(method: str, payload: dict) -> dict:
        body = json.dumps(payload).encode()
        array = (ctypes.c_uint8 * len(body)).from_buffer_copy(body)
        buffer = Buffer()
        status = api.call(method.encode(), array, len(body), ctypes.byref(buffer))
        if status != 0:
            raise RuntimeError(f"{method} returned {status}")
        if not buffer.ptr or not buffer.len:
            return {}
        raw = ctypes.string_at(buffer.ptr, buffer.len)
        api.free_buffer(buffer.ptr, buffer.len)
        return json.loads(raw)

    # The host base64-encodes the complete plugin config YAML. This is the
    # direct object shape produced by the CPA Management Center object editor.
    configured_key = "sk-test-configured-1234567890abcdef"
    exception_key = "sk-test-exception-1234567890"
    ui_config = f"""enabled: true
priority: 100
keys:
  {configured_key}:
    allowed_models:
      - gpt-5.6-luna
      - gpt-5.5
  {exception_key}:
    unrestricted: true
default:
  denied_models:
    - hy3-201
    - hy3-preview-201
    - hy3-preview-agent-201
    - hy4-preview-201
    - LongCat-2.0-201
""".encode()
    registration = call("plugin.register", {"config_yaml": base64.b64encode(ui_config).decode()})
    if not registration.get("ok"):
        print(f"FAIL: plugin.register rejected the config: {registration}", file=sys.stderr)
        return 1
    fields = registration.get("result", {}).get("metadata", {}).get("ConfigFields", [])
    advertised = {(field.get("Name"), field.get("Type")) for field in fields}
    if ("keys", "object") not in advertised or ("default", "object") not in advertised:
        print(f"FAIL: registration did not advertise default/keys object fields: {fields}", file=sys.stderr)
        return 1
    print("plugin.register ok: ConfigFields contains default/keys objects")

    def mask_for_test_key(api_key):
        if not api_key:
            return "unknown"
        return api_key[:8] + "..." + api_key[-4:] if len(api_key) > 12 else "***"

    failures = 0
    cases = [
        ("gpt-5.6-luna", "gpt-5.6-luna", "sk-unknown", False, "global non-denied model must pass"),
        ("codex-auto-review", "codex-auto-review", "sk-unknown", False, "codex model is outside denied list"),
        ("hy3-201", "hy3-201", "sk-unknown", True, "global denied model must terminate"),
        ("hy3-preview-201", "hy3-preview-201", "sk-unknown", True, "global denied preview must terminate"),
        ("gpt-5.6-luna", "gpt-5.6-luna", configured_key, False, "configured key allowed model must pass"),
        ("gpt-5.5", "gpt-5.5", configured_key, False, "configured key second allowed model must pass"),
        ("gpt-6-astra", "gpt-6-astra", configured_key, True, "configured key other GPT must terminate"),
        ("hy3-201", "hy3-201", configured_key, True, "configured key non-GPT must terminate"),
        ("hy3-201", "hy3-201", exception_key, False, "exception key denied model must pass"),
        ("LongCat-2.0-201", "LongCat-2.0-201", exception_key, False, "exception key LongCat must pass"),
        ("hy3", "hy3-201", "sk-unknown", True, "denied resolved alias must terminate"),
        ("hy3-201", "hy3", "sk-unknown", True, "denied requested alias must terminate"),
        ("gpt-5.6-luna", "gpt-5.6-luna", None, False, "missing caller scope must pass"),
    ]
    for model, requested_model, api_key, expect_terminate, label in cases:
        metadata = {"caller_scope": caller_scope(api_key)} if api_key else {}
        response = call(
            "request.intercept_after",
            {"Model": model, "RequestedModel": requested_model, "Metadata": metadata},
        )
        result = response.get("result") or {}
        terminated = bool(result.get("Terminate"))
        ok = terminated == expect_terminate
        failures += 0 if ok else 1
        detail = f" status={result.get('StatusCode')}" if terminated else ""
        print(f"{'OK  ' if ok else 'FAIL'} {label}: Model={model} RequestedModel={requested_model} Terminate={terminated}{detail}")

    decision_logs = [entry for entry in host_logs if entry.get("fields", {}).get("plugin") == "key-model-restriction"]
    if len(decision_logs) != len(cases):
        print(f"FAIL: expected {len(cases)} host.log calls, got {len(decision_logs)}", file=sys.stderr)
        failures += 1
    else:
        for index, entry in enumerate(decision_logs):
            fields = entry.get("fields", {})
            if not fields.get("decision") or not fields.get("reason"):
                print(f"FAIL: incomplete decision log: {entry}", file=sys.stderr)
                failures += 1
            if "key_fingerprint=" not in entry.get("message", ""):
                print(f"FAIL: key fingerprint is not visible in log message: {entry}", file=sys.stderr)
                failures += 1
            if "api_key_masked=" not in entry.get("message", ""):
                print(f"FAIL: masked API key is not visible in log message: {entry}", file=sys.stderr)
                failures += 1
            expected_masked = ("unknown", "unknown", "unknown", "unknown",
                               mask_for_test_key(configured_key), mask_for_test_key(configured_key),
                               mask_for_test_key(configured_key), mask_for_test_key(configured_key),
                               mask_for_test_key(exception_key), mask_for_test_key(exception_key),
                               "unknown", "unknown", "unknown")[index]
            if fields.get("api_key_masked") != expected_masked:
                print(f"FAIL: masked API key = {fields.get('api_key_masked')!r}, want {expected_masked!r}", file=sys.stderr)
                failures += 1
            expected_source = "none" if index == len(cases) - 1 else ("key" if index >= 4 and index <= 9 else "default")
            if fields.get("rule_source") != expected_source:
                print(f"FAIL: rule source = {fields.get('rule_source')!r}, want {expected_source!r}", file=sys.stderr)
                failures += 1
            if configured_key in entry.get("message", ""):
                print("FAIL: full configured API key leaked into log message", file=sys.stderr)
                failures += 1
        print(f"host.log decision records: {len(decision_logs)}")
    if any(entry.get("fields", {}).get("api_key") for entry in decision_logs):
        print("FAIL: host.log contains forbidden api_key field", file=sys.stderr)
        failures += 1

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
