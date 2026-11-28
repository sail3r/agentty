#pragma once
// agentty::ui::{ink, tone, mark} — the only words agentty's view code has
// for how something looks. Each is an alias of maya::look, which lives in
// the agentty folder of maya (widget/agentty/look.hpp) and is the one place
// that decides colour, weight and symbol. agentty names roles; look decides.

#include <maya/widget/agentty/look.hpp>

namespace agentty::ui {
namespace ink  = maya::look::ink;
namespace tone = maya::look::tone;
namespace mark = maya::look::mark;
} // namespace agentty::ui
