// tool_output.cpp — the tool-output viewer (^O): list + body stages.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"

#include <maya/anim/text_reveal.hpp>   // the transcript's typewriter
#include <maya/core/animation.hpp>   // RateCursor
#include <maya/widget/agentty/spinner.hpp>      // the transcript's spinner frames
#include <maya/core/anim_clock.hpp>
#include <maya/core/motion.hpp>      // keep_animating
#include <maya/text/unicode_width.hpp>   // char_width
#include <maya/widget/agentty/tool_body_preview.hpp>
#include <maya/widget/agentty/tool_viewer.hpp>

#include <cmath>    // lround: tok/s
#include <format>

#include "agentty/runtime/panel/tool_output.hpp"

namespace agentty::ui {

namespace {

// The overlay's reveal: the same RateCursor pacing and decorate_text_reveal
// effect the transcript's StreamingMarkdown uses, on the overlay's own rows.
// One per open entry; a different entry, or text that shrank, starts fresh
// at the edge (only text arriving while you watch types in).
struct OverlayReveal {
    std::string            key;
    maya::anim::RateCursor cursor{60.0, 0.55};
    std::size_t            total_cp = 0;    // codepoints the entry has now
    std::int64_t           last_us  = 0;
    bool                   ramped   = false;   // finish deadline armed
};

OverlayReveal& overlay_reveal() {
    static OverlayReveal r;   // UI thread only, like the body cache
    return r;
}

// Byte offset of codepoint `cp` in `s` (s.size() when past the end).
std::size_t cp_offset(std::string_view s, std::size_t cp) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (n == cp) return i;
            ++n;
        }
    return s.size();
}

// Advance the reveal for `e` and return how many codepoints are typed.
// Same pacing as the transcript: reasoning 60 cps / 0.55 s lag, adaptive
// 45..280; tool output 45 / 0.40, adaptive 25..180.
std::size_t advance_reveal(const tool_output::Entry& e, std::size_t total, bool running) {
    auto& r = overlay_reveal();
    const std::string key = std::string{tool_output::entry_key(e)};
    const std::int64_t now_us = maya::anim_now_us();
    if (key != r.key || total < r.total_cp) {
        // Opened on this entry, or it was replaced: show what's there.
        r = OverlayReveal{};
        r.key = key;
        if (e.thinking) {
            r.cursor.set_pacing(60.0, 0.55);
            r.cursor.set_adaptive(true, 45.0, 280.0);
        } else {
            r.cursor.set_pacing(45.0, 0.40);
            r.cursor.set_adaptive(true, 25.0, 180.0);
        }
        r.cursor.set_pos(static_cast<double>(total));
    }
    r.total_cp = total;
    const double dt = r.last_us == 0 ? 0.0
        : std::min(0.120, static_cast<double>(now_us - r.last_us) / 1e6);
    r.last_us = now_us;
    // Settled: finish the glide quickly instead of at the live pace.
    if (!running && !r.ramped) { r.cursor.set_deadline(0.25); r.ramped = true; }
    if (running && r.ramped)   { r.cursor.clear_deadline(); r.ramped = false; }
    const double pos = r.cursor.tick(static_cast<double>(total), dt);
    return std::min(total, static_cast<std::size_t>(pos));
}

} // namespace

namespace {
// Split one line into rows of at most `w` columns. Breaks at spaces only
// (output is code, paths and flags, where a hyphen is part of the token),
// and hard-splits a token longer than a row.
std::vector<std::string_view> wrap_rows(std::string_view s, int w) {
    std::vector<std::string_view> out;
    if (w < 1) w = 1;
    std::size_t start = 0;
    while (start < s.size()) {
        int cols = 0;
        std::size_t i = start, last_space = std::string_view::npos;
        while (i < s.size()) {
            std::size_t next = i;
            const char32_t cp = maya::decode_utf8(s, next);
            const int cw = maya::unicode::char_width(cp);
            if (cols + cw > w) break;
            cols += cw;
            if (cp == U' ') last_space = next;
            i = next;
        }
        if (i >= s.size()) { out.push_back(s.substr(start)); break; }
        std::size_t cut = (last_space != std::string_view::npos && last_space > start)
                          ? last_space : i;
        if (cut == start) {   // a zero-room row: take one character
            std::size_t next = start;
            (void)maya::decode_utf8(s, next);
            cut = next;
        }
        out.push_back(s.substr(start, cut - start));
        start = cut;
    }
    if (out.empty()) out.push_back(std::string_view{});
    return out;
}

// `s` cut to `w` columns, with "…" when it was cut.
std::string fit_cols(std::string_view s, int w) {
    if (w <= 0) return {};
    if (string_width(s) <= w) return std::string{s};
    std::size_t i = 0;
    int cols = 0;
    while (i < s.size()) {
        std::size_t next = i;
        const char32_t cp = maya::decode_utf8(s, next);
        const int cw = maya::unicode::char_width(cp);
        if (cols + cw > w - 1) break;
        cols += cw;
        i = next;
    }
    std::string out{s.substr(0, i)};
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + "\xe2\x80\xa6";
}
} // namespace

Element tool_output_panel(const Model& m) {
    const auto* o = m.ui.panel.get<pn::ToolOutput>();
    if (!o) return Panel::empty();

    const int sz = static_cast<int>(o->entries.size());
    const int cur = std::clamp(o->index, 0, std::max(0, sz - 1));

    Panel::Config cfg;
    // Deliberately NOT one of the shared kPanel* floors. Overlay stretch
    // supplies all available columns, and a large minimum used to force the
    // picker past phone/SSH terminal bounds and clip its right side; keep only
    // the border's structural floor and let row flex do the rest. Captured
    // output is arbitrary-width content, so "how narrow may this get" is
    // answered by the terminal, not by the shape of a label column.
    cfg.min_width  = 1;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.tool_viewer_scroll;

    // "n/N" position — shown in both stages so the user always knows
    // where they are in the output stack.
    const std::string pos =
        std::to_string(cur + 1) + "/" + std::to_string(sz);

    if (!o->viewing) {
        // ── LIST stage ──
        cfg.title    = (o->thinking_only ? " Thinking \xc2\xb7 " : " Tool Outputs \xc2\xb7 ")
                     + std::to_string(sz) + " ";
        cfg.accent   = o->thinking_only ? Color{role_brand} : highlight;
        cfg.selected = sz == 0 ? -1 : cur;
        // LIST chrome is border+padding (4) plus blank+hint footer (2).
        // At truly tiny heights, drop the optional footer and devote every
        // remaining row to selectable outputs.
        const int list_term_rows = panel_terminal_rows();
        const bool compact_list = list_term_rows < 6;
        // The fit loop at the end shrinks this to what the terminal has.
        cfg.viewport_h = std::clamp(static_cast<int>(o->entries.size()), 1, panel_viewport_h());

        // Badge column width: longest display name across the entries,
        // so every detail line starts at the same column and the badge
        // hues read as a vertical colour strip.
        int badge_w = 0;
        for (const auto& e : o->entries)
            badge_w = std::max(badge_w, e.is_live ? string_width("\xe2\x97\x8f LIVE")
                                                  : string_width(e.title));
        ++badge_w;   // the widest name still gets a gap before its detail
        // Room for the detail: the overlay is 4 columns in, then border (2),
        // padding (4), scrollbar gutter (1), cursor bar (2), the badge plus
        // its 1-column gap, and the 2-column gap before the status. The
        // detail gives way so the status is never cut.
        const int row_w = std::max(10, panel_terminal_cols() - 4 - 2 - 4 - 1 - 2 - 1);
        auto fit_detail = [&](const std::string& lead, const std::string& trail) {
            return fit_cols(lead, row_w - badge_w - string_width(trail) - 2);
        };

        cfg.items.reserve(o->entries.size());
        for (int i = 0; i < sz; ++i) {
            const auto& e = o->entries[static_cast<std::size_t>(i)];
            Panel::Item row;
            const Color cat_hue = tool_category_color(e.name);
            if (e.is_live) {
                // The currently-running tool, pinned to the top. A bright
                // "● LIVE" badge in the tool's hue reads as "streaming now";
                // the detail line carries the tool name so the row is still
                // self-describing.
                std::string badge = "\xe2\x97\x8f LIVE";
                const int live_w = string_width(badge);
                if (live_w < badge_w)
                    badge.append(static_cast<std::size_t>(badge_w - live_w), ' ');
                row.badge          = std::move(badge);
                row.badge_style    = fg_bold(cat_hue);
                row.leading        = fit_detail(e.title + (e.detail.empty() ? "" : "  " + e.detail),
                                                e.trailing);
                row.leading_style  = fg_of(fg);
                row.trailing       = e.trailing;
                row.trailing_style = fg_dim(cat_hue);
                cfg.items.push_back(std::move(row));
                continue;
            }
            row.badge = e.title;
            const int title_w = string_width(e.title);
            if (title_w < badge_w)
                row.badge.append(static_cast<std::size_t>(badge_w - title_w), ' ');
            // Category hue — the same colour identity the transcript
            // card used, so "which tool was that?" is answered by hue
            // before the label is even read. Failures go red on the
            // badge too: status outranks category.
            row.badge_style    = e.failed ? fg_bold(danger)
                               : e.thinking ? fg_bold(role_brand)
                                            : fg_bold(cat_hue);
            row.leading        = e.detail.empty() ? std::string{"\xe2\x80\xa6"}
                                                  : fit_detail(e.detail, e.trailing);
            row.leading_style  = e.failed ? fg_of(danger) : fg_of(fg);
            row.trailing       = e.trailing;
            row.trailing_style = e.failed ? fg_of(danger)
                               : e.streaming ? fg_of(role_brand) : fg_dim(muted);
            cfg.items.push_back(std::move(row));
        }
        if (!compact_list) {
            cfg.footer.push_back(Line::blank());
            std::vector<Hint> list_hints{
                {"\xe2\x86\x91\xe2\x86\x93", "move", 5},   // ↑↓
                {"Enter", "view", 6},
                {"/", "find", 4},
                {"y", "copy", 4},
                {"Esc", "close", 3},
            };
            if (o->fresh_above > 0)
                list_hints.insert(list_hints.begin() + 2,
                    {"t", std::to_string(o->fresh_above) + " new above", 6});
            cfg.footer.push_back(key_hints(std::move(list_hints)));
        }
        // Same rule as the body: never taller than the room the overlay has.
        return Panel::fit(std::move(cfg), std::max(3, list_term_rows - 2),
                          std::max(10, panel_terminal_cols() - 4));
    }

    // ── BODY stage ──
    const auto& e = o->entries[static_cast<std::size_t>(cur)];
    const Color tool_hue = e.failed ? danger
                         : e.thinking ? Color{role_brand}
                                      : tool_category_color(e.name);
    // Still arriving: the body follows its newest line.
    const bool following_kind = e.is_live || e.streaming;

    // Thinking: is it still going, read from the message itself each frame
    // (the entry is a snapshot from the last delta, so a quiet stretch would
    // freeze it), and how long it's been. Same words and spinner as the
    // transcript's block: "Thinking ⠋" while live, "Reasoned" once done.
    bool thinking_live = false;
    std::int64_t thought_ms = -1;
    if (e.thinking) {
        for (const auto& msg : m.d.current.messages)
            if (msg.id == e.source) {
                // The transcript block's clock: it advances every frame, the
                // model's `now` only on a message.
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    maya::anim_now().time_since_epoch()).count();
                thinking_live = msg.thinking_open();
                thought_ms = msg.thinking_elapsed_ms(now_ms);
                break;
            }
    }
    std::string spin;
    if (thinking_live) {
        constexpr auto f = maya::detail::get_spinner_frames<maya::SpinnerStyle::Dots>();
        spin = std::string{f.frames[maya::anim::frame_index(
            static_cast<std::size_t>(f.count), static_cast<double>(f.interval) * 1000.0)]};
        // The spinner wakes itself; the timer's next whole second does too.
        maya::anim::keep_animating_after(1000 - (thought_ms % 1000));
    }
    // Just finished: say so loudly for a moment, then settle. The reducer
    // stamps the entry when it sees thinking end with the reader open, so
    // another block, or one that ended while the reader was shut, never flashes.
    const std::int64_t now_anim = maya::anim_now_ms();
    const bool just_done = e.thinking && !thinking_live && e.done_at_ms != 0
                           && now_anim - e.done_at_ms < 2500;
    if (just_done) maya::anim::keep_animating_after(2500 - (now_anim - e.done_at_ms));
    auto clock = [](std::int64_t ms) {
        return ms < 60'000 ? std::format("{}s", ms / 1000)
                           : std::format("{}m {:02}s", ms / 60'000, (ms / 1000) % 60);
    };
    const std::string title = e.thinking && e.turn > 0
        ? e.title + " \xc2\xb7 turn " + std::to_string(e.turn) : e.title;
    cfg.title    = (e.is_live || thinking_live ? std::string(" \xe2\x97\x8f LIVE \xc2\xb7 ") + title
                              : " " + title)
                 + " \xc2\xb7 " + pos + " ";
    cfg.accent   = just_done ? Color{success} : tool_hue;
    cfg.selected = -1;   // read-only — no cursor row; manual scroll rules
    // Normal BODY chrome is 9 rows: border+padding (4), header+separator
    // (2), blank+range+hints footer (3). Below 10 terminal rows there is
    // room for none of that optional chrome, so the border title carries
    // identity and every remaining row goes to output. This keeps the modal
    // inside all viable terminal heights (5+ rows) instead of inheriting the
    // shared picker's four-row viewport floor.
    const int body_term_rows = panel_terminal_rows();
    const bool compact_body = body_term_rows < 6;   // the fit loop handles the rest
    // The same height every other panel uses (Settings → Appearance →
    // Density: compact / normal / roomy), not the whole terminal. A long
    // output scrolls inside it like any list. The fit loop below still
    // shrinks it for a short terminal.
    cfg.viewport_h = std::max(1, std::min(body_term_rows, panel_viewport_h()));

    // Header: tool name, what it ran on, and how it went. The status never
    // clips; the detail gives way first. Thinking has no "ran on": its
    // header is the turn and the size.
    std::optional<maya::ToolViewer::Header> viewer_header;
    if (!compact_body) {
        std::string detail = e.thinking ? std::string{} : e.detail;
        std::string trailing = e.trailing;
        // The border already names the turn; don't spend the row on it twice.
        // Thinking says what state it's in, the transcript's way.
        const std::string head = !e.thinking ? title
                               : thinking_live ? "Thinking " + spin
                               : just_done     ? std::string{"\xe2\x9c\x93 Done thinking"}
                                               : std::string{"\xe2\x9c\x93 Reasoned"};   // ✓
        std::vector<std::string> mid;
        if (e.thinking) {
            // The facts share the header row: model, effort, kind in the
            // middle, time and tokens on the right. No extra rows. What
            // doesn't fit is dropped whole, last first, never cut mid-word.
            std::string time, tokens;
            for (const auto& [k, v] : e.facts) {
                if (k == "tokens") tokens = v;
                else if (k != "status" && k != "thought for")
                    mid.push_back(k == "blocks" ? v + " blocks" : v);
            }
            // Live time, ticking every second; settled time to a tenth.
            if (thought_ms >= 100)
                time = thinking_live ? clock(thought_ms)
                                     : std::format("{:.1f}s", static_cast<double>(thought_ms) / 1000.0);
            trailing = tokens.empty() ? time : time.empty() ? tokens : time + " \xc2\xb7 " + tokens;
        }
        const Style head_st  = e.thinking && !thinking_live ? fg_bold(success) : fg_bold(tool_hue);
        const Style trail_st = e.failed ? fg_of(danger)
                             : thinking_live ? fg_of(role_brand) : fg_dim(muted);
        viewer_header = maya::ToolViewer::Header{head, detail, trailing, mid, head_st, trail_st, fg};
    }

    // Body rows: built once per (entry, width, query), then windowed.
    //
    // A 256 KiB output is thousands of rows, so every frame copies only the
    // visible slice. Each cached row is exactly one screen row: long lines
    // are wrapped here, at the body width, so the scroll maths counts what
    // is drawn and nothing past the right edge is lost.
    struct BodyCache {
        const void*   entries_key = nullptr;
        int           index       = -1;
        const void*   bytes_key   = nullptr;
        std::size_t   bytes_len   = 0;
        std::uint64_t call_key    = 0;
        int           width       = -1;
        std::string   query;
        int           child_cursor = -1;
        std::size_t   child_open_n = 0;
        std::string   child_open_key;
        std::vector<Element> rows;
        std::vector<int>     hit_rows;   // row of each match, in order
        int           out_lines   = 0;   // output lines (pre-wrap)
        std::size_t   words       = 0;   // thinking: word count
        bool          plain       = false;
        // For the reveal: each row's text span in `shown`, in codepoints
        // (len 0 for rows with no text), and the total. Filled for plain
        // and thinking bodies only; empty = this body doesn't reveal.
        std::vector<std::size_t> row_cp_begin, row_cp_len;
        std::size_t   total_cp    = 0;
        std::size_t   reveal_row0 = 0;   // first body row (after the $ command)
        // Thinking, built incrementally while it streams: the source seen
        // last build, and a resume point at its last complete line (bytes
        // into `shown`, rows and reveal spans kept, counters at that point).
        std::size_t   thinking_src_len  = 0;
        std::string   thinking_src_head;
        std::size_t   thinking_shown_cut = 0, thinking_keep_rows = 0, thinking_keep_cp_rows = 0;
        std::size_t   thinking_lines = 0, thinking_cut_cp = 0;
        std::size_t   words_base = 0, words_cut = 0;
    };
    static BodyCache cache;   // UI thread only, like the other pickers' statics

    // Text width inside the pane. The overlay is 4 columns narrower than
    // the terminal, the frame and its padding take 6 more, and 1 is the
    // scrollbar gutter. While searching, a 1-column margin marks the match.
    const int pane_w   = std::max(20, panel_terminal_cols() - 11 - (o->query.empty() ? 0 : 1));
    const void* entries_key = static_cast<const void*>(o->entries.data());
    const void* bytes_key   = static_cast<const void*>(e.output.data());
    const auto call_key     = e.call.compute_render_key();
    std::string child_open_key;
    for (const auto& id : o->child_open) { child_open_key += id; child_open_key += '\x1f'; }
    // A thinking block that only GREW since last frame (it's still
    // streaming): keep every row up to the last complete line and rebuild
    // just the tail. Rebuilding all of a 200 KB block per delta was ~60 ms a
    // frame, which is what made scrolling a live ^U feel stuck.
    const bool grew_only = e.thinking && cache.entries_key == entries_key && cache.index == cur
        && cache.width == pane_w && cache.query == o->query && o->query.empty()
        && cache.thinking_src_len > 0 && e.output.size() > cache.thinking_src_len
        && cache.thinking_keep_rows <= cache.rows.size()
        && cache.thinking_keep_rows > cache.reveal_row0
        && std::string_view{e.output}.substr(0, cache.thinking_src_len) == cache.thinking_src_head;
    if (grew_only) {
        cache.rows.resize(cache.thinking_keep_rows);
        cache.row_cp_begin.resize(std::min(cache.row_cp_begin.size(), cache.thinking_keep_cp_rows));
        cache.row_cp_len.resize(std::min(cache.row_cp_len.size(), cache.thinking_keep_cp_rows));
        cache.bytes_key = bytes_key;
        cache.bytes_len = e.output.size();
        cache.call_key  = call_key;
    }
    if (grew_only || cache.entries_key != entries_key || cache.index != cur
        || cache.bytes_key != bytes_key || cache.bytes_len != e.output.size()
        || cache.call_key != call_key || cache.width != pane_w
        || cache.query != o->query
        || cache.child_cursor != o->child_cursor || cache.child_open_key != child_open_key) {
      if (!grew_only) {
        cache.child_cursor   = o->child_cursor;
        cache.child_open_key = std::move(child_open_key);
        cache.entries_key = entries_key;
        cache.index       = cur;
        cache.bytes_key   = bytes_key;
        cache.bytes_len   = e.output.size();
        cache.call_key    = call_key;
        cache.width       = pane_w;
        cache.query       = o->query;
        cache.rows.clear();
        cache.hit_rows.clear();
        cache.out_lines = 0;
        cache.words     = 0;
        cache.plain     = false;
        cache.row_cp_begin.clear();
        cache.row_cp_len.clear();
        cache.total_cp  = 0;
        cache.thinking_src_len = 0;
        cache.thinking_shown_cut = cache.thinking_keep_rows = cache.thinking_keep_cp_rows = 0;
        cache.thinking_lines = cache.thinking_cut_cp = 0;
      }

        // One screen row of text: base colour, output tokens on top (when
        // asked), then find hits on top of everything -- maya paints them.
        auto line_row = [&](std::string_view s, Style base, bool tokens) {
            return maya::ToolViewer::output_span(s, base, tokens, tool_output::hit_spans(s, o->query));
        };
        auto note_hits = [&](std::string_view s) {
            if (!o->query.empty() && !tool_output::hit_spans(s, o->query).empty())
                cache.hit_rows.push_back(static_cast<int>(cache.rows.size()));
        };

        // A command-running tool shows its full command first: the header's
        // copy is one clipped line.
        if (e.call.args.is_object()) {
            std::string command;
            if (auto it = e.call.args.find("command");
                it != e.call.args.end() && it->is_string())
                command = it->get<std::string>();
            if (!command.empty()) {
                const Color cmd_hue = e.failed ? danger : tool_hue;
                bool first = true;
                std::string_view rest{command};
                while (!rest.empty() && (rest.back() == '\n' || rest.back() == ' '))
                    rest.remove_suffix(1);
                for (std::size_t p = 0; p <= rest.size();) {
                    auto nl = rest.find('\n', p);
                    if (nl == std::string_view::npos) nl = rest.size();
                    for (auto v : wrap_rows(rest.substr(p, nl - p), std::max(10, pane_w - 2))) {
                        note_hits(v);
                        cache.rows.push_back(Line::row({
                            Span{first ? "$ " : "  ", fg_bold(cmd_hue)},
                            line_row(v, fg_of(fg), false)}));
                        first = false;
                    }
                    if (nl == rest.size()) break;
                    p = nl + 1;
                }
                cache.rows.push_back(Line::rule());
            }
        }

        using Kind = maya::ToolBodyPreview::Kind;
        auto bp = tool_body_preview_config(e.call);
        // The structured previews (diff, file, todo) already read well; with
        // a search on, everything is plain text so matches can be marked.
        const bool structured = o->query.empty() && !e.failed
            && (bp.kind == Kind::EditDiff || bp.kind == Kind::GitDiff
                || bp.kind == Kind::FileRead || bp.kind == Kind::FileWrite
                || bp.kind == Kind::TodoList);
        const std::string shown = tool_output::display_text(e);
        // Codepoint position of a byte in `shown`, walked forward as rows are
        // built (rows come in order, so this is linear overall).
        std::size_t walk_byte = 0, walk_cp = 0;
        auto cp_at = [&](const char* p) {
            const auto b = static_cast<std::size_t>(p - shown.data());
            for (; walk_byte < b && walk_byte < shown.size(); ++walk_byte)
                if ((static_cast<unsigned char>(shown[walk_byte]) & 0xC0) != 0x80) ++walk_cp;
            return walk_cp;
        };
        auto note_span = [&](std::string_view v) {
            cache.row_cp_begin.push_back(cp_at(v.data()));
            std::size_t n = 0;
            for (const char c : v)
                if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
            cache.row_cp_len.push_back(n);
        };
        auto note_blank = [&](const char* p) {
            cache.row_cp_begin.push_back(cp_at(p));
            cache.row_cp_len.push_back(0);
        };
        if (!grew_only) cache.reveal_row0 = cache.rows.size();

        // A subagent (`task`) reads as what it did, then what it found: every
        // call it made, one row each, then its report as prose -- headings,
        // bold, `code` and bullets drawn, not printed as markdown. A search
        // falls through to the plain numbered view so hits can be marked.
        const bool task_view = e.call.name.value == "task" && o->query.empty()
                               && !e.thinking && (!e.call.children.empty() || !e.is_live);

        if (task_view) {
            using S = ToolUse::Child::State;
            const auto& kids = e.call.children;
            // While it runs: the same progress row the card shows (turn
            // budget bar, time, tool count), so ^O isn't the less-informed
            // view of a live agent.
            if (e.is_live) {
                std::string_view feed = e.call.progress_text();
                if (feed.starts_with("\xe2\x97\x86 ")) {   // ◆ status line
                    if (auto row = task_status_row(feed.substr(0, feed.find('\n'))); !row.empty())
                        cache.rows.push_back(Line::row({Span{row, fg_of(tool_hue)}}));
                }
            }
            if (!kids.empty()) {
                int bad = 0;
                for (const auto& c : kids)
                    bad += c.state == S::Failed && c.output != ToolUse::kCancelledOutput;
                std::string head = std::to_string(kids.size()) + (kids.size() == 1 ? " call" : " calls");
                if (bad > 0) head += "  \xc2\xb7  " + std::to_string(bad) + " failed";
                head += "  \xc2\xb7  Tab to pick, Enter to open";
                cache.rows.push_back(Line::row({Span{head, fg_dim(muted)}}));
                const auto t0 = e.call.started_at();
                const int out_w = std::max(10, pane_w - 6);
                for (std::size_t ci = 0; ci < kids.size(); ++ci) {
                    const auto& c = kids[ci];
                    const ToolUse kt = child_as_tool_use(c, t0);
                    const bool cancelled = c.state == S::Failed && c.output == ToolUse::kCancelledOutput;
                    const bool failed = c.state == S::Failed && !cancelled;
                    const bool selected = o->child_cursor == static_cast<int>(ci);
                    const bool open = std::ranges::find(o->child_open, c.id) != o->child_open.end();
                    const char* icon = c.state == S::Done ? "\xe2\x9c\x93 "     // ✓
                                     : failed ? "\xe2\x9c\x97 "                  // ✗
                                     : cancelled ? "\xe2\x8a\x98 "               // ⊘
                                                 : "\xe2\x97\x8b ";             // ○
                    const Color ic = c.state == S::Done ? Color{success}
                                   : failed ? Color{danger} : Color{muted};
                    // Always a duration cell, so the column lines up: the
                    // time once settled, "…" while running, "–" for none.
                    std::string dur = c.state == S::Running ? std::string{"   \xe2\x80\xa6"}
                                    : c.ms >= 1000 ? std::format("{:.1f}s", static_cast<double>(c.ms) / 1000.0)
                                    : c.ms > 0     ? std::format("{}ms", c.ms)
                                                   : std::string{"\xe2\x80\x93"};
                    if (dur.size() < 7) dur.insert(0, 7 - std::min<std::size_t>(7, string_width(dur)), ' ');
                    Span what{tool_timeline_detail(kt), failed ? fg_of(danger) : fg_dim(fg)};
                    what.fill = true;
                    cache.rows.push_back(Line::row({
                        Span{selected ? "\xe2\x96\xb8" : " ", fg_bold(accent)},   // ▸ cursor
                        Span{icon, fg_bold(ic)},
                        Span{tool_display_name(c.name),
                             cancelled ? fg_bold(muted) : fg_bold(tool_category_color(c.name))},
                        Span{"  "},
                        std::move(what),
                        Span{"  " + dur, fg_dim(muted)}}));
                    // Opened: the call's own output under it, on a rail.
                    if (open) {
                        std::string_view body = c.output;
                        if (body.empty())
                            body = c.state == S::Running ? "(still running)" : "(no output)";
                        int shown_lines = 0;
                        constexpr int kMaxOpenLines = 40;
                        for (std::size_t p = 0; p <= body.size() && shown_lines < kMaxOpenLines;) {
                            auto nl = body.find('\n', p);
                            if (nl == std::string_view::npos) nl = body.size();
                            for (auto v : wrap_rows(body.substr(p, nl - p), out_w)) {
                                if (shown_lines++ >= kMaxOpenLines) break;
                                Span txt{std::string{v}, failed ? fg_of(danger) : fg_dim(fg)};
                                txt.fill = true;
                                cache.rows.push_back(Line::row({
                                    Span{"   \xe2\x94\x8a ", fg_dim(failed ? danger : muted)},   // ┊
                                    std::move(txt)}));
                            }
                            if (nl == body.size()) break;
                            p = nl + 1;
                        }
                        if (shown_lines >= kMaxOpenLines)
                            cache.rows.push_back(Line::row({Span{"   \xe2\x94\x8a \xe2\x8b\xaf more in the full log",
                                                                fg_italic(muted)}}));
                    }
                }
            }

            if (!e.is_live && !shown.empty()) {
                if (!kids.empty()) cache.rows.push_back(Line::rule());
                const Style prose  = e.failed ? fg_of(danger) : fg_of(fg);
                const Style strong = fg_bold(fg);
                const Style head_st = fg_bold(tool_hue);
                const Style code_st = fg_of(accent);
                const Style mark_st = fg_dim(tool_hue);
                const int text_w = std::max(10, pane_w - 3);
                // One source line (inline markdown already resolved by
                // maya) -> wrapped rows under the rail, with a hanging
                // indent so a bullet's continuation lines up.
                using Marked = maya::ToolViewer::Marked;
                auto emit = [&](const std::string& lead, Style lead_st, const Marked& md, Style base) {
                    const int lead_w = string_width(lead);
                    bool first = true;
                    for (auto v : wrap_rows(md.plain, std::max(4, text_w - lead_w))) {
                        const auto off = static_cast<std::size_t>(v.data() - md.plain.data());
                        Span txt = maya::ToolViewer::marked_span(md, off, v.size(), base);
                        cache.rows.push_back(Line::row({
                            Span{"\xe2\x94\x83 ", fg_dim(tool_hue)},   // ┃
                            Span{first ? lead : std::string(static_cast<std::size_t>(lead_w), ' '), lead_st},
                            std::move(txt)}));
                        first = false;
                    }
                };
                // A blank line is drawn only once more text follows it, so
                // the report never starts or ends on an empty rail row.
                bool in_code = false, any = false, pending_blank = false;
                auto flush_blank = [&] {
                    if (pending_blank && any)
                        cache.rows.push_back(Line::row({Span{"\xe2\x94\x83", fg_dim(tool_hue)}}));
                    pending_blank = false;
                    any = true;
                };
                for (std::size_t p = 0; p <= shown.size();) {
                    auto nl = shown.find('\n', p);
                    if (nl == std::string::npos) nl = shown.size();
                    std::string_view ln{shown.data() + p, nl - p};
                    p = nl + 1;
                    while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.remove_suffix(1);
                    // The runner's own header lines: the card header says it.
                    if (ln.starts_with("Subagent report (") || ln.starts_with("tools: ")) {
                        if (nl == shown.size()) break;
                        continue;
                    }
                    std::string_view t = ln;
                    while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
                    const std::size_t indent = ln.size() - t.size();
                    if (t.starts_with("```")) {
                        in_code = !in_code;
                    } else if (in_code) {
                        flush_blank();
                        emit("  ", code_st, Marked{std::string{ln}, std::vector<Style>(ln.size(), code_st)}, code_st);
                    } else if (t.empty()) {
                        pending_blank = true;
                    } else if (t.starts_with('|') && t.find_first_not_of("|-: ") == std::string_view::npos) {
                        // table separator row: drop
                    } else {
                        std::string lead(indent, ' ');
                        Style base = prose;
                        std::string_view body = t;
                        std::string table;
                        if (body.starts_with('#')) {
                            while (body.starts_with('#')) body.remove_prefix(1);
                            while (body.starts_with(' ')) body.remove_prefix(1);
                            base = head_st;
                        } else if (body.starts_with("- ") || body.starts_with("* ")) {
                            lead += "\xe2\x80\xa2 ";   // •
                            body.remove_prefix(2);
                        } else if (std::size_t d = 0;
                                   [&] { while (d < body.size() && body[d] >= '0' && body[d] <= '9') ++d;
                                         return d > 0 && d + 1 < body.size()
                                             && (body[d] == '.' || body[d] == ')') && body[d + 1] == ' '; }()) {
                            lead += std::string{body.substr(0, d + 2)};
                            body.remove_prefix(d + 2);
                        } else if (body.starts_with('|')) {
                            // "| a | b |" -> "a · b"
                            for (std::size_t i = 1; i < body.size();) {
                                const auto j = body.find('|', i);
                                std::string_view cell = body.substr(i, (j == std::string_view::npos ? body.size() : j) - i);
                                while (!cell.empty() && cell.front() == ' ') cell.remove_prefix(1);
                                while (!cell.empty() && cell.back() == ' ') cell.remove_suffix(1);
                                if (!cell.empty()) { if (!table.empty()) table += " \xc2\xb7 "; table += cell; }
                                if (j == std::string_view::npos) break;
                                i = j + 1;
                            }
                            body = table;
                        }
                        const Marked md = maya::ToolViewer::inline_markdown(body, base, strong, code_st);
                        flush_blank();
                        emit(lead, mark_st, md, base);
                    }
                    if (nl == shown.size()) break;
                }
            }
        } else if (structured) {
            bp.show_all   = true;       // the full output, no "⋯ N more"
            bp.tail_only  = e.is_live;  // live: newest at the bottom
            bp.show_streaming_placeholder = false;
            // One row per screen row, so the window can address rows.
            for (auto& row : maya::ToolViewer::preview_rows(std::move(bp)))
                cache.rows.push_back(std::move(row));
        } else if (e.thinking && !shown.empty()) {
            if (!grew_only) cache.reveal_row0 = cache.rows.size();
            // Prose, the way the transcript draws it: the brand rail, word
            // wrap, no line numbers. Headings and lines that are all bold
            // (the model's "beats") stand out; a bare `**` is dropped.
            const int text_w = std::max(10, pane_w - 3);
            const Style prose = fg_of(fg);
            const Style beat  = fg_bold(role_brand);
            std::size_t lines = 0;
            // Resume after the rows kept from last frame (grew_only): skip
            // to the first byte not yet built, carrying the counters. The
            // reveal's codepoint walk restarts from the same byte.
            std::size_t start = 0;
            if (grew_only && cache.thinking_shown_cut <= shown.size()) {
                start = cache.thinking_shown_cut;
                lines = cache.thinking_lines;
                walk_byte = start;
                walk_cp   = cache.thinking_cut_cp;
            }
            for (std::size_t p = start; p <= shown.size();) {
                auto nl = shown.find('\n', p);
                if (nl == std::string::npos) nl = shown.size();
                std::string_view ln{shown.data() + p, nl - p};
                while (!ln.empty() && ln.back() == '\r') ln.remove_suffix(1);
                ++lines;
                std::string_view body = ln;
                bool strong = false;
                if (body.starts_with("#")) {
                    while (!body.empty() && body.front() == '#') body.remove_prefix(1);
                    while (!body.empty() && body.front() == ' ') body.remove_prefix(1);
                    strong = true;
                } else if (body.size() > 4 && body.starts_with("**") && body.ends_with("**")
                           && body.substr(2, body.size() - 4).find("**") == std::string_view::npos) {
                    body = body.substr(2, body.size() - 4);
                    strong = true;
                }
                if (body.empty()) {
                    note_blank(ln.data());
                    cache.rows.push_back(Line::row({Span{"\xe2\x94\x83", fg_dim(role_brand)}}));   // ┃
                } else {
                    for (auto v : wrap_rows(body, text_w)) {
                        note_hits(v);
                        note_span(v);
                        cache.rows.push_back(Line::row({
                            Span{"\xe2\x94\x83 ", fg_dim(role_brand)},
                            line_row(v, strong ? beat : prose, false)}));
                    }
                }
                if (nl == shown.size()) break;
                p = nl + 1;
                // Every row so far ends on a complete line: a safe resume
                // point for when the block grows.
                cache.thinking_shown_cut   = p;
                cache.thinking_keep_rows   = cache.rows.size();
                cache.thinking_keep_cp_rows = cache.row_cp_begin.size();
                cache.thinking_lines       = lines;
                cache.thinking_cut_cp      = cp_at(shown.data() + p);
            }
            cache.plain     = true;
            cache.out_lines = static_cast<int>(lines);
            cache.total_cp  = cp_at(shown.data() + shown.size());
            // Words: [0, words_cut) counted once and kept; the tail after the
            // last complete line recounted each build (it's short).
            auto count_words = [&](std::size_t from, std::size_t to) {
                std::size_t w = 0; bool in = false;
                for (std::size_t i = from; i < to; ++i) {
                    const char ch = shown[i];
                    const bool sp = ch == ' ' || ch == '\n' || ch == '\t';
                    if (!sp && !in) ++w;
                    in = !sp;
                }
                return w;
            };
            const std::size_t cut = std::min(cache.thinking_shown_cut, shown.size());
            if (!grew_only || cache.words_cut > cut) { cache.words_base = 0; cache.words_cut = 0; }
            // Every complete line ends in '\n', so a word never straddles
            // words_cut and the two counts simply add.
            cache.words_base += count_words(cache.words_cut, cut);
            cache.words_cut   = cut;
            cache.words       = cache.words_base + count_words(cut, shown.size());
            // Remember what this build saw, for the grew-only check.
            cache.thinking_src_len  = e.output.size();
            cache.thinking_src_head = e.output;
        } else if (!shown.empty()) {
            cache.plain = true;
            const Color pipe_hue = e.failed ? danger : tool_hue;
            std::vector<std::string_view> lines;
            for (std::size_t p = 0; p <= shown.size();) {
                auto nl = shown.find('\n', p);
                if (nl == std::string::npos) nl = shown.size();
                lines.emplace_back(shown.data() + p, nl - p);
                if (nl == shown.size()) break;
                p = nl + 1;
            }
            cache.out_lines = static_cast<int>(lines.size());
            const int gutter_w = static_cast<int>(std::to_string(lines.size()).size());
            // " N " gutter, then "│ ".
            const int text_w = std::max(10, pane_w - gutter_w - 4);
            const Style base = e.failed ? fg_of(danger) : fg_of(fg);
            const std::string blank_gutter(static_cast<std::size_t>(gutter_w) + 2, ' ');
            cache.rows.reserve(cache.rows.size() + lines.size());
            for (std::size_t i = 0; i < lines.size(); ++i) {
                std::string num = std::to_string(i + 1);
                num.insert(0, static_cast<std::size_t>(gutter_w) - num.size(), ' ');
                const bool cut = lines[i].starts_with("\xe2\x8b\xaf ");   // ⋯ note
                bool first = true;
                for (auto v : wrap_rows(lines[i], text_w)) {
                    note_hits(v);
                    if (v.empty()) note_blank(lines[i].data()); else note_span(v);
                    cache.rows.push_back(Line::row({
                        Span{first ? " " + num + " " : blank_gutter, fg_dim(muted)},
                        Span{first ? "\xe2\x94\x82 " : "\xe2\x94\x8a ", fg_dim(pipe_hue)},   // │ ┊
                        cut ? line_row(v, fg_italic(muted), false)
                            : line_row(v, base, !e.failed)}));
                    first = false;
                }
            }
            cache.total_cp = cp_at(shown.data() + shown.size());
        } else if (!e.is_live) {
            cache.rows.push_back(Line::row({Span{"  (no output)", fg_italic(muted)}}));
        }
    }

    // Thinking ends the way the transcript's block does: a blank row, then
    // "✦ Reasoned · 216 tokens · 4.3s · 50 tok/s". Built per frame (the
    // clock ticks while live), so it sits after the cached rows.
    std::optional<maya::ToolViewer::Tail> viewer_tail;
    if (e.thinking && !cache.rows.empty() && o->query.empty()) {
        const std::size_t toks = (e.output.size() + 3) / 4;
        auto tok_label = [](std::size_t t) {
            if (t < 1000) return std::to_string(t);
            if (t < 10000) return std::format("{}.{}k", t / 1000, (t % 1000) / 100);
            return std::format("{}k", (t + 500) / 1000);
        };
        std::string meta;
        if (toks > 0)
            meta += "  \xc2\xb7  " + tok_label(toks) + (thinking_live ? " tok" : " tokens");
        if (thought_ms > 0) {
            meta += "  \xc2\xb7  ";
            if (thought_ms >= 60'000)
                meta += std::format("{}m{:02}s", thought_ms / 60'000, (thought_ms / 1000) % 60);
            else if (thought_ms >= 10'000)
                meta += std::format("{}s", (thought_ms + 500) / 1000);
            else
                meta += std::format("{:.1f}s", static_cast<double>(thought_ms) / 1000.0);
        }
        if (thought_ms >= 1500 && toks > 0)
            if (const long r = std::lround(static_cast<double>(toks) * 1000.0
                                           / static_cast<double>(thought_ms)); r > 0)
                meta += std::format("  \xc2\xb7  {} tok/s", r);
        viewer_tail = maya::ToolViewer::Tail{thinking_live, meta, role_brand, text_secondary};
    }

    // Window, typewriter, status, hints and fitting: maya::ToolViewer.
    const int nhits = static_cast<int>(cache.hit_rows.size());
    const int cur_hit = nhits > 0 ? ((o->hit % nhits) + nhits) % nhits : -1;
    // Find: bring the current match into view once per jump.
    static int seen_seq = -1;
    const bool jump_to_hit = cur_hit >= 0 && seen_seq != o->hit_seq;
    seen_seq = o->hit_seq;

    maya::ToolViewer::Config vc;
    vc.base   = std::move(cfg);
    vc.header = std::move(viewer_header);
    vc.rows   = &cache.rows;
    vc.tail   = std::move(viewer_tail);

    // ── Reveal ── the transcript's typewriter on text arriving while you
    // watch, gated by the same Motion pref and env switches. Only plain and
    // thinking bodies.
    const bool running = e.is_live || e.streaming;
    const bool reveal_on = m.env.reveal && m.env.reveal_typewriter
                           && agentty::ui_prefs::animations_on()
                           && !cache.row_cp_begin.empty() && o->query.empty();
    std::size_t typed = cache.total_cp;
    if (reveal_on) typed = advance_reveal(e, cache.total_cp, running);
    const bool revealing = reveal_on && typed < cache.total_cp;
    if (revealing) maya::anim::keep_animating();
    vc.reveal = {revealing, typed, cache.reveal_row0, &cache.row_cp_begin, &cache.row_cp_len,
                 overlay_reveal().cursor.effective_rate(),
                 agentty::ui_prefs::reveal_decoration_on() && m.env.reveal_decorate};

    vc.query       = o->query;
    vc.typing      = o->typing;
    vc.hits        = nhits;
    vc.current_hit = cur_hit;
    vc.current_row = cur_hit >= 0 ? cache.hit_rows[static_cast<std::size_t>(cur_hit)] : -1;
    vc.jump_to_hit = jump_to_hit;
    vc.scroll      = &m.ui.tool_viewer_scroll;
    vc.y_before    = m.ui.tool_viewer_scroll.y;
    // A running entry follows its newest output unless the user scrolled up.
    vc.follow      = running && m.ui.tool_viewer_tail && o->query.empty();
    vc.compact     = compact_body;
    if (cache.plain && cache.out_lines > 0)
        vc.count = e.thinking
            ? std::to_string(cache.words) + (cache.words == 1 ? " word" : " words")
            : std::to_string(cache.out_lines) + (cache.out_lines == 1 ? " line" : " lines");
    // Watching: you'll be taken to each new entry as it starts.
    const bool watching = o->following && o->index == 0 && o->query.empty()
                          && !o->typing && m.ui.tool_viewer_tail;
    vc.following   = (following_kind || watching) && m.ui.tool_viewer_tail && o->query.empty();
    vc.where_color = following_kind ? tool_hue : Color{muted};
    vc.fresh_above = o->fresh_above;

    auto hint = [](std::string k, std::string l, int p) { return maya::KeyHint{std::move(k), std::move(l), p, fg}; };
    if (o->typing) {
        vc.hints = {hint("Enter", "keep", 5),
                    hint("\xe2\x86\x91\xe2\x86\x93", "prev/next match", 4),   // ↑↓
                    hint("Esc", "clear", 3)};
    } else {
        vc.hints = {hint("\xe2\x86\x91\xe2\x86\x93", "scroll", 6),        // ↑↓
                    hint("PgUp/PgDn", "page", 3),
                    hint("/", "find", 5)};
        if (!o->query.empty()) vc.hints.push_back(hint("n/N", "next/prev", 5));
        if (following_kind || o->following) vc.hints.push_back(hint("End", "follow", 4));
        if (o->entries.size() > 1)
            vc.hints.push_back(hint("\xe2\x86\x90\xe2\x86\x92",   // ←→
                                    o->thinking_only ? "older/newer" : "other output", 4));
        if (o->fresh_above > 0) vc.hints.push_back(hint("t", "newest", 6));
        if (!e.is_live) vc.hints.push_back(hint("y", "copy", 4));
        vc.hints.push_back(hint("Esc", !o->query.empty() ? "clear"
                                       : o->thinking_only ? "all blocks" : "back", 3));
    }

    // The overlay floats 2 rows above the bottom and is 4 columns narrower
    // than the terminal (compose_panel / maya::Overlay), so that's the room.
    // What fit last frame is kept, keyed on this entry and size.
    static maya::ToolViewer::FitMemo fit;   // UI thread only, like the body cache
    vc.budget        = std::max(3, body_term_rows - 2);
    vc.cols          = std::max(10, panel_terminal_cols() - 4);
    vc.memo          = &fit;
    vc.term_rows     = body_term_rows;
    vc.index         = cur;
    vc.thinking_only = o->thinking_only;
    vc.text = fg; vc.muted = muted; vc.warn = warn; vc.danger = danger; vc.highlight = highlight;
    return maya::ToolViewer{std::move(vc)}.build();
}

} // namespace agentty::ui
