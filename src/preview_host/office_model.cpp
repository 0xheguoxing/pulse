// office_model.cpp - see office_model.h.
#include "office_model.h"
#include "zip_entry.h"
#include "xlsx_parts.h"
#include <objidl.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace pulse::preview {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kW[] = L"http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr wchar_t kRel[] = L"http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr size_t kMaxBlocks = 40;
constexpr size_t kMaxTableRows = 8;
constexpr size_t kDocumentBytes = 32u << 20;   // prefix; the first section's sectPr may sit late
constexpr size_t kStylesBytes = 4u << 20;
constexpr size_t kSheetBytes = 4u << 20;
constexpr size_t kSharedStringsBytes = 8u << 20;
constexpr size_t kSmallPartBytes = 1u << 20;

void SetError(std::wstring* error, const wchar_t* text) {
    if (error) *error = text;
}

// ---- XmlLite, resolved on first use like d2d1/dwrite --------------------------

using CreateXmlReaderFn = HRESULT(WINAPI*)(REFIID, void**, IMalloc*);

CreateXmlReaderFn XmlLiteFactory() {
    static const CreateXmlReaderFn create = [] {
        const HMODULE module = LoadLibraryExW(L"xmllite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<CreateXmlReaderFn>(GetProcAddress(module, "CreateXmlReader"))
                      : nullptr;
    }();
    return create;
}

// Pull reader over an in-memory part with an element-name stack. Truncated
// input simply ends the walk.
class Xml {
public:
    bool Open(const std::vector<unsigned char>& bytes) {
        const CreateXmlReaderFn create = XmlLiteFactory();
        if (!create || bytes.empty()) return false;
        stream_.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
        if (!stream_ || FAILED(create(__uuidof(IXmlReader), reinterpret_cast<void**>(reader_.GetAddressOf()), nullptr)))
            return false;
        reader_->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
        return SUCCEEDED(reader_->SetInput(stream_.Get()));
    }

    // Advances to the next element start, element end or text node.
    bool Next() {
        if (pop_) { stack_.pop_back(); pop_ = false; }
        for (;;) {
            XmlNodeType type = XmlNodeType_None;
            if (reader_->Read(&type) != S_OK) return false;
            if (type == XmlNodeType_Element) {
                kind_ = Kind::Start;
                name_ = LocalName();
                empty_ = reader_->IsEmptyElement() != FALSE;
                parent_ = stack_.empty() ? std::wstring() : stack_.back();
                grand_ = stack_.size() < 2 ? std::wstring() : stack_[stack_.size() - 2];
                stack_.push_back(name_);
                return true;
            }
            if (type == XmlNodeType_EndElement) {
                kind_ = Kind::End;
                name_ = stack_.empty() ? std::wstring() : stack_.back();
                pop_ = true;
                return true;
            }
            if (type == XmlNodeType_Text || type == XmlNodeType_CDATA ||
                type == XmlNodeType_Whitespace) {
                kind_ = Kind::Text;
                const wchar_t* value = nullptr;
                UINT length = 0;
                text_.clear();
                if (SUCCEEDED(reader_->GetValue(&value, &length)) && value) text_.assign(value, length);
                return true;
            }
        }
    }

    // After a Start of an empty element: report its End on the next call.
    bool StartOfEmpty() const { return kind_ == Kind::Start && empty_; }
    void FinishEmpty() { kind_ = Kind::End; pop_ = true; }

    bool Start(std::wstring_view name) const { return kind_ == Kind::Start && name_ == name; }
    bool End(std::wstring_view name) const { return kind_ == Kind::End && name_ == name; }
    bool IsText() const { return kind_ == Kind::Text; }
    bool IsStart() const { return kind_ == Kind::Start; }
    const std::wstring& Name() const { return name_; }
    const std::wstring& Parent() const { return parent_; }
    const std::wstring& Grand() const { return grand_; }
    const std::wstring& Text() const { return text_; }
    size_t Depth() const { return stack_.size(); }   // including the current element

    bool Attr(const wchar_t* local, const wchar_t* ns, std::wstring& out) {
        out.clear();
        if (reader_->MoveToAttributeByName(local, ns) != S_OK) return false;
        const wchar_t* value = nullptr;
        UINT length = 0;
        if (SUCCEEDED(reader_->GetValue(&value, &length)) && value) out.assign(value, length);
        reader_->MoveToElement();
        return true;
    }

private:
    enum class Kind { Start, End, Text };
    std::wstring LocalName() {
        const wchar_t* value = nullptr;
        UINT length = 0;
        if (FAILED(reader_->GetLocalName(&value, &length)) || !value) return {};
        return std::wstring(value, length);
    }

    ComPtr<IStream> stream_;
    ComPtr<IXmlReader> reader_;
    std::vector<std::wstring> stack_;
    std::wstring name_, parent_, grand_, text_;
    Kind kind_ = Kind::Text;
    bool empty_ = false;
    bool pop_ = false;
};

bool OnOff(const std::wstring& value) {   // absent w:val means "on"
    return !(value == L"0" || value == L"false" || value == L"off" || value == L"none");
}

int ToInt(const std::wstring& value, int fallback) {
    if (value.empty()) return fallback;
    wchar_t* end = nullptr;
    const double v = wcstod(value.c_str(), &end);
    return end == value.c_str() || !std::isfinite(v) ? fallback : static_cast<int>(v);
}

DocAlign ParseAlign(const std::wstring& value) {
    if (value == L"center") return DocAlign::Center;
    if (value == L"right" || value == L"end") return DocAlign::Right;
    if (value == L"both" || value == L"distribute") return DocAlign::Justify;
    return DocAlign::Left;
}

std::wstring Trim(const std::wstring& text) {
    size_t a = 0, b = text.size();
    while (a < b && std::iswspace(text[a])) ++a;
    while (b > a && std::iswspace(text[b - 1])) --b;
    return text.substr(a, b - a);
}

// ---- styles.xml -----------------------------------------------------------------

struct Style {
    std::wstring based;
    int half_points = -1;
    int bold = -1;
    int align = -1;
};

struct Styles {
    int default_half_points = 21;   // 10.5 pt when the package declares nothing
    std::wstring default_paragraph;
    std::unordered_map<std::wstring, Style> map;

    template <typename Get>
    int Resolve(std::wstring id, Get get, int fallback) const {
        for (int guard = 0; guard < 10 && !id.empty(); ++guard) {
            const auto it = map.find(id);
            if (it == map.end()) break;
            const int value = get(it->second);
            if (value >= 0) return value;
            id = it->second.based;
        }
        return fallback;
    }
};

void ReadStyles(const std::wstring& path, Styles& styles) {
    std::vector<unsigned char> bytes;
    if (!ReadZipEntry(path, "word/styles.xml", kStylesBytes, bytes, nullptr)) return;
    Xml xml;
    if (!xml.Open(bytes)) return;
    Style* current = nullptr;
    std::wstring value, type, is_default;
    while (xml.Next()) {
        if (xml.IsStart()) {
            const std::wstring& name = xml.Name();
            if (name == L"style") {
                std::wstring id;
                xml.Attr(L"styleId", kW, id);
                current = &styles.map[id];
                if (xml.Attr(L"type", kW, type) && type == L"paragraph" &&
                    xml.Attr(L"default", kW, is_default) && OnOff(is_default))
                    styles.default_paragraph = id;
            } else if (current && name == L"basedOn" && xml.Parent() == L"style") {
                xml.Attr(L"val", kW, current->based);
            } else if (name == L"sz" && xml.Parent() == L"rPr") {
                xml.Attr(L"val", kW, value);
                if (current && xml.Grand() == L"style") current->half_points = ToInt(value, -1);
                else if (xml.Grand() == L"rPrDefault") styles.default_half_points = ToInt(value, 21);
            } else if (current && name == L"b" && xml.Parent() == L"rPr" && xml.Grand() == L"style") {
                const bool has = xml.Attr(L"val", kW, value);
                current->bold = (!has || OnOff(value)) ? 1 : 0;
            } else if (current && name == L"jc" && xml.Parent() == L"pPr" && xml.Grand() == L"style") {
                xml.Attr(L"val", kW, value);
                current->align = static_cast<int>(ParseAlign(value));
            }
            if (xml.StartOfEmpty()) xml.FinishEmpty();
        }
        if (xml.End(L"style")) current = nullptr;
    }
}

// ---- first section page setup (raw scan: it may be past the parsed prefix) ----

std::string_view FindTag(std::string_view xml, std::string_view local, size_t from = 0) {
    // "<prefix:local" or "<local", followed by whitespace, '/' or '>'.
    for (size_t at = xml.find(local, from); at != std::string_view::npos; at = xml.find(local, at + 1)) {
        if (at == 0) continue;
        const char before = xml[at - 1];
        const size_t after = at + local.size();
        if (after >= xml.size()) break;
        const char next = xml[after];
        if ((before == ':' || before == '<') && (next == ' ' || next == '/' || next == '>' ||
                                                 next == '\t' || next == '\r' || next == '\n')) {
            size_t open = xml.rfind('<', at);
            const size_t close = xml.find('>', after);
            if (open == std::string_view::npos || close == std::string_view::npos) break;
            return xml.substr(open, close - open + 1);
        }
    }
    return {};
}

bool TagNumber(std::string_view tag, std::string_view attribute, double& out) {
    for (size_t at = tag.find(attribute); at != std::string_view::npos; at = tag.find(attribute, at + 1)) {
        const char before = at ? tag[at - 1] : ' ';
        const size_t eq = at + attribute.size();
        if ((before == ':' || before == ' ') && eq + 1 < tag.size() && tag[eq] == '=' &&
            (tag[eq + 1] == '"' || tag[eq + 1] == '\'')) {
            out = std::atof(std::string(tag.substr(eq + 2, 24)).c_str());
            return true;
        }
    }
    return false;
}

void ReadFirstSection(const std::vector<unsigned char>& document, DocModel& model) {
    const std::string_view xml(reinterpret_cast<const char*>(document.data()), document.size());
    const std::string_view open = FindTag(xml, "sectPr");
    if (open.empty()) return;
    const size_t start = static_cast<size_t>(open.data() - xml.data());
    size_t end = xml.find("sectPr>", start + open.size());
    if (open.size() >= 2 && open[open.size() - 2] == '/') end = start + open.size();
    if (end == std::string_view::npos) end = (std::min)(xml.size(), start + 4096);
    const std::string_view section = xml.substr(start, end - start);
    double v = 0;
    if (const auto size = FindTag(section, "pgSz"); !size.empty()) {
        if (TagNumber(size, "w", v) && v > 1000) model.page_width = static_cast<float>(v / 20);
        if (TagNumber(size, "h", v) && v > 1000) model.page_height = static_cast<float>(v / 20);
    }
    if (const auto margin = FindTag(section, "pgMar"); !margin.empty()) {
        if (TagNumber(margin, "top", v)) model.margin_top = static_cast<float>(std::fabs(v) / 20);
        if (TagNumber(margin, "left", v)) model.margin_left = static_cast<float>(v / 20);
        if (TagNumber(margin, "right", v)) model.margin_right = static_cast<float>(v / 20);
    }
    if (const auto cols = FindTag(section, "cols"); !cols.empty()) {
        if (TagNumber(cols, "num", v) && v >= 1) model.columns = (std::min)(4, static_cast<int>(v));
        if (TagNumber(cols, "space", v) && v >= 0) model.column_gap = static_cast<float>(v / 20);
    }
    const float text_width = model.page_width - model.margin_left - model.margin_right;
    if (text_width < 36.0f || model.margin_top * 2 > model.page_height - 36.0f) {
        const DocModel defaults;   // nonsense geometry: keep a readable page
        model.margin_top = defaults.margin_top;
        model.margin_left = model.margin_right = model.page_width * 0.12f;
        model.columns = 1;
    }
}

// ---- xlsx helpers -----------------------------------------------------------------

std::string ToUtf8(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>((std::max)(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                   out.data(), n, nullptr, nullptr);
    return out;
}

bool ParseCellRef(const std::wstring& ref, int& row, int& col) {
    size_t i = 0;
    col = 0;
    while (i < ref.size() && std::iswalpha(ref[i])) {
        col = col * 26 + (std::towupper(ref[i]) - L'A' + 1);
        ++i;
        if (col > 16384) return false;
    }
    if (!col || i == ref.size()) return false;
    row = _wtoi(ref.c_str() + i);
    return row > 0;
}

std::wstring FormatNumber(const std::wstring& raw) {
    wchar_t* end = nullptr;
    const double v = wcstod(raw.c_str(), &end);
    if (end == raw.c_str() || *end || !std::isfinite(v)) return raw;
    wchar_t buffer[48];
    swprintf_s(buffer, L"%.10g", v);
    return buffer;
}

} // namespace

bool ReadDocxModel(const std::wstring& path, DocModel& model, std::wstring* error) {
    model = DocModel{};
    std::vector<unsigned char> document;
    if (!ReadZipEntryPrefix(path, "word/document.xml", kDocumentBytes, document, nullptr, error))
        return false;
    ReadFirstSection(document, model);
    Styles styles;
    ReadStyles(path, styles);

    Xml xml;
    if (!xml.Open(document)) {
        SetError(error, L"office-xml-failed");
        return false;
    }
    bool body_seen = false;
    // paragraph state
    bool in_para = false;
    size_t para_depth = 0;
    DocBlock para;
    int para_half = -1, para_align = -1, run_half = -1;
    bool para_bold = false;
    std::wstring para_style;
    // table state
    bool in_table = false;
    size_t table_depth = 0;
    DocBlock table;
    bool row_open = false, cell_open = false;
    bool in_text = false;
    std::wstring value;

    auto finish_paragraph = [&] {
        const std::wstring style = para_style.empty() ? styles.default_paragraph : para_style;
        const int half = para_half >= 0 ? para_half
            : styles.Resolve(style, [](const Style& s) { return s.half_points; }, styles.default_half_points);
        para.text = Trim(para.text);
        para.points = (std::clamp)(half / 2.0f, 4.0f, 200.0f);
        para.bold = para_bold || styles.Resolve(style, [](const Style& s) { return s.bold; }, 0) == 1;
        para.align = static_cast<DocAlign>(para_align >= 0 ? para_align
            : styles.Resolve(style, [](const Style& s) { return s.align; }, 0));
        model.blocks.push_back(std::move(para));
        para = DocBlock{};
        in_para = false;
    };

    while (model.blocks.size() < kMaxBlocks && xml.Next()) {
        if (xml.IsStart()) {
            const std::wstring& name = xml.Name();
            if (name == L"body") body_seen = true;
            if (name == L"p" && xml.Parent() == L"body") {
                in_para = true;
                para_depth = xml.Depth();
                para = DocBlock{};
                para_half = para_align = run_half = -1;
                para_bold = false;
                para_style.clear();
            } else if (name == L"tbl" && xml.Parent() == L"body") {
                in_table = true;
                table_depth = xml.Depth();
                table = DocBlock{};
                table.table = true;
                row_open = cell_open = false;
            } else if (in_para) {
                if (name == L"pStyle" && xml.Parent() == L"pPr" && xml.Grand() == L"p") {
                    xml.Attr(L"val", kW, para_style);
                } else if (name == L"jc" && xml.Parent() == L"pPr" && xml.Grand() == L"p") {
                    xml.Attr(L"val", kW, value);
                    para_align = static_cast<int>(ParseAlign(value));
                } else if (name == L"b") {
                    const bool has = xml.Attr(L"val", kW, value);
                    if (!has || OnOff(value)) para_bold = true;
                } else if (name == L"r") {
                    run_half = -1;
                } else if (name == L"sz" && xml.Parent() == L"rPr" && xml.Grand() == L"r") {
                    xml.Attr(L"val", kW, value);
                    run_half = ToInt(value, -1);
                } else if (name == L"t") {
                    in_text = true;
                    if (run_half >= 0 && para_half < 0) para_half = run_half;
                } else if ((name == L"tab" || name == L"br") && xml.Parent() == L"r") {
                    para.text += L' ';
                }
            } else if (in_table) {
                if (name == L"tr" && xml.Depth() == table_depth + 1) {
                    row_open = table.rows.size() < kMaxTableRows;
                    if (row_open) table.rows.emplace_back();
                    cell_open = false;
                } else if (name == L"tc" && row_open && xml.Depth() == table_depth + 2) {
                    table.rows.back().emplace_back();
                    cell_open = true;
                } else if (name == L"t" && cell_open) {
                    in_text = true;
                } else if (name == L"p" && cell_open && !table.rows.back().back().empty()) {
                    table.rows.back().back() += L' ';
                }
            }
            if (xml.StartOfEmpty()) xml.FinishEmpty(); else continue;
        }
        if (xml.IsText()) {
            if (in_text) {
                if (in_para) para.text += xml.Text();
                else if (in_table && cell_open) table.rows.back().back() += xml.Text();
            }
            continue;
        }
        // element end (real or of an empty element)
        const std::wstring& name = xml.Name();
        if (name == L"t") in_text = false;
        if (in_para && name == L"p" && xml.Depth() == para_depth) finish_paragraph();
        if (in_table && name == L"tbl" && xml.Depth() == table_depth) {
            for (auto& row : table.rows)
                for (auto& cell : row) cell = Trim(cell);
            model.blocks.push_back(std::move(table));
            table = DocBlock{};
            in_table = false;
        }
    }
    if (!body_seen) {
        SetError(error, L"office-not-word");
        return false;
    }
    return true;
}

bool ReadXlsxModel(const std::wstring& path, int rows, int cols, SheetModel& model,
                   std::wstring* error) {
    model = SheetModel{};
    rows = (std::clamp)(rows, 1, 200);
    cols = (std::clamp)(cols, 1, 60);
    // First sheet in workbook order, through its relationship id.
    std::vector<unsigned char> bytes;
    std::wstring sheet_rid, value;
    // The workbook from the package relationships; its sheet and shared
    // strings from the workbook's (generators may rename every part).
    std::string workbook_part = "xl/workbook.xml";
    if (ReadZipEntry(path, "_rels/.rels", kSmallPartBytes, bytes, nullptr)) {
        Xml xml;
        if (xml.Open(bytes)) {
            std::wstring type, target;
            while (xml.Next()) {
                if (xml.Start(L"Relationship") && xml.Attr(L"Type", L"", type) &&
                    RelationshipTypeIs(ToUtf8(type), "officeDocument") &&
                    xml.Attr(L"Target", L"", target) && !target.empty()) {
                    workbook_part = ResolvePartTarget("", ToUtf8(target));
                    break;
                }
                if (xml.StartOfEmpty()) xml.FinishEmpty();
            }
        }
    }
    if (!ReadZipEntry(path, workbook_part, kSmallPartBytes, bytes, error)) {
        if (workbook_part == "xl/workbook.xml") return false;
        workbook_part = "xl/workbook.xml";
        if (!ReadZipEntry(path, workbook_part, kSmallPartBytes, bytes, error)) return false;
    }
    {
        Xml xml;
        if (!xml.Open(bytes)) { SetError(error, L"office-xml-failed"); return false; }
        while (xml.Next()) {
            if (xml.Start(L"sheet")) { xml.Attr(L"id", kRel, sheet_rid); break; }
            if (xml.StartOfEmpty()) xml.FinishEmpty();
        }
    }
    std::string sheet_part = "xl/worksheets/sheet1.xml", shared_part = "xl/sharedStrings.xml";
    if (ReadZipEntry(path, RelationshipsPart(workbook_part), kSmallPartBytes, bytes, nullptr)) {
        Xml xml;
        if (xml.Open(bytes)) {
            std::wstring id, type, target;
            while (xml.Next()) {
                if (xml.Start(L"Relationship") && xml.Attr(L"Target", L"", target) && !target.empty()) {
                    if (!xml.Attr(L"Id", L"", id)) id.clear();
                    if (!xml.Attr(L"Type", L"", type)) type.clear();
                    const std::string part = ResolvePartTarget(workbook_part, ToUtf8(target));
                    if (!sheet_rid.empty() && id == sheet_rid) sheet_part = part;
                    else if (RelationshipTypeIs(ToUtf8(type), "sharedStrings")) shared_part = part;
                }
                if (xml.StartOfEmpty()) xml.FinishEmpty();
            }
        }
    }
    std::vector<std::wstring> shared;
    if (ReadZipEntryPrefix(path, shared_part, kSharedStringsBytes, bytes, nullptr, nullptr)) {
        Xml xml;
        if (xml.Open(bytes)) {
            bool in_si = false, in_t = false, in_phonetic = false;
            while (xml.Next()) {
                if (xml.IsStart()) {
                    if (xml.Name() == L"si") { in_si = true; shared.emplace_back(); }
                    else if (xml.Name() == L"rPh") in_phonetic = true;
                    else if (xml.Name() == L"t" && in_si && !in_phonetic) in_t = true;
                    if (xml.StartOfEmpty()) xml.FinishEmpty(); else continue;
                }
                if (xml.IsText()) { if (in_t) shared.back() += xml.Text(); continue; }
                if (xml.Name() == L"t") in_t = false;
                else if (xml.Name() == L"rPh") in_phonetic = false;
                else if (xml.Name() == L"si") in_si = false;
            }
        }
    }
    if (!ReadZipEntryPrefix(path, sheet_part, kSheetBytes, bytes, nullptr, error)) return false;
    Xml xml;
    if (!xml.Open(bytes)) { SetError(error, L"office-xml-failed"); return false; }
    std::map<std::pair<int, int>, std::wstring> cells;
    SharedFormulas formulas;
    int first_row = 0, min_col = 0;
    int row = 0, col = 0, row_ref = 0, next_col = 1;
    std::wstring type, raw, formula, formula_kind, formula_group;
    bool in_cell = false, in_value = false, in_inline = false, in_formula = false;
    bool done = false;
    while (!done && xml.Next()) {
        if (xml.IsStart()) {
            const std::wstring& name = xml.Name();
            if (name == L"row") {
                const int r = xml.Attr(L"r", L"", value) ? _wtoi(value.c_str()) : 0;
                row_ref = r > 0 ? r : row_ref + 1;
                next_col = 1;
            } else if (name == L"c") {
                // r is optional: a cell without it follows the previous one.
                int ref_row = 0, ref_col = 0;
                if (xml.Attr(L"r", L"", value) && ParseCellRef(value, ref_row, ref_col)) {
                    row = ref_row;
                    col = ref_col;
                } else {
                    row = row_ref;
                    col = next_col;
                }
                in_cell = row > 0 && col > 0;
                row_ref = row;
                next_col = col + 1;
                if (!xml.Attr(L"t", L"", type)) type.clear();
                raw.clear();
                formula.clear();
            } else if (in_cell && name == L"v") {
                in_value = true;
            } else if (in_cell && name == L"f") {
                in_formula = true;
                if (!xml.Attr(L"t", L"", formula_kind)) formula_kind.clear();
                if (!xml.Attr(L"si", L"", formula_group)) formula_group.clear();
            } else if (in_cell && name == L"t" && type == L"inlineStr") {
                in_inline = true;
            }
            if (xml.StartOfEmpty()) xml.FinishEmpty(); else continue;
        }
        if (xml.IsText()) {
            if (in_value || in_inline) raw += xml.Text();
            else if (in_formula) formula += xml.Text();
            continue;
        }
        const std::wstring& name = xml.Name();
        if (name == L"v") in_value = false;
        else if (name == L"t") in_inline = false;
        else if (name == L"f" && in_formula) {
            in_formula = false;
            if (formula_kind == L"shared" && !formula_group.empty())
                formulas.Resolve(static_cast<unsigned>(_wtoi(formula_group.c_str())), row, col, formula);
        }
        else if (name == L"c" && in_cell) {
            in_cell = false;
            std::wstring shown;
            if (Trim(raw).empty() && !formula.empty()) {
                shown = L"=" + formula;   // never calculated: no cached result
            } else if (type == L"s") {
                const int index = _wtoi(raw.c_str());
                if (index >= 0 && static_cast<size_t>(index) < shared.size()) shown = shared[index];
            } else if (type == L"b") {
                shown = raw == L"1" ? L"TRUE" : (raw.empty() ? L"" : L"FALSE");
            } else if (type.empty() || type == L"n") {
                shown = FormatNumber(Trim(raw));
            } else {
                shown = raw;   // str, inlineStr, e
            }
            if (Trim(shown).empty()) continue;
            if (!first_row) first_row = row;
            if (row >= first_row + rows) { done = true; continue; }
            if (row < first_row) continue;
            min_col = min_col ? (std::min)(min_col, col) : col;
            cells[{row, col}] = shown;
        }
    }
    if (!first_row) {   // empty sheet: an empty grid, still a valid workbook
        model.grid.assign(static_cast<size_t>(rows), std::vector<std::wstring>(static_cast<size_t>(cols)));
        return true;
    }
    model.first_row = first_row;
    model.first_col = min_col;
    model.grid.assign(static_cast<size_t>(rows), std::vector<std::wstring>(static_cast<size_t>(cols)));
    for (const auto& [key, text] : cells) {
        const int r = key.first - first_row, c = key.second - min_col;
        if (r >= 0 && r < rows && c >= 0 && c < cols) model.grid[r][c] = text;
    }
    return true;
}

} // namespace pulse::preview
