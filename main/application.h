#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>
#include <atomic>
#include <cstdint>
#include <vector>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "button.h"
#include "notify/notify_player.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED     (1 << 13)
#define MAIN_EVENT_STARTUP_HINT_TIMEOUT (1 << 14)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

// Family protocol (FAMILY-PROTOCOL v1): voice messages to family members,
// periodic location reports and SOS alarms. See
// wawabot-api/app/wawabot/docs/FAMILY-PROTOCOL.md §1.
enum class FamilyLocationSource {
    kGps,
    kWifi,
    kLbs,
};

struct FamilyLocationFix {
    bool valid = false;  // false = no fix available, nothing is reported
    double lat = 0.0;
    double lng = 0.0;
    FamilyLocationSource source = FamilyLocationSource::kLbs;
    double accuracy_m = 0.0;
    int battery_pct = -1;  // -1 = unknown, falls back to the board battery level
};

// Injectable location source. Boards without positioning hardware simply do
// not register a provider, in which case no location report is ever sent.
using FamilyLocationProvider = std::function<FamilyLocationFix()>;

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

    /**
     * Family protocol (FAMILY-PROTOCOL v1), all thread-safe.
     * StartFamilyMessageRecording / StopFamilyMessageRecording toggle the
     * voice-message recording (encoded opus frames are buffered locally
     * instead of being sent to the conversation pipeline, capped by
     * CONFIG_FAMILY_MESSAGE_MAX_DURATION_MS with automatic stop + send).
     * SendFamilySos fires the SOS uplink (trigger "button").
     * SetFamilyLocationProvider injects the positioning source; without one
     * the periodic timer produces no report.
     */
    void StartFamilyMessageRecording();
    void StopFamilyMessageRecording();
    bool IsFamilyRecordingMessage() const { return family_recording_; }
    void SendFamilySos(const char* trigger);
    void SetFamilyLocationProvider(FamilyLocationProvider provider) {
        family_location_provider_ = std::move(provider);
    }

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    esp_timer_handle_t startup_wake_word_hint_timer_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    std::string startup_wake_word_hint_message_;
    AudioService audio_service_;
    NotifyPlayer notify_player_;
    uint32_t notification_playback_id_ = 0;
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    // Family protocol (FAMILY-PROTOCOL v1)
    std::atomic<bool> family_recording_{false};
    std::vector<uint8_t> family_record_buffer_;
    int family_record_frames_ = 0;
    std::string family_pending_played_message_id_;
    std::unique_ptr<Button> family_sos_button_;
    esp_timer_handle_t family_location_timer_ = nullptr;
    FamilyLocationProvider family_location_provider_;

    bool has_server_time_ = false;
    bool startup_completed_ = false;
    bool pending_startup_wake_word_hint_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    bool pending_listening_start_ = false;  // Waiting for playback to drain before starting listening (auto mode)
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void ConfigureWakeWordForListening();
    void ShowStartupWakeWordHint();
    void HideStartupWakeWordHint();
    void StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles);
    void StopNotification();
    void HandleNotificationFinished(uint32_t playback_id, bool success);

    // Activation task (runs in background)
    void ActivationTask();

    // Family protocol (FAMILY-PROTOCOL v1)
    void InitializeFamilyFeatures();
    void StartFamilyMessageRecordingInternal();
    void StopFamilyMessageRecordingInternal(bool send);
    void HandleFamilyJson(const cJSON* root);
    void HandleFamilyMessageArrived(std::string message_id, std::string audio_url,
                                    std::string sender_name);
    void HandleFamilySosAcked(std::string sos_event_id, std::string acked_by_name);
    void HandleFamilyAlarmFire(std::string alarm_id, std::string label, std::string time);
    void HandleFamilyCallIncoming(std::string call_id);
    void SendFamilyLocationReport();
    static std::string GenerateFamilyMessageId();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void ShowPairingQr(Display* display);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
