#include "mcp_tools.h"
#include <cstring>

namespace pulse::ai_host {
namespace {

constexpr int64_t kMaxPath = 32767;

std::vector<ToolDef> MakeTools() {
    const char* kPathsNote = "Absolute Windows paths, e.g. C:\\Users\\me\\Documents\\report.pdf.";
    std::vector<ToolDef> tools;
    tools.push_back({"get_context", "Current Pulse view",
        "Returns the folder open in Pulse's active tab and the items the user has selected there (up to 200 "
        "paths). Use it to find out what \"this folder\" or \"these files\" refers to.",
        true, false, true, {}});
    tools.push_back({"list_folder", "List a folder",
        "Lists the items in a folder: name, size, modified time and whether each is a folder. File contents are "
        "never read. Pass the returned cursor to get the next page.",
        true, false, true,
        {{"path", ParamKind::String, true, 1, kMaxPath, "Absolute path of the folder."},
         {"limit", ParamKind::Integer, false, 1, 500, "Items per page (default 100)."},
         {"cursor", ParamKind::String, false, 1, 256, "The cursor from the previous page."}}});
    tools.push_back({"search", "Search files",
        "Finds files and folders by name through Pulse's index, optionally inside one folder, of one type or "
        "modified after a date. Returns paths, names, sizes and modified times; matches file names, not contents.",
        true, false, true,
        {{"query", ParamKind::String, true, 1, 256, "Words to find in names; Pulse's search syntax is accepted."},
         {"folder", ParamKind::String, false, 1, kMaxPath, "Only search inside this folder."},
         {"type", ParamKind::String, false, 1, 16, "Only this kind of item.",
          {"file", "folder", "image", "video", "audio", "document", "archive"}},
         {"modified_after", ParamKind::Date, false, 10, 10, "Only items modified on or after this day (YYYY-MM-DD)."},
         {"limit", ParamKind::Integer, false, 1, 200, "Maximum results (default 50)."}}});
    tools.push_back({"get_properties", "File properties",
        "Returns properties of files and folders: size, times, and where available image dimensions, page "
        "count, installer version and an EXIF summary. File contents are not returned.",
        true, false, true,
        {{"paths", ParamKind::StringArray, true, 1, 50, kPathsNote}}});
    tools.push_back({"reveal", "Show in Pulse",
        "Shows items to the user in Pulse: opens their folder and selects them (one tab per client is reused). "
        "Changes nothing on disk.",
        true, false, true,
        {{"paths", ParamKind::StringArray, true, 1, 200, kPathsNote}}});
    tools.push_back({"create_zip", "Create a zip archive",
        "Packs files and folders into a new .zip archive. Pulse asks the user to confirm first unless they chose "
        "to always allow this for the destination folder; the archive can be undone with undo_last. Existing "
        "files are never overwritten.",
        false, false, false,
        {{"paths", ParamKind::StringArray, true, 1, 1000, kPathsNote},
         {"dest_folder", ParamKind::String, false, 1, kMaxPath,
          "Folder for the archive (default: the folder of the first item)."},
         {"name", ParamKind::String, false, 1, 255, "File name of the archive; .zip is added if missing."}}});
    tools.push_back({"undo_last", "Undo the last AI change",
        "Undoes the most recent change an AI client made through Pulse (for example a zip it created, which goes "
        "to the Recycle Bin). Refuses when the user's own last operation is on top.",
        false, true, false, {}});
    return tools;
}

const char* KindType(ParamKind kind) {
    switch (kind) {
        case ParamKind::Integer: return "integer";
        case ParamKind::StringArray: return "array";
        default: return "string";
    }
}

bool HasNul(const std::string& s) { return s.find('\0') != std::string::npos; }

// Length in UTF-16 code units, the way Windows counts paths.
int64_t Utf16Length(const std::string& s) {
    int64_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) == 0x80) continue;    // continuation byte
        n += c >= 0xF0 ? 2 : 1;              // 4-byte sequences are surrogate pairs
    }
    return n;
}

bool IsDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
        if (s[i] < '0' || s[i] > '9') return false;
    const int month = (s[5] - '0') * 10 + (s[6] - '0');
    const int day = (s[8] - '0') * 10 + (s[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

std::string CheckString(const Param& p, const json::Value& v, const std::string& where) {
    if (!v.is_string()) return where + " must be a string";
    const std::string& s = v.as_string();
    if (HasNul(s)) return where + " must not contain NUL characters";
    const int64_t n = Utf16Length(s);
    if (n < p.minimum) return where + (p.minimum == 1 ? " must not be empty" : " is too short");
    if (n > p.maximum) return where + " is too long (at most " + std::to_string(p.maximum) + " characters)";
    if (p.kind == ParamKind::Date && !IsDate(s)) return where + " must be a date as YYYY-MM-DD";
    if (!p.choices.empty()) {
        bool known = false;
        for (const char* choice : p.choices) known |= s == choice;
        if (!known) {
            std::string list;
            for (const char* choice : p.choices) list += (list.empty() ? "" : ", ") + std::string(choice);
            return where + " must be one of: " + list;
        }
    }
    return {};
}

} // namespace

const std::vector<ToolDef>& Tools() {
    static const std::vector<ToolDef> tools = MakeTools();
    return tools;
}

const ToolDef* FindTool(std::string_view name) {
    for (const ToolDef& tool : Tools())
        if (name == tool.name) return &tool;
    return nullptr;
}

json::Value InputSchema(const ToolDef& tool) {
    json::Value properties = json::Value::Object();
    json::Value required = json::Value::Array();
    for (const Param& p : tool.params) {
        json::Value prop = json::Value::Object();
        prop.set("type", KindType(p.kind));
        prop.set("description", p.description);
        switch (p.kind) {
            case ParamKind::Integer:
                prop.set("minimum", p.minimum);
                prop.set("maximum", p.maximum);
                break;
            case ParamKind::StringArray: {
                json::Value items = json::Value::Object();
                items.set("type", "string");
                items.set("minLength", 1);
                prop.set("items", std::move(items));
                prop.set("minItems", p.minimum);
                prop.set("maxItems", p.maximum);
                break;
            }
            case ParamKind::Date:
                prop.set("format", "date");
                prop.set("pattern", "^[0-9]{4}-[0-9]{2}-[0-9]{2}$");
                break;
            case ParamKind::String:
                prop.set("minLength", p.minimum);
                prop.set("maxLength", p.maximum);
                if (!p.choices.empty()) {
                    json::Value choices = json::Value::Array();
                    for (const char* choice : p.choices) choices.push(choice);
                    prop.set("enum", std::move(choices));
                }
                break;
        }
        properties.set(p.name, std::move(prop));
        if (p.required) required.push(p.name);
    }
    json::Value schema = json::Value::Object();
    schema.set("type", "object");
    schema.set("properties", std::move(properties));
    if (required.size()) schema.set("required", std::move(required));
    schema.set("additionalProperties", false);
    return schema;
}

json::Value ToolsList() {
    json::Value list = json::Value::Array();
    for (const ToolDef& tool : Tools()) {
        json::Value annotations = json::Value::Object();
        annotations.set("title", tool.title);
        annotations.set("readOnlyHint", tool.read_only);
        annotations.set("destructiveHint", tool.destructive);
        annotations.set("idempotentHint", tool.idempotent);
        annotations.set("openWorldHint", false);
        json::Value t = json::Value::Object();
        t.set("name", tool.name);
        t.set("title", tool.title);
        t.set("description", tool.description);
        t.set("inputSchema", InputSchema(tool));
        t.set("annotations", std::move(annotations));
        list.push(std::move(t));
    }
    json::Value result = json::Value::Object();
    result.set("tools", std::move(list));
    return result;
}

std::string ValidateArguments(const ToolDef& tool, const json::Value& args) {
    if (!args.is_object()) return "arguments must be an object";
    for (const std::string& key : args.keys()) {
        bool known = false;
        for (const Param& p : tool.params) known |= key == p.name;
        if (!known) return "unknown argument \"" + key + "\"";
    }
    for (const Param& p : tool.params) {
        const json::Value* v = args.find(p.name);
        const std::string where = std::string("\"") + p.name + "\"";
        if (!v) {
            if (p.required) return where + " is required";
            continue;
        }
        switch (p.kind) {
            case ParamKind::Integer: {
                int64_t n = 0;
                if (!v->get_int(n)) return where + " must be an integer";
                if (n < p.minimum || n > p.maximum)
                    return where + " must be between " + std::to_string(p.minimum) + " and " + std::to_string(p.maximum);
                break;
            }
            case ParamKind::StringArray: {
                if (!v->is_array()) return where + " must be an array of strings";
                const auto count = static_cast<int64_t>(v->size());
                if (count < p.minimum) return where + " must not be empty";
                if (count > p.maximum) return where + " has too many items (at most " + std::to_string(p.maximum) + ")";
                const Param item{p.name, ParamKind::String, true, 1, kMaxPath, ""};
                for (size_t i = 0; i < v->size(); ++i) {
                    std::string reason = CheckString(item, v->at(i), where + "[" + std::to_string(i) + "]");
                    if (!reason.empty()) return reason;
                }
                break;
            }
            case ParamKind::String:
            case ParamKind::Date: {
                std::string reason = CheckString(p, *v, where);
                if (!reason.empty()) return reason;
                break;
            }
        }
    }
    return {};
}

} // namespace pulse::ai_host
