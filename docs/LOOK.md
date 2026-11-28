# agentty's look lives in one folder

Everything about how agentty looks, from the shape of a tool card to the colour
of a check mark, is in maya's agentty folder:

```
third_party/maya/include/maya/widget/agentty/   widgets (Config in, Element out)
third_party/maya/src/widget/agentty/            their out-of-line halves
```

agentty's own code (`src/`, `include/agentty/`) never decides how anything
looks. It fills widget Configs with facts from the Model and names
**roles**. Change any file in the folder and agentty follows without being
edited, rebuilt against new names, or reviewed.

## The three layers

| Layer | File | agentty says | The folder decides |
|-------|------|--------------|--------------------|
| structure | one widget per surface (`turn.hpp`, `panel.hpp`, `status_bar.hpp`, `report.hpp`, `run_transcript.hpp`, …) | *what* is on screen: a turn with these tool calls, a panel with these rows | layout, borders, spacing, truncation, every glyph the widget draws itself |
| vocabulary | `look.hpp` | *which role* a piece plays: `ink::status_ok`, `tone::chrome()`, `mark::ok` | the colour, weight and symbol of that role |
| theme | `maya/style/` schemes | nothing: the user picks it | the hue behind every slot |

`look.hpp` is the part that makes this complete. Before it existed agentty
still chose hues (`ThemeSlot::Warning`), weights (`.with_bold()`) and
glyphs (`"✓"`) at about 400 call sites, so restyling meant editing agentty.
Now those call sites name roles, and each role is defined once:

```cpp
namespace maya::look {
namespace ink  { … status_ok, code_path, brand …   tool(ToolCat), family(Family), phase(Phase) … }
namespace tone { … plain, strong, soft, note, chrome, hint, caret, chip, badge … }
namespace mark { … ok, fail, warn, on, off, cursor, enter, updown, rail, sep … }
}
```

agentty reaches them as `ui::ink`, `ui::tone` and `ui::mark`
(`include/agentty/runtime/view/look.hpp`). The older `ui::` names
(`ui::accent`, `ui::fg_dim(c)`, `ui::status_warn`) are aliases into the
same vocabulary, so existing view code reads the same.

## Facts in, looks out

Where a colour depends on data, agentty passes the **fact** and the folder
maps it:

```cpp
// agentty: which category a tool is in
ink::ToolCat tool_category(const std::string& name);   // edit -> Mutate
// the folder: what that category looks like
constexpr Color ink::tool(ToolCat c);                  // Mutate -> brand
```

The same applies to model families (`ink::family`), profiles
(`ink::profile`), phases (`ink::phase`, `mark::phase`), capability tiers
(`ink::tier`) and reasoning prose (`ink::reasoning()`, a computed contrast-safe
ink).

## Examples: restyling by editing the folder only

- **Check marks become "+".** Edit `mark::ok` in `look.hpp`. Every "✓" in
  agentty (done tools, signed-in rows, doctor checks) follows.
- **Brand is blue, not magenta.** Point `ink::brand` at another slot. The
  thread rail, the `You` label, Write chips and edit tool cards follow.
- **Hints in italic.** Change `tone::hint()`.
- **Retrieval gauge as dots.** Edit `mark::gauge_on` / `gauge_off`.
- **A new tool-card layout.** Edit `agent_timeline.hpp` / `tool_body_preview.hpp`.
- **CLI tables in a different style.** Edit `report.hpp` (`Table`, `check`).

These were tried: making the first two edits changed the `ui_snapshots`
fixtures and nothing else.

The folder also guards its own rules. A model family may never borrow a
status hue, and `look.hpp` refuses one at compile time. Pointing `brand` at
`Success`, for example, fails the build.

## What keeps it true

`ctest -R no_ui_in_agentty` (`tests/lint/no_ui_in_agentty.py`, no allowlist)
fails if agentty makes any of these decisions itself:

- builds UI (`dsl::`, `text(`, `Element{}`, `| border(` …)
- writes raw escapes, styled runs or `printf` column layout
- **picks a look**: `ThemeSlot::`, `Color::slot/rgb/…`, `Style{}`, `.with_fg(` /
  `.with_bold(` / …
- **spells an icon**: a string literal in view code holding an icon glyph
  instead of its `ui::mark::` name. Prose typography (— … · “ ”) is text
  and is allowed. Two files that parse a wire protocol whose markers are
  glyphs are named in the lint.

`ctest -R ui_snapshots` pins every surface byte for byte, colour included,
so a change to the folder shows up as an intentional fixture update:

```sh
tests/ui_snapshots.sh build/tool_gallery --update
```
