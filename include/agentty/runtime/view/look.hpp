#pragma once
// agentty::ui::{ink, tone, mark} — the only words agentty's view code has
// for how something looks. Each is an alias of maya::look, which lives in
// the agentty folder of maya (widget/agentty/look.hpp) and is the one place
// that decides colour, weight and symbol, and of maya::rows (rows.hpp),
// which decides the layout of every kind of pane row. agentty names roles
// and kinds; the folder decides.

#include <maya/widget/agentty/look.hpp>
#include <maya/widget/agentty/rows.hpp>

namespace agentty::ui {
namespace ink  = maya::look::ink;
namespace tone = maya::look::tone;
namespace mark = maya::look::mark;
namespace pane = maya::rows;   // a pane row, by what it is: pane::Rule{}, pane::Position{…}
} // namespace agentty::ui
