#pragma once
// agentty::ui — the names agentty's view code uses for colour and weight.
//
// NOTHING IS DECIDED HERE. Every name below is an alias into maya's look
// vocabulary (third_party/maya/include/maya/widget/agentty/look.hpp), which
// is the one place that says what a role looks like. Change a hue, a weight
// or a glyph there and every surface agentty draws follows; this file never
// needs to move.
//
// The names are the ones agentty has always used (ui::accent, ui::fg_dim,
// ui::status_ok …), kept so view code reads the same. New code can spell
// maya::look::ink / tone / mark directly.

#include <maya/style/color.hpp>
#include <maya/style/style.hpp>
#include "agentty/runtime/view/look.hpp"

#include "agentty/domain/ui_theme.hpp"        // ui_prefs (transitively relied on)
#include "agentty/runtime/panel/palette.hpp"   // PaletteContext, Command
#include "agentty/runtime/model.hpp"            // Model

namespace agentty::ui {

// ── colour, by role ──
inline constexpr maya::Color fg        = ink::fg;
inline constexpr maya::Color muted     = ink::muted;
inline constexpr maya::Color accent    = ink::accent;
inline constexpr maya::Color info      = ink::info;
inline constexpr maya::Color success   = ink::success;
inline constexpr maya::Color warn      = ink::warn;
inline constexpr maya::Color danger    = ink::danger;
inline constexpr maya::Color highlight = ink::highlight;

inline constexpr maya::Color text_primary   = ink::text;
inline constexpr maya::Color text_secondary = ink::secondary;
inline constexpr maya::Color text_tertiary  = ink::muted;
inline constexpr maya::Color text_inverse   = ink::inverse;

inline constexpr maya::Color status_ok    = ink::status_ok;
inline constexpr maya::Color status_info  = ink::status_info;
inline constexpr maya::Color status_warn  = ink::status_warn;
inline constexpr maya::Color status_error = ink::status_error;

inline constexpr maya::Color code_path = ink::code_path;
inline constexpr maya::Color code_text = ink::code_text;

inline constexpr maya::Color role_brand     = ink::brand;
inline constexpr maya::Color role_brand_alt = ink::brand_alt;
inline constexpr maya::Color role_info      = ink::role_info;

// ── weight, by role ──
[[nodiscard]] inline maya::Style fg_of(maya::Color c)     { return tone::plain(c); }
[[nodiscard]] inline maya::Style fg_bold(maya::Color c)   { return tone::strong(c); }
[[nodiscard]] inline maya::Style fg_dim(maya::Color c)    { return tone::soft(c); }
[[nodiscard]] inline maya::Style fg_italic(maya::Color c) { return tone::note(c); }
[[nodiscard]] inline maya::Style dim()    { return tone::dim(); }
[[nodiscard]] inline maya::Style bold()   { return tone::bold(); }
[[nodiscard]] inline maya::Style italic() { return tone::italic(); }
[[nodiscard]] inline maya::Style chip_style(maya::Color hue) { return tone::chip(hue); }
[[nodiscard]] inline bool theme_owns_canvas() noexcept { return tone::owns_canvas(); }

// ── Palette row visibility ─────────────────────────────────────
//
// Rows are GATED — Review/Accept-all need pending changes, Run-code-block a
// fenced reply, Update a release — and THREE places must agree on which are
// visible: the VIEW that renders the list, the REDUCER that resolves a cursor
// to a command, and back_to() restoring a cursor after Esc. When they
// disagree the palette shows one list while the cursor indexes another, so
// Enter fires a neighbour and Esc restores the wrong row.
//
// It was written out twice — in palette.cpp and in nav_pickers.cpp — each
// commented "the SAME predicate" as the other. One definition makes that
// comment true rather than aspirational.
//
// Lives here rather than beside the command table because it needs the Model,
// and command_palette.hpp is deliberately Model-free.
[[nodiscard]] PaletteContext palette_context(const Model& m);

} // namespace agentty::ui
