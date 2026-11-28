// symbol.cpp — the #-symbol palette.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"
#include "agentty/runtime/view/look.hpp"
#include <maya/widget/agentty/rows.hpp>

namespace agentty::ui {

Element symbol_panel(const Model& m) {
    auto* o = m.ui.panel.get<pn::Symbol>();
    if (!o) return maya::Panel::empty();

    const auto& matches = o->picker.filtered();
    const auto& entries = o->picker.entries();

    Panel::Config cfg;
    cfg.title      = " Symbol ";
    cfg.accent     = highlight;
    cfg.min_width  = kPanelWide;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.symbol_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->picker.index();

    // The sigil is the trigger character, so the header reads as a
    // continuation of what was typed in the composer.
    cfg.header.push_back(
        filter_header(o->picker.query(), "symbols", highlight, "# "));
    cfg.header.push_back(ui::pane::Rule{});

    if (entries.empty()) {
        cfg.prebuilt.push_back(ui::pane::Empty{o->picker.source_ready()
                ? "no symbols indexed"
                : "indexing symbols… (type to filter as it fills)"});
    } else if (matches.empty()) {
        cfg.prebuilt.push_back(ui::pane::Empty{"no matches"});
    } else {
        cfg.items.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& sym = entries[matches[static_cast<std::size_t>(i)]];
            auto [fname, dir] = split_name_dir(sym.path);
            Panel::Item row;
            // Combine symbol name + locus into the leading cell so a
            // long parent-dir trailing still has room to render; the
            // "name  file:line" pair is what the user is scanning.
            row.leading        = sym.name + "  " + std::string{fname}
                               + ":" + std::to_string(sym.line_number);
            row.leading_style  = fg_of(fg);
            // Highlight the matched chars of the symbol NAME (which is the
            // leading segment, so its offsets map directly onto row.leading).
            if (!o->picker.query().empty()) {
                auto fm = fuzzy::score(sym.name, o->picker.query());
                if (fm.matched()) { row.highlight = std::move(fm.positions);
                                    row.highlight_fg = highlight; }
            }
            row.trailing       = parent_segment(dir);
            row.trailing_style = fg_dim(muted);
            cfg.items.push_back(std::move(row));
        }
    }

    if (static_cast<int>(matches.size()) > kViewportH) {
        cfg.footer.push_back(ui::pane::Position{.at = static_cast<int>(o->picker.index()),
                                            .of = static_cast<int>(matches.size())});
    }

    // Same gap as the @ picker: the keys worked, nothing said so.
    cfg.footer.push_back(key_hints({
        {ui::mark::updown, "move", 5},   // ↑↓
        {"Enter", matches.empty() ? "" : "insert", 6},
        {"Esc", "close", 4},
    }));

    return Panel{std::move(cfg)}.build();
}


} // namespace agentty::ui
