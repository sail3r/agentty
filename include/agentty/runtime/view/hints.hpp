// SPDX-License-Identifier: Apache-2.0
#pragma once
// agentty::ui — the key-hint footer and filter line, as maya widget Configs.
//
// The drawing lives in maya (widget/agentty/key_hints.hpp). These keep the
// names every pane already calls, and only fill the widget's Config.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <maya/widget/agentty/key_hints.hpp>
#include "agentty/runtime/view/palette.hpp"   // color tokens (fg, muted…)

namespace agentty::ui {

// One key-binding hint: key, short label, priority (higher = kept longer
// under width pressure) and an accent for the KEY (success for "keep",
// danger for "undo", so a destructive key never looks like a benign one).
struct Hint {
    std::string  key;
    std::string  label;
    int          priority = 0;
    maya::Color  key_color = fg;
};

[[nodiscard]] inline maya::Element key_hints(std::vector<Hint> hints) {
    maya::KeyHints::Config cfg;
    cfg.hints.reserve(hints.size());
    for (auto& h : hints)
        cfg.hints.push_back({std::move(h.key), std::move(h.label), h.priority, h.key_color});
    cfg.label = muted;
    return maya::KeyHints{std::move(cfg)}.build();
}

// The filter line at the top of a picker. `noun` names what is filtered
// ("providers"); empty gives the bare "type to filter…". A `caret` colour
// makes it read as a prompt you type into.
[[nodiscard]] inline maya::Element filter_header(std::string_view query,
                                                std::string_view noun = {},
                                                std::optional<maya::Color> caret = {},
                                                std::string_view glyph = "\xf0\x9f\x94\x8d ") {
    maya::FilterLine::Config cfg;
    cfg.query = std::string{query};
    cfg.noun  = std::string{noun};
    cfg.caret = caret;
    cfg.glyph = std::string{glyph};
    cfg.text  = fg;
    cfg.muted = muted;
    return maya::FilterLine{std::move(cfg)}.build();
}

} // namespace agentty::ui
