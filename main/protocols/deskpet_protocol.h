#ifndef DESKPET_PROTOCOL_H
#define DESKPET_PROTOCOL_H

#include "protocol.h"

#include <web_socket.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

// 对接"桌宠自建后端"的协议实现(全部在同一个 WS 上,裸 PCM):
//   wss://<host>/ws/v1/chat?api_key=<key>
//   上行: {"type":"audio_start","request_id":"aN"}  →  {"type":"asr_ready",...}
//         之后发 PCM(pcm_s16le 16k 单声道)二进制帧
//         {"type":"audio_stop","request_id":"aN","reason":"silence"}
//         →  {"type":"asr_end",...} / {"type":"transcript","text":"识别文字"}
//         {"type":"chat","request_id":"cN","text":"..."}
//   下行: {"type":"meta"|"delta"|"audio"|"final"} + 二进制 PCM 帧
class DeskpetProtocol : public Protocol {
public:
    DeskpetProtocol();
    ~DeskpetProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendMcpMessage(const std::string& message) override;

protected:
    bool SendText(const std::string& text) override;

private:
    void OnData(const char* data, size_t len, bool binary);
    void OnJson(const cJSON* root);
    void OnBinaryPcm(const char* data, size_t len);
    void StartChat(const std::string& text);
    void FinishChat();
    void EmitJson(const std::string& json);
    void EmitTtsSentence(const std::string& text);  // 让屏幕显示助手回答(边生成边显示)
    void EmitEmotion(const std::string& emotion);   // 把服务器 emotion 转给 app(驱动表情/舵机)
    std::string ChatUrl() const;

    std::unique_ptr<WebSocket> ws_;
    std::mutex mutex_;

    std::string api_key_;
    std::string chat_url_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> asr_active_{false};     // 已发 audio_start,正在收音频
    std::atomic<bool> chat_active_{false};
    std::atomic<bool> tts_started_{false};
    std::atomic<int> tx_frames_{0};
    // 延迟统计:最后一次发送上行音频的时刻(≈"用户说完")
    std::atomic<int64_t> t_last_tx_us_{0};
    std::atomic<int64_t> t_last_voice_us_{0};  // 最后检测到人声的时刻(真正的"说完")
    std::atomic<int64_t> t_stop_us_{0};        // 发 audio_stop 的时刻
    std::atomic<int64_t> t_first_audio_us_{0}; // 收到第一段回复音频的时刻
    std::atomic<int64_t> t_transcript_us_{0};  // 收到识别文字的时刻
    std::atomic<int64_t> rx_bytes_{0};         // 本轮收到的回复音频总字节
    std::atomic<int> rx_rate_{0};              // 服务器音频采样率
    std::atomic<bool> speech_started_{false};  // 本轮是否已检测到人声(用于静音判停)
    double noise_floor_ = 300.0;               // 自适应噪声底(仅在未检测到人声时更新)
    std::atomic<int64_t> t_audio_start_us_{0}; // 本轮 audio_start 的时刻(用于统计上传时长)

    std::string req_id_;                       // 当前 audio_start 的 request_id
    int req_counter_ = 0;
    std::string chat_text_;                    // 本轮助手回答的累计文本

    // 下行 PCM 元数据(收到 format=pcm 的 audio 消息后,下一个二进制帧即音频)
    std::atomic<bool> pending_pcm_{false};
    std::atomic<int> pending_rate_{16000};
    std::atomic<int> pending_channels_{1};
};

#endif  // DESKPET_PROTOCOL_H
