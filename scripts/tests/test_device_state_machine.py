"""L1 host test: DeviceStateMachine full transition matrix + observer behavior.

Compiles the real main/device_state_machine.cc (esp_log stubbed) and runs a C++
driver that pins the complete 12x12 transition matrix (the contract documented
in docs/architecture/05-startup-and-state-machine.md), observer notification
semantics, the FatalError absorbing state, and GetStateName.

`#define private public` is used in the driver for exactly two reasons: the
full matrix is expressed in the private IsValidTransition(), and FatalError is
unreachable through the public API (no legal transition targets it).
"""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MAIN_DIR = REPO_ROOT / "main"

STUB_ESP_LOG = """#pragma once
#define ESP_LOGI(...) do {} while (0)
#define ESP_LOGW(...) do {} while (0)
#define ESP_LOGE(...) do {} while (0)
#define ESP_LOGD(...) do {} while (0)
"""

DRIVER = r"""
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#define private public
#include "device_state_machine.h"
#undef private

static const std::vector<std::pair<DeviceState, DeviceState>> kLegalEdges = {
    {kDeviceStateUnknown, kDeviceStateStarting},
    {kDeviceStateStarting, kDeviceStateWifiConfiguring},
    {kDeviceStateStarting, kDeviceStateActivating},
    {kDeviceStateWifiConfiguring, kDeviceStateActivating},
    {kDeviceStateWifiConfiguring, kDeviceStateAudioTesting},
    {kDeviceStateAudioTesting, kDeviceStateWifiConfiguring},
    {kDeviceStateActivating, kDeviceStateUpgrading},
    {kDeviceStateActivating, kDeviceStateIdle},
    {kDeviceStateActivating, kDeviceStateWifiConfiguring},
    {kDeviceStateUpgrading, kDeviceStateIdle},
    {kDeviceStateUpgrading, kDeviceStateActivating},
    {kDeviceStateIdle, kDeviceStateConnecting},
    {kDeviceStateIdle, kDeviceStateListening},
    {kDeviceStateIdle, kDeviceStateSpeaking},
    {kDeviceStateIdle, kDeviceStateNotifying},
    {kDeviceStateIdle, kDeviceStateActivating},
    {kDeviceStateIdle, kDeviceStateUpgrading},
    {kDeviceStateIdle, kDeviceStateWifiConfiguring},
    {kDeviceStateConnecting, kDeviceStateIdle},
    {kDeviceStateConnecting, kDeviceStateListening},
    {kDeviceStateListening, kDeviceStateSpeaking},
    {kDeviceStateListening, kDeviceStateIdle},
    {kDeviceStateSpeaking, kDeviceStateListening},
    {kDeviceStateSpeaking, kDeviceStateIdle},
    {kDeviceStateNotifying, kDeviceStateIdle},
};

static const DeviceState kAllStates[] = {
    kDeviceStateUnknown, kDeviceStateStarting, kDeviceStateWifiConfiguring,
    kDeviceStateIdle, kDeviceStateConnecting, kDeviceStateListening,
    kDeviceStateSpeaking, kDeviceStateNotifying, kDeviceStateUpgrading,
    kDeviceStateActivating, kDeviceStateAudioTesting, kDeviceStateFatalError,
};
static const int kStateCount =
    static_cast<int>(sizeof(kAllStates) / sizeof(kAllStates[0]));

static bool Legal(DeviceState from, DeviceState to) {
    if (from == to) {
        return true;
    }
    for (const auto& e : kLegalEdges) {
        if (e.first == from && e.second == to) {
            return true;
        }
    }
    return false;
}

static int g_failures = 0;
static void CHECK(bool ok, const std::string& msg) {
    if (!ok) {
        printf("FAIL: %s\n", msg.c_str());
        ++g_failures;
    }
}

int main() {
    DeviceStateMachine sm;

    // 1. Full matrix: every (from, to) pair must match the pinned contract.
    for (int i = 0; i < kStateCount; ++i) {
        sm.current_state_.store(kAllStates[i]);
        for (int j = 0; j < kStateCount; ++j) {
            DeviceState from = kAllStates[i], to = kAllStates[j];
            bool actual = sm.IsValidTransition(from, to);
            bool expected = Legal(from, to);
            if (actual != expected) {
                CHECK(false,
                      std::string("matrix ") + DeviceStateMachine::GetStateName(from) +
                          " -> " + DeviceStateMachine::GetStateName(to) +
                          (actual ? " unexpectedly legal" : " unexpectedly illegal"));
            }
        }
    }
    printf("MATRIX_CHECKED=%d\n", kStateCount * kStateCount);

    // 2. Legal transition: state updated, listener called exactly once.
    {
        sm.current_state_.store(kDeviceStateUnknown);
        int calls = 0;
        DeviceState old_arg = kDeviceStateFatalError, new_arg = kDeviceStateFatalError;
        int id = sm.AddStateChangeListener([&](DeviceState o, DeviceState n) {
            ++calls;
            old_arg = o;
            new_arg = n;
        });
        CHECK(sm.TransitionTo(kDeviceStateStarting), "Unknown->Starting succeeds");
        CHECK(sm.GetState() == kDeviceStateStarting, "state updated by TransitionTo");
        CHECK(calls == 1, "listener invoked exactly once");
        CHECK(old_arg == kDeviceStateUnknown && new_arg == kDeviceStateStarting,
              "listener receives (old, new)");
        sm.RemoveStateChangeListener(id);
    }

    // 3. Illegal transition: rejected, state unchanged, no listener call.
    {
        sm.current_state_.store(kDeviceStateUnknown);
        int calls = 0;
        sm.AddStateChangeListener([&](DeviceState, DeviceState) { ++calls; });
        CHECK(!sm.TransitionTo(kDeviceStateIdle), "Unknown->Idle rejected");
        CHECK(sm.GetState() == kDeviceStateUnknown, "state unchanged after reject");
        CHECK(calls == 0, "no listener call on rejected transition");
    }

    // 4. Same-state no-op: returns true, no listener call.
    {
        sm.current_state_.store(kDeviceStateIdle);
        int calls = 0;
        sm.AddStateChangeListener([&](DeviceState, DeviceState) { ++calls; });
        CHECK(sm.TransitionTo(kDeviceStateIdle), "same-state returns true");
        CHECK(calls == 0, "no listener call on no-op");
    }

    // 5. FatalError absorbs everything (except its own no-op).
    {
        sm.current_state_.store(kDeviceStateFatalError);
        for (int j = 0; j < kStateCount; ++j) {
            DeviceState to = kAllStates[j];
            if (to == kDeviceStateFatalError) {
                CHECK(sm.TransitionTo(to), "FatalError self no-op returns true");
            } else {
                CHECK(!sm.TransitionTo(to), "FatalError must not transition out");
            }
        }
        CHECK(sm.GetState() == kDeviceStateFatalError, "still in FatalError");
    }

    // 6. GetStateName coverage.
    CHECK(std::string(DeviceStateMachine::GetStateName(kDeviceStateUnknown)) == "unknown",
          "name(unknown)");
    CHECK(std::string(DeviceStateMachine::GetStateName(kDeviceStateFatalError)) == "fatal_error",
          "name(fatal_error)");
    CHECK(std::string(DeviceStateMachine::GetStateName(static_cast<DeviceState>(99))) ==
              "invalid_state",
          "name(out of range)");

    if (g_failures == 0) {
        printf("STATE_MACHINE_ALL_OK\n");
        return 0;
    }
    printf("STATE_MACHINE_FAILURES=%d\n", g_failures);
    return 1;
}
"""


def gpp() -> str:
    path = shutil.which("g++") or shutil.which("c++")
    return path or ""


def build(workdir: Path) -> Path:
    stub = workdir / "stub"
    stub.mkdir(parents=True)
    (stub / "esp_log.h").write_text(STUB_ESP_LOG, encoding="utf-8")
    driver = workdir / "driver.cpp"
    driver.write_text(DRIVER, encoding="utf-8")
    binary = workdir / "state_machine_test"
    cmd = [
        gpp(), "-std=c++20", "-pthread",
        f"-I{stub}", f"-I{MAIN_DIR}",
        str(driver), str(MAIN_DIR / "device_state_machine.cc"),
        "-o", str(binary),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError("compile failed:\n" + proc.stderr)
    return binary


class StateMachineHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not gpp():
            raise unittest.SkipTest("g++ not available")
        cls._tmp = tempfile.TemporaryDirectory()
        try:
            cls.binary = build(Path(cls._tmp.name))
        except RuntimeError as e:
            cls._tmp.cleanup()
            msg = str(e)
            if "std=c++20" in msg or "unrecognized" in msg:
                raise unittest.SkipTest("toolchain lacks C++20 support")
            raise
        cls._tmp_path = Path(cls._tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def run_driver(self):
        return subprocess.run([str(self.binary)], capture_output=True, text=True)

    def test_full_matrix_and_observations(self):
        proc = self.run_driver()
        self.assertEqual(proc.returncode, 0, msg=proc.stdout + proc.stderr)
        self.assertIn("STATE_MACHINE_ALL_OK", proc.stdout)
        self.assertNotIn("FAIL:", proc.stdout)

    def test_matrix_covers_12x12(self):
        proc = self.run_driver()
        self.assertIn("MATRIX_CHECKED=144", proc.stdout)


if __name__ == "__main__":
    unittest.main()
