// batch_rename_content.h - 批量重命名 "按内容命名": the model reads the text found
// in each file (never the file itself) and suggests a name with its basis.
#pragma once

#include <string>
#include <vector>

namespace pulse::app::rename {

enum class NameFormat { DateTypeSubject, TypeSubjectDate, LikeFolder };
enum class NameLanguage { Chinese, English, FollowText };

struct ContentFile {
    int id = 0;                 // echoed back by the model
    std::wstring name;          // current name with extension
    std::wstring text;          // already compacted (CompactText)
};

struct ContentAsk {
    std::wstring model;
    NameFormat format = NameFormat::DateTypeSubject;
    NameLanguage language = NameLanguage::Chinese;
    std::string reason_language = "Simplified Chinese";   // the UI's: reasons for keep are shown as-is
    std::vector<std::wstring> folder_names;   // LikeFolder: other names in the folder
    std::vector<ContentFile> files;
};

struct ContentSuggestion {
    int id = 0;
    std::wstring name;     // new name without extension (cleaned); empty with keep
    bool keep = false;     // the current name is already clear, or nothing to go on
    std::wstring basis;    // the words the name rests on, or why it keeps the name
};

// Collapses whitespace runs to one space (paragraph breaks to " / ") and cuts
// at max_chars without splitting a surrogate pair.
std::wstring CompactText(const std::wstring& text, size_t max_chars);

std::string ContentRequestBody(const ContentAsk& ask);

// False when the reply is not the expected JSON.
bool ParseContentReply(const std::string& utf8, std::vector<ContentSuggestion>& out);

// A name the file system takes: no \ / : * ? " < > | or control characters,
// no trailing dots or spaces, at most max_chars.
std::wstring CleanName(const std::wstring& name, size_t max_chars = 80);

} // namespace pulse::app::rename
