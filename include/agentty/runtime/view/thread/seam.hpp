#pragma once
// agentty::ui — the ONE definition of every inter-turn seam Element.
//
// The frozen builder (frozen.cpp / freeze_range) and the live-tail
// builder (conversation.cpp / build_live_tail) must produce
// byte-identical row sequences for the same input: at the freeze
// instant the live rows are simply re-labelled as frozen, and any
// 1-row shape delta shifts rows already committed to native terminal
// scrollback against the re-layout — the classic freeze-seam ghost.
//
// Historically each builder had its OWN copy of these Elements with a
// "MUST stay byte-identical to the other file" comment. That is a
// convention, not a guarantee — one drifted edit = one scrollback
// ghost. This header replaces the convention with a single definition
// site: both builders call the same functions, so the seam cannot
// diverge. (INLINE_SCROLLBACK.md pin #3 — divider symmetry — is now
// enforced by the linker, not by review.)

#include <cstddef>
#include <string>
#include <vector>

#include <maya/element/element.hpp>
#include <maya/render/cache_id.hpp>
#include <maya/widget/agentty/conversation.hpp>
#include <maya/widget/agentty/turn.hpp>

#include "agentty/domain/conversation.hpp"
#include "agentty/domain/ui_live.hpp"   // compact_turns: the seam's height
#include "agentty/foreign/names.hpp"     // import_seam_row's "from Codex"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

// Inter-turn seam between every pair of adjacent turns — a blank row,
// the dim ─ rule, then another blank row. Pushed before each settled
// turn, between live-tail turns, and at the frozen↔live boundary.
//
// COMPACT drops the two blank rows and keeps the rule, so turns still
// separate but cost one row instead of three. On a 30-row terminal a long
// session spends a tenth of the screen on whitespace; that is a real
// trade, not a cosmetic one, which is why it is a setting.
//
// ── The height and the element MUST agree ────────────────────────────
// Frozen scrollback seals each element with an explicit row count
// (push_frozen(m, gap_row(), kGapRows)); if the two disagree the ledger
// drifts and the canvas tears. So the count is a FUNCTION of the same
// pref the element reads — gap_rows(), never a constant that has to be
// remembered — and both are evaluated at the same instant.
inline maya::Element gap_row() {
    return maya::Conversation::gap(ui_prefs::current().compact_turns);
}
[[nodiscard]] inline int gap_rows() noexcept {
    return maya::Conversation::gap_rows(ui_prefs::current().compact_turns);
}
// Retained as the NON-compact height for call sites that reason about the
// roomy layout specifically. Anything sealing a gap_row() must use
// gap_rows() instead — see above.
inline constexpr int kGapRows = 3;

// Compaction-boundary divider: a single-row centered labeled rule
//   ┈┈┈┈┈  ≡ Conversation compacted  ┈┈┈┈┈
// emitted before a run that begins on a compaction boundary. Rendered
// as a real horizontal separator (not a speaker Turn with a hanging
// left rail, which read as a broken empty message) so the boundary
// looks like the section break it is. Visually distinct from the
// inter-turn `─` rule: a lighter QUAD-DASH rule (┈) flanks the label,
// the `≡` glyph carries the brand accent, and the label itself sits a
// tone brighter than the rule so the event reads at a glance instead
// of dissolving into chrome.
//
// MUST stay exactly ONE row: freeze_range (frozen.cpp) and
// build_live_tail (conversation.cpp) both emit this at the boundary, and
// any height delta at the freeze seam ghosts committed scrollback rows
// (INLINE_SCROLLBACK.md pin #3). A single-row h() of styled segments
// keeps the seam height-stable by construction — and both builders call
// THIS function, so the bytes cannot drift either.
//
// The label literal "Conversation compacted" is load-bearing: the seam
// tests (midrun_seam_test) grep for it to count divider copies.
inline maya::Element compaction_divider_row() {
    return maya::Conversation::section_break({
        .glyph = "\xe2\x89\xa1",                  // ≡
        .label = "Conversation compacted",
        .from  = {},
        .cache_tag = {"agentty.compaction.divider.v2"},
        .accent = accent, .secondary = text_secondary, .muted = muted});
}

// True iff a run starting at message index `idx` opens on a compaction
// boundary (and must therefore be preceded by the divider). Shared so
// the frozen and live builders agree on WHEN the divider appears, not
// just what it looks like.
inline bool compaction_boundary_at(
        const std::vector<Thread::CompactionRecord>& recs,
        std::size_t idx, std::size_t total) {
    for (const auto& rec : recs) {
        if (rec.up_to_index == idx && rec.up_to_index > 0
            && rec.up_to_index <= total) return true;
    }
    return false;
}

// Where a thread imported from another tool ends and agentty takes over:
// "→ continued in agentty · from Codex". One row, same rules as the
// compaction divider (both builders call this, so the bytes can't drift).
inline maya::Element import_seam_row(std::string source_name) {
    // The tag view must outlive the call: section_break hashes it up front.
    return maya::Conversation::section_break({
        .glyph = "\xe2\x86\x92",                  // →
        .label = "continued in agentty",
        .from  = "from " + source_name,
        .cache_tag = {"agentty.import.seam.v1", source_name},
        .accent = accent, .secondary = text_secondary, .muted = muted});
}

// The message index the import seam sits before, or SIZE_MAX for none: the
// first message added after the import, once there is one.
inline std::size_t import_seam_index(const Thread& t) noexcept {
    if (!t.imported || t.imported->messages == 0) return SIZE_MAX;
    const std::size_t at = t.imported->messages;
    return at < t.messages.size() ? at : SIZE_MAX;
}

} // namespace agentty::ui
