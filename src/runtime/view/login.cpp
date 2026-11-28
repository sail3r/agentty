// In-app login modal.
//
// Five sub-states keyed off `ui::login::State`:
//
//   Picking          choose OAuth (1) or API key (2)
//   OAuthCode        browser opened; user pastes callback code.
//                    The authorize URL gets a dedicated bordered box
//                    + `[c] copy URL` / `[o] open in browser again`
//                    affordances so the URL is one keystroke from the
//                    user's clipboard — no terminal mouse-select needed.
//   OAuthExchanging  HTTP POST in flight.
//   ApiKeyInput      paste an sk-ant-… key.
//   Failed           error toast above the Picking panel.
//
// Sizing is responsive: the outer wrapper sets a `min_width` floor
// and the Overlay widget's default Stretch lets it fill the available
// width (minus 2-col edge padding). Every text node uses TextWrap::Wrap
// so long URLs / API key labels reflow rather than truncate. Mirrors
// the picker chrome (see view/pickers.cpp's wrap_picker).

#include "agentty/runtime/view/login.hpp"

#include <string>
#include <variant>
#include <vector>

#include "agentty/domain/ui_live.hpp"   // panel_rows(): Density
#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/view/palette.hpp"

#include <maya/widget/agentty/login_pane.hpp>
#include "agentty/runtime/view/look.hpp"

namespace agentty::ui {

// `login::` resolves to `agentty::ui::login::` from this scope without an
// alias — and MSVC rejects an alias whose name shadows the existing
// nested namespace, so don't write one.

namespace {

using LP = maya::LoginPane;
using Blocks = std::vector<LP::Block>;

LP::Config base_config() {
    LP::Config c;
    c.accent    = accent;
    c.text      = fg;
    c.muted     = muted;
    c.danger    = danger;
    c.highlight = highlight;
    c.code_path = code_path;
    return c;
}

Blocks picking_blocks(std::string_view provider, bool failed, std::string_view fail_msg) {
    // "Does this provider offer a CHOICE of auth method" is a registry
    // capability (method_menu), not a name — the reducer's gate reads the
    // same flag, so the view and the behaviour cannot drift.
    const auto* prow = provider::preset_for(provider);
    const bool anthropic_only = prow && prow->method_menu;
    Blocks b;
    b.push_back(LP::title(anthropic_only ? "Add a " + std::string{prow->label} + " account"
                                         : std::string{"Sign in to agentty"}));
    b.push_back(LP::prose(anthropic_only
        ? "Choose how to authenticate this new Anthropic account."
        : "Bring your own model. Pick how you want to connect — you can "
          "change this any time from the provider picker."));
    b.push_back(LP::gap());
    if (failed) {
        b.push_back(LP::danger(std::string{std::string{ui::mark::warn} + " "} + std::string{fail_msg}));
        b.push_back(LP::gap());
    }
    // API key FIRST: the unambiguous, provider-neutral path, whereas
    // subscription OAuth is a third-party-client path some would rather avoid.
    b.push_back(LP::option("1) ", "Paste an API key", "Anthropic sk-ant-…, or any provider's key"));
    b.push_back(LP::gap());
    b.push_back(LP::option("2) ", "OAuth via claude.ai", "use your Claude Pro / Max subscription"));
    b.push_back(LP::gap());
    if (!anthropic_only) {
        b.push_back(LP::option("3) ", "Sign in with ChatGPT", "use your ChatGPT Plus / Pro (GPT-5 Codex)"));
        b.push_back(LP::gap());
        b.push_back(LP::option("4) ", "Custom OpenAI-compatible host", {},
                               "  \xe2\x80\x94 llama.cpp, vLLM, LM Studio, Ollama"));
        b.push_back(LP::gap());
        b.push_back(LP::option("5) ", "Sign in with GitHub Copilot", "use your GitHub Copilot subscription"));
        b.push_back(LP::gap());
        b.push_back(LP::option("6) ", "Sign in with Kimi", "use your Kimi plan (K2 models)"));
        b.push_back(LP::gap());
    }
    b.push_back(LP::hints({{anthropic_only ? "1/2" : "1\xe2\x80\x93" "6", "choose"}, {"Esc", "close"}}));
    return b;
}

// One-item Panel for a single-field input state. The field is a REAL panel
// item — Secret (count-only masking, the type-level can't-leak guarantee) or
// Text (caret windowing). Prose rides above it; Enter SUBMITS here, so the
// derived editing hint is overridden.
LP::Config input_config(std::string title, Blocks prose, maya::PanelControl control,
                        std::string note, std::string editing_note, Blocks footer = {}) {
    LP::Config c = base_config();
    maya::Panel::Config cfg;
    cfg.title     = std::move(title);
    cfg.accent    = accent;
    cfg.min_width = 48;
    cfg.selected  = 0;
    maya::Panel::Item it;
    it.leading = ui::mark::chevron;                    // › — the prompt marker
    it.leading_style = fg_bold(accent);
    it.control = std::move(control);
    // Prompt-style: the input hugs the › (this row has no label column).
    it.value_primary = true;
    cfg.items.push_back(std::move(it));
    cfg.note         = std::move(note);
    cfg.editing_note = std::move(editing_note);
    c.panel = std::move(cfg);
    prose.push_back(LP::gap());
    c.panel_header = std::move(prose);
    c.panel_footer = std::move(footer);
    return c;
}

LP::Config oauth_code(const login::OAuthCode& s) {
    Blocks prose{LP::prose("  Step 1 \xe2\x80\x94 open this URL and authorize agentty:"),
                 LP::gap(), LP::url(s.authorize_url), LP::gap(),
                 LP::prose("  Step 2 \xe2\x80\x94 paste the callback code below:")};
    // The code is a credential in transit: Secret, count-only.
    maya::panel::Secret sec;
    sec.filled      = s.code_input.size();
    sec.caret       = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(0, s.cursor)), s.code_input.size());
    sec.placeholder = "paste the code from claude.ai";
    Blocks footer{LP::gap(), LP::hints({{"c", "copy URL"}, {"o", "open browser"},
                                        {"^Y", "copy"}, {"^O", "open"}})};
    return input_config(" OAuth via claude.ai ", std::move(prose), std::move(sec),
                        std::string{ui::mark::enter} + " submit code" + ui::mark::sep + "esc cancel",
                        std::string{ui::mark::enter} + " submit code" + ui::mark::sep + "esc cancel", std::move(footer));
}

Blocks oauth_exchanging() {
    return {LP::title("Exchanging authorization code\xE2\x80\xA6"), LP::gap(),
            LP::prose("Talking to platform.claude.com — this should take a second."),
            LP::gap(), LP::hints({{"Esc", "cancel"}})};
}

Blocks chatgpt_waiting(const login::ChatGptWaiting& s) {
    Blocks b;
    if (s.device_auth) {
        b.push_back(LP::title(s.user_code.empty() ? "Requesting ChatGPT device code\xE2\x80\xA6"
                                                  : "Sign in to ChatGPT"));
        b.push_back(LP::gap());
        if (s.user_code.empty()) {
            b.push_back(LP::prose("This SSH session uses device authorization. Waiting for "
                                  "OpenAI to issue a one-time code\xE2\x80\xA6"));
        } else {
            b.push_back(LP::prose("Open this link in a browser on any device:"));
            b.push_back(LP::gap());
            b.push_back(LP::url(s.authorize_url));
            b.push_back(LP::gap());
            b.push_back(LP::prose("Then enter this one-time code:"));
            b.push_back(LP::title(s.user_code));
            b.push_back(LP::gap());
            b.push_back(LP::prose("The code expires in 15 minutes. Continue only because you "
                                  "started this login in agentty."));
        }
    } else {
        b.push_back(LP::title("Waiting for ChatGPT\xE2\x80\xA6"));
        b.push_back(LP::gap());
        b.push_back(LP::prose(
            "A browser tab has opened at chatgpt.com \xE2\x80\x94 authorize agentty "
            "there. The sign-in completes automatically over a local callback "
            "(http://localhost:1455); you don't need to paste anything."));
        b.push_back(LP::gap());
        if (!s.authorize_url.empty()) {
            b.push_back(LP::prose("Browser didn't open? Visit this URL manually:"));
            b.push_back(LP::gap());
            b.push_back(LP::url(s.authorize_url));
            b.push_back(LP::gap());
        }
    }
    b.push_back(LP::gap());
    b.push_back(LP::hints({{"Esc", "cancel"}}));
    return b;
}

Blocks device_waiting(const login::DeviceWaiting& s) {
    const std::string label = s.provider_label.empty() ? "your provider" : s.provider_label;
    Blocks b;
    b.push_back(LP::title(s.user_code.empty() ? "Requesting device code\xE2\x80\xA6"
                                              : "Sign in with " + label));
    b.push_back(LP::gap());
    if (s.user_code.empty()) {
        b.push_back(LP::prose("Waiting for " + label + " to issue a one-time code\xE2\x80\xA6"));
        b.push_back(LP::gap());
        b.push_back(LP::hints({{"Esc", "cancel"}}));
        return b;
    }
    b.push_back(LP::prose("Open this URL to approve the device (it already includes your"));
    b.push_back(LP::prose("code). On SSH, press [u] to copy it, or scan/type it on any device:"));
    b.push_back(LP::gap());
    b.push_back(LP::url(s.authorize_url));
    b.push_back(LP::gap());
    b.push_back(LP::code("   if the page asks for a code, enter: ", s.user_code));
    b.push_back(LP::gap());
    b.push_back(LP::prose("After signing in to " + label + ", approve the device. The code "
                          "expires in ~15 minutes; no API key is needed."));
    if (s.provider == "copilot") {
        // GitHub labels this consent page "GitHub Copilot Plugin", not
        // agentty: the token exchange only accepts VS Code's GitHub App id,
        // and the model allowlist is keyed to it (docs/design/copilot-provider.md
        // §2). Say so, so nobody aborts thinking they clicked the wrong link.
        b.push_back(LP::gap());
        b.push_back(LP::prose(
            "Note: GitHub will label the app \"GitHub Copilot Plugin\" on "
            "the consent page. That is expected \xe2\x80\x94 agentty signs "
            "in as VS Code's Copilot client because the API's model "
            "allowlist is keyed to that identity."));
    }
    b.push_back(LP::gap());
    b.push_back(LP::hints({{"c", "copy code"}, {"u", "copy URL"},
                           {"o", "open browser"}, {"Esc", "cancel"}}));
    return b;
}

LP::Config api_key(const login::ApiKeyInput& s) {
    const bool anthropic = s.provider.empty();
    const bool custom = !anthropic && !provider::preset_for(s.provider);
    const bool optional = custom && !s.key_required;
    Blocks prose{LP::prose(
        anthropic
            ? "  Paste an sk-ant-\xe2\x80\xa6 key. It's saved encrypted under "
              "~/.agentty/credentials."
        : optional
            ? "  Paste this host's API key, or leave it empty if the server "
              "needs none. A key is saved encrypted under ~/.agentty/credentials."
        : custom
            ? "  This host answered 401: it needs an API key. Paste it here; "
              "it's saved encrypted under ~/.agentty/credentials."
            : "  Paste your " + s.provider_label + " API key to switch to "
              "it. It's saved encrypted under ~/.agentty/credentials so you "
              "won't be asked again.")};
    // The KEY never reaches the widget — panel::Secret carries a count.
    maya::panel::Secret sec;
    sec.filled      = s.key_input.size();
    sec.caret       = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(0, s.cursor)), s.key_input.size());
    sec.placeholder = anthropic ? "sk-ant-\xe2\x80\xa6"
                    : optional  ? "API key (optional)\xe2\x80\xa6"
                                : "paste API key\xe2\x80\xa6";
    const char* back = login::has_parent(s.origin) ? "esc back" : "esc cancel";
    const std::string enter = optional && s.key_input.empty()
        ? std::string{ui::mark::enter} + " continue without a key" + ui::mark::sep
        : std::string{ui::mark::enter} + " submit" + ui::mark::sep;
    return input_config(
        " " + (anthropic ? std::string{"Anthropic"} : s.provider_label) + " API key ",
        std::move(prose), std::move(sec), enter + back, enter + back);
}

// Connect probe in flight: name the host and what's being tried so the
// short wait reads as WORK, not a hang. Esc cancels back to the input.
Blocks host_probing(const login::HostProbing& s) {
    return {LP::title("Checking " + s.spec + ui::mark::ellipsis), LP::gap(),
            LP::prose("Dialing the server's model list \xe2\x80\x94 the configured path, "
                      "then /v1/models, then Ollama's /api/tags \xe2\x80\x94 to detect what "
                      "it speaks and confirm it's alive before switching."),
            LP::gap(), LP::hints({{"Esc", "cancel"}})};
}

LP::Config custom_host(const login::CustomHostInput& s) {
    Blocks prose{LP::prose(
        "  Enter a host or host:port for any server that speaks the OpenAI "
        "chat API \xe2\x80\x94 llama.cpp, vLLM, LM Studio, a proxy, or a "
        "remote box. A non-443 port uses plain HTTP (the local-server "
        "convention); a bare host uses HTTPS on 443 and defaults to the "
        "/v1 API prefix. For a server on a different prefix, paste the "
        "full URL (https://host/path) and the prefix is honoured verbatim. "
        "Append #name to keep several accounts on the SAME endpoint "
        "(e.g. \xe2\x80\xa6/v1#work and \xe2\x80\xa6/v1#personal \xe2\x80\x94 "
        "each keeps its own API key and model).")};
    // Text, not Secret — a host is not a credential, and the caret
    // windowing means a long pasted URL scrolls under the caret.
    maya::panel::Text t;
    t.value       = s.host_input;
    t.caret       = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(0, s.cursor)), s.host_input.size());
    t.placeholder = "localhost:8080";
    const char* back = login::has_parent(s.origin) ? "esc back" : "esc cancel";
    Blocks footer{LP::gap(), LP::prose(
        "  Examples:  localhost:8080  \xc2\xb7  https://ollama.com/v1#work  "
        "\xc2\xb7  inference.example.com  \xc2\xb7  https://inference.example.com/api")};
    return input_config(
        " Custom OpenAI-compatible host ", std::move(prose), std::move(t),
        std::string{std::string{ui::mark::enter} + " connect" + ui::mark::sep} + back,
        std::string{std::string{ui::mark::enter} + " connect" + ui::mark::sep} + back,
        std::move(footer));
}

// The account switcher, as a real maya::Panel — the same widget every
// picker uses: cursor bar, badge column, viewport scrolling.
//   badge    ✓ on the ACTIVE account — ⚠ while delete is armed
//   trailing "press d again to confirm" while armed — SECONDARY, so the
//            label survives narrow terminals
//   + Add another account… rides as the last item, dim until selected
LP::Config account_list(const login::AccountList& s, maya::ScrollState* scroll,
                        std::string_view active_error) {
    LP::Config c = base_config();
    maya::Panel::Config cfg;
    cfg.title     = " " + s.provider_label + " accounts ";
    cfg.accent    = accent;
    cfg.min_width = 48;
    // Scroll like every list panel: many accounts must window inside the
    // modal, not grow it past the terminal edge.
    cfg.scroll     = scroll;
    cfg.viewport_h = agentty::ui_prefs::panel_rows();   // Density, like every panel
    const int n = static_cast<int>(s.rows.size());
    cfg.selected  = std::min(s.cursor, n);   // last item = add-new
    if (s.rows.empty())
        c.panel_header = {LP::prose("  No saved accounts. Add one to sign in without leaving agentty."),
                          LP::gap()};
    for (const auto& r : s.rows) {
        maya::Panel::Item it;
        const bool confirming = (s.confirm_remove == r.label);
        it.leading = r.label;
        it.active  = r.active;
        if (confirming) {
            it.badge           = ui::mark::warn;           // ⚠
            it.badge_style     = fg_of(danger);
            it.leading_style   = fg_bold(danger);
            it.trailing        = "press d again to confirm";
            it.trailing_style  = fg_of(danger);
        } else if (r.active && !active_error.empty()) {
            // The host refused this key on the last request: say so here,
            // where the user can add a working one.
            it.badge         = ui::mark::warn;
            it.badge_style   = fg_of(warn);
            it.leading_style = fg_bold(warn);
            it.trailing       = "rejected by the host";
            it.trailing_style = fg_of(warn);
        } else if (r.active) {
            it.badge         = ui::mark::ok;             // ✓
            it.badge_style   = fg_of(accent);
            it.leading_style = fg_bold(accent);
            it.trailing       = "active";
            it.trailing_style = fg_dim(muted);
        }
        it.trailing_secondary = true;
        cfg.items.push_back(std::move(it));
    }
    {
        maya::Panel::Item add;
        add.leading       = "+ Add another account\xe2\x80\xa6";
        add.leading_style = fg_dim(muted);
        cfg.items.push_back(std::move(add));
    }
    cfg.note = s.confirm_remove.empty()
                   ? std::string{ui::mark::updown} + " move" + ui::mark::sep + ui::mark::enter + " select" + ui::mark::sep + "d remove" + ui::mark::sep + "esc close"
                   : "d confirm remove \xc2\xb7 any other key cancels";
    c.panel = std::move(cfg);
    return c;
}

} // namespace

maya::Element login_modal(const Model& m) {
    if (!login::is_open(m.ui.login)) return LP::empty();
    LP::Config cfg = std::visit([&m](const auto& s) -> LP::Config {
        using T = std::decay_t<decltype(s)>;
        auto prose = [](Blocks b) { LP::Config c = base_config(); c.blocks = std::move(b); return c; };
        if constexpr (std::same_as<T, login::Closed>) {
            return base_config();
        } else if constexpr (std::same_as<T, login::Picking>) {
            return prose(picking_blocks(s.provider, false, ""));
        } else if constexpr (std::same_as<T, login::OAuthCode>) {
            return oauth_code(s);
        } else if constexpr (std::same_as<T, login::OAuthExchanging>) {
            return prose(oauth_exchanging());
        } else if constexpr (std::same_as<T, login::ChatGptWaiting>) {
            return prose(chatgpt_waiting(s));
        } else if constexpr (std::same_as<T, login::DeviceWaiting>) {
            return prose(device_waiting(s));
        } else if constexpr (std::same_as<T, login::ApiKeyInput>) {
            return api_key(s);
        } else if constexpr (std::same_as<T, login::CustomHostInput>) {
            return custom_host(s);
        } else if constexpr (std::same_as<T, login::HostProbing>) {
            return prose(host_probing(s));
        } else if constexpr (std::same_as<T, login::AccountList>) {
            // The active provider's last fetch said its key was refused.
            std::string_view key_err;
            const bool auth_err = m.s.models_error.find("HTTP 401") != std::string::npos
                               || m.s.models_error.find("HTTP 403") != std::string::npos;
            const auto& sel = m.d.selection;
            const std::string active_id = !sel.provider_id().empty()
                ? std::string{sel.provider_id()}
                : (sel.kind == provider::Kind::OpenAI ? sel.openai_endpoint.label
                                                      : std::string{});
            if (auth_err && !active_id.empty() && s.provider == active_id)
                key_err = m.s.models_error;
            return account_list(s, &m.ui.account_list_scroll, key_err);
        } else if constexpr (std::same_as<T, login::Failed>) {
            return prose(picking_blocks("", true, s.message));
        }
    }, m.ui.login);
    return maya::LoginPane{std::move(cfg)}.build();
}

} // namespace agentty::ui
