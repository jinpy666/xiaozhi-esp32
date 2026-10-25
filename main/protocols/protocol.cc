#include "protocol.h"
#include "assets.h"

#include <esp_log.h>
#include <cJSON.h>

#include <cstdio>
#include <cstring>

#define TAG "Protocol"

void Protocol::AddTextFontCapabilities(cJSON* root) {
    auto capability = Assets::GetInstance().text_font_capability();
    cJSON* features = cJSON_GetObjectItem(root, "features");
    if (cJSON_IsObject(features)) {
        cJSON_AddBoolToObject(features, "glyph_push", capability.glyph_push);
    }

    if (!capability.glyph_push) {
        return;
    }
    cJSON* font = cJSON_CreateObject();
    cJSON_AddStringToObject(font, "bundle", capability.bundle.c_str());
    cJSON_AddStringToObject(font, "charset", capability.charset.c_str());
    cJSON_AddNumberToObject(font, "size", capability.size);
    cJSON_AddNumberToObject(font, "bpp", capability.bpp);
    cJSON_AddItemToObject(root, "text_font", font);
}

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::OnConnected(std::function<void()> callback) { on_connected_ = callback; }

void Protocol::OnDisconnected(std::function<void()> callback) { on_disconnected_ = callback; }

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

void Protocol::SetError(const std::string& message, const std::string& detail) {
    if (detail.empty()) {
        SetError(message);
        return;
    }
    SetError(message + "\n" + detail);
}

void Protocol::SendAbortSpeaking(AbortReason reason) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected) {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    std::string json = "{\"session_id\":\"" + session_id_ +
                       "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + wake_word +
                       "\"}";
    SendText(json);
}

void Protocol::SendStartListening(ListeningMode mode) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeRealtime) {
        message += ",\"mode\":\"realtime\"";
    } else if (mode == kListeningModeAutoStop) {
        message += ",\"mode\":\"auto\"";
    } else {
        message += ",\"mode\":\"manual\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendStopListening() {
    std::string message =
        "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

void Protocol::SendMcpMessage(const std::string& payload) {
    std::string message =
        "{\"session_id\":\"" + session_id_ + "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}

void Protocol::OnFamilyMessage(std::function<void(const cJSON* root)> callback) {
    on_family_message_ = std::move(callback);
}

void Protocol::DispatchFamilyMessage(const cJSON* root) {
    auto action = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action) || action->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "Family message missing action");
        return;
    }
    // FAMILY-PROTOCOL v1 §1.2 downlink actions plus the §1.3 server ack.
    if (strcmp(action->valuestring, "message_arrived") != 0 &&
        strcmp(action->valuestring, "sos_acked") != 0 &&
        strcmp(action->valuestring, "alarm_fire") != 0 &&
        strcmp(action->valuestring, "call_incoming") != 0 &&
        strcmp(action->valuestring, "ack") != 0) {
        ESP_LOGW(TAG, "Unknown family action: %s", action->valuestring);
        return;
    }
    if (on_family_message_ != nullptr) {
        on_family_message_(root);
    }
}

bool Protocol::SendFamilyAction(const char* action, cJSON* payload) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(root, "type", "family");
    cJSON_AddStringToObject(root, "action", action);
    if (payload != nullptr) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach (item, payload) {
            cJSON_AddItemToObject(root, item->string, cJSON_Duplicate(item, 1));
        }
    }
    auto json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == nullptr) {
        return false;
    }
    std::string message(json_str);
    cJSON_free(json_str);
    return SendText(message);
}

bool Protocol::SendFamilyVoiceMessage(const std::string& message_id, const std::string& audio_b64,
                                      int duration_ms) {
    // Built by hand instead of through cJSON: the base64 payload is large and
    // message_id/audio_b64 never need JSON escaping (UUID and base64 alphabet).
    std::string message = "{\"type\":\"family\",\"action\":\"message_send\",\"message_id\":\"" +
                          message_id + "\",\"audio_format\":\"opus\",\"audio_b64\":\"" + audio_b64 +
                          "\",\"duration_ms\":" + std::to_string(duration_ms) + "}";
    return SendText(message);
}

bool Protocol::SendFamilyLocationReport(double lat, double lng, const char* source,
                                        double accuracy_m, int battery_pct) {
    cJSON* payload = cJSON_CreateObject();
    if (payload == nullptr) {
        return false;
    }
    cJSON_AddNumberToObject(payload, "lat", lat);
    cJSON_AddNumberToObject(payload, "lng", lng);
    cJSON_AddStringToObject(payload, "source", source);
    cJSON_AddNumberToObject(payload, "accuracy_m", accuracy_m);
    if (battery_pct >= 0) {
        cJSON_AddNumberToObject(payload, "battery_pct", battery_pct);
    }
    bool sent = SendFamilyAction("location_report", payload);
    cJSON_Delete(payload);
    return sent;
}

bool Protocol::SendFamilySos(const std::string& message_id, const char* trigger) {
    cJSON* payload = cJSON_CreateObject();
    if (payload == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(payload, "message_id", message_id.c_str());
    cJSON_AddStringToObject(payload, "trigger", trigger);
    bool sent = SendFamilyAction("sos", payload);
    cJSON_Delete(payload);
    return sent;
}

bool Protocol::SendFamilyMessagePlayed(const std::string& message_id) {
    cJSON* payload = cJSON_CreateObject();
    if (payload == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(payload, "message_id", message_id.c_str());
    bool sent = SendFamilyAction("message_played", payload);
    cJSON_Delete(payload);
    return sent;
}

bool Protocol::SendFamilyAck(const char* ref_action, const char* ref_id, bool ok,
                             const char* error) {
    cJSON* payload = cJSON_CreateObject();
    if (payload == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(payload, "ref_action", ref_action);
    if (ref_id != nullptr) {
        cJSON_AddStringToObject(payload, "ref_id", ref_id);
    }
    cJSON_AddBoolToObject(payload, "ok", ok);
    if (error != nullptr) {
        cJSON_AddStringToObject(payload, "error", error);
    }
    bool sent = SendFamilyAction("ack", payload);
    cJSON_Delete(payload);
    return sent;
}

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}
