// ai_voice_capture.cpp - Microphone capture and sentence splitting (ai_voice_capture.h).
#include "ai_voice_capture.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace pulse::app::ai {

using Microsoft::WRL::ComPtr;

struct VoiceRecorder::Shared {
    mutable std::mutex mu;
    Options options;
    std::atomic<bool> stop{false};
    HANDLE wake = nullptr;   // set by Stop: the capture loop ends at once
    // Under mu.
    State state = State::Starting;
    HRESULT error = S_OK;
    uint32_t elapsed_ms = 0;
    bool speaking = false;
    std::array<float, kLevels> levels{};
    std::wstring device;
    std::deque<std::vector<int16_t>> sentences;

    ~Shared() {
        if (wake) CloseHandle(wake);
    }
};

namespace {

// PKEY_Device_FriendlyName ({a45c254e-df1c-4efd-8020-67d146a850e0}, 14),
// spelled out so no property-key library is needed.
constexpr PROPERTYKEY kFriendlyName{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::wstring FriendlyName(IMMDevice* device) {
    std::wstring name;
    ComPtr<IPropertyStore> props;
    if (!device || FAILED(device->OpenPropertyStore(STGM_READ, &props))) return name;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = v.pwszVal;
    PropVariantClear(&v);
    return name;
}

std::wstring DeviceId(IMMDevice* device) {
    std::wstring id;
    LPWSTR raw = nullptr;
    if (device && SUCCEEDED(device->GetId(&raw)) && raw) {
        id = raw;
        CoTaskMemFree(raw);
    }
    return id;
}

std::wstring RegString(HKEY root, const wchar_t* path, const wchar_t* value) {
    wchar_t buffer[64]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, path, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
}

// What the device delivers, turned into 16 kHz mono floats.
class Converter {
public:
    explicit Converter(const WAVEFORMATEX& fmt) {
        channels_ = (std::max)(1, static_cast<int>(fmt.nChannels));
        bits_ = fmt.wBitsPerSample;
        bool is_float = fmt.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (fmt.wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt.cbSize >= 22) {
            const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(fmt);
            // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT / _PCM: {0000000X-0000-0010-8000-00aa00389b71}.
            is_float = ext.SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
        }
        float_ = is_float && bits_ == 32;
        ok_ = (float_ || bits_ == 16 || bits_ == 24 || bits_ == 32) && fmt.nSamplesPerSec > 0;
        step_ = static_cast<double>(fmt.nSamplesPerSec) / kVoiceRate;
        frame_bytes_ = static_cast<size_t>(channels_) * (bits_ / 8);
    }
    bool Ok() const { return ok_; }

    // `silent`: the device marked the packet as silence.
    void Push(const BYTE* data, size_t frames, bool silent, std::vector<float>& out) {
        for (size_t i = 0; i < frames; ++i) {
            float sum = 0.0f;
            if (!silent && data) {
                const BYTE* p = data + i * frame_bytes_;
                for (int ch = 0; ch < channels_; ++ch) sum += Sample(p + static_cast<size_t>(ch) * (bits_ / 8));
            }
            const float x = sum / static_cast<float>(channels_);
            if (step_ >= 1.0) {
                // Down: the average of the samples each output covers.
                acc_ += x;
                ++count_;
                phase_ += 1.0;
                if (phase_ >= step_) {
                    out.push_back(acc_ / static_cast<float>(count_));
                    acc_ = 0.0f;
                    count_ = 0;
                    phase_ -= step_;
                }
            } else {
                // Up (a rare 8 kHz device): repeat.
                phase_ += 1.0;
                while (phase_ >= step_) {
                    out.push_back(x);
                    phase_ -= step_;
                }
            }
        }
    }

private:
    float Sample(const BYTE* p) const {
        if (float_) {
            float f;
            std::memcpy(&f, p, sizeof(f));
            return std::isfinite(f) ? std::clamp(f, -1.0f, 1.0f) : 0.0f;
        }
        if (bits_ == 16) {
            int16_t v;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(v) / 32768.0f;
        }
        if (bits_ == 24) {
            const int32_t v = (static_cast<int32_t>(p[0]) << 8) | (static_cast<int32_t>(p[1]) << 16) |
                              (static_cast<int32_t>(p[2]) << 24);
            return static_cast<float>(v) / 2147483648.0f;
        }
        int32_t v;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) / 2147483648.0f;
    }

    int channels_ = 1;
    int bits_ = 16;
    bool float_ = false;
    bool ok_ = false;
    size_t frame_bytes_ = 2;
    double step_ = 1.0;
    double phase_ = 0.0;
    float acc_ = 0.0f;
    int count_ = 0;
};

// Splits the 16 kHz stream into sentences: 20 ms frames, a speech threshold
// that follows the room's noise floor, 300 ms kept from before the onset and
// 200 ms after the last voiced frame.
class Splitter {
public:
    explicit Splitter(int pause_ms) : pause_frames_(pause_ms > 0 ? (std::max)(10, pause_ms / kFrameMs) : 0) {}

    void Feed(const float* x, size_t n, VoiceRecorder::Shared& sh) {
        for (size_t i = 0; i < n; ++i) {
            frame_.push_back(x[i]);
            if (frame_.size() == kFrame) {
                Frame(sh);
                frame_.clear();
            }
        }
    }

    // The capture ended: what was being said still counts.
    void Flush(VoiceRecorder::Shared& sh) {
        if (in_) End(sh);
    }

private:
    static constexpr int kFrameMs = 20;
    static constexpr size_t kFrame = kVoiceRate / 1000 * kFrameMs;   // 320
    static constexpr size_t kPreroll = 15;                            // 300 ms
    static constexpr int kTail = 10;                                  // 200 ms
    static constexpr int kMinVoiced = 12;                             // 240 ms of speech
    static constexpr size_t kMaxPaused = kVoiceRate * 30;             // 30 s: cut anyway
    static constexpr size_t kMaxManual = kVoiceRate * 120;            // 手动: 2 min a piece

    static int16_t ToPcm(float v) {
        return static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    }

    void Frame(VoiceRecorder::Shared& sh) {
        double energy = 0.0;
        for (const float v : frame_) energy += static_cast<double>(v) * v;
        const float rms = static_cast<float>(std::sqrt(energy / static_cast<double>(kFrame)));
        if (!in_) {
            floor_ = rms < floor_ ? floor_ * 0.9f + rms * 0.1f : floor_ * 0.998f + rms * 0.002f;
            floor_ = std::clamp(floor_, 0.0008f, 0.05f);
        }
        const bool voiced = rms > (std::max)(0.010f, floor_ * 3.0f);
        recent_ = ((recent_ << 1) | (voiced ? 1u : 0u)) & 0x1Fu;
        std::vector<int16_t> pcm(kFrame);
        for (size_t i = 0; i < kFrame; ++i) pcm[i] = ToPcm(frame_[i]);
        if (!in_) {
            preroll_.push_back(std::move(pcm));
            if (preroll_.size() > kPreroll) preroll_.pop_front();
            int count = 0;
            for (uint32_t m = recent_; m; m >>= 1) count += static_cast<int>(m & 1u);
            if (count >= 3) {
                in_ = true;
                sentence_.clear();
                for (const auto& f : preroll_) sentence_.insert(sentence_.end(), f.begin(), f.end());
                preroll_.clear();
                voiced_ = count;
                quiet_ = 0;
            }
        } else {
            sentence_.insert(sentence_.end(), pcm.begin(), pcm.end());
            if (voiced) {
                ++voiced_;
                quiet_ = 0;
            } else {
                ++quiet_;
            }
            if (pause_frames_ > 0 && quiet_ >= pause_frames_) {
                End(sh);
            } else if (sentence_.size() >= (pause_frames_ > 0 ? kMaxPaused : kMaxManual)) {
                Emit(sh);   // a long stretch: hand it over and go on
                voiced_ = 0;
                quiet_ = 0;
            }
        }
        // The waveform: the loudest of three frames, on a 36 dB scale.
        const float level = std::clamp((20.0f * std::log10(rms + 1e-6f) + 52.0f) / 36.0f, 0.0f, 1.0f);
        level_max_ = (std::max)(level_max_, level);
        std::lock_guard<std::mutex> lock(sh.mu);
        sh.elapsed_ms += kFrameMs;
        sh.speaking = in_;
        if (++level_n_ == 3) {
            std::rotate(sh.levels.begin(), sh.levels.begin() + 1, sh.levels.end());
            sh.levels.back() = level_max_;
            level_n_ = 0;
            level_max_ = 0.0f;
        }
    }

    void End(VoiceRecorder::Shared& sh) {
        const int extra = (std::max)(0, quiet_ - kTail);
        const size_t cut = (std::min)(sentence_.size(), static_cast<size_t>(extra) * kFrame);
        sentence_.resize(sentence_.size() - cut);
        if (voiced_ >= kMinVoiced) Emit(sh);
        sentence_.clear();
        in_ = false;
        voiced_ = 0;
        quiet_ = 0;
        recent_ = 0;
        std::lock_guard<std::mutex> lock(sh.mu);
        sh.speaking = false;
    }

    void Emit(VoiceRecorder::Shared& sh) {
        if (sentence_.empty()) return;
        std::lock_guard<std::mutex> lock(sh.mu);
        sh.sentences.push_back(std::move(sentence_));
        sentence_ = {};
    }

    const int pause_frames_;
    std::vector<float> frame_;
    std::deque<std::vector<int16_t>> preroll_;
    std::vector<int16_t> sentence_;
    bool in_ = false;
    int voiced_ = 0;
    int quiet_ = 0;
    uint32_t recent_ = 0;
    float floor_ = 0.004f;
    int level_n_ = 0;
    float level_max_ = 0.0f;
};

void SetState(VoiceRecorder::Shared& sh, VoiceRecorder::State state, HRESULT error = S_OK) {
    std::lock_guard<std::mutex> lock(sh.mu);
    sh.state = state;
    sh.error = error;
}

HRESULT Capture(VoiceRecorder::Shared& sh) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return hr;
    ComPtr<IMMDevice> device;
    if (!sh.options.mic_id.empty()) {
        DWORD state = 0;
        if (FAILED(enumerator->GetDevice(sh.options.mic_id.c_str(), &device)) || FAILED(device->GetState(&state)) ||
            state != DEVICE_STATE_ACTIVE)
            device.Reset();
    }
    if (!device) {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        if (FAILED(hr)) return hr;   // E_NOTFOUND: no microphone at all
    }
    {
        std::lock_guard<std::mutex> lock(sh.mu);
        sh.device = FriendlyName(device.Get());
    }
    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) return hr;   // E_ACCESSDENIED: the privacy switch
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) return hr;
    struct MixGuard {
        WAVEFORMATEX* p;
        ~MixGuard() { CoTaskMemFree(p); }
    } mix_guard{mix};
    Converter converter(*mix);
    if (!converter.Ok()) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0, mix, nullptr);
    if (FAILED(hr)) return hr;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready) return HRESULT_FROM_WIN32(GetLastError());
    struct EventGuard {
        HANDLE h;
        ~EventGuard() { CloseHandle(h); }
    } event_guard{ready};
    hr = client->SetEventHandle(ready);
    if (FAILED(hr)) return hr;
    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) return hr;
    hr = client->Start();
    if (FAILED(hr)) return hr;
    SetState(sh, VoiceRecorder::State::Listening);
    Splitter splitter(sh.options.pause_ms);
    std::vector<float> mono;
    HANDLE waits[2] = {ready, sh.wake};
    while (!sh.stop.load()) {
        WaitForMultipleObjects(2, waits, FALSE, 500);
        if (sh.stop.load()) break;
        UINT32 packet = 0;
        hr = capture->GetNextPacketSize(&packet);
        while (SUCCEEDED(hr) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            mono.clear();
            converter.Push(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, mono);
            capture->ReleaseBuffer(frames);
            splitter.Feed(mono.data(), mono.size(), sh);
            hr = capture->GetNextPacketSize(&packet);
        }
        if (FAILED(hr)) break;   // unplugged (AUDCLNT_E_DEVICE_INVALIDATED)
    }
    client->Stop();
    splitter.Flush(sh);
    return FAILED(hr) ? hr : S_OK;
}

// --test-instance: a 16-bit PCM WAV file plays as the microphone, in real
// time, followed by silence until Stop.
HRESULT PlayFile(VoiceRecorder::Shared& sh) {
    HANDLE file = CreateFileW(sh.options.test_wav.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    std::vector<BYTE> bytes;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 44 && size.QuadPart < 64 * 1024 * 1024) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
            bytes.clear();
    }
    CloseHandle(file);
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    WAVEFORMATEX fmt{};
    const BYTE* data = nullptr;
    size_t data_bytes = 0;
    for (size_t at = 12; at + 8 <= bytes.size();) {
        uint32_t len = 0;
        std::memcpy(&len, bytes.data() + at + 4, 4);
        const size_t body = at + 8;
        if (body + len > bytes.size()) len = static_cast<uint32_t>(bytes.size() - body);
        if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0 && len >= 16) {
            std::memcpy(&fmt, bytes.data() + body, 16);
            fmt.cbSize = 0;
        } else if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
            data = bytes.data() + body;
            data_bytes = len;
        }
        at = body + len + (len & 1u);
    }
    if (!data || fmt.wFormatTag != WAVE_FORMAT_PCM || fmt.wBitsPerSample != 16 || fmt.nChannels == 0)
        return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    {
        std::lock_guard<std::mutex> lock(sh.mu);
        const size_t slash = sh.options.test_wav.find_last_of(L"\\/");
        sh.device = slash == std::wstring::npos ? sh.options.test_wav : sh.options.test_wav.substr(slash + 1);
    }
    Converter converter(fmt);
    Splitter splitter(sh.options.pause_ms);
    SetState(sh, VoiceRecorder::State::Listening);
    const size_t frame_bytes = static_cast<size_t>(fmt.nChannels) * 2;
    const size_t chunk = (std::max)(static_cast<size_t>(1), static_cast<size_t>(fmt.nSamplesPerSec) / 50);   // 20 ms
    const size_t total = data_bytes / frame_bytes;
    std::vector<float> mono;
    for (size_t at = 0; !sh.stop.load(); at += chunk) {
        mono.clear();
        if (at < total) converter.Push(data + at * frame_bytes, (std::min)(chunk, total - at), false, mono);
        else converter.Push(nullptr, chunk, true, mono);
        splitter.Feed(mono.data(), mono.size(), sh);
        WaitForSingleObject(sh.wake, 20);
    }
    splitter.Flush(sh);
    return S_OK;
}

void Run(std::shared_ptr<VoiceRecorder::Shared> sh) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT hr = sh->options.test_wav.empty() ? Capture(*sh) : PlayFile(*sh);
    SetState(*sh, FAILED(hr) ? VoiceRecorder::State::Failed : VoiceRecorder::State::Ended, hr);
    if (SUCCEEDED(init)) CoUninitialize();
}

} // namespace

std::vector<VoiceMic> ListVoiceMics(std::wstring* default_id) {
    std::vector<VoiceMic> mics;
    if (default_id) default_id->clear();
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
            ComPtr<IMMDevice> fallback;
            if (default_id && SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &fallback)))
                *default_id = DeviceId(fallback.Get());
            ComPtr<IMMDeviceCollection> all;
            UINT count = 0;
            if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &all)) &&
                SUCCEEDED(all->GetCount(&count))) {
                for (UINT i = 0; i < count; ++i) {
                    ComPtr<IMMDevice> device;
                    if (FAILED(all->Item(i, &device))) continue;
                    VoiceMic mic{DeviceId(device.Get()), FriendlyName(device.Get())};
                    if (!mic.id.empty()) mics.push_back(std::move(mic));
                }
            }
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return mics;
}

MicAccess ReadMicAccess() {
    constexpr wchar_t kStore[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone";
    constexpr wchar_t kDesktop[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone\\NonPackaged";
    if (RegString(HKEY_LOCAL_MACHINE, kStore, L"Value") == L"Deny" || RegString(HKEY_CURRENT_USER, kStore, L"Value") == L"Deny")
        return MicAccess::DeniedAll;
    if (RegString(HKEY_CURRENT_USER, kDesktop, L"Value") == L"Deny") return MicAccess::DeniedDesktop;
    return MicAccess::Allowed;
}

std::shared_ptr<VoiceRecorder> VoiceRecorder::Start(Options options) {
    std::shared_ptr<VoiceRecorder> recorder(new VoiceRecorder());
    auto sh = std::make_shared<Shared>();
    sh->options = std::move(options);
    sh->wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    recorder->shared_ = sh;
    std::thread([sh]() { Run(sh); }).detach();
    return recorder;
}

VoiceRecorder::~VoiceRecorder() { Stop(); }

void VoiceRecorder::Stop() {
    if (!shared_ || shared_->stop.exchange(true)) return;
    if (shared_->wake) SetEvent(shared_->wake);
}

VoiceRecorder::Snapshot VoiceRecorder::Read() const {
    Snapshot snap;
    if (!shared_) return snap;
    std::lock_guard<std::mutex> lock(shared_->mu);
    snap.state = shared_->state;
    snap.error = shared_->error;
    snap.elapsed_ms = shared_->elapsed_ms;
    snap.speaking = shared_->speaking;
    snap.levels = shared_->levels;
    snap.device = shared_->device;
    return snap;
}

bool VoiceRecorder::PopSentence(std::vector<int16_t>& pcm) {
    if (!shared_) return false;
    std::lock_guard<std::mutex> lock(shared_->mu);
    if (shared_->sentences.empty()) return false;
    pcm = std::move(shared_->sentences.front());
    shared_->sentences.pop_front();
    return true;
}

std::string EncodeVoiceWav(const std::vector<int16_t>& pcm) {
    const uint32_t data_bytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    std::string wav(44 + static_cast<size_t>(data_bytes), '\0');
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav[at], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4);
    put32(4, 36 + data_bytes);
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, WAVE_FORMAT_PCM);
    put16(22, 1);
    put32(24, kVoiceRate);
    put32(28, kVoiceRate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(&wav[36], "data", 4);
    put32(40, data_bytes);
    if (data_bytes) std::memcpy(&wav[44], pcm.data(), data_bytes);
    return wav;
}

std::vector<int16_t> VoiceTestClip() {
    // 0.6 s: a 220 Hz hum that fades in and out, quiet enough to read as no
    // speech; the service answering at all is the test.
    constexpr size_t n = kVoiceRate * 6 / 10;
    std::vector<int16_t> pcm(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kVoiceRate;
        const double env = std::sin(3.14159265358979 * static_cast<double>(i) / static_cast<double>(n));
        pcm[i] = static_cast<int16_t>(std::lround(900.0 * env * std::sin(2.0 * 3.14159265358979 * 220.0 * t)));
    }
    return pcm;
}

} // namespace pulse::app::ai
