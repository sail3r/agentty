// stats.cpp — the stats panel. Table-driven: no per-tab branches.
//
// The whole view is:
//
//   for each Section in the active tab's TabDesc
//       extract() its Metrics
//       build ONE card Element from them, chosen by the Section's Viz
//       push the card into cfg.prebuilt
//
// Adding a tab touches this file NOT AT ALL. That is the design's claim,
// and this file is where it either holds or does not.
//
// Formatting lives in domain/stats/unit.hpp (one format() for the whole
// panel), and each Viz maps to one standalone maya widget (Donut,
// Sparkline, Gauge, BarChart, Histogram, LineChart). The cards flow into
// responsive columns via maya::Panel's col_max_width layout, so this file
// owns exactly one thing: which theme colour a domain hue slot maps to.

#include "panels_prologue.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <maya/widget/agentty/stats_body.hpp>
#include <maya/widget/agentty/tab_strip.hpp>

#include "agentty/domain/stats/tabs.hpp"
#include "agentty/runtime/panel/stats.hpp"

namespace agentty::ui {
namespace {


}  // namespace

Element stats_panel(const Model& m) {
    const auto* o = m.ui.panel.get<pn::Stats>();
    if (!o) return Panel::empty();

    // One incremental refresh. On a settled thread this folds nothing; while
    // streaming it folds exactly the live tail. See domain/stats/facts.hpp
    // for why the cursor and the epoch guard are shaped the way they are.
    const stats::Facts& f = o->projection.refresh(m.d.current);

    // A tab can become unavailable under the user (the last tool call was
    // rewound away). Landing on it would show an empty panel with no way to
    // understand why, so the selection is corrected here rather than
    // defended in every extractor.
    const auto visible = stats::visible_tabs(f);
    stats::Tab active = o->tab;
    if (!stats::tab_available(active, f)) active = visible.front();
    o->tab = active;

    maya::panel::Config cfg;
    cfg.title    = "Stats";
    cfg.subtitle = std::string{stats::tab_subtitle(active)};
    cfg.accent   = maya::theme::series(maya::theme::live(), 0);

    // Tabs are the WIDGET's chrome, not this host's: it owns the padding,
    // the selected treatment and how the strip degrades on a narrow frame,
    // so every tabbed panel looks the same by construction.
    //
    // Editor marking because these tabs are PEERS you switch between, not
    // views of one underlying thing — " │ " dividers carry the structure
    // and there is no underline rule. Filled because a mark that works by
    // CONTRAST says nothing about the first tab in the strip; a chip
    // states "this is the live view" on its own.
    //
    // The strip shows only the AVAILABLE tabs, so a session that never ran
    // a tool has no Tools chip to land on — an empty tab is a tab admitting
    // it should not have been drawn.
    cfg.tabs.reserve(visible.size());
    for (auto t : visible) cfg.tabs.emplace_back(stats::tab_title(t));
    cfg.tab_active = 0;
    for (std::size_t i = 0; i < visible.size(); ++i)
        if (visible[i] == active) { cfg.tab_active = static_cast<int>(i); break; }
    cfg.tab_mark = maya::TabMark::Editor;
    cfg.tab_fill = true;

    // The panel body: one card per section, fanned into responsive columns
    // by maya::viewport() — the same layout the agent_stats example is built
    // around. viewport() picks the column count from a width CEILING (a slot
    // at or under max_width is one column, over it two, over twice it
    // three…), fills left-to-right then wraps, and keeps every card
    // responsive to its OWN column rather than the screen.
    //
    // The whole grid goes in as ONE prebuilt Element. Panel only runs its
    // own column flow for `items`, and it measures a prebuilt body against
    // an unbounded probe (1<<14) — so the grid is handed the REAL width via
    // .width, which is both what makes the columns come out right and what
    // stops a self-sizing chart from sizing itself to sixteen thousand
    // columns and then losing its tail when painted in the real body.
    // Extract and FORMAT every section's figures (formatting is the
    // domain's: domain/stats/unit.hpp); maya::StatsBody lays them out.
    maya::StatsBody::Config body;
    body.viewport_h = panel_detail::panel_viewport_h();
    const auto& sections = stats::tab_desc(active).sections;
    std::vector<stats::Metric> scratch;
    scratch.reserve(32);
    // What the column widths depend on: every figure this tab shows, the
    // tab, and the viewport height (a donut sizes to it). The fit layout
    // keeps its plan until this changes.
    std::uint64_t fit_key = 0xcbf29ce484222325ull;
    auto mix = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { fit_key ^= b[i]; fit_key *= 0x100000001b3ull; }
    };
    auto mix_s = [&](const std::string& s) { mix(s.data(), s.size()); mix("\0", 1); };
    mix(&active, sizeof active);
    mix(&body.viewport_h, sizeof body.viewport_h);
    for (const auto& sec : sections) {
        maya::StatsBody::Section out;
        out.heading = std::string{sec.heading};
        using BV = maya::StatsBody::Viz;
        switch (sec.viz) {
            case stats::Viz::Kv:    out.viz = BV::Kv;    break;
            case stats::Viz::Bars:  out.viz = BV::Bars;  break;
            case stats::Viz::Spark: out.viz = BV::Spark; break;
            case stats::Viz::Band:  out.viz = BV::Band;  break;
            case stats::Viz::Donut: out.viz = BV::Donut; break;
            case stats::Viz::Plot:  out.viz = BV::Plot;  break;
            case stats::Viz::Hero:  out.viz = BV::Hero;  break;
            case stats::Viz::Dist:  out.viz = BV::Dist;  break;
            case stats::Viz::Hist:  out.viz = BV::Hist;  break;
        }
        scratch.clear();
        if (sec.extract) sec.extract(f, scratch);
        for (const auto& mt : scratch) {
            mix_s(mt.label);
            mix_s(mt.detail);
            mix(&mt.value, sizeof mt.value);
            for (const auto& pt : mt.parts) { mix_s(pt.label); mix(&pt.value, sizeof pt.value); }
            mix(mt.series.data(), mt.series.size() * sizeof(double));
            maya::StatsBody::Metric bm;
            bm.label      = mt.label;
            bm.value_text = stats::format(mt.unit, mt.value);
            bm.ratio      = mt.unit == stats::Unit::Ratio;
            bm.value      = mt.value;
            bm.of         = mt.of;
            bm.detail     = mt.detail;
            for (const auto& pt : mt.parts) bm.parts.push_back({pt.label, pt.value, pt.hue});
            bm.series     = mt.series;
            bm.format     = [u = mt.unit](double v) { return stats::format(u, v); };
            out.metrics.push_back(std::move(bm));
        }
        body.sections.push_back(std::move(out));
    }
    if (fit_key == 0) fit_key = 1;   // 0 means "don't cache"
    body.fit_key = fit_key;
    cfg.prebuilt.push_back(maya::StatsBody{std::move(body)}.build());

    // No row selection: this is a document, not a picker.
    cfg.selected = -1;

    // Flow cards into responsive columns. A card needs ~30 columns to keep
    // its labels and values whole; capped at 40 so a very wide terminal
    // does not stretch a two-value tile across half the screen. The gap
    // keeps neighbouring cards from reading as one.
    // NOTE: col_max_width/col_min_width/col_gap are NOT set. Panel only runs
    // its column flow for `items`; a `prebuilt` body is one opaque stack —
    // here, the single viewport() grid above, which does the columns itself.

    // No rule off the headings. The accent heading and the column split
    // already separate the sections; a rule across every heading turns a
    // page of figures into a form.
    cfg.header_rule = false;

    cfg.scroll = &o->scroll;
    // Body height: the same one every other panel uses. A panel that matches
    // its siblings is worth more than one tuned to its own content; overflow
    // scrolls, which is what the scrollbar is for.
    cfg.viewport_h = panel_detail::panel_viewport_h();

    // Hold that height rather than shrink-wrapping to the tab. These tabs
    // differ in length, so without this the frame would resize every time
    // the reader pressed tab.
    cfg.fixed_viewport = true;

    // Keys go through the shared key_hints helper, not a hand-spelled
    // string in `note`: note is plain text below the body, so it cannot
    // shed keys under width pressure and its spacing drifted from every
    // other panel's footer. `tab` only appears when there IS another view
    // to switch to.
    std::vector<Hint> keys;
    if (visible.size() > 1) keys.push_back({"tab", "switch view", 3});
    keys.push_back({"\xe2\x86\x91\xe2\x86\x93", "scroll", 5});   // ↑↓
    keys.push_back({"esc", "close", 4});
    cfg.footer.push_back(key_hints(std::move(keys)));
    return maya::Panel{std::move(cfg)}.build();
}

}  // namespace agentty::ui
