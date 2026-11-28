#pragma once
#include <maya/widget/agentty/phase_chip.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::PhaseChip::Config phase_chip_config(const Model& m);

} // namespace agentty::ui
