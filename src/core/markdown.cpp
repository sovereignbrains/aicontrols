#include "core/markdown.h"

#include <cwctype>

namespace aic {
namespace {

bool IsWordChar(wchar_t c) {
    return std::iswalnum(c) != 0;
}

bool IsSpace(wchar_t c) {
    return std::iswspace(c) != 0;
}

bool EndsWith(const std::wstring& text, std::wstring_view tail) {
    return text.size() >= tail.size() && std::wstring_view(text).substr(text.size() - tail.size()) == tail;
}

// Appends `s` to out.text without markup; spans are recorded after their content (inner ones first).
void Inline(std::wstring_view s, MdText& out) {
    const auto styled = [&](std::wstring_view inner, unsigned flag, bool nested) {
        const size_t start = out.text.size();
        if (nested) {
            Inline(inner, out);
        } else {
            out.text.append(inner);
        }
        if (out.text.size() > start) {
            out.spans.push_back(MdSpan{start, out.text.size() - start, flag, 1.f, false});
        }
    };

    size_t i = 0;
    while (i < s.size()) {
        const wchar_t c = s[i];
        if (c == L'\\' && i + 1 < s.size() && std::wstring_view(L"\\`*_[]()#-+.!>|").find(s[i + 1]) != std::wstring_view::npos) {
            out.text += s[i + 1];
            i += 2;
            continue;
        }
        if (c == L'`') {
            size_t ticks = 0;
            while (i + ticks < s.size() && s[i + ticks] == L'`') {
                ++ticks;
            }
            const std::wstring marker(ticks, L'`');
            const size_t close = s.find(marker, i + ticks);
            if (close != std::wstring_view::npos && close > i + ticks) {
                std::wstring_view inner = s.substr(i + ticks, close - i - ticks);
                if (inner.size() >= 2 && inner.front() == L' ' && inner.back() == L' ') {
                    inner = inner.substr(1, inner.size() - 2);
                }
                styled(inner, kMdCode, false);
                i = close + ticks;
                continue;
            }
            out.text.append(marker);
            i += ticks;
            continue;
        }
        if ((c == L'*' || c == L'_') && i + 1 < s.size() && s[i + 1] == c) {
            const bool boundary = c == L'*' || i == 0 || !IsWordChar(s[i - 1]);
            const std::wstring marker(2, c);
            size_t close = s.find(marker, i + 2);
            while (close != std::wstring_view::npos && close + 2 < s.size() && s[close + 2] == c) {
                ++close;  // a run of three: the last two close the bold, the first belongs to the italic inside
            }
            if (boundary && close != std::wstring_view::npos && close > i + 2 && !IsSpace(s[i + 2]) &&
                !IsSpace(s[close - 1])) {
                styled(s.substr(i + 2, close - i - 2), kMdBold, true);
                i = close + 2;
                continue;
            }
        } else if ((c == L'*' || c == L'_') && i + 1 < s.size() && !IsSpace(s[i + 1])) {
            const bool boundary = c == L'*' || i == 0 || !IsWordChar(s[i - 1]);
            size_t close = std::wstring_view::npos;
            for (size_t j = i + 2; boundary && j < s.size(); ++j) {
                if (s[j] != c || IsSpace(s[j - 1]) || (j + 1 < s.size() && s[j + 1] == c)) {
                    continue;
                }
                if (c == L'_' && j + 1 < s.size() && IsWordChar(s[j + 1])) {
                    continue;
                }
                close = j;
                break;
            }
            if (close != std::wstring_view::npos) {
                styled(s.substr(i + 1, close - i - 1), kMdItalic, true);
                i = close + 1;
                continue;
            }
        }
        if (c == L'[') {
            const size_t middle = s.find(L"](", i + 1);
            const size_t end = middle == std::wstring_view::npos ? middle : s.find(L')', middle + 2);
            if (end != std::wstring_view::npos && middle > i + 1) {
                styled(s.substr(i + 1, middle - i - 1), kMdLink, true);
                i = end + 1;
                continue;
            }
        }
        out.text += c;
        ++i;
    }
}

// Ends the current line; the invariant between lines: out.text is empty or ends with '\n'.
void EndLine(MdText& out) {
    out.text += L'\n';
}

// A blank line before a block, unless there already is one (or nothing yet).
void Separate(MdText& out) {
    if (!out.text.empty() && !EndsWith(out.text, L"\n\n")) {
        out.text += L'\n';
    }
}

}  // namespace

MdText ParseMarkdown(std::wstring_view source) {
    MdText out;
    bool inCode = false;
    size_t codeStart = 0;

    const auto closeCode = [&] {
        inCode = false;
        size_t end = out.text.size();
        while (end > codeStart && out.text[end - 1] == L'\n') {
            --end;
        }
        if (end > codeStart) {
            out.spans.push_back(MdSpan{codeStart, end - codeStart, kMdCode, 1.f, true});
        }
        out.text.resize(end);
        out.text += L"\n\n";
    };

    size_t pos = 0;
    while (pos <= source.size()) {
        size_t newline = source.find(L'\n', pos);
        if (newline == std::wstring_view::npos) {
            newline = source.size();
        }
        std::wstring_view line = source.substr(pos, newline - pos);
        pos = newline + 1;
        if (!line.empty() && line.back() == L'\r') {
            line.remove_suffix(1);
        }
        size_t indent = 0;
        while (indent < line.size() && (line[indent] == L' ' || line[indent] == L'\t')) {
            ++indent;
        }
        const std::wstring_view rest = line.substr(indent);

        if (rest.starts_with(L"```")) {
            if (inCode) {
                closeCode();
            } else {
                inCode = true;
                Separate(out);
                codeStart = out.text.size();
            }
            continue;
        }
        if (inCode) {
            out.text.append(line);
            EndLine(out);
            continue;
        }

        if (rest.empty()) {
            if (!out.text.empty() && !EndsWith(out.text, L"\n\n")) {
                EndLine(out);
            }
            continue;
        }

        size_t hashes = 0;
        while (hashes < rest.size() && rest[hashes] == L'#') {
            ++hashes;
        }
        if (hashes >= 1 && hashes <= 6 && hashes < rest.size() && rest[hashes] == L' ') {
            Separate(out);
            const size_t start = out.text.size();
            Inline(rest.substr(hashes + 1), out);
            static constexpr float scales[] = {1.f, 1.45f, 1.3f, 1.15f, 1.08f, 1.04f, 1.f};
            out.spans.push_back(MdSpan{start, out.text.size() - start, kMdHeading | kMdBold, scales[hashes], false});
            EndLine(out);
            continue;
        }

        const bool rule = rest.size() >= 3 && rest.find_first_not_of(rest[0]) == std::wstring_view::npos &&
                          (rest[0] == L'-' || rest[0] == L'*' || rest[0] == L'_');
        if (rule) {
            const size_t start = out.text.size();
            out.text.append(24, L'─');
            out.spans.push_back(MdSpan{start, 24, kMdQuote, 1.f, false});
            EndLine(out);
            continue;
        }

        if (rest.starts_with(L"> ") || rest == L">") {
            const size_t start = out.text.size();
            Inline(rest.size() > 2 ? rest.substr(2) : std::wstring_view(), out);
            out.spans.push_back(MdSpan{start, out.text.size() - start, kMdQuote, 1.f, false});
            EndLine(out);
            continue;
        }

        if (rest.size() >= 2 && (rest[0] == L'-' || rest[0] == L'*' || rest[0] == L'+') && rest[1] == L' ') {
            out.text.append(indent / 2 * 2, L' ');
            out.text += L"• ";
            Inline(rest.substr(2), out);
            EndLine(out);
            continue;
        }

        out.text.append(indent > 0 && rest.size() > 1 && std::iswdigit(rest[0]) ? indent / 2 * 2 : 0, L' ');
        Inline(rest, out);
        EndLine(out);
    }
    if (inCode) {
        closeCode();
    }
    while (!out.text.empty() && out.text.back() == L'\n') {
        out.text.pop_back();
    }
    // Spans that ended in trimmed newlines (none do, but a rule at the end is cheap to keep safe).
    for (MdSpan& span : out.spans) {
        if (span.start + span.length > out.text.size()) {
            span.length = span.start < out.text.size() ? out.text.size() - span.start : 0;
        }
    }
    return out;
}

}  // namespace aic
