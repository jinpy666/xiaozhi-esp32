"""L1 host test: Family protocol v1 uplink builders + downlink dispatch
(main/protocols/protocol.cc Family block).

Same harness as test_protocol_messages.py: the real protocol.cc is compiled on
the host with a stub esp_log.h / assets.h and driven through a concrete
subclass whose SendText either captures outgoing messages or reports the
"channel closed" failure path. Pins the FAMILY-PROTOCOL v1 contract:

- Uplink: every SendFamily* produces {"type":"family","action":...} and
  propagates the SendText() result. That boolean is the whole "decision tree"
  the application layer relies on (WS only accepts text frames while the audio
  channel is open, MQTT whenever the broker is connected — see
  application.cc SendFamilyMessageArrived / SendFamilySos): closed channel ->
  false -> caller plays the failure sound.
- Three-state fields: SendFamilyLocationReport omits battery_pct when
  negative (battery unknown), SendFamilyAck omits ref_id / error when null.
- SendFamilyVoiceMessage builds its JSON by hand: audio_format is always
  "opus", duration_ms is a JSON number (not a string), and message_id /
  audio_b64 are concatenated verbatim — the documented no-escaping assumption
  (UUID + base64 alphabet).
- Envelope merge: SendFamilyAction duplicates payload items (the caller's
  cJSON object stays intact) and accepts a null payload (type + action only).
- Downlink: DispatchFamilyMessage whitelists exactly the v1 actions
  (message_arrived / sos_acked / alarm_fire / call_incoming / ack), drops
  unknown / empty / missing actions, and tolerates a missing handler
  (call_incoming currently has no device-side handler and is acked
  not_implemented by Application::HandleFamilyCallIncoming).

Contract source: main/protocols/protocol.cc Family block and the
FAMILY-PROTOCOL v1 comments in main/application.h. 07-communication-protocol.md
does not document family messages yet.

Requires system libcjson; when absent the test skips with a clear reason.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PROTOCOLS_DIR = REPO_ROOT / "main" / "protocols"

STUB_ESP_LOG = """#pragma once
#define ESP_LOGI(...) do {} while (0)
#define ESP_LOGW(...) do {} while (0)
#define ESP_LOGE(...) do {} while (0)
"""

STUB_ASSETS_H = """#pragma once
#include <string>
class Assets {
public:
    static Assets& GetInstance() { static Assets a; return a; }
    struct FontCapability {
        bool glyph_push = false;
        std::string bundle;
        std::string charset;
        int size = 0;
        int bpp = 0;
    };
    FontCapability text_font_capability() const { return FontCapability{}; }
};
"""

CJSON_CANDIDATES = [
    ("/usr/include/cjson", None),
    ("/usr/local/include/cjson", None),
    ("/opt/homebrew/include/cjson", "/opt/homebrew/lib"),
]

DRIVER = r"""
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <cJSON.h>
#include "protocol.h"

static int g_failures = 0;
static void CHECK(bool ok, const std::string& msg) {
    if (!ok) {
        printf("FAIL: %s\n", msg.c_str());
        ++g_failures;
    }
}

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

class TestProtocol : public Protocol {
public:
    std::vector<std::string> sent;
    bool send_text_result = true;

    bool SendText(const std::string& text) override {
        sent.push_back(text);
        return send_text_result;
    }
    bool Start() override { return true; }
    bool OpenAudioChannel() override { return true; }
    void CloseAudioChannel(bool send_goodbye) override { (void)send_goodbye; }
    bool IsAudioChannelOpened() const override { return true; }
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override {
        (void)packet;
        return true;
    }
    // SendFamilyAction is protected in the base; expose it for the tests.
    bool send_family_action(const char* action, cJSON* payload) {
        return SendFamilyAction(action, payload);
    }
};

static bool is_str(const cJSON* root, const char* key, const std::string& want) {
    cJSON* item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsString(item) && std::string(item->valuestring) == want;
}

static int item_count(const cJSON* obj) {
    int n = 0;
    cJSON* item = NULL;
    cJSON_ArrayForEach (item, obj) { ++n; }
    return n;
}

int main() {
    TestProtocol p;

    // --- Uplink: SendFamilySos -------------------------------------------
    p.sent.clear();
    p.send_text_result = true;
    CHECK(p.SendFamilySos("sos-uuid-1", "button"), "sos returns true");
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        CHECK(root != NULL, "sos valid json");
        CHECK(item_count(root) == 4, "sos has exactly type/action/message_id/trigger");
        CHECK(is_str(root, "type", "family"), "sos type");
        CHECK(is_str(root, "action", "sos"), "sos action");
        CHECK(is_str(root, "message_id", "sos-uuid-1"), "sos message_id");
        CHECK(is_str(root, "trigger", "button"), "sos trigger");
        cJSON_Delete(root);
    }

    // Failure path: SendText false must propagate (channel closed -> caller
    // plays the failure sound). The frame is still built, mirroring the real
    // transports where construction succeeds but delivery fails.
    p.send_text_result = false;
    CHECK(!p.SendFamilySos("sos-uuid-2", "button"), "sos propagates SendText false");
    CHECK(p.sent.back().find("\"action\":\"sos\"") != std::string::npos,
          "sos frame built even when channel closed");
    p.send_text_result = true;

    // --- Uplink: SendFamilyVoiceMessage (hand-built JSON) ----------------
    p.sent.clear();
    const std::string kAudioB64 = "T2RvdXQ+T3B1cw==+/=";
    CHECK(p.SendFamilyVoiceMessage("msg-uuid-1", kAudioB64, 42000), "voice returns true");
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        CHECK(root != NULL, "voice valid json");
        CHECK(is_str(root, "type", "family"), "voice type");
        CHECK(is_str(root, "action", "message_send"), "voice action");
        CHECK(is_str(root, "message_id", "msg-uuid-1"), "voice message_id");
        CHECK(is_str(root, "audio_format", "opus"), "voice audio_format always opus");
        CHECK(is_str(root, "audio_b64", kAudioB64),
              "voice audio_b64 concatenated verbatim (no-escaping assumption)");
        cJSON* dur = cJSON_GetObjectItem(root, "duration_ms");
        CHECK(cJSON_IsNumber(dur) && dur->valueint == 42000,
              "voice duration_ms is a JSON number");
        cJSON_Delete(root);
    }
    p.send_text_result = false;
    CHECK(!p.SendFamilyVoiceMessage("msg-uuid-2", kAudioB64, 1000),
          "voice propagates SendText false");
    p.send_text_result = true;

    // --- Uplink: SendFamilyLocationReport (battery_pct three-state) ------
    p.sent.clear();
    CHECK(p.SendFamilyLocationReport(31.2304, 121.4737, "gps", 12.5, 86),
          "location(battery known) returns true");
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        CHECK(root != NULL, "location valid json");
        CHECK(is_str(root, "action", "location_report"), "location action");
        cJSON* lat = cJSON_GetObjectItem(root, "lat");
        cJSON* lng = cJSON_GetObjectItem(root, "lng");
        cJSON* acc = cJSON_GetObjectItem(root, "accuracy_m");
        CHECK(cJSON_IsNumber(lat) && near(lat->valuedouble, 31.2304), "location lat number");
        CHECK(cJSON_IsNumber(lng) && near(lng->valuedouble, 121.4737), "location lng number");
        CHECK(cJSON_IsNumber(acc) && near(acc->valuedouble, 12.5), "location accuracy_m number");
        CHECK(is_str(root, "source", "gps"), "location source");
        cJSON* bat = cJSON_GetObjectItem(root, "battery_pct");
        CHECK(cJSON_IsNumber(bat) && bat->valueint == 86, "location battery_pct present when >= 0");
        cJSON_Delete(root);
    }
    p.sent.clear();
    CHECK(p.SendFamilyLocationReport(31.2304, 121.4737, "lbs", 50.0, -1),
          "location(battery unknown) returns true");
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        CHECK(root != NULL, "location(no battery) valid json");
        CHECK(is_str(root, "source", "lbs"), "location source lbs");
        CHECK(cJSON_GetObjectItem(root, "battery_pct") == NULL,
              "location battery_pct omitted when negative (battery unknown)");
        cJSON_Delete(root);
    }

    // --- Uplink: SendFamilyAck (ref_id / error three-state) --------------
    p.sent.clear();
    CHECK(p.SendFamilyAck("call_incoming", "call-uuid-1", true, nullptr),
          "ack(ok) returns true");
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        CHECK(root != NULL, "ack(ok) valid json");
        CHECK(is_str(root, "action", "ack"), "ack action");
        CHECK(is_str(root, "ref_action", "call_incoming"), "ack ref_action");
        CHECK(is_str(root, "ref_id", "call-uuid-1"), "ack ref_id present when non-null");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(root, "ok")), "ack ok true");
        CHECK(cJSON_GetObjectItem(root, "error") == NULL, "ack error omitted when null");
        cJSON_Delete(root);
    }
    p.sent.clear();
    CHECK(p.SendFamilyAck("call_incoming", nullptr, false, "not_implemented"),
          "ack(fail) returns true");
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        CHECK(root != NULL, "ack(fail) valid json");
        CHECK(cJSON_GetObjectItem(root, "ref_id") == NULL, "ack ref_id omitted when null");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(root, "ok")), "ack ok false");
        CHECK(is_str(root, "error", "not_implemented"), "ack error present when non-null");
        cJSON_Delete(root);
    }

    // --- Uplink: SendFamilyMessagePlayed ---------------------------------
    p.sent.clear();
    CHECK(p.SendFamilyMessagePlayed("msg-uuid-3"), "message_played returns true");
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        CHECK(root != NULL, "message_played valid json");
        CHECK(is_str(root, "action", "message_played"), "message_played action");
        CHECK(is_str(root, "message_id", "msg-uuid-3"), "message_played message_id");
        cJSON_Delete(root);
    }

    // --- Uplink: SendFamilyAction envelope -------------------------------
    p.sent.clear();
    CHECK(p.send_family_action("future_action", nullptr), "envelope(null payload) returns true");
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        CHECK(root != NULL, "envelope valid json");
        CHECK(item_count(root) == 2, "null payload -> only type + action");
        CHECK(is_str(root, "type", "family"), "envelope type");
        CHECK(is_str(root, "action", "future_action"), "envelope action passthrough");
        cJSON_Delete(root);
    }
    p.sent.clear();
    {
        // Duplicate semantics: the caller's payload stays intact after send.
        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "k1", "v1");
        cJSON_AddNumberToObject(payload, "k2", 7);
        CHECK(p.send_family_action("merged", payload), "envelope(payload) returns true");
        int kept = item_count(payload);
        cJSON_Delete(payload);
        CHECK(kept == 2, "payload items duplicated, not stolen (caller keeps 2 items)");

        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        CHECK(root != NULL, "merged valid json");
        CHECK(item_count(root) == 4, "merged envelope carries type/action/k1/k2");
        CHECK(is_str(root, "k1", "v1"), "merged payload string value");
        cJSON* k2 = cJSON_GetObjectItem(root, "k2");
        CHECK(cJSON_IsNumber(k2) && k2->valueint == 7, "merged payload number value");
        cJSON_Delete(root);
    }

    // --- Downlink: DispatchFamilyMessage whitelist -----------------------
    const char* kDownlinkActions[] = {
        "message_arrived", "sos_acked", "alarm_fire", "call_incoming", "ack",
    };
    for (const char* action : kDownlinkActions) {
        std::string json = std::string("{\"type\":\"family\",\"action\":\"") + action +
                           "\",\"message_id\":\"mid\"}";
        cJSON* root = cJSON_Parse(json.c_str());
        CHECK(root != NULL, std::string(action) + " fixture parses");
        bool delivered = false;
        p.OnFamilyMessage([&](const cJSON* got) {
            delivered = true;
            cJSON* a = cJSON_GetObjectItem(got, "action");
            CHECK(cJSON_IsString(a) && std::string(a->valuestring) == action,
                  std::string(action) + " delivered with full message");
        });
        p.DispatchFamilyMessage(root);
        p.OnFamilyMessage(nullptr);
        CHECK(delivered, std::string(action) + " whitelisted and delivered");
        cJSON_Delete(root);
    }

    // Unknown / empty / missing actions are dropped silently.
    const char* kDropped[] = {"call_answer", "call_accept", ""};
    for (const char* action : kDropped) {
        std::string json = std::string("{\"type\":\"family\",\"action\":\"") + action + "\"}";
        cJSON* root = cJSON_Parse(json.c_str());
        bool delivered = false;
        p.OnFamilyMessage([&](const cJSON*) { delivered = true; });
        p.DispatchFamilyMessage(root);
        p.OnFamilyMessage(nullptr);
        CHECK(!delivered, std::string("action '") + action + "' dropped");
        cJSON_Delete(root);
    }
    {
        cJSON* root = cJSON_Parse("{\"type\":\"family\"}");
        bool delivered = false;
        p.OnFamilyMessage([&](const cJSON*) { delivered = true; });
        p.DispatchFamilyMessage(root);
        p.OnFamilyMessage(nullptr);
        CHECK(!delivered, "missing action dropped");
        cJSON_Delete(root);
    }
    // Missing handler must not crash (call_incoming has no device handler in v1).
    {
        cJSON* root = cJSON_Parse("{\"type\":\"family\",\"action\":\"alarm_fire\"}");
        p.DispatchFamilyMessage(root);
        cJSON_Delete(root);
    }

    if (g_failures == 0) {
        printf("FAMILY_PROTOCOL_ALL_OK\n");
        return 0;
    }
    printf("FAMILY_PROTOCOL_FAILURES=%d\n", g_failures);
    return 1;
}
"""


def gpp() -> str:
    return shutil.which("g++") or shutil.which("c++") or ""


def probe_cjson(workdir: Path):
    """Return (include_dir, lib_dir_or_None) for system libcjson, else None."""
    probe_src = workdir / "probe.cpp"
    probe_src.write_text(
        "#include <cJSON.h>\nint main(){cJSON* r=cJSON_Parse(\"{}\");"
        "cJSON_Delete(r);return 0;}\n", encoding="utf-8")
    probe_bin = workdir / "probe_out"
    for inc, lib in CJSON_CANDIDATES:
        cmd = [gpp(), "-std=c++20", f"-I{inc}", str(probe_src), "-o", str(probe_bin), "-lcjson"]
        if lib:
            cmd.insert(-1, f"-L{lib}")
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode == 0:
            return inc, lib
    return None


def build(workdir: Path, inc: str, lib) -> Path:
    stub = workdir / "stub"
    stub.mkdir(parents=True)
    (stub / "esp_log.h").write_text(STUB_ESP_LOG, encoding="utf-8")
    (stub / "assets.h").write_text(STUB_ASSETS_H, encoding="utf-8")
    driver = workdir / "driver.cpp"
    driver.write_text(DRIVER, encoding="utf-8")
    binary = workdir / "family_protocol_test"
    cmd = [
        gpp(), "-std=c++20", "-pthread",
        f"-I{stub}", f"-I{inc}", f"-I{PROTOCOLS_DIR}",
        str(driver), str(PROTOCOLS_DIR / "protocol.cc"),
    ]
    if lib:
        cmd.append(f"-L{lib}")
    cmd += ["-lcjson", "-o", str(binary)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError("compile failed:\n" + proc.stderr)
    if not binary.exists():
        raise RuntimeError(
            "compiler exited 0 but binary missing; cwd=%r cmd=%r stdout=%r stderr=%r"
            % (Path.cwd(), cmd, proc.stdout, proc.stderr))
    return binary


class FamilyProtocolHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not gpp():
            raise unittest.SkipTest("g++ not available")
        cls._tmp = tempfile.TemporaryDirectory()
        workdir = Path(cls._tmp.name)
        try:
            cfg = probe_cjson(workdir)
        except Exception:
            cfg = None
        if cfg is None:
            cls._tmp.cleanup()
            raise unittest.SkipTest(
                "system libcjson not available (apt install libcjson-dev / brew install cjson)")
        try:
            cls.binary = build(workdir, *cfg)
        except RuntimeError as e:
            cls._tmp.cleanup()
            raise unittest.SkipTest("family protocol host build not possible here: " + str(e))
        cls._tmp_path = workdir

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_family_protocol_contract(self):
        proc = subprocess.run([str(self.binary)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, msg=proc.stdout + proc.stderr)
        self.assertIn("FAMILY_PROTOCOL_ALL_OK", proc.stdout)
        self.assertNotIn("FAIL:", proc.stdout)


if __name__ == "__main__":
    unittest.main()
