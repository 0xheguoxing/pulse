// ai_model_key.h - The model service's API key (Pulse 助手, DeepSeek), kept
// only on this machine: encrypted with DPAPI for the current user in
// %LOCALAPPDATA%\Pulse\ai\model_key.bin. Never written to ai.json, the
// activity log or the runtime log.
#pragma once

#include <string>
#include <string_view>

namespace pulse::app::ai {

// Small file, local disk: fine on the UI thread.
bool SaveModelKey(std::wstring_view key);
bool LoadModelKey(std::wstring& key);   // false: none stored or unreadable
bool HasModelKey();
bool ClearModelKey();
// "sk-…a1b2" for the settings page; empty for an empty key.
std::wstring MaskModelKey(std::wstring_view key);

} // namespace pulse::app::ai
