#include "core/markdown.h"

#include <algorithm>
#include <cwctype>
#include <optional>

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
    const auto styled = [&](std::wstring_view inner, unsigned flag, bool nested, std::wstring_view url = {}) {
        const size_t start = out.text.size();
        if (nested) {
            Inline(inner, out);
        } else {
            out.text.append(inner);
        }
        if (out.text.size() > start) {
            MdSpan span{start, out.text.size() - start, flag, 1.f, false};
            span.url = url;
            out.spans.push_back(std::move(span));
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
                const bool absolute = IsLocalPath(inner);
                if ((absolute || IsRelativePath(inner)) && inner.find(L'\n') == std::wstring_view::npos) {
                    MdSpan link{out.text.size() - inner.size(), inner.size(), absolute ? kMdLink : kMdLink | kMdRelative, 1.f,
                                false};
                    link.url = inner;
                    out.spans.push_back(std::move(link));
                }
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
                std::wstring_view url = s.substr(middle + 2, end - middle - 2);
                url = url.substr(0, url.find(L' '));  // [text](url "title"): the title is dropped
                styled(s.substr(i + 1, middle - i - 1), kMdLink, true, url);
                i = end + 1;
                continue;
            }
        }
        if (c == L'h' && (i == 0 || !IsWordChar(s[i - 1])) &&
            (s.substr(i).starts_with(L"http://") || s.substr(i).starts_with(L"https://"))) {
            size_t end = i;
            while (end < s.size() && !IsSpace(s[end]) && s[end] != L'<' && s[end] != L'>' && s[end] != L'`') {
                ++end;
            }
            while (end > i) {  // sentence punctuation after an address is not part of it
                const wchar_t last = s[end - 1];
                const std::wstring_view address = s.substr(i, end - i);
                const bool unbalanced = last == L')' && std::count(address.begin(), address.end(), L'(') <
                                                            std::count(address.begin(), address.end(), L')');
                if (std::wstring_view(L".,;:!?'\"*]").find(last) == std::wstring_view::npos && !unbalanced) {
                    break;
                }
                --end;
            }
            if (end > i + 8) {
                styled(s.substr(i, end - i), kMdLink, false, s.substr(i, end - i));
                i = end;
                continue;
            }
        }
        if (std::iswalpha(c) != 0 && (i == 0 || !IsWordChar(s[i - 1])) && IsLocalPath(s.substr(i))) {
            size_t end = i;  // a bare path ends at a space: one with spaces in it needs `code`
            while (end < s.size() && !IsSpace(s[end]) && std::wstring_view(L"<>\"|*?`").find(s[end]) == std::wstring_view::npos) {
                ++end;
            }
            while (end > i + 3 && std::wstring_view(L".,;:!?')*]").find(s[end - 1]) != std::wstring_view::npos) {
                --end;
            }
            if (end > i + 3) {
                styled(s.substr(i, end - i), kMdLink, false, s.substr(i, end - i));
                i = end;
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

std::wstring_view Trim(std::wstring_view s) {
    while (!s.empty() && IsSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && IsSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

// The cells of a table row; the pipes at its ends are optional, \| stays inside a cell.
std::vector<std::wstring_view> SplitRow(std::wstring_view row) {
    row = Trim(row);
    if (row.starts_with(L'|')) {
        row.remove_prefix(1);
    }
    if (row.ends_with(L'|') && !(row.size() >= 2 && row[row.size() - 2] == L'\\')) {
        row.remove_suffix(1);
    }
    std::vector<std::wstring_view> cells;
    size_t from = 0;
    for (size_t i = 0; i <= row.size(); ++i) {
        if (i == row.size() || (row[i] == L'|' && (i == 0 || row[i - 1] != L'\\'))) {
            cells.push_back(Trim(row.substr(from, i - from)));
            from = i + 1;
        }
    }
    return cells;
}

enum class Align { Left, Right, Center };

// `|---|:-:|--:|`: the alignments, or nothing when the line is not a delimiter row.
std::optional<std::vector<Align>> DelimiterRow(std::wstring_view line) {
    if (line.find(L'|') == std::wstring_view::npos) {
        return std::nullopt;
    }
    std::vector<Align> aligns;
    for (std::wstring_view cell : SplitRow(line)) {
        const bool left = cell.starts_with(L':');
        const bool right = cell.ends_with(L':');
        if (left) {
            cell.remove_prefix(1);
        }
        if (right && !cell.empty()) {
            cell.remove_suffix(1);
        }
        if (cell.empty() || cell.find_first_not_of(L'-') != std::wstring_view::npos) {
            return std::nullopt;
        }
        aligns.push_back(right ? (left ? Align::Center : Align::Right) : Align::Left);
    }
    return aligns;
}

std::wstring Plain(std::wstring_view markdown) {
    MdText cell;
    Inline(markdown, cell);
    return cell.text;
}

// Narrows the widest columns until the row, with its " │ " separators, fits `limit` characters (0: any width).
// A column is not narrowed below a few characters: past that the feed wraps the lines instead.
void FitWidths(std::vector<size_t>& widths, size_t limit) {
    constexpr size_t kNarrowest = 6;
    if (limit == 0 || widths.empty()) {
        return;
    }
    const size_t separators = 3 * (widths.size() - 1);
    size_t total = separators;
    for (const size_t width : widths) {
        total += width;
    }
    while (total > limit) {
        const auto widest = std::max_element(widths.begin(), widths.end());
        if (*widest <= kNarrowest) {
            return;
        }
        --*widest;
        --total;
    }
}

// A cell's lines at `width`: broken at spaces, a word longer than the column cut where it ends.
std::vector<std::wstring> WrapCell(const std::wstring& cell, size_t width) {
    std::vector<std::wstring> lines;
    std::wstring_view rest = cell;
    while (rest.size() > width) {
        size_t cut = rest.substr(0, width + 1).find_last_of(L' ');
        if (cut == std::wstring_view::npos || cut == 0) {
            cut = width;
        }
        lines.emplace_back(Trim(rest.substr(0, cut)));
        rest = Trim(rest.substr(cut));
    }
    lines.emplace_back(rest);
    return lines;
}

void AppendPadded(std::wstring& out, const std::wstring& cell, size_t width, Align align, bool last) {
    const size_t gap = width > cell.size() ? width - cell.size() : 0;
    const size_t before = align == Align::Right ? gap : (align == Align::Center ? gap / 2 : 0);
    out.append(before, L' ');
    out += cell;
    if (!last || align != Align::Left) {
        out.append(gap - before, L' ');
    }
}

// The line at `pos` of `source` (no line end), if there is one.
std::optional<std::wstring_view> LineAt(std::wstring_view source, size_t pos) {
    if (pos >= source.size()) {
        return std::nullopt;
    }
    size_t end = source.find(L'\n', pos);
    if (end == std::wstring_view::npos) {
        end = source.size();
    }
    std::wstring_view line = source.substr(pos, end - pos);
    if (line.ends_with(L'\r')) {
        line.remove_suffix(1);
    }
    return line;
}

}  // namespace

bool IsLocalPath(std::wstring_view text) {
    if (text.size() >= 3 && ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')) &&
        text[1] == L':' && (text[2] == L'\\' || text[2] == L'/')) {
        return true;
    }
    return text.size() >= 3 && text.starts_with(L"\\\\") && IsWordChar(text[2]);
}

bool IsRelativePath(std::wstring_view text) {
    const auto isSeparator = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    if (text.size() < 3 || text.size() > 260 || IsLocalPath(text) || text.find(L"://") != std::wstring_view::npos ||
        text.find_first_of(L"<>\"|*?=\t\n") != std::wstring_view::npos || text.front() == L' ' || text.front() == L'-' ||
        isSeparator(text.front())) {
        return false;
    }
    const size_t firstSeparator = text.find_first_of(L"\\/");
    if (firstSeparator == std::wstring_view::npos) {
        return false;
    }
    const std::wstring_view head = text.substr(0, firstSeparator);
    constexpr std::wstring_view kPlaces[] = {L"~", L".", L"..", L"Desktop", L"Documents", L"Downloads", L"AppData",
                                             L"Рабочий стол", L"Документы", L"Загрузки"};
    bool place = head.find(L'%') != std::wstring_view::npos;
    for (const std::wstring_view known : kPlaces) {
        place = place || (head.size() == known.size() &&
                          std::equal(head.begin(), head.end(), known.begin(),
                                     [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); }));
    }
    if (place) {
        return true;
    }
    // A command is not a path: its first word has to carry a separator (git add src/a.cpp does not).
    if (head.find(L' ') != std::wstring_view::npos) {
        return false;
    }
    // Elsewhere a file's name has an extension: the last part, :12 or :12:5 cut off, ends in .ext
    std::wstring_view name = text.substr(text.find_last_of(L"\\/") + 1);
    for (int i = 0; i < 2; ++i) {
        const size_t colon = name.find_last_of(L':');
        if (colon != std::wstring_view::npos && colon + 1 < name.size() &&
            name.find_first_not_of(L"0123456789", colon + 1) == std::wstring_view::npos) {
            name = name.substr(0, colon);
        }
    }
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot + 1 >= name.size() || name.size() - dot - 1 > 10) {
        return false;
    }
    return std::ranges::all_of(name.substr(dot + 1), [](wchar_t c) { return std::iswalnum(c) != 0; });
}

MdText ParseMarkdown(std::wstring_view source, size_t tableWidth) {
    MdText out;
    bool inCode = false;
    size_t codeStart = 0;
    std::wstring_view language;

    const auto closeCode = [&] {
        inCode = false;
        size_t end = out.text.size();
        while (end > codeStart && out.text[end - 1] == L'\n') {
            --end;
        }
        if (end > codeStart) {
            out.spans.push_back(MdSpan{codeStart, end - codeStart, kMdCode, 1.f, true});
            for (const Token& token : Highlight(std::wstring_view(out.text).substr(codeStart, end - codeStart), language)) {
                MdSpan span{codeStart + token.start, token.length, kMdToken, 1.f, false};
                span.token = token.kind;
                out.spans.push_back(std::move(span));
            }
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
                language = Trim(rest.substr(3));
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

        if (rest.find(L'|') != std::wstring_view::npos) {
            const auto next = LineAt(source, pos);
            const auto aligns = next ? DelimiterRow(*next) : std::nullopt;
            if (aligns) {  // a table: the header, the delimiter row, then rows while they have a pipe
                std::vector<std::vector<std::wstring>> rows;
                rows.emplace_back();
                for (const std::wstring_view cell : SplitRow(rest)) {
                    rows.back().push_back(Plain(cell));
                }
                pos += next->size() + 1;
                if (pos > 0 && pos <= source.size() && source[pos - 1] == L'\r') {
                    ++pos;  // the delimiter row's "\r\n"
                }
                while (const auto row = LineAt(source, pos)) {
                    const std::wstring_view body = Trim(*row);
                    if (body.empty() || body.find(L'|') == std::wstring_view::npos || body.starts_with(L"```")) {
                        break;
                    }
                    rows.emplace_back();
                    for (const std::wstring_view cell : SplitRow(body)) {
                        rows.back().push_back(Plain(cell));
                    }
                    pos += row->size() + 1;
                    if (pos <= source.size() && source[pos - 1] == L'\r') {
                        ++pos;
                    }
                }
                const size_t columns = aligns->size();
                std::vector<size_t> widths(columns, 1);
                for (std::vector<std::wstring>& row : rows) {
                    row.resize(columns);
                    for (size_t c = 0; c < columns; ++c) {
                        widths[c] = std::max(widths[c], row[c].size());
                    }
                }
                FitWidths(widths, tableWidth);
                // Each row as its cells' lines; once a row takes several, rules part all the rows.
                std::vector<std::vector<std::vector<std::wstring>>> wrapped(rows.size());
                bool tall = false;
                for (size_t r = 0; r < rows.size(); ++r) {
                    for (size_t c = 0; c < columns; ++c) {
                        wrapped[r].push_back(WrapCell(rows[r][c], widths[c]));
                        tall = tall || wrapped[r].back().size() > 1;
                    }
                }
                const auto rule = [&](bool bottom = false) {  // the bottom one's crossings are tees: no stubs below it
                    out.text += L'\n';
                    for (size_t c = 0; c < columns; ++c) {
                        out.text += c == 0 ? L"" : (bottom ? L"─┴─" : L"─┼─");
                        out.text.append(widths[c], L'─');
                    }
                };
                Separate(out);
                const size_t start = out.text.size();
                size_t headerEnd = start;
                for (size_t r = 0; r < rows.size(); ++r) {
                    if (r > 1 && tall) {
                        rule();
                    }
                    size_t height = 1;
                    for (const std::vector<std::wstring>& cell : wrapped[r]) {
                        height = std::max(height, cell.size());
                    }
                    for (size_t at = 0; at < height; ++at) {
                        if (r > 0 || at > 0) {
                            out.text += L'\n';
                        }
                        for (size_t c = 0; c < columns; ++c) {
                            out.text += c == 0 ? L"" : L" │ ";
                            const std::vector<std::wstring>& cell = wrapped[r][c];
                            AppendPadded(out.text, at < cell.size() ? cell[at] : std::wstring(), widths[c],
                                         (*aligns)[c], c + 1 == columns);
                        }
                    }
                    if (r == 0) {
                        headerEnd = out.text.size();
                        rule();
                    }
                }
                if (tall) {  // closed below, so the dividers of a many-lined last row do not end as stubs
                    rule(true);
                }
                out.spans.push_back(MdSpan{start, out.text.size() - start, kMdCode | kMdTable, 1.f, true});
                out.spans.push_back(MdSpan{start, headerEnd - start, kMdBold, 1.f, false});
                out.text += L"\n\n";
                continue;
            }
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
