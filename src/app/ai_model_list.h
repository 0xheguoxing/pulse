// ai_model_list.h - The connected service's models, as the service names them.
//
// GET /models lists the ids a service accepts; Pulse shows and sends them
// unchanged (another service's dated ids stay whole). The list of the last
// endpoint is cached in %LOCALAPPDATA%\Pulse\ai\models.txt. UI thread only.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace pulse::app::ai {

// base_url's models in the service's order: the cached list, else for the
// official DeepSeek endpoint the two it serves, else none.
const std::vector<std::string>& ListedModels(const std::wstring& base_url);

// A successful GET /models of base_url (empty lists are ignored).
void StoreListedModels(const std::wstring& base_url, const std::vector<std::string>& ids);

// Fetches base_url's list in the background, with the stored key, when it is
// missing or a day old (a failed try waits 10 minutes). hwnd is repainted
// once a new list is in.
void RefreshListedModels(HWND hwnd, const std::wstring& base_url);

} // namespace pulse::app::ai
