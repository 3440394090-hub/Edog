#include "deskpet_protocol.h"

#include "board.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <cstring>
#include <cmath>

#define TAG "Deskpet"

// 判定"有人在说话"的音量阈值(int16 RMS)
#define SPEECH_RMS_THRESHOLD 800
// 说过话之后,静音多久就认为"说完了"(ms)
#define SILENCE_STOP_MS 900
// 能量阈值判停(兜底)。设备自带 WebRTC VAD 判停已启用,这里设为 0 以免互相干扰;
// 若 VAD 方案失效,改回 1 即可恢复。
#define DESKPET_ENERGY_ENDPOINT 0

#ifndef CONFIG_WEBSOCKET_URL
#define CONFIG_WEBSOCKET_URL "ws://127.0.0.1/ws/v1/chat"
#endif
#ifndef CONFIG_WEBSOCKET_TOKEN
#define CONFIG_WEBSOCKET_TOKEN ""
#endif

DeskpetProtocol::DeskpetProtocol() {
    chat_url_ = CONFIG_WEBSOCKET_URL;
    api_key_ = CONFIG_WEBSOCKET_TOKEN;
    ESP_LOGI(TAG, "chat url = %s", chat_url_.c_str());
}

DeskpetProtocol::~DeskpetProtocol() { CloseAudioChannel(); }

bool DeskpetProtocol::Start() { return true; }

std::string DeskpetProtocol::ChatUrl() const {
    if (api_key_.empty()) {
        return chat_url_;
    }
    return chat_url_ + (chat_url_.find('?') == std::string::npos ? "?" : "&") + "api_key=" + api_key_;
}

void DeskpetProtocol::EmitJson(const std::string& json) {
    if (on_incoming_json_) {
        auto root = cJSON_Parse(json.c_str());
        if (root) {
            on_incoming_json_(root);
            cJSON_Delete(root);
        }
    }
}

void DeskpetProtocol::EmitTtsSentence(const std::string& text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "state", "sentence_start");
    cJSON_AddStringToObject(root, "text", text.c_str());
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s != nullptr) {
        EmitJson(s);
        cJSON_free(s);
    }
}

// 把服务器情绪名映射到项目表情素材名(happy/sad/angry/surprised/relaxed/neutral...)
static const char* MapEmotionForDisplay(const char* e) {
    if (e == nullptr || *e == '\0') return "neutral";
    if (strcmp(e, "natural") == 0) return "neutral";
    if (strcmp(e, "gentle") == 0) return "relaxed";
    if (strcmp(e, "happy") == 0) return "happy";
    if (strcmp(e, "sad") == 0) return "sad";
    if (strcmp(e, "surprised") == 0) return "surprised";
    if (strcmp(e, "angry") == 0) return "angry";
    return "neutral";
}

void DeskpetProtocol::EmitEmotion(const std::string& emotion) {
    if (emotion.empty()) {
        return;
    }
    const char* mapped = MapEmotionForDisplay(emotion.c_str());
    ESP_LOGI(TAG, "emotion => %s (mapped: %s)", emotion.c_str(), mapped);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "llm");
    cJSON_AddStringToObject(root, "emotion", mapped);
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s != nullptr) {
        EmitJson(s);
        cJSON_free(s);
    }
}

bool DeskpetProtocol::OpenAudioChannel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ws_) {
        ws_->Close();
        ws_.reset();
    }
    auto network = Board::GetInstance().GetNetwork();
    ws_ = network->CreateWebSocket(4);
    if (!ws_) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }
    ws_->OnData([this](const char* data, size_t len, bool binary) { OnData(data, len, binary); });
    ws_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "websocket disconnected (chat_active=%d)", (int)chat_active_.load());
        connected_.store(false);
        asr_active_.store(false);
        // 若不在放音阶段,通知 app 回到 idle(否则会卡在 listening,不再重试)
        if (!chat_active_.load() && on_audio_channel_closed_) {
            on_audio_channel_closed_();
        }
    });

    std::string url = ChatUrl();
    ESP_LOGI(TAG, "Connecting: %s", url.c_str());
    if (!ws_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect, code=%d", ws_->GetLastError());
        SetError("无法连接到服务");
        return false;
    }
    connected_.store(true);
    ESP_LOGI(TAG, "connected");
    return true;
}

bool DeskpetProtocol::IsAudioChannelOpened() const {
    return ws_ != nullptr && ws_->IsConnected() && connected_.load();
}

void DeskpetProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    if (ws_) {
        ws_->Close();
        ws_.reset();
    }
    connected_.store(false);
    asr_active_.store(false);
    chat_active_.store(false);
}

void DeskpetProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    if (!IsAudioChannelOpened()) {
        ESP_LOGW(TAG, "start listening but not connected");
        return;
    }
    req_id_ = "a" + std::to_string(++req_counter_);
    tx_frames_.store(0);
    speech_started_.store(false);
    noise_floor_ = 300.0;
    t_audio_start_us_.store(esp_timer_get_time());
    char buf[192];
    snprintf(buf, sizeof(buf), "{\"type\":\"audio_start\",\"request_id\":\"%s\"}", req_id_.c_str());
    ws_->Send(buf);
    asr_active_.store(true);
    ESP_LOGI(TAG, "audio_start sent (req=%s)", req_id_.c_str());
}

bool DeskpetProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!asr_active_.load() || !IsAudioChannelOpened()) {
        return false;
    }
    if (packet->payload.empty()) {
        return false;
    }
    // 逐帧算音量,记录"最后一次检测到人声"的时刻(真正的"用户说完")
    {
        const int16_t* s = reinterpret_cast<const int16_t*>(packet->payload.data());
        size_t cnt = packet->payload.size() / sizeof(int16_t);
        double sum = 0;
        for (size_t i = 0; i < cnt; i++) {
            double v = s[i];
            sum += v * v;
        }
        double rms = cnt ? std::sqrt(sum / cnt) : 0.0;
        int64_t now = esp_timer_get_time();
        // 自适应噪声底:未检测到人声时缓慢跟踪环境噪声;阈值 = max(500, 噪声底*3)。
        // 固定阈值会对"高底噪麦克风"失效(一直以为有人在说话)。
        if (!speech_started_.load()) {
            noise_floor_ = noise_floor_ * 0.92 + rms * 0.08;
        }
        double thresh = noise_floor_ * 3.0;
        if (thresh < 500.0) {
            thresh = 500.0;
        }
        bool voiced = rms >= thresh;
        int n = ++tx_frames_;
        if (n == 1 || n % 25 == 0) {
            ESP_LOGI(TAG, "TX #%d rms=%.0f floor=%.0f thresh=%.0f voiced=%d speech=%d", n, rms,
                     noise_floor_, thresh, (int)voiced, (int)speech_started_.load());
        }
        if (voiced) {
            t_last_voice_us_.store(now);
            speech_started_.store(true);
        }
#if DESKPET_ENERGY_ENDPOINT
        else if (speech_started_.load() &&
                   (now - t_last_voice_us_.load()) >= (int64_t)SILENCE_STOP_MS * 1000) {
            // 客户端静音判停:说过话后静音 ~0.9s => 立刻告诉服务器"说完了",
            // 否则服务器要硬等到 max_seconds(30s) 才收尾,用户白等。
            int64_t quiet_ms = (now - t_last_voice_us_.load()) / 1000;
            speech_started_.store(false);
            asr_active_.store(false);
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"audio_stop\",\"request_id\":\"%s\",\"reason\":\"silence\"}",
                     req_id_.c_str());
            ws_->Send(buf);
            ESP_LOGI(TAG, "检测到静音 %lldms -> 自动 audio_stop (req=%s)", (long long)quiet_ms,
                     req_id_.c_str());
            return true;
        }
#endif
    }
    t_last_tx_us_.store(esp_timer_get_time());
    return ws_->Send(packet->payload.data(), packet->payload.size(), true);
}

void DeskpetProtocol::SendStopListening() {
    if (!IsAudioChannelOpened() || !asr_active_.load()) {
        return;
    }
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"audio_stop\",\"request_id\":\"%s\",\"reason\":\"silence\"}", req_id_.c_str());
    ws_->Send(buf);
    asr_active_.store(false);
    ESP_LOGI(TAG, "audio_stop sent (req=%s)", req_id_.c_str());
}

void DeskpetProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    if (IsAudioChannelOpened()) {
        char buf[192];
        snprintf(buf, sizeof(buf), "{\"type\":\"audio_cancel\",\"request_id\":\"%s\"}", req_id_.c_str());
        ws_->Send(buf);
    }
    asr_active_.store(false);
    chat_active_.store(false);
}

void DeskpetProtocol::SendWakeWordDetected(const std::string& wake_word) { (void)wake_word; }
void DeskpetProtocol::SendMcpMessage(const std::string& message) { (void)message; }
bool DeskpetProtocol::SendText(const std::string& text) { (void)text; return false; }

void DeskpetProtocol::OnData(const char* data, size_t len, bool binary) {
    if (binary) {
        OnBinaryPcm(data, len);
        return;
    }
    auto root = cJSON_ParseWithLength(data, len);
    if (!root) {
        return;
    }
    OnJson(root);
    cJSON_Delete(root);
}

void DeskpetProtocol::OnJson(const cJSON* root) {
    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        return;
    }
    const char* t = type->valuestring;

    if (strcmp(t, "asr_ready") == 0) {
        ESP_LOGI(TAG, "asr_ready (req=%s)", req_id_.c_str());
    } else if (strcmp(t, "asr_end") == 0) {
        asr_active_.store(false);  // 服务器已判定结束,停止继续上传音频
        int64_t up_ms = t_audio_start_us_.load()
                            ? (esp_timer_get_time() - t_audio_start_us_.load()) / 1000
                            : -1;
        ESP_LOGW(TAG, "[上传] 本轮共上传音频 %lld ms (=%d 帧)", (long long)up_ms, tx_frames_.load());
        int64_t ref = t_last_voice_us_.load() ? t_last_voice_us_.load() : t_last_tx_us_.load();
        int64_t d = (esp_timer_get_time() - ref) / 1000;
        ESP_LOGI(TAG, "[延时] 说完 -> asr_end: %lld ms", (long long)d);
    } else if (strcmp(t, "transcript") == 0) {
        auto text = cJSON_GetObjectItem(root, "text");
        std::string s = cJSON_IsString(text) ? text->valuestring : "";
        int64_t ref = t_last_voice_us_.load() ? t_last_voice_us_.load() : t_last_tx_us_.load();
        int64_t d = (esp_timer_get_time() - ref) / 1000;
        t_transcript_us_.store(esp_timer_get_time());
        ESP_LOGI(TAG, "[延时] 说完 -> 识别文字: %lld ms  \"%s\"", (long long)d, s.c_str());
        asr_active_.store(false);
        if (!s.empty()) {
            EmitJson("{\"type\":\"stt\",\"text\":\"" + s + "\"}");
            StartChat(s);
        }
    } else if (strcmp(t, "chat") == 0) {
        // 可能是服务器的 chat ack,忽略
    } else if (strcmp(t, "meta") == 0) {
        // 会话建立
    } else if (strcmp(t, "delta") == 0) {
        // 流式文字:累计并推给 app,让屏幕(若装了)实时显示助手回答
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            chat_text_ += text->valuestring;
            EmitTtsSentence(chat_text_);
        }
    } else if (strcmp(t, "audio") == 0) {
        // 若服务器在音频消息里提前带了情绪(emotion / tts_emotion),立刻应用 ——
        // 这样"播放期间"就能显示回答表情(目前服务器只在 final 里给,所以做不到)
        {
            auto emo = cJSON_GetObjectItem(root, "emotion");
            if (!cJSON_IsString(emo)) {
                emo = cJSON_GetObjectItem(root, "tts_emotion");
            }
            if (cJSON_IsString(emo)) {
                EmitEmotion(emo->valuestring);
            }
        }
        auto fmt = cJSON_GetObjectItem(root, "format");
        if (cJSON_IsString(fmt) && strcmp(fmt->valuestring, "pcm") == 0) {
            auto rate = cJSON_GetObjectItem(root, "sample_rate");
            auto ch = cJSON_GetObjectItem(root, "channels");
            pending_rate_.store(cJSON_IsNumber(rate) ? rate->valueint : 16000);
            pending_channels_.store(cJSON_IsNumber(ch) ? ch->valueint : 1);
            pending_pcm_.store(true);
        } else {
            ESP_LOGW(TAG, "encoded audio (%s) skipped (only PCM)", cJSON_IsString(fmt) ? fmt->valuestring : "?");
        }
    } else if (strcmp(t, "final") == 0) {
        // 服务器在本消息里下发 emotion(如 happy/gentle)-> 转给 app 驱动表情/舵机
        auto emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(emotion)) {
            EmitEmotion(emotion->valuestring);
        }
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && chat_text_.empty()) {
            chat_text_ = text->valuestring;
            EmitTtsSentence(chat_text_);
        }
        FinishChat();
    } else if (strcmp(t, "error") == 0) {
        auto code = cJSON_GetObjectItem(root, "code");
        auto msg = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "error: %s %s", cJSON_IsString(code) ? code->valuestring : "",
                 cJSON_IsString(msg) ? msg->valuestring : "");
        asr_active_.store(false);
        EmitJson("{\"type\":\"tts\",\"state\":\"stop\"}");
    }
}

void DeskpetProtocol::OnBinaryPcm(const char* data, size_t len) {
    if (!pending_pcm_.load()) {
        return;
    }
    pending_pcm_.store(false);
    int rate = pending_rate_.load();
    int channels = pending_channels_.load();
    rx_bytes_ += (int64_t)len;
    rx_rate_.store(rate);
    {
        static int cnt = 0;
        static int64_t t0 = 0;
        int64_t now = esp_timer_get_time();
        if (t0 == 0) {
            t0 = now;
        }
        if ((cnt++ % 25) == 0) {
            double secs = (now - t0) / 1e6;
            double got = secs > 0 ? (double)rx_bytes_.load() / secs : 0;
            double nat = 2.0 * (rate > 0 ? rate : 16000);
            ESP_LOGI(TAG, "收音频: 累计 %lld B / %.2fs => 到达 %.0f B/s (音频本速 %.0f B/s, %s)",
                     (long long)rx_bytes_.load(), secs, got, nat,
                     got > nat * 1.3 ? "服务器发得太快!" : "正常");
        }
    }

    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = rate > 0 ? rate : 16000;
    packet->frame_duration = 20;
    packet->payload.assign(reinterpret_cast<const uint8_t*>(data),
                           reinterpret_cast<const uint8_t*>(data) + len);
    if (channels > 1) {
        size_t n = packet->payload.size() / (2 * channels);
        std::vector<uint8_t> mono(n * 2);
        const int16_t* src = reinterpret_cast<const int16_t*>(packet->payload.data());
        int16_t* dst = reinterpret_cast<int16_t*>(mono.data());
        for (size_t i = 0; i < n; i++) {
            int32_t acc = 0;
            for (int c = 0; c < channels; c++) {
                acc += src[i * channels + c];
            }
            dst[i] = (int16_t)(acc / channels);
        }
        packet->payload = std::move(mono);
    }
    // 首次收到音频前,先让设备进入"说话"状态(否则音频会被丢弃)
    if (!tts_started_.load()) {
        tts_started_.store(true);
        t_first_audio_us_.store(esp_timer_get_time());
        int64_t ref = t_last_voice_us_.load() ? t_last_voice_us_.load() : t_last_tx_us_.load();
        int64_t d = (t_first_audio_us_.load() - ref) / 1000;
        int64_t srv = t_transcript_us_.load()
                          ? (t_first_audio_us_.load() - t_transcript_us_.load()) / 1000
                          : -1;
        ESP_LOGI(TAG, "[延时] 说完 -> 第一段回复音频开始: %lld ms  (其中 后端LLM+TTS: %lld ms)",
                 (long long)d, (long long)srv);
        EmitJson("{\"type\":\"tts\",\"state\":\"start\"}");
    }
    if (on_incoming_audio_) {
        on_incoming_audio_(std::move(packet));
    }
    last_incoming_time_ = std::chrono::steady_clock::now();
}

void DeskpetProtocol::StartChat(const std::string& text) {
    if (!IsAudioChannelOpened()) {
        return;
    }
    chat_active_.store(true);
    tts_started_.store(false);
    pending_pcm_.store(false);
    rx_bytes_.store(0);
    chat_text_.clear();
    req_counter_++;

    // 用 cJSON 构造(避免识别文字含引号/特殊字符时破坏 JSON),并附上可选的语气/声音参数
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "chat");
    char rid[32];
    snprintf(rid, sizeof(rid), "c%d", req_counter_);
    cJSON_AddStringToObject(root, "request_id", rid);
    cJSON_AddStringToObject(root, "text", text.c_str());
    // 以下均由 Kconfig 配置;留空 = 不发送该字段,交由服务器决定
    if (strlen(CONFIG_DESKPET_PERSONA) > 0) {
        cJSON_AddStringToObject(root, "persona", CONFIG_DESKPET_PERSONA);
    }
    if (strlen(CONFIG_DESKPET_TTS_EMOTION) > 0) {
        cJSON_AddStringToObject(root, "tts_emotion", CONFIG_DESKPET_TTS_EMOTION);
    }
    if (strlen(CONFIG_DESKPET_TTS_SPEED) > 0) {
        cJSON_AddStringToObject(root, "tts_speed", CONFIG_DESKPET_TTS_SPEED);
    }
    if (strlen(CONFIG_DESKPET_TTS_AGE) > 0) {
        cJSON_AddStringToObject(root, "tts_age", CONFIG_DESKPET_TTS_AGE);
    }
    if (strlen(CONFIG_DESKPET_TTS_GENDER) > 0) {
        cJSON_AddStringToObject(root, "tts_gender", CONFIG_DESKPET_TTS_GENDER);
    }
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json != nullptr) {
        ws_->Send(json);
        ESP_LOGI(TAG, "chat sent: %s", json);
        cJSON_free(json);
    }
    // 问题已发给服务器,等待回答期间显示"思考"表情
    EmitJson("{\"type\":\"llm\",\"emotion\":\"thinking\"}");
}

void DeskpetProtocol::FinishChat() {
    if (chat_active_.exchange(false)) {
        pending_pcm_.store(false);
        EmitJson("{\"type\":\"tts\",\"state\":\"stop\"}");
        int64_t ref = t_last_voice_us_.load() ? t_last_voice_us_.load() : t_last_tx_us_.load();
        int64_t d = (esp_timer_get_time() - ref) / 1000;
        int64_t say = t_first_audio_us_.load() ? (t_first_audio_us_.load() - ref) / 1000 : -1;
        int64_t play_ms = t_first_audio_us_.load()
                              ? (esp_timer_get_time() - t_first_audio_us_.load()) / 1000
                              : -1;
        int rr = rx_rate_.load() ? rx_rate_.load() : 16000;
        int64_t audio_ms = rx_bytes_.load() / 2 * 1000 / rr;
        ESP_LOGI(TAG, "[延时] 说完 -> 全部回复播完: %lld ms (其中 说完->首句 %lld ms)", (long long)d,
                 (long long)say);
        ESP_LOGI(TAG, "[音频] 收到 %lld B @%dHz = %lld ms 音频; 播放耗时 %lld ms",
                 (long long)rx_bytes_.load(), rr, (long long)audio_ms, (long long)play_ms);
    }
}
