// The `question` card: the question tool's state, handed to maya's
// QuestionCard. See the header. All drawing is in maya
// (widget/agentty/question_card.hpp).

#include "agentty/runtime/view/thread/turn/question.hpp"

#include <utility>

#include <maya/widget/agentty/question_card.hpp>

namespace agentty::ui {

maya::Element question_card(const question::State& s, bool drafting) {
    maya::QuestionCard::Config cfg;
    cfg.drafting = drafting;
    auto& st = cfg.state;
    st.current   = s.current;
    st.cursor    = s.cursor;
    st.reviewing = s.reviewing;
    st.questions.reserve(s.questions.size());
    for (const auto& q : s.questions) {
        maya::QuestionCard::Question mq{q.question, q.header, {}, q.multiple};
        mq.options.reserve(q.options.size());
        for (const auto& o : q.options) mq.options.push_back({o.label, o.description, o.recommended});
        st.questions.push_back(std::move(mq));
    }
    st.answers.reserve(s.answers.size());
    for (const auto& a : s.answers) st.answers.push_back({a.chosen, a.typed, a.answered});
    return maya::QuestionCard{std::move(cfg)}.build();
}

} // namespace agentty::ui
