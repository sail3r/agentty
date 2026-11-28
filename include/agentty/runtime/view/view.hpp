#pragma once
// agentty::ui::view — top-level composition.
//
// Assembles the per-panel views into the root Element.  All overlay logic
// (modals, pickers) lives here so per-panel files stay focused.

#include <optional>

#include <maya/maya.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::Element view(const Model& m);
// The overlay the view floats over the base this frame (nullopt = none):
// the SAME pick view() makes. Exposed for the UI snapshot fixture.
[[nodiscard]] std::optional<maya::Element> pick_panel(const Model& m);

} // namespace agentty::ui
