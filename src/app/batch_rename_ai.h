// batch_rename_ai.h - 批量重命名 "一句话生成步骤": the request sent to the model
// and the steps read back from its reply.
//
// Only the sentence, the number of files and their extensions are sent; file
// names and contents never leave the PC. The model answers with one JSON
// object that lists steps in the engine's terms (batch_rename_plan.h).
#pragma once
#include "batch_rename_plan.h"

#include <string>
#include <utility>
#include <vector>

namespace pulse::json { class Value; }

namespace pulse::app::rename {

struct AiAsk {
    std::wstring sentence;
    std::wstring model;                                   // id sent as "model"
    size_t files = 0;
    std::vector<std::pair<std::wstring, size_t>> exts;    // ".jpg" -> count, most first
};

// The chat/completions body (stream: true, JSON output, no thinking).
std::string AiRequestBody(const AiAsk& ask);

struct AiReply {
    std::vector<Step> steps;   // ai = true; template tokens are the English ones
    bool has_order = false;
    Order order = Order::List;
    bool daily = false;        // numbers start over each day
    std::wstring note;         // the model's reason when it cannot do it
};

// False when the reply is not the expected JSON (or lists no usable step and
// gives no reason).
bool ParseAiReply(const std::string& utf8, AiReply& out);
// The same from an object already parsed (Pulse 助手's rename_files call):
// "steps", "order", "daily" and "note".
bool ReadAiSteps(const json::Value& root, AiReply& out);

} // namespace pulse::app::rename
