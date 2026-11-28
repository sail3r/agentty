// todo.cpp — the ambient todo modal (renders UNDER the slot).
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"
#include <maya/widget/agentty/rows.hpp>

namespace agentty::ui {

Element todo_panel(const Model& m) {
    if (!pick::is_open(m.ui.todo.open)) return maya::Panel::empty();

    Panel::Config cfg;
    cfg.title      = " Plan ";
    cfg.accent     = info;
    cfg.min_width  = kPanelNarrow;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.todo_scroll;
    // No selection cursor in the todo modal — read-only. Pass -1
    // so the auto-scroll-to-selection is a no-op and the user's
    // manual scroll position is fully respected.
    cfg.selected   = -1;

    if (m.ui.todo.items.empty()) {
        cfg.prebuilt.push_back(ui::pane::Empty{"No tasks yet."});
        cfg.prebuilt.push_back(ui::pane::Help{"The agent will create tasks as it works."});
    } else {
        // PlanView returns one Element with all tasks. It lives in
        // the scrollable region so a long task list pages cleanly
        // when it overflows the viewport.
        maya::PlanView plan;
        for (const auto& item : m.ui.todo.items) {
            maya::TaskStatus ts;
            switch (item.status) {
                case TodoStatus::Pending:    ts = maya::TaskStatus::Pending; break;
                case TodoStatus::InProgress: ts = maya::TaskStatus::InProgress; break;
                case TodoStatus::Completed:  ts = maya::TaskStatus::Completed; break;
            }
            plan.add(item.content, ts);
        }
        cfg.prebuilt.push_back(plan.build());

        int total = static_cast<int>(m.ui.todo.items.size());
        int done_count = 0;
        for (const auto& item : m.ui.todo.items)
            if (item.status == TodoStatus::Completed) ++done_count;
        cfg.footer.push_back(ui::pane::Gap{});
        cfg.footer.push_back(ui::pane::Status{.lead = std::to_string(done_count) + "/" + std::to_string(total),
                                          .text = " completed",
                                          .kind = done_count == total ? ui::pane::Status::Kind::Ok
                                                                      : ui::pane::Status::Kind::Info});
    }

    cfg.footer.push_back(ui::pane::Gap{});
    cfg.footer.push_back(ui::pane::KeyAction{.key = "Esc", .action = "close", .margin = false});

    return Panel{std::move(cfg)}.build();
}


} // namespace agentty::ui
