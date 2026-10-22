#!/usr/bin/env python3
"""Local mock of the Wi-Fi configuration portal.

Develop and test an OEM portal pack (L3 full-page replacement) in a browser
without flashing hardware. Endpoints mirror docs/portal-contract.md.

Usage:
    python3 scripts/product/portal_mock.py --brand acme --port 8000
    python3 scripts/product/portal_mock.py --pack product/profiles/acme/portal
    python3 scripts/product/portal_mock.py            # stock page, no brand

Then open http://127.0.0.1:8000/ (or the host shown) in a browser or a phone
on the same network.
"""

import argparse
import importlib.util
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Optional
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[2]
STOCK_PACK = ROOT / "components/esp-wifi-connect/assets"
SPEC = importlib.util.spec_from_file_location("build", ROOT / "scripts/build.py")
build = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(build)

FAKE_NETWORKS = [
    {"ssid": "Demo-Office", "rssi": -48, "authmode": 3},
    {"ssid": "Demo-Guest", "rssi": -61, "authmode": 4},
    {"ssid": "Open-Cafe", "rssi": -74, "authmode": 0},
]

CAPTIVE_PATHS = {
    "/hotspot-detect.html",
    "/connectivity-check.html",
    "/portal.html",
    "/library/test/success.html",
    "/generate_204",
    "/ncsi.txt",
}


class PortalState:
    def __init__(self) -> None:
        self.saved: list[str] = ["Demo-Office"]
        self.brand_js: Optional[bytes] = None
        self.logo: Optional[bytes] = None
        self.logo_mime = "image/svg+xml"
        self.pack_dir = STOCK_PACK
        # Map canonical embedded names to the files present in the pack.
        self.page_names = {
            "wifi_configuration.html": "wifi_configuration.html",
            "wifi_configuration_done.html": "wifi_configuration_done.html",
        }
        self.events: list[str] = []


class Handler(BaseHTTPRequestHandler):
    state: PortalState

    # -- helpers ---------------------------------------------------------
    def _send(self, status: int, body: bytes, content_type: str, headers: Optional[dict[str, str]] = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _send_json(self, payload: Any, status: int = 200) -> None:
        self._send(status, json.dumps(payload, ensure_ascii=False).encode(), "application/json")

    def _read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            payload = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            return {}
        return payload if isinstance(payload, dict) else {}

    def _serve_page(self, name: str, fallback: Path) -> None:
        candidate = self.state.pack_dir / self.state.page_names.get(name, name)
        path = candidate if candidate.is_file() else fallback
        self._send(200, path.read_bytes(), "text/html; charset=utf-8")

    def log_message(self, fmt: str, *args: Any) -> None:
        sys.stderr.write("[mock] %s\n" % (fmt % args))

    # -- routes -----------------------------------------------------------
    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        route = parsed.path

        if route in CAPTIVE_PATHS:
            self.send_response(302)
            self.send_header("Location", "/?lang=zh-CN")
            self.end_headers()
            return

        if route == "/":
            self._serve_page("wifi_configuration.html", STOCK_PACK / "wifi_configuration.html")
        elif route == "/done.html":
            self._serve_page("wifi_configuration_done.html", STOCK_PACK / "wifi_configuration_done.html")
        elif route == "/brand.js":
            if self.state.brand_js is None:
                self._send(404, b"not found", "text/plain")
            else:
                self._send(200, self.state.brand_js, "application/javascript", {"Cache-Control": "no-store"})
        elif route == "/brand-logo":
            if self.state.logo is None:
                self._send(404, b"not found", "text/plain")
            else:
                self._send(200, self.state.logo, self.state.logo_mime, {"Cache-Control": "no-store"})
        elif route == "/scan":
            self._send_json({"support_5g": False, "aps": FAKE_NETWORKS})
        elif route == "/saved/list":
            self._send_json(self.state.saved)
        elif route == "/saved/set_default":
            index = self._index(parsed.query)
            if index is None or not (0 <= index < len(self.state.saved)):
                self._send_json({"success": False, "error": "bad index"}, 400)
                return
            default = self.state.saved.pop(index)
            self.state.saved.insert(0, default)
            self._send_json({"success": True})
        elif route == "/saved/delete":
            index = self._index(parsed.query)
            if index is None or not (0 <= index < len(self.state.saved)):
                self._send_json({"success": False, "error": "bad index"}, 400)
                return
            del self.state.saved[index]
            self._send_json({"success": True})
        elif route == "/advanced/config":
            self._send_json(
                {
                    "ota_url": "",
                    "max_tx_power": 0,
                    "remember_bssid": False,
                    "sleep_mode": False,
                    "show_ota_config": True,
                    "show_sleep_config": True,
                }
            )
        else:
            self._send(404, b"not found", "text/plain")

    def do_HEAD(self) -> None:  # noqa: N802
        self.do_GET()

    def do_POST(self) -> None:  # noqa: N802
        route = urlparse(self.path).path
        payload = self._read_json()

        if route == "/submit":
            ssid = payload.get("ssid")
            if not isinstance(ssid, str) or not ssid:
                self._send_json({"success": False, "error": "ssid is required"}, 400)
                return
            if ssid not in self.state.saved:
                self.state.saved.insert(0, ssid)
            self.state.events.append(f"submit ssid={ssid!r} password={'*' * len(payload.get('password') or '')}")
            self._send_json({"success": True})
        elif route == "/advanced/submit":
            self.state.events.append(f"advanced {json.dumps(payload, ensure_ascii=False)}")
            self._send_json({"success": True})
        elif route == "/exit":
            self.state.events.append("exit requested")
            self._send(200, b"ok", "text/plain")
        else:
            self._send(404, b"not found", "text/plain")

    def _index(self, query: str) -> Optional[int]:
        values = parse_qs(query).get("index")
        if not values:
            return None
        try:
            return int(values[0])
        except ValueError:
            return None


def _load_brand(brand: str, state: PortalState) -> None:
    profile = build._load_brand_profile(brand)
    web = profile.get("web", {})
    staged = {
        "logo_format": None,
        "brand_config": "declarative" if web.get("brand_config") else None,
    }

    pack = web.get("portal_pack")
    if pack:
        state.pack_dir = build._brand_product_path(pack, f"brand {brand}")
        # Profile packs use the authoring layout (index.html/done.html).
        state.page_names["wifi_configuration.html"] = "index.html"
        state.page_names["wifi_configuration_done.html"] = "done.html"

    logo = web.get("logo")
    if logo:
        logo_path = build._brand_product_path(logo, f"brand {brand}")
        state.logo = logo_path.read_bytes()
        state.logo_mime = "image/svg+xml" if logo_path.suffix.lower() == ".svg" else "image/png"
        staged["logo_format"] = "svg" if logo_path.suffix.lower() == ".svg" else "png"

    config = build._brand_portal_config(profile, staged)
    if config:
        state.brand_js = (
            "window.BRAND_CONFIG = " + json.dumps(config, ensure_ascii=False) + ";"
        ).encode("utf-8")


def main(argv: Optional[list[str]] = None) -> None:
    parser = argparse.ArgumentParser(description="Mock the Wi-Fi configuration portal")
    parser.add_argument("--brand", metavar="TENANT", help="Brand profile from product/profiles")
    parser.add_argument("--pack", metavar="DIR", help="Portal pack directory (overrides --brand)")
    parser.add_argument("--port", type=int, default=8000, help="Listen port (default 8000)")
    parser.add_argument("--host", default="0.0.0.0", help="Bind address (default 0.0.0.0)")
    args = parser.parse_args(argv)

    state = PortalState()
    if args.pack:
        state.pack_dir = Path(args.pack).resolve()
        has_index = (state.pack_dir / "index.html").is_file()
        has_canonical = (state.pack_dir / "wifi_configuration.html").is_file()
        if has_index:
            state.page_names["wifi_configuration.html"] = "index.html"
            state.page_names["wifi_configuration_done.html"] = "done.html"
        elif not has_canonical:
            parser.error(f"{state.pack_dir} has neither index.html nor wifi_configuration.html")
    elif args.brand:
        _load_brand(args.brand, state)

    Handler.state = state
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Portal mock: http://{args.host}:{args.port}/  (pack: {state.pack_dir})")
    print("Seed networks:", ", ".join(net["ssid"] for net in FAKE_NETWORKS))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if state.events:
            print("\nRecorded events:")
            for event in state.events:
                print("  -", event)


if __name__ == "__main__":
    main()
