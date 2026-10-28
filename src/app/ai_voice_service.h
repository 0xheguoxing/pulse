// ai_voice_service.h - The speech recognition service behind the composer's
// microphone (third batch, flow V): any OpenAI-format endpoint,
// POST {base}/audio/transcriptions (multipart: file, model) -> {"text": ...}.
// 硅基流动 (FunAudioLLM/SenseVoiceSmall) is the default; OpenAI and a custom
// address work the same way.
//
// Blocking calls: run them on a worker thread. The key travels only in the
// Authorization header and is wiped afterwards; the audio is sent once and
// never kept.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app::ai {

struct VoiceEndpoint {
    std::wstring base_url;   // https://api.siliconflow.cn/v1
    std::wstring key;
    std::string model;       // FunAudioLLM/SenseVoiceSmall
};

struct VoiceAnswer {
    int http_status = 0;               // 200: fine; 0: no HTTP answer
    DWORD win32_error = 0;
    uint32_t ms = 0;                   // round trip
    std::string text;                  // TranscribeVoice: the words (UTF-8, trimmed)
    std::vector<std::string> models;   // ListVoiceModels: speech recognition ids, the service's order
    std::string error_type;
    std::string error_message;
    bool Ok() const { return http_status == 200; }
};

// The service's speech recognition models: 硅基流动 is asked for
// sub_type=speech-to-text; elsewhere the ids that read like speech
// recognition (whisper, transcribe, asr, sensevoice, …), or every id when none
// does (a custom server).
VoiceAnswer ListVoiceModels(VoiceEndpoint endpoint);
// One sentence: `wav` from EncodeVoiceWav (ai_voice_capture.h).
VoiceAnswer TranscribeVoice(VoiceEndpoint endpoint, const std::string& wav);

// The preset services of the connection card.
struct VoicePreset {
    const char* id;            // ai.json voice.service
    const wchar_t* base_url;   // empty for custom
    const char* model;         // the one picked when the service lists it
};
const VoicePreset* FindVoicePreset(std::string_view id);
// The presets in the card's order: 硅基流动, OpenAI, 自定义.
const VoicePreset* VoicePresets(size_t& count);
// The model to preselect from a listed set: the preset's when listed, else the first.
std::string PickVoiceModel(std::string_view service, const std::vector<std::string>& models);
// "SenseVoiceSmall" for "FunAudioLLM/SenseVoiceSmall": the part after the last '/'.
std::wstring ShortVoiceModel(const std::string& model);

} // namespace pulse::app::ai
