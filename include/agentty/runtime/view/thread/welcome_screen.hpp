#pragma once
#include <maya/widget/agentty/welcome_screen.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::WelcomeScreen::Config welcome_screen_config(const Model& m);

} // namespace agentty::ui
