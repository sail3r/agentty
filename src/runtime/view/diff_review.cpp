#include "agentty/runtime/view/diff_review.hpp"
#include "agentty/util/digits.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <maya/core/render_context.hpp>   // available_height
#include <maya/widget/agentty/diff_review.hpp>

#include "agentty/domain/ui_live.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/look.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::ui {


namespace {

// The path as the user thinks of it: relative to the workspace, else ~/...
[[nodiscard]] std::string shown_path(const std::string& p) {
    const auto roots = agentty::ui_prefs::path_roots();
    auto under = [&](const std::string& base) {
        return !base.empty() && p.size() > base.size() &&
               p.compare(0, base.size(), base) == 0 && p[base.size()] == '/';
    };
    if (under(roots.cwd))  return p.substr(roots.cwd.size() + 1);
    if (under(roots.home)) return "~/" + p.substr(roots.home.size() + 1);
    return p;
}

// A parsed diff line: its sign, the code text (sign stripped), and its old-
// and new-side line numbers (0 = none on that side).
struct DiffLine {
    char sign; std::string code; int oldno; int newno;
    // Byte ranges in `code` that changed against the paired line (word diff).
    std::vector<std::pair<std::size_t, std::size_t>> changed;
};

// Tokens for the word diff: runs of word chars, runs of spaces, and runs of
// other symbols, so `==` -> `<=` is one changed operator, not one char.
[[nodiscard]] std::vector<std::string_view> tokens(std::string_view s) {
    std::vector<std::string_view> out;
    auto word = [](unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; };
    auto space = [](char c) { return c == ' ' || c == '\t'; };
    auto bracket = [](char c) { return c == '(' || c == ')' || c == '[' || c == ']'
                                     || c == '{' || c == '}' || c == ',' || c == ';'; };
    for (std::size_t i = 0; i < s.size();) {
        std::size_t j = i + 1;
        const auto c = static_cast<unsigned char>(s[i]);
        if (word(c))
            while (j < s.size() && word(static_cast<unsigned char>(s[j]))) ++j;
        else if (space(s[i]))
            while (j < s.size() && space(s[j])) ++j;
        else if (!bracket(s[i]))   // an operator: = == <= -> += ...
            while (j < s.size() && !word(static_cast<unsigned char>(s[j]))
                   && !space(s[j]) && !bracket(s[j])) ++j;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

// Mark the tokens of `a` and `b` outside their LCS. Skipped when they share
// under 40% of the SHORTER line's text: a word diff of unrelated lines is
// noise, but a line that grew (`= 30` -> `= config().timeout`) is an edit.
void word_diff(DiffLine& a, DiffLine& b) {
    const auto ta = tokens(a.code), tb = tokens(b.code);
    const std::size_t n = ta.size(), k = tb.size();
    if (n == 0 || k == 0 || n * k > 40'000) return;
    std::vector<std::vector<std::uint16_t>> dp(n + 1, std::vector<std::uint16_t>(k + 1, 0));
    for (std::size_t i = n; i-- > 0;)
        for (std::size_t j = k; j-- > 0;)
            dp[i][j] = ta[i] == tb[j] ? static_cast<std::uint16_t>(dp[i + 1][j + 1] + 1)
                                      : std::max(dp[i + 1][j], dp[i][j + 1]);
    auto is_blank = [](std::string_view t) { return t.find_first_not_of(" \t") == std::string_view::npos; };
    std::size_t common = 0;
    for (std::size_t i = 0, j = 0; i < n && j < k;) {
        if (ta[i] == tb[j]) { if (!is_blank(ta[i])) common += ta[i].size(); ++i; ++j; }
        else if (dp[i + 1][j] >= dp[i][j + 1]) ++i; else ++j;
    }
    auto ink = [&](std::string_view s) {
        std::size_t n = 0;
        for (const char c : s) if (c != ' ' && c != '\t') ++n;
        return n;
    };
    const std::size_t shorter = std::min(ink(a.code), ink(b.code));
    if (shorter == 0 || common * 10 < shorter * 4) return;
    auto mark = [&](DiffLine& d, std::string_view t) {
        if (is_blank(t)) return;   // whitespace isn't news
        const auto off = static_cast<std::size_t>(t.data() - d.code.data());
        if (!d.changed.empty() && d.changed.back().first + d.changed.back().second == off)
            d.changed.back().second += t.size();
        else
            d.changed.emplace_back(off, t.size());
    };
    std::size_t i = 0, j = 0;
    while (i < n || j < k) {
        if (i < n && j < k && ta[i] == tb[j]) { ++i; ++j; }
        else if (j >= k || (i < n && dp[i + 1][j] >= dp[i][j + 1])) mark(a, ta[i++]);
        else mark(b, tb[j++]);
    }
}

// Pair each run of removed lines with the added run right after it, line by
// line, and word-diff each pair.
void pair_changes(std::vector<DiffLine>& lines) {
    for (std::size_t i = 0; i < lines.size();) {
        if (lines[i].sign != '-') { ++i; continue; }
        std::size_t r = i;
        while (r < lines.size() && lines[r].sign == '-') ++r;
        std::size_t a = r;
        while (a < lines.size() && lines[a].sign == '+') ++a;
        for (std::size_t p = 0; i + p < r && r + p < a; ++p) word_diff(lines[i + p], lines[r + p]);
        i = std::max(a, i + 1);
    }
}

// Lines in a file's text, the way an editor counts them.
[[nodiscard]] int line_count(const std::string& s) {
    int n = static_cast<int>(std::count(s.begin(), s.end(), '\n'));
    return (!s.empty() && s.back() != '\n') ? n + 1 : n;
}

// `old_lines`/`new_lines` drop the empty "line" diff::compute emits past a
// trailing newline; it isn't in the file.
[[nodiscard]] std::vector<DiffLine> parse_hunk(std::string_view patch,
                                               int old_lines = 1 << 30,
                                               int new_lines = 1 << 30,
                                               int old_start = 1, int new_start = 1) {
    std::vector<DiffLine> out;
    // diff::compute's patches carry no @@ header, so start from the hunk
    int oldno = old_start, newno = new_start;
    std::size_t i = 0;
    while (i < patch.size()) {
        std::size_t nl = patch.find('\n', i);
        std::string_view line = patch.substr(i, (nl == std::string_view::npos ? patch.size() : nl) - i);
        i = (nl == std::string_view::npos) ? patch.size() : nl + 1;
        if (line.empty()) continue;
        if (line.starts_with("@@")) {
            // Seed both counters from "-start" and "+start".
            auto seed = [&](char c) {
                int v = 0;
                if (auto p = line.find(c, 2); p != std::string_view::npos)
                    for (std::size_t k = p + 1; k < line.size() && line[k] >= '0' && line[k] <= '9'; ++k)
                        v = ::agentty::util::push_digit(v, line[k]);
                return v;
            };
            oldno = seed('-');
            newno = seed('+');
            continue;   // the @@ header itself isn't rendered as a code line
        }
        char sign = line[0];
        std::string code{line.substr(1)};
        int o = 0, n = 0;
        if (sign == '-' || sign == ' ') o = oldno++;
        if (sign == '+' || sign == ' ') n = newno++;
        const bool past_end = code.empty() && (o == 0 || o > old_lines) && (n == 0 || n > new_lines);
        if (past_end) continue;
        out.push_back({sign, std::move(code), o, n});
    }
    return out;
}

} // namespace

maya::Element diff_review(const Model& m) {
    using DR = maya::DiffReview;
    auto* cursor = m.ui.panel.get<pn::DiffReview>();
    if (!cursor || m.d.pending_changes.empty()) return maya::DiffReview::empty();
    const auto& files = m.d.pending_changes;
    const int fidx = std::min<int>(cursor->file_index, static_cast<int>(files.size()) - 1);
    const auto& fc = files[static_cast<std::size_t>(fidx)];
    auto status_of = [](Hunk::Status s) {
        return s == Hunk::Status::Accepted ? DR::Status::Kept
             : s == Hunk::Status::Rejected ? DR::Status::Undone
                                           : DR::Status::Pending;
    };

    DR::Config cfg;
    cfg.accent  = accent;
    cfg.text    = fg;
    cfg.muted   = muted;
    cfg.success = success;
    cfg.danger  = danger;
    cfg.warn    = warn;

    // ── Totals across every file ──
    for (const auto& f : files) {
        cfg.added   += f.added;
        cfg.removed += f.removed;
        for (const auto& hk : f.hunks) {
            ++cfg.total;
            if (hk.status == Hunk::Status::Accepted) ++cfg.kept;
            if (hk.status == Hunk::Status::Rejected) ++cfg.undone;
        }
    }
    cfg.files_n = static_cast<int>(files.size());
    const int decided = cfg.kept + cfg.undone;
    // the reducer closes the pane on the last decision, so this lasts a frame
    cfg.all_done = decided == cfg.total && cfg.total > 0;

    // Room for the focused change: Settings → Appearance → Panel height,
    // the ceiling every panel uses, and never more than the terminal less
    // this pane's chrome. The list of changes in this file is a WINDOW too:
    // a third of the setting (compact 3 · normal 4 · roomy 6 · tall 9) rows,
    // kept around the focused one, with "↑/↓ N more" markers.
    //
    // Row budget, counted from the rendered pane: 17 fixed rows (border and
    // padding 4, summary + gap 2, tabs + gap 2, path + rule 2, the code's
    // blank-before / ↑ / ↓ / blank-after 4, progress + gap + keys 3), then
    // the change list, then the code. On a short terminal the list gives way
    // first (down to the focused change alone), then the code (down to 3).
    const int density_rows = ui_prefs::viewport_rows(m.d.ui().density);
    const int nhunks = static_cast<int>(fc.hunks.size());
    const int term_rows = maya::have_render_context() ? maya::available_height() : 40;
    constexpr int kFixed = 17;
    const int avail = term_rows - kFixed;
    auto top_of = [&](int win) {
        return std::clamp(cursor->hunk_index - win / 2, 0, std::max(0, nhunks - win));
    };
    auto marks_of = [&](int win) {
        const int t = top_of(win);
        return (t > 0 ? 1 : 0) + (t + win < nhunks ? 1 : 0);
    };
    int hunk_win = std::min(nhunks, std::max(3, density_rows / 3));
    while (hunk_win > 1 && avail - hunk_win - marks_of(hunk_win) < 6) --hunk_win;
    const int hunk_top = top_of(hunk_win);
    const int body_rows = std::max(3, std::min(density_rows,
                                               avail - hunk_win - marks_of(hunk_win)));

    // ── One tab per file: its mark is the file's state ──
    for (const auto& f : files) {
        bool anyrej = false, anypend = false;
        for (const auto& hk : f.hunks) {
            anyrej  |= hk.status == Hunk::Status::Rejected;
            anypend |= hk.status == Hunk::Status::Pending;
        }
        std::string name = f.path;
        if (auto sl = name.rfind('/'); sl != std::string::npos) name = name.substr(sl + 1);
        cfg.files.push_back({std::move(name),
                             anypend ? DR::Status::Pending : anyrej ? DR::Status::Undone : DR::Status::Kept,
                             f.added, f.removed});
    }
    cfg.current = fidx;

    // ── The current file ──
    {
        std::string base = shown_path(fc.path);
        if (auto sl = base.rfind('/'); sl != std::string::npos) {
            cfg.dir  = base.substr(0, sl + 1);
            base = base.substr(sl + 1);
        }
        cfg.base = std::move(base);
        // A new or emptied file says so: that's the first thing to know.
        cfg.kind = fc.original_contents.empty() ? "new file"
                 : fc.new_contents.empty()      ? "deleted"
                                                : "";
        cfg.kind_deletes  = fc.new_contents.empty();
        cfg.changes_n     = nhunks;
        cfg.focused_index = cursor->hunk_index;
        if (auto dot = fc.path.rfind('.'); dot != std::string::npos) cfg.lang = fc.path.substr(dot + 1);
    }

    int max_ln = 1;
    for (const auto& hk : fc.hunks)
        max_ln = std::max({max_ln, hk.new_start + hk.new_len, hk.old_start + hk.old_len});
    cfg.gutter_w = std::max(2, static_cast<int>(std::to_string(max_ln).size()));
    const int old_n = line_count(fc.original_contents);
    const int new_n = line_count(fc.new_contents);
    bool focused_overflows = false;

    cfg.hidden_above = hunk_top;
    cfg.hidden_below = std::max(0, nhunks - hunk_top - hunk_win);
    for (int hi = hunk_top; hi < hunk_top + hunk_win; ++hi) {
        const auto& hk = fc.hunks[static_cast<std::size_t>(hi)];
        const bool sel = hi == cursor->hunk_index;
        auto lines = parse_hunk(hk.patch, old_n, new_n, hk.old_start, hk.new_start);
        if (sel) pair_changes(lines);
        DR::Change ch;
        ch.status  = status_of(hk.status);
        ch.focused = sel;
        int first = 0, last = 0;
        for (const auto& dl : lines) {
            if (dl.sign == '+') ++ch.added; else if (dl.sign == '-') ++ch.removed;
            if (dl.sign != ' ') {
                const int ln = dl.newno ? dl.newno : dl.oldno;
                if (!first) first = ln;
                last = ln;
            }
        }
        ch.where = first == last ? std::format("line {}", first)
                                 : std::format("lines {}\xe2\x80\x93{}", first, last);
        if (sel) {
            // The enclosing function, like git's hunk header: the last line
            // above the change that starts at column 0 and isn't a brace.
            std::string_view src = fc.original_contents.empty() ? fc.new_contents : fc.original_contents;
            const int upto = std::max(0, (fc.original_contents.empty() ? hk.new_start : hk.old_start) - 1);
            int ln = 0;
            for (std::size_t p = 0; p < src.size() && ln < upto; ++ln) {
                auto nl = src.find('\n', p);
                if (nl == std::string_view::npos) nl = src.size();
                const auto line = src.substr(p, nl - p);
                if (!line.empty() && line[0] != ' ' && line[0] != '\t' && line[0] != '}'
                    && line[0] != '{' && line[0] != '#' && line[0] != '/' && line[0] != '*')
                    ch.scope = std::string{line.substr(0, 60)};
                p = nl + 1;
            }
            while (!ch.scope.empty() && (ch.scope.back() == '{' || ch.scope.back() == ' ' || ch.scope.back() == '\r'))
                ch.scope.pop_back();
            // Already on screen in the hunk's context: don't say it twice.
            for (const auto& dl : lines)
                if (!ch.scope.empty() && dl.code.starts_with(ch.scope)) { ch.scope.clear(); break; }

            // The focused change's lines, sized so the keys stay on screen;
            // PgUp/PgDn page through a bigger one.
            const int nlines = static_cast<int>(lines.size());
            const int off = std::clamp(cursor->body_scroll, 0, std::max(0, nlines - body_rows));
            focused_overflows = nlines > body_rows;
            ch.above = off;
            for (int li = off; li < nlines && li < off + body_rows; ++li) {
                auto& dl = lines[static_cast<std::size_t>(li)];
                ch.lines.push_back({dl.sign, std::move(dl.code), dl.oldno, dl.newno, std::move(dl.changed)});
            }
            ch.below = nlines - off - static_cast<int>(ch.lines.size());
        }
        cfg.changes.push_back(std::move(ch));
    }

    // ── Keys ──
    cfg.confirm_undo_all = cursor->confirm_reject_all;
    if (!cfg.confirm_undo_all) {
        auto hint = [](std::string k, std::string l, int p, maya::Color c = fg) {
            return maya::KeyHint{std::move(k), std::move(l), p, c};
        };
        cfg.hints = {hint("y", "keep", 9, success), hint("n", "undo", 9, danger),
                     hint(ui::mark::updown, "change", 7)};
        if (files.size() > 1) cfg.hints.push_back(hint(ui::mark::leftright, "file", 6));
        if (focused_overflows) cfg.hints.push_back(hint("PgUp/PgDn", "scroll", 3));
        cfg.hints.push_back(hint("^A", "keep all", 4, success));
        cfg.hints.push_back(hint("^X", "undo all", 4, danger));
        // Esc closes; anything not decided stays as it is on disk
        cfg.hints.push_back(hint("Esc", decided == 0 ? "keep all, close" : "close, keep rest", 8));
    }

    return maya::DiffReview{std::move(cfg)}.build();
}

} // namespace agentty::ui
