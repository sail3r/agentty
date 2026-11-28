#pragma once
#include <maya/widget/agentty/changes_strip.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::ChangesStrip::Config changes_strip_config(const Model& m);

} // namespace agentty::ui
