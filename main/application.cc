#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "cjson_utils.h"
#include "display.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "websocket_protocol.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

#define TAG "Application"

Application::Application() : notify_player_(audio_service_) {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    notify_player_.Stop();
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (startup_wake_word_hint_timer_ != nullptr) {
        esp_timer_stop(startup_wake_word_hint_timer_);
        esp_timer_delete(startup_wake_word_hint_timer_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();
    ESP_LOGI(TAG, "After board/audio init");
    SystemInfo::PrintHeapStats();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    callbacks.on_playback_progress = [this](uint32_t playback_id, uint32_t media_position_ms) {
        notify_player_.OnPlaybackProgress(playback_id, media_position_ms);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();
    // Registration is complete here, so this is the only place the whole inventory exists.
    // It is logged because a server that accepts a bounded number of tools drops the rest
    // quietly: the only sign on this device was an alert at boot and a sad face, which is
    // easy to see and hard to connect to a tool list.
    mcp_server.LogToolInventory();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Family protocol: optional SOS button (Kconfig GPIO) and the periodic
    // location report timer (no-op until a location provider is injected)
    InitializeFamilyFeatures();
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED | MAIN_EVENT_STARTUP_HINT_TIMEOUT;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
            }
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            if (audio_service_.IsPlaybackIdle()) {
                notify_player_.OnPlaybackDrained();
            }
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (family_recording_) {
                    // Family voice message recording: accumulate the encoded
                    // opus frames locally instead of the conversation uplink.
                    family_record_buffer_.insert(family_record_buffer_.end(),
                                                 packet->payload.begin(), packet->payload.end());
                    family_record_frames_++;
                    if (family_record_frames_ * OPUS_FRAME_DURATION_MS >=
                        CONFIG_FAMILY_MESSAGE_MAX_DURATION_MS) {
                        ESP_LOGW(TAG, "Family message reached the %d ms limit, stopping recording",
                                 CONFIG_FAMILY_MESSAGE_MAX_DURATION_MS);
                        StopFamilyMessageRecordingInternal(true);
                        break;
                    }
                    continue;
                }
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    // Drop the remaining packets. Leaving them in the queue would
                    // stall the Opus codec task (it waits for queue space), which in
                    // turn deadlocks the whole audio input pipeline, as no new
                    // MAIN_EVENT_SEND_AUDIO event would ever be triggered again.
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
        if (bits & MAIN_EVENT_STARTUP_HINT_TIMEOUT) {
            HideStartupWakeWordHint();
        }
        ShowStartupWakeWordHint();
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateNotifying) {
        StopNotification();
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    // The pairing QR is only meaningful while waiting for activation.
    Board::GetInstance().GetDisplay()->HideQrCode();

    has_server_time_ = ota_->HasServerTime();

    // Protocol start may have already raised MAIN_EVENT_ERROR. Do not replace
    // that alert with the "ready" UI/sound — the main loop can process both
    // events back-to-back because the activation task is lower priority.
    const bool has_error = !last_error_message_.empty();
    if (!startup_completed_) {
        startup_completed_ = true;
        pending_startup_wake_word_hint_ = !has_error;
    }
    if (!has_error) {
        auto display = Board::GetInstance().GetDisplay();
        std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
    }

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    if (!has_error) {
        Schedule([this]() {
            // Play the success sound to indicate the device is ready
            audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
        });
    }
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    // "robot_2" is declared only by a board shipping its own pack: esp-vocat maps it to
    // neutral.eaf, and the rgb-matrix display special-cases the string. None of the shared
    // esp_emote_assets packs (320_240, 360_360, 1024_600) declares it, so on every board that
    // uses one this call fails, logs two errors and leaves whatever face was already up.
    // Fall back to an emotion all three packs do declare.
    if (!display->SetEmotion("robot_2")) {
        display->SetEmotion("neutral");
    }
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;  // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        auto check = ota_->CheckVersion();
        if (!check) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            const auto& err = check.error();
            char error_message[160];
            int error_message_length =
                snprintf(error_message, sizeof(error_message), "%s", err.ToString().c_str());
            if (error_message_length < 0 ||
                error_message_length >= static_cast<int>(sizeof(error_message))) {
                snprintf(error_message, sizeof(error_message), "%s", err.Message());
            }

            char buffer[320];
            int alert_message_length =
                snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED,
                         retry_delay, error_message);
            if (alert_message_length < 0 ||
                alert_message_length >= static_cast<int>(sizeof(buffer))) {
                snprintf(buffer, sizeof(buffer), "%s", err.Message());
            }
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10;  // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }
        // Render the device UID as a QR code so the app can bind by scanning
        // (content: "device_uid[,device_type]" per the consumer QR contract).
        ShowPairingQr(display);

        // Stop polling once the server-side activation window (timeout_ms)
        // has elapsed; the outer loop re-fetches a fresh code and QR.
        auto activation_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(ota_->GetActivationTimeoutMs());

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
            if (std::chrono::steady_clock::now() > activation_deadline) {
                ESP_LOGW(TAG, "Activation window elapsed, refetching code");
                display->SetChatMessage("system", Lang::Strings::ACTIVATION_EXPIRED);
                break;
            }
        }
        // Leave the activation screen no matter why the polling ended; the outer
        // loop re-renders a fresh code and QR if another round is needed.
        display->HideQrCode();
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() { DismissAlert(); });

    // Family protocol downlink (FAMILY-PROTOCOL v1 §1.2). Runs on the
    // transport receive task; HandleFamilyJson parses and schedules.
    protocol_->OnFamilyMessage([this](const cJSON* root) { HandleFamilyJson(root); });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, "
                     "resampling may cause distortion",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
        // Family device call (spec §5): an announcement that arrived while the
        // channel was still opening goes out now. Runs in the main task.
        if (!family_call_pending_announcement_.empty()) {
            protocol_->SendWakeWordDetected(family_call_pending_announcement_);
            family_call_pending_announcement_.clear();
        }
    });

    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            // Any channel close ends the family call window: the peer's hang
            // up arrives as goodbye, network loss closes here. The SOS
            // fallback and hang-up gesture clear the state themselves before
            // closing, so this is the passive path only.
            ClearFamilyCallState();
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        if (strcmp(type->valuestring, "notify") == 0) {
            auto audio_url = cJSON_GetObjectItem(root, "audio_url");
            if (!cJSON_IsString(audio_url) || audio_url->valuestring[0] == '\0') {
                ESP_LOGW(TAG, "Notify message requires audio_url");
                return;
            }

            std::vector<NotifySubtitle> subtitles;
            auto subtitles_json = cJSON_GetObjectItem(root, "subtitles");
            if (subtitles_json != nullptr && !cJSON_IsArray(subtitles_json)) {
                ESP_LOGW(TAG, "Notify subtitles must be an array");
                return;
            }
            if (cJSON_IsArray(subtitles_json)) {
                cJSON* item = nullptr;
                cJSON_ArrayForEach (item, subtitles_json) {
                    auto start_ms = cJSON_GetObjectItem(item, "start_ms");
                    auto text = cJSON_GetObjectItem(item, "text");
                    if (!cJSON_IsNumber(start_ms) || start_ms->valuedouble < 0 ||
                        start_ms->valuedouble > std::numeric_limits<uint32_t>::max() ||
                        !cJSON_IsString(text)) {
                        ESP_LOGW(TAG, "Ignoring invalid notify subtitle");
                        continue;
                    }
                    subtitles.push_back({.start_ms = static_cast<uint32_t>(start_ms->valuedouble),
                                         .text = text->valuestring});
                }
            }

            Schedule([this, url = std::string(audio_url->valuestring),
                      subtitles = std::move(subtitles)]() mutable {
                StartNotification(std::move(url), std::move(subtitles));
            });
        } else if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring),
                              glyphs = std::move(glyphs), bpp]() {
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring),
                          glyphs = std::move(glyphs), bpp]() {
                    // Family call (spec §5): the [device_call] announcement
                    // also comes back as an stt display message; only a
                    // different utterance means the user answered or rejected
                    // by voice, which closes the ring wait (the server's
                    // call_device flow takes it from there).
                    if (family_call_state_ == FamilyCallState::kIncoming &&
                        message != family_call_announcement_) {
                        ESP_LOGI(TAG, "Family call %s: user replied, ring wait closed",
                                 family_call_id_.c_str());
                        StopFamilyCallRingTimer();
                        family_call_state_ = FamilyCallState::kActive;
                    }
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            CJsonStringUniquePtr root_json(cJSON_PrintUnformatted(root));
            ESP_LOGI(TAG, "Received custom message: %s", root_json ? root_json.get() : "");
            if (cJSON_IsObject(payload)) {
                CJsonStringUniquePtr payload_json(cJSON_PrintUnformatted(payload));
                if (payload_json) {
                    Schedule(
                        [this, display, payload_str = std::string(payload_json.get())]() {
                            display->SetChatMessage("system", payload_str.c_str());
                        });
                }
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else if (strcmp(type->valuestring, "family") == 0) {
            // FAMILY-PROTOCOL v1: dispatch by "action" to the family handler
            protocol_->DispatchFamilyMessage(root);
        } else if (strcmp(type->valuestring, "caption") == 0) {
            // 同传字幕（wawabot 服务端扩展；仅 hello 声明 features.caption 的
            // 设备会收到）。有译文显示译文、原文换行附后；翻译失败只有原文。
            // final=false 为中间态，同一句段覆盖式刷新——固件不留字幕历史
            // （ESP 内存约束，滚动由服务端 seq 定稿流保证最终一致）。
            auto src = cJSON_GetObjectItem(root, "source");
            auto dst = cJSON_GetObjectItem(root, "target");
            const char* source = cJSON_IsString(src) ? src->valuestring : "";
            const char* target = cJSON_IsString(dst) ? dst->valuestring : "";
            if (source[0] == '\0' && target[0] == '\0') {
                ESP_LOGW(TAG, "Caption message without text");
            } else if (display != nullptr) {
                std::string text = target[0] != '\0' ? target : source;
                if (target[0] != '\0' && source[0] != '\0') {
                    text += "\n";
                    text += source;
                }
                Schedule([this, display, text]() {
                    display->SetChatMessage("system", text.c_str());
                });
            }
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::ShowPairingQr(Display* display) {
    if (display == nullptr) {
        return;
    }
    // Consumer QR contract (parseConsumerQrScene): "device_uid[,device_type][,expire_at]".
    // device_uid must match the Device-Id OTA header verbatim — it is the key the
    // server addresses the device by. expire_at is omitted (0 = no expiry, the
    // server has the final say). On small monochrome screens the type field is
    // dropped to keep the QR version (and module size) minimal.
    std::string content = SystemInfo::GetMacAddress();
    if (!display->IsMonochrome()) {
        content += ",";
        content += BOARD_NAME;
    }
    if (!display->ShowQrCode(content)) {
        ESP_LOGW(TAG, "Pairing QR not rendered (unsupported display)");
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    last_error_message_.clear();
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

void Application::HandleToggleChatEvent() {
    // A press while recording a family message stops and sends it
    if (family_recording_) {
        StopFamilyMessageRecordingInternal(true);
        return;
    }

    // Family call hang-up gesture (device-call spec §5 / adjudication B1):
    // a short press of the chat button while a family call phase is active
    // rejects the ring wait or hangs up the call. The SOS long press lives on
    // the dedicated SOS button (CONFIG_FAMILY_SOS_GPIO) and is untouched by
    // this branch.
    if (family_call_state_ != FamilyCallState::kIdle) {
        bool was_incoming = family_call_state_ == FamilyCallState::kIncoming;
        ESP_LOGI(TAG, "Family call %s ended by hang-up gesture",
                 family_call_id_.c_str());
        EndFamilyCall(was_incoming ? "rejected" : nullptr);
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    // A press while recording a family message stops and sends it
    if (family_recording_) {
        StopFamilyMessageRecordingInternal(true);
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    // Wake word detection is disabled while recording a family message;
    // ignore a late event instead of interrupting the recording.
    if (family_recording_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateNotifying) {
        StopNotification();
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    // Must run in the main task with the device in idle state
    audio_service_.EncodeWakeWord();

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error), and
            // wake word detection is re-enabled by the idle state handler.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // Keep a just-raised network error visible. SetDeviceState(idle)
            // queues STATE_CHANGED after Alert(), and the idle handler would
            // otherwise wipe the status, emotion, and chat message.
            if (last_error_message_.empty()) {
                display->SetStatus(Lang::Strings::STANDBY);
                display->ClearChatMessages();  // Clear messages first
                display->SetEmotion(
                    "neutral");  // Then set emotion (wechat mode checks child count)
            }
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for the playback queue to drain before enabling
                // voice processing. This prevents audio truncation when STOP arrives
                // late due to network jitter. Instead of blocking the main loop here,
                // defer the start until MAIN_EVENT_PLAYBACK_DRAINED arrives.
                if (listening_mode_ == kListeningModeAutoStop && !audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateNotifying:
            display->SetStatus(Lang::Strings::SPEAKING);
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::ShowStartupWakeWordHint() {
    // A coalesced activation/state event can queue another idle UI cleanup.
    // Wait for it before adding the hint, or ClearChatMessages() would erase it.
    if (!pending_startup_wake_word_hint_ ||
        (xEventGroupGetBits(event_group_) & MAIN_EVENT_STATE_CHANGED)) {
        return;
    }
    pending_startup_wake_word_hint_ = false;
    const auto state = GetDeviceState();
    const bool wake_word_running = audio_service_.IsWakeWordRunning();
    if (state != kDeviceStateIdle || !last_error_message_.empty() || !wake_word_running) {
        ESP_LOGI(TAG, "Startup wake word hint skipped (state=%d, wake word running=%d, error=%d)",
                 static_cast<int>(state), wake_word_running, !last_error_message_.empty());
        return;
    }

    const auto wake_words = audio_service_.GetWakeWords();
    std::string words;
    for (auto it = wake_words.begin(); it != wake_words.end(); ++it) {
        if (it->empty() || std::find(wake_words.begin(), it, *it) != it) {
            continue;
        }
        if (!words.empty()) {
            words += " / ";
        }
        words += *it;
    }
    if (words.empty()) {
        ESP_LOGW(TAG, "Startup wake word hint skipped: no available wake words");
        return;
    }
    std::string message = Lang::Strings::WAKE_WORD_HINT;
    const auto placeholder = message.find("%s");
    if (placeholder == std::string::npos) {
        ESP_LOGE(TAG, "Startup wake word hint skipped: invalid message template");
        return;
    }
    message.replace(placeholder, 2, words);
    if (startup_wake_word_hint_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    auto app = static_cast<Application*>(arg);
                    xEventGroupSetBits(app->event_group_, MAIN_EVENT_STARTUP_HINT_TIMEOUT);
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "startup_hint",
            .skip_unhandled_events = false,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &startup_wake_word_hint_timer_));
    }
    ESP_LOGI(TAG, "Startup wake word hint: %s", message.c_str());
    Board::GetInstance().GetDisplay()->SetChatMessage("assistant", message.c_str());
    startup_wake_word_hint_message_ = std::move(message);
    ESP_ERROR_CHECK(esp_timer_start_once(startup_wake_word_hint_timer_, 5000 * 1000));
}

void Application::HideStartupWakeWordHint() {
    if (startup_wake_word_hint_message_.empty()) {
        return;
    }
    // A conversation or alert may have replaced the hint while its timer was running.
    Board::GetInstance().GetDisplay()->RemoveChatMessage("assistant",
                                                         startup_wake_word_hint_message_.c_str());
    startup_wake_word_hint_message_.clear();
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

    // Send the start listening command
    protocol_->SendStartListening(listening_mode_);
    audio_service_.EnableVoiceProcessing(true);

    ConfigureWakeWordForListening();

    // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles) {
    if (GetDeviceState() != kDeviceStateIdle || notify_player_.IsBusy()) {
        ESP_LOGW(TAG, "Ignoring notify message while device is busy");
        return;
    }

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
    audio_service_.ReleaseWakeWordResources();
    while (audio_service_.PopPacketFromSendQueue()) {
        // Discard microphone audio left over from a previous conversation.
    }

    if (!SetDeviceState(kDeviceStateNotifying)) {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return;
    }

    audio_service_.ResetDecoder();
    uint32_t playback_id = ++notification_playback_id_;
    if (playback_id == 0) {
        playback_id = ++notification_playback_id_;
    }
    audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);

    bool started = notify_player_.Start(
        std::move(audio_url), std::move(subtitles), playback_id,
        [this](uint32_t id, const std::string& text) {
            Schedule([this, id, text]() {
                if (GetDeviceState() == kDeviceStateNotifying && notification_playback_id_ == id) {
                    Board::GetInstance().GetDisplay()->SetChatMessage("assistant", text.c_str());
                }
            });
        },
        [this](uint32_t id, bool success) {
            Schedule([this, id, success]() { HandleNotificationFinished(id, success); });
        });

    if (!started) {
        ESP_LOGE(TAG, "Failed to start notification playback");
        StopNotification();
    }
}

void Application::StopNotification() {
    notify_player_.Stop();
    audio_service_.ResetDecoder();
    family_pending_played_message_id_.clear();
    auto& board = Board::GetInstance();
    board.GetDisplay()->SetChatMessage("assistant", "");
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (GetDeviceState() == kDeviceStateNotifying) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleNotificationFinished(uint32_t playback_id, bool success) {
    if (GetDeviceState() != kDeviceStateNotifying || notification_playback_id_ != playback_id) {
        return;
    }
    ESP_LOGI(TAG, "Notification playback %lu %s", static_cast<unsigned long>(playback_id),
             success ? "completed" : "failed");
    // Family message played receipt (FAMILY-PROTOCOL v1 §1.3): only reported
    // after the audio_url playback completed successfully.
    std::string played_message_id = family_pending_played_message_id_;
    StopNotification();
    if (success && !played_message_id.empty() && protocol_) {
        protocol_->SendFamilyMessagePlayed(played_message_id);
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // May be called from outside the main task (e.g. board button
        // callbacks), so schedule the invocation instead of running it here
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateNotifying) {
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::ResetProtocol() {
    Schedule([this]() {
        if (GetDeviceState() == kDeviceStateNotifying) {
            StopNotification();
        }
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

// ---------------------------------------------------------------------------
// Family protocol (FAMILY-PROTOCOL v1)
// ---------------------------------------------------------------------------

void Application::InitializeFamilyFeatures() {
#if CONFIG_FAMILY_SOS_GPIO >= 0
    family_sos_button_ = std::make_unique<Button>(
        static_cast<gpio_num_t>(CONFIG_FAMILY_SOS_GPIO), CONFIG_FAMILY_SOS_ACTIVE_HIGH,
        CONFIG_FAMILY_SOS_LONG_PRESS_MS);
    family_sos_button_->OnLongPress([this]() { SendFamilySos("button"); });
    ESP_LOGI(TAG, "Family SOS button on GPIO %d", CONFIG_FAMILY_SOS_GPIO);
#endif

    // Periodic location report. Without an injected provider (see
    // SetFamilyLocationProvider) every tick is a no-op.
    esp_timer_create_args_t timer_args = {
        .callback =
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->Schedule([app]() { app->SendFamilyLocationReport(); });
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "family_loc",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &family_location_timer_) == ESP_OK) {
        esp_timer_start_periodic(family_location_timer_,
                                 static_cast<uint64_t>(CONFIG_FAMILY_LOCATION_REPORT_INTERVAL_MS) *
                                     1000ULL);
    } else {
        ESP_LOGE(TAG, "Failed to create family location timer");
    }
}

void Application::StartFamilyMessageRecording() {
    // Thread-safe: the actual start runs in the main task like other
    // application mutations.
    Schedule([this]() { StartFamilyMessageRecordingInternal(); });
}

void Application::StopFamilyMessageRecording() {
    Schedule([this]() { StopFamilyMessageRecordingInternal(true); });
}

void Application::StartFamilyMessageRecordingInternal() {
    if (family_recording_) {
        ESP_LOGW(TAG, "Family message recording already in progress");
        return;
    }
    // Mutually exclusive with wake word / conversation / notifications:
    // recording is only allowed from idle, wake word detection is turned off
    // and button events stop the recording instead of starting a chat.
    if (GetDeviceState() != kDeviceStateIdle) {
        ESP_LOGW(TAG, "Cannot record a family message in state %d",
                 static_cast<int>(GetDeviceState()));
        return;
    }

    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    family_record_buffer_.clear();
    // Rough opus size estimate (~16 kbps at 16 kHz mono) to limit reallocations
    family_record_buffer_.reserve((CONFIG_FAMILY_MESSAGE_MAX_DURATION_MS / 1000) * 2048);
    family_record_frames_ = 0;
    family_recording_ = true;
    audio_service_.EnableWakeWordDetection(false);
    // Reuse the regular input path: mic -> AFE -> opus encoder -> send queue.
    // The main loop intercepts the send queue while family_recording_ is set.
    audio_service_.EnableVoiceProcessing(true);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus("Recording");
    display->SetChatMessage("system", "Recording voice message");
}

void Application::StopFamilyMessageRecordingInternal(bool send) {
    if (!family_recording_) {
        return;
    }
    family_recording_ = false;
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(true);
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    // Collect the last frames still sitting in the send queue, then discard
    // anything beyond the duration cap.
    const int max_frames = CONFIG_FAMILY_MESSAGE_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS;
    while (family_record_frames_ < max_frames) {
        auto packet = audio_service_.PopPacketFromSendQueue();
        if (packet == nullptr) {
            break;
        }
        family_record_buffer_.insert(family_record_buffer_.end(), packet->payload.begin(),
                                     packet->payload.end());
        family_record_frames_++;
    }
    while (audio_service_.PopPacketFromSendQueue())
        ;

    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::STANDBY);
    display->SetChatMessage("system", "");

    const int duration_ms = family_record_frames_ * OPUS_FRAME_DURATION_MS;
    family_record_frames_ = 0;
    if (!send || family_record_buffer_.empty() || duration_ms <= 0) {
        ESP_LOGW(TAG, "Family message recording discarded (empty)");
        family_record_buffer_.clear();
        return;
    }
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "Protocol not initialized, family message dropped");
        family_record_buffer_.clear();
        return;
    }

    // Base64 encode the accumulated opus stream (same helper pattern as
    // mcp_server.h).
    size_t dlen = 0;
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &dlen, family_record_buffer_.data(),
                          family_record_buffer_.size());
    std::string audio_b64(dlen, '\0');
    int rc = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(audio_b64.data()),
                                   audio_b64.size(), &olen, family_record_buffer_.data(),
                                   family_record_buffer_.size());
    family_record_buffer_.clear();
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to base64 encode family message, rc=%d", rc);
        return;
    }
    audio_b64.resize(olen);

    auto message_id = GenerateFamilyMessageId();
    if (protocol_->SendFamilyVoiceMessage(message_id, audio_b64, duration_ms)) {
        ESP_LOGI(TAG, "Family message %s sent (%d ms)", message_id.c_str(), duration_ms);
    } else {
        // WebSocket transports only accept text frames while the audio channel
        // is open; MQTT accepts them whenever the broker is connected.
        ESP_LOGE(TAG, "Failed to send family message %s (channel closed?)", message_id.c_str());
        audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
    }
}

void Application::SendFamilySos(const char* trigger) {
    // May be called from the button callback task; schedule the actual send.
    Schedule([this, trigger = std::string(trigger != nullptr ? trigger : "button")]() {
        if (protocol_ == nullptr) {
            ESP_LOGE(TAG, "Cannot send SOS: protocol not initialized");
            return;
        }
        // Adjudication B1 (device-call spec §5): SOS delivery wins over call
        // continuation. In gateway call mode the caller side loses its bridge
        // (app.js:1148) and the gateway drops control uplinks without one
        // (app.js:529-536), so the SOS is sent only after the call is torn
        // down. Applied uniformly on both ends (callee / caller) so the
        // fallback does not depend on which side of the bridge we are on.
        if (family_call_state_ != FamilyCallState::kIdle) {
            TearDownFamilyCallForSos();
        }
        auto message_id = GenerateFamilyMessageId();
        if (protocol_->SendFamilySos(message_id, trigger.c_str())) {
            ESP_LOGI(TAG, "SOS sent (message_id=%s)", message_id.c_str());
            audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
        } else {
            ESP_LOGE(TAG, "Failed to send SOS (channel closed?)");
            audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        }
    });
}

void Application::SendFamilyLocationReport() {
    if (protocol_ == nullptr) {
        return;
    }

    FamilyLocationFix fix;
    if (family_location_provider_) {
        fix = family_location_provider_();
    }
    if (!fix.valid) {
        // Board provides no location source (or no fix yet): skip this tick.
        ESP_LOGD(TAG, "Family location report skipped: no fix available");
        return;
    }

    static const char* kSourceNames[] = {"gps", "wifi", "lbs"};
    const char* source = kSourceNames[static_cast<int>(fix.source)];
    int battery_pct = fix.battery_pct;
    if (battery_pct < 0) {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
            battery_pct = level;
        }
    }
    bool sent =
        protocol_->SendFamilyLocationReport(fix.lat, fix.lng, source, fix.accuracy_m, battery_pct);
    if (sent) {
        ESP_LOGI(TAG, "Family location report sent (%.6f, %.6f via %s)", fix.lat, fix.lng, source);
    } else {
        ESP_LOGW(TAG, "Failed to send family location report (channel closed?)");
    }
}

std::string Application::GenerateFamilyMessageId() {
    // UUID v4, same shape as Board::GenerateUuid().
    uint8_t uuid[16];
    esp_fill_random(uuid, sizeof(uuid));
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
    char uuid_str[37];
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", uuid[0],
             uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return uuid_str;
}

void Application::HandleFamilyJson(const cJSON* root) {
    // Runs on the transport receive task: extract fields, then schedule the
    // actual handling into the main task (the cJSON tree is deleted on return).
    auto action = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action)) {
        ESP_LOGW(TAG, "Family message without action");
        return;
    }

    if (strcmp(action->valuestring, "message_arrived") == 0) {
        auto message_id = cJSON_GetObjectItem(root, "message_id");
        auto audio_url = cJSON_GetObjectItem(root, "audio_url");
        auto sender_name = cJSON_GetObjectItem(root, "sender_name");
        if (!cJSON_IsString(message_id) || !cJSON_IsString(audio_url) ||
            audio_url->valuestring[0] == '\0') {
            ESP_LOGW(TAG, "Invalid family message_arrived payload");
            return;
        }
        Schedule([this, message_id = std::string(message_id->valuestring),
                  audio_url = std::string(audio_url->valuestring),
                  sender_name = cJSON_IsString(sender_name) ? std::string(sender_name->valuestring)
                                                            : std::string()]() {
            HandleFamilyMessageArrived(message_id, audio_url, sender_name);
        });
    } else if (strcmp(action->valuestring, "sos_acked") == 0) {
        auto sos_event_id = cJSON_GetObjectItem(root, "sos_event_id");
        auto acked_by_name = cJSON_GetObjectItem(root, "acked_by_name");
        Schedule([this, sos_event_id = cJSON_IsString(sos_event_id)
                                            ? std::string(sos_event_id->valuestring)
                                            : std::string(),
                  acked_by_name = cJSON_IsString(acked_by_name)
                                      ? std::string(acked_by_name->valuestring)
                                      : std::string()]() {
            HandleFamilySosAcked(sos_event_id, acked_by_name);
        });
    } else if (strcmp(action->valuestring, "alarm_fire") == 0) {
        auto alarm_id = cJSON_GetObjectItem(root, "alarm_id");
        auto label = cJSON_GetObjectItem(root, "label");
        auto time = cJSON_GetObjectItem(root, "time");
        Schedule([this, alarm_id = cJSON_IsString(alarm_id) ? std::string(alarm_id->valuestring)
                                                            : std::string(),
                  label = cJSON_IsString(label) ? std::string(label->valuestring) : std::string(),
                  time = cJSON_IsString(time) ? std::string(time->valuestring) : std::string()]() {
            HandleFamilyAlarmFire(alarm_id, label, time);
        });
    } else if (strcmp(action->valuestring, "call_incoming") == 0) {
        auto call_id = cJSON_GetObjectItem(root, "call_id");
        auto caller_name = cJSON_GetObjectItem(root, "caller_name");
        Schedule([this,
                  call_id = cJSON_IsString(call_id) ? std::string(call_id->valuestring)
                                                    : std::string(),
                  caller_name = cJSON_IsString(caller_name)
                                    ? std::string(caller_name->valuestring)
                                    : std::string()]() {
            HandleFamilyCallIncoming(call_id, caller_name);
        });
    } else if (strcmp(action->valuestring, "ack") == 0) {
        // Server acknowledgement for one of our uplinks (§1.3): log only.
        auto ref_action = cJSON_GetObjectItem(root, "ref_action");
        auto ok = cJSON_GetObjectItem(root, "ok");
        auto error = cJSON_GetObjectItem(root, "error");
        ESP_LOGI(TAG, "Family ack: action=%s ok=%s error=%s",
                 cJSON_IsString(ref_action) ? ref_action->valuestring : "?",
                 cJSON_IsTrue(ok) ? "true" : "false",
                 cJSON_IsString(error) ? error->valuestring : "-");
        if (cJSON_IsObject(ok) && !cJSON_IsTrue(ok)) {
            Schedule([this]() { audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION); });
        }
    }
}

void Application::HandleFamilyMessageArrived(std::string message_id, std::string audio_url,
                                             std::string sender_name) {
    if (family_recording_) {
        ESP_LOGW(TAG, "Family message %s ignored while recording a message",
                 message_id.c_str());
        return;
    }
    if (GetDeviceState() != kDeviceStateIdle || notify_player_.IsBusy()) {
        // Not played, so no message_played receipt; the server side is
        // responsible for redelivery.
        ESP_LOGW(TAG, "Family message %s ignored while device is busy", message_id.c_str());
        return;
    }

    ESP_LOGI(TAG, "Family message arrived from %s (%s)", sender_name.c_str(), message_id.c_str());
    if (!sender_name.empty()) {
        Board::GetInstance().GetDisplay()->SetChatMessage("system", sender_name.c_str());
    }
    family_pending_played_message_id_ = std::move(message_id);
    // Reuse the notify player: it already streams audio from a URL through
    // the audio service decode/playback pipeline.
    StartNotification(std::move(audio_url), {});
}

void Application::HandleFamilySosAcked(std::string sos_event_id, std::string acked_by_name) {
    ESP_LOGI(TAG, "Family SOS %s acknowledged by %s", sos_event_id.c_str(),
             acked_by_name.c_str());
    std::string message = "SOS";
    if (!acked_by_name.empty()) {
        message += " - " + acked_by_name;
    }
    Alert("SOS", message.c_str(), "neutral", Lang::Sounds::OGG_SUCCESS);
}

void Application::HandleFamilyAlarmFire(std::string alarm_id, std::string label,
                                        std::string time) {
    ESP_LOGI(TAG, "Family alarm fired: %s", alarm_id.c_str());
    std::string message = label.empty() ? std::string("Alarm") : label;
    if (!time.empty()) {
        message += " " + time;
    }
    // Alert() shows status/emotion/chat message and plays the bundled sound;
    // contract sound "chime" is mapped to the built-in exclamation sound.
    Alert("Alarm", message.c_str(), "neutral", Lang::Sounds::OGG_EXCLAMATION);
}

void Application::HandleFamilyCallIncoming(std::string call_id, std::string caller_name) {
    // FAMILY-PROTOCOL v1 §1.2 downlink (device-call spec §5). Runs in the
    // main task (scheduled from HandleFamilyJson). The v1 device presents the
    // call and waits for the user's spoken reply; answering/rejecting is a
    // conversation intent handled by the server (call_device with answer=true
    // via the [device_call] incoming-call flag). The firmware only manages
    // the incoming-call phase and the ack.
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "call_incoming dropped: protocol not initialized");
        return;
    }
    if (call_id.empty()) {
        ESP_LOGW(TAG, "call_incoming without call_id");
        protocol_->SendFamilyAck("call_incoming", "", false, "invalid");
        return;
    }
    auto state = GetDeviceState();
    if (family_recording_ || family_call_state_ != FamilyCallState::kIdle ||
        state == kDeviceStateStarting || state == kDeviceStateActivating ||
        state == kDeviceStateUpgrading || state == kDeviceStateWifiConfiguring ||
        state == kDeviceStateAudioTesting || state == kDeviceStateFatalError) {
        ESP_LOGW(TAG, "Family call %s rejected: device busy (state=%d, call_phase=%d)",
                 call_id.c_str(), static_cast<int>(state),
                 static_cast<int>(family_call_state_));
        protocol_->SendFamilyAck("call_incoming", call_id.c_str(), false, "busy");
        return;
    }

    ESP_LOGI(TAG, "Family call incoming (call_id=%s, caller=%s)", call_id.c_str(),
             caller_name.empty() ? "<unknown>" : caller_name.c_str());
    family_call_state_ = FamilyCallState::kIncoming;
    family_call_id_ = call_id;

    // Announcement text for the server's [device_call] detect-text path: the
    // server speaks it verbatim (direct TTS, no LLM) and arms the call_device
    // answer flag for the user's spoken reply. Fallback wording when the
    // signaling carries no caller name.
    family_call_announcement_ = caller_name.empty()
                                    ? "您收到一个新的来电，是否接听？"
                                    : "您收到来自" + caller_name + "的来电，是否接听？";

    if (!caller_name.empty()) {
        Board::GetInstance().GetDisplay()->SetChatMessage("system", caller_name.c_str());
    }

    // The ring wait is now open; the timeout or the user's reply closes it.
    protocol_->SendFamilyAck("call_incoming", call_id.c_str(), true, nullptr);
    StartFamilyCallRingTimer();
    InjectFamilyCallAnnouncement(family_call_announcement_);
}

void Application::InjectFamilyCallAnnouncement(std::string text) {
    // Runs in the main task. Sends the announcement as listen-detect text so
    // the server session (the [device_call] handler) announces the call; the
    // audio channel is normally already up because the gateway wakes the
    // callee (self.remote_wakeup) before the api pushes call_incoming.
    auto state = GetDeviceState();
    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }
    if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    }
    if (protocol_->IsAudioChannelOpened()) {
        protocol_->SendWakeWordDetected(text);
        return;
    }
    // Channel not up yet: hold the announcement and get a channel. The open
    // path sends it via the OnAudioChannelOpened hook.
    family_call_pending_announcement_ = std::move(text);
    state = GetDeviceState();
    if (state == kDeviceStateIdle) {
        SetDeviceState(kDeviceStateConnecting);
        Schedule([this]() { ContinueFamilyCallAnnouncement(); });
    } else if (state == kDeviceStateConnecting) {
        // An open already in flight (remote_wakeup) fires the hook.
    } else {
        // Listening/Speaking with a closed channel: reopen in place; the
        // announcement goes out from the OnAudioChannelOpened hook.
        Schedule([this]() {
            if (protocol_ != nullptr && !protocol_->IsAudioChannelOpened()) {
                protocol_->OpenAudioChannel();
            }
        });
    }
}

void Application::ContinueFamilyCallAnnouncement() {
    // Runs in the main task; entered from Idle via Connecting.
    if (family_call_pending_announcement_.empty()) {
        return;
    }
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle; the ring timeout acks the failure.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }
    // The channel is up and OnAudioChannelOpened already sent the pending
    // announcement; move to listening so the user can answer by voice.
    SetListeningMode(GetDefaultListeningMode());
}

void Application::StartFamilyCallRingTimer() {
    StopFamilyCallRingTimer();
    esp_timer_create_args_t timer_args = {
        .callback =
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->Schedule([app]() { app->HandleFamilyCallRingTimeout(); });
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "family_ring",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &family_call_ring_timer_) == ESP_OK) {
        esp_timer_start_once(family_call_ring_timer_,
                             static_cast<uint64_t>(CONFIG_FAMILY_CALL_RING_TIMEOUT_MS) * 1000ULL);
    } else {
        ESP_LOGE(TAG, "Failed to create family call ring timer");
    }
}

void Application::StopFamilyCallRingTimer() {
    if (family_call_ring_timer_ != nullptr) {
        esp_timer_stop(family_call_ring_timer_);
        esp_timer_delete(family_call_ring_timer_);
        family_call_ring_timer_ = nullptr;
    }
}

void Application::HandleFamilyCallRingTimeout() {
    if (family_call_state_ != FamilyCallState::kIncoming) {
        return;
    }
    ESP_LOGW(TAG, "Family call %s ring timeout", family_call_id_.c_str());
    // Bound aligned with the gateway's pending-call cleanup (60s): after it
    // the pending call is dead server-side, so give up and ack the failure.
    EndFamilyCall("timeout");
}

void Application::ClearFamilyCallState() {
    if (family_call_state_ == FamilyCallState::kIdle) {
        return;
    }
    ESP_LOGI(TAG, "Family call %s cleared (channel closed)", family_call_id_.c_str());
    family_call_state_ = FamilyCallState::kIdle;
    family_call_id_.clear();
    family_call_announcement_.clear();
    family_call_pending_announcement_.clear();
    StopFamilyCallRingTimer();
}

void Application::EndFamilyCall(const char* ack_error, bool close_channel) {
    // Runs in the main task. Clears the call phase, acks the call_incoming
    // failure when the call was still in the ring wait, and closes the audio
    // channel: the goodbye tells the gateway to release the call bridge.
    std::string call_id = std::move(family_call_id_);
    family_call_state_ = FamilyCallState::kIdle;
    family_call_announcement_.clear();
    family_call_pending_announcement_.clear();
    StopFamilyCallRingTimer();
    if (protocol_ == nullptr) {
        return;
    }
    if (ack_error != nullptr && !call_id.empty()) {
        protocol_->SendFamilyAck("call_incoming", call_id.c_str(), false, ack_error);
    }
    if (close_channel) {
        protocol_->CloseAudioChannel(true);
        // The audio-channel-closed callback returns the device to idle.
    }
}

void Application::TearDownFamilyCallForSos() {
    // Runs in the main task, from SendFamilySos. Adjudication B1: the call is
    // sacrificed for the SOS uplink.
    bool was_incoming = family_call_state_ == FamilyCallState::kIncoming;
    ESP_LOGW(TAG, "Family call aborted by SOS (call_id=%s)", family_call_id_.c_str());
    if (protocol_->ControlPlaneSurvivesChannelClose()) {
        // MQTT: goodbye releases the gateway call and the broker session
        // survives. The bridge the gateway may have dropped at call start
        // only comes back with a fresh hello, so reopen before the uplink.
        EndFamilyCall(was_incoming ? "rejected" : nullptr, true);
        if (!protocol_->IsAudioChannelOpened()) {
            protocol_->OpenAudioChannel();  // best effort; see comment below
        }
    } else {
        // WebSocket: closing the channel would destroy the socket that has to
        // carry the SOS itself, and a WS deployment has no gateway bridge that
        // could drop the uplink — clear the state and send over the live
        // socket as-is.
        EndFamilyCall(was_incoming ? "rejected" : nullptr, false);
    }
}

void Application::RemoteWakeup(const std::string& reason) {
    // Runs in the main task (MCP tool calls are dispatched there). Issued by
    // the MQTT gateway for an incoming device call (device-call spec §1/§5).
    // Same exclusivity as the wake word path: never interrupt a family voice
    // message recording.
    if (family_recording_) {
        ESP_LOGW(TAG, "Remote wakeup ignored: recording a family message");
        return;
    }
    auto state = GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    if (state != kDeviceStateIdle) {
        ESP_LOGW(TAG, "Remote wakeup ignored in state %s",
                 DeviceStateMachine::GetStateName(state));
        return;
    }
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "Remote wakeup failed: protocol not initialized");
        return;
    }
    if (!SetDeviceState(kDeviceStateConnecting)) {
        return;
    }
    if (protocol_->IsAudioChannelOpened()) {
        ContinueRemoteWakeup(reason);
    } else {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second.
        Schedule([this, reason]() { ContinueRemoteWakeup(reason); });
    }
}

void Application::ContinueRemoteWakeup(const std::string& reason) {
    // Check state again in case it was changed during scheduling.
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }
    if (!reason.empty()) {
        ESP_LOGI(TAG, "Remote wakeup: %s", reason.c_str());
        // Forward the reason as the wake-word context (listen detect text) so
        // the server session knows why the device woke up.
        protocol_->SendWakeWordDetected(reason);
    }
    SetListeningMode(GetDefaultListeningMode());
}
