"""L1 host test: Protocol base-class message builders (main/protocols/protocol.cc).

Compiles the real protocol.cc on the host with a stub esp_log.h and a stub
assets.h (AddTextFontCapabilities is compiled but not exercised), then drives a
concrete subclass whose SendText captures outgoing messages. Every message is
parsed back with cJSON and field-asserted.

Requires system libcjson; when absent (e.g. espressif/idf CI container) the
test skips with a clear reason. Include/link candidates cover Debian and
Homebrew layouts.

Pinned contract: docs/architecture/07-communication-protocol.md §3 / §4.
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

class TestProtocol : public Protocol {
public:
    std::vector<std::string> sent;

    bool SendText(const std::string& text) override { sent.push_back(text); return true; }
    bool Start() override { return true; }
    bool OpenAudioChannel() override { return true; }
    void CloseAudioChannel(bool send_goodbye) override { (void)send_goodbye; }
    bool IsAudioChannelOpened() const override { return false; }
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override {
        (void)packet;
        return true;
    }
    void set_session(const std::string& s) { session_id_ = s; }
    void emit_error(const std::string& m) { SetError(m); }
    void emit_error(const std::string& m, const std::string& d) { SetError(m, d); }
};

static std::string expect_valid_json(TestProtocol& p, const std::string& what) {
    cJSON* root = cJSON_Parse(p.sent.back().c_str());
    if (root == NULL) {
        CHECK(false, "valid json (" + what + "): " + p.sent.back());
        return "";
    }
    std::string session;
    cJSON* sid = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(sid)) session = sid->valuestring;
    cJSON_Delete(root);
    CHECK(session == p.session_id(), "session_id present in " + what);
    return p.sent.back();
}

int main() {
    TestProtocol p;
    p.set_session("sess-1");

    // SendStartListening: three modes
    p.sent.clear();
    p.SendStartListening(kListeningModeAutoStop);
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        cJSON* type = root ? cJSON_GetObjectItem(root, "type") : NULL;
        cJSON* state = root ? cJSON_GetObjectItem(root, "state") : NULL;
        cJSON* mode = root ? cJSON_GetObjectItem(root, "mode") : NULL;
        CHECK(root != NULL, "start(auto) valid json");
        CHECK(cJSON_IsString(type) && std::string(type->valuestring) == "listen", "start(auto) type");
        CHECK(cJSON_IsString(state) && std::string(state->valuestring) == "start", "start(auto) state");
        CHECK(cJSON_IsString(mode) && std::string(mode->valuestring) == "auto", "start(auto) mode");
        cJSON_Delete(root);
        expect_valid_json(p, "start(auto)");
    }
    p.SendStartListening(kListeningModeManualStop);
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        cJSON* mode = root ? cJSON_GetObjectItem(root, "mode") : NULL;
        CHECK(root != NULL, "start(manual) valid json");
        CHECK(cJSON_IsString(mode) && std::string(mode->valuestring) == "manual", "start(manual) mode");
        cJSON_Delete(root);
    }
    p.SendStartListening(kListeningModeRealtime);
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        cJSON* mode = root ? cJSON_GetObjectItem(root, "mode") : NULL;
        CHECK(root != NULL, "start(realtime) valid json");
        CHECK(cJSON_IsString(mode) && std::string(mode->valuestring) == "realtime", "start(realtime) mode");
        cJSON_Delete(root);
    }

    // SendStopListening
    p.sent.clear();
    p.SendStopListening();
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        cJSON* state = root ? cJSON_GetObjectItem(root, "state") : NULL;
        CHECK(root != NULL, "stop valid json");
        CHECK(cJSON_IsString(state) && std::string(state->valuestring) == "stop", "stop state");
        cJSON_Delete(root);
    }

    // SendAbortSpeaking: with and without reason
    p.sent.clear();
    p.SendAbortSpeaking(kAbortReasonNone);
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        cJSON* type = root ? cJSON_GetObjectItem(root, "type") : NULL;
        cJSON* reason = root ? cJSON_GetObjectItem(root, "reason") : NULL;
        CHECK(root != NULL, "abort none valid json");
        CHECK(cJSON_IsString(type) && std::string(type->valuestring) == "abort", "abort type");
        CHECK(reason == NULL, "abort none has no reason");
        cJSON_Delete(root);
    }
    p.SendAbortSpeaking(kAbortReasonWakeWordDetected);
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        cJSON* reason = root ? cJSON_GetObjectItem(root, "reason") : NULL;
        CHECK(root != NULL, "abort wake valid json");
        CHECK(cJSON_IsString(reason) &&
                  std::string(reason->valuestring) == "wake_word_detected",
              "abort wake reason");
        cJSON_Delete(root);
    }

    // SendWakeWordDetected: UTF-8 payload round-trips
    p.sent.clear();
    p.SendWakeWordDetected("nihaoxiaozhi");
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        cJSON* text = root ? cJSON_GetObjectItem(root, "text") : NULL;
        CHECK(root != NULL, "detect valid json");
        CHECK(cJSON_IsString(text) && std::string(text->valuestring) == "nihaoxiaozhi",
              "detect text");
        cJSON_Delete(root);
    }
    // Pinned current behavior: wake word text is naive-concatenated, so a
    // double quote produces invalid JSON (documented caveat, not asserted
    // as desired behavior).
    p.SendWakeWordDetected("a\"b");
    CHECK(cJSON_Parse(p.sent.back().c_str()) == NULL,
          "quote in wake word breaks JSON (current behavior pinned)");

    // SendMcpMessage: embedded payload parses back
    p.sent.clear();
    p.SendMcpMessage(R"({"a":1})");
    {
        cJSON* root = cJSON_Parse(p.sent.back().c_str());
        cJSON* payload = root ? cJSON_GetObjectItem(root, "payload") : NULL;
        cJSON* a = payload ? cJSON_GetObjectItem(payload, "a") : NULL;
        CHECK(root != NULL, "mcp valid json");
        CHECK(cJSON_IsObject(payload), "mcp payload object");
        CHECK(cJSON_IsNumber(a) && a->valueint == 1, "mcp payload.a == 1");
        cJSON_Delete(root);
    }

    // SetError: single message and message+detail routing
    std::string error_received;
    p.OnNetworkError([&](const std::string& m) { error_received = m; });
    p.emit_error("boom");
    CHECK(error_received == "boom", "error routed to callback");
    p.emit_error("boom", "detail");
    CHECK(error_received == "boom\ndetail", "error + detail joined with newline");

    if (g_failures == 0) {
        printf("PROTOCOL_MESSAGES_ALL_OK\n");
        return 0;
    }
    printf("PROTOCOL_MESSAGES_FAILURES=%d\n", g_failures);
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
    binary = workdir / "protocol_messages_test"
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


class ProtocolMessagesHostTests(unittest.TestCase):
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
            raise unittest.SkipTest("protocol host build not possible here: " + str(e))
        cls._tmp_path = workdir

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_message_contract(self):
        proc = subprocess.run([str(self.binary)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, msg=proc.stdout + proc.stderr)
        self.assertIn("PROTOCOL_MESSAGES_ALL_OK", proc.stdout)
        self.assertNotIn("FAIL:", proc.stdout)


if __name__ == "__main__":
    unittest.main()
