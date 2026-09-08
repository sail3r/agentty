// pareto_distribution_test — the Smart-Mode main-turn routing BENCHMARK.
//
// The Pareto routing change lets the turn-complexity classifier pick the
// MODEL, not merely the effort: Trivial/Simple→Utility, Standard→
// Implementation, Complex→Strategic. This test locks the mapping, the tier
// resolution on a real ollama-style catalog, and — the point of the feature —
// the resulting WORK + COST distribution over a realistic developer corpus,
// measured against an all-flagship baseline.
//
// It is a benchmark as well as a test: run with doctest's filtering/verbose
// flags (`agentty_tests -ts=pareto_distribution`), or read the tally printed
// by the MEASURE block. Thresholds are calibrated to the corpus (see the
// constants below) — if the classifier weights or thresholds change, re-run
// this and re-calibrate rather than silently loosening the windows.
#include "agtest.hpp"

#include <string>
#include <vector>

#include "agentty/domain/complexity.hpp"
#include "agentty/domain/smart_mode.hpp"
#include "agentty/domain/catalog.hpp"

namespace sm = agentty::smart;
using agentty::Effort;
using agentty::ModelInfo;
using agentty::ModelId;
using agentty::ModelCapabilities;

namespace {

ModelInfo mi(const char* id, int ctx = 200000) {
    ModelInfo m;
    m.id = ModelId{id};
    m.context_window = ctx;
    m.supports_tools = true;
    return m;
}

// Relative per-token price weights for the three pinned ollama models. The
// exact numbers only scale the ratio; what matters is the SPREAD (flagship ≈
// 4× mid ≈ 16× cheap). Kimi k3 flagship pricing, k2.7-code mid, glm-5.3-flash
// budget.
constexpr double kStratCost = 4.0, kImplCost = 1.0, kUtilCost = 0.25;

struct Turn {
    const char* text;
    sm::Complexity prev;   // session state when the prompt arrives
};

// A realistic mid-session mix: ambient one-liners and lookups dominate, a
// working middle of edits/builds, and a hard tail of root-cause / design /
// multi-file work. Deliberately NOT adversarial — this is what a normal
// coding session looks like, which is exactly what the feature must serve.
const std::vector<Turn> kCorpus = {
    // ── ambient / trivial (Utility) ──────────────────────────────────────
    {"yes",                                          sm::Complexity::Simple},
    {"thanks, that fixed it",                        sm::Complexity::Simple},
    {"what files handle routing?",                   sm::Complexity::Simple},
    {"update the copyright year",                    sm::Complexity::Simple},
    {"git status",                                   sm::Complexity::Simple},
    {"show me the log for the failing test",         sm::Complexity::Simple},
    {"add a newline at the end of that file",        sm::Complexity::Simple},
    {"what does this flag do?",                      sm::Complexity::Simple},
    {"grep for usages of resolve_role",              sm::Complexity::Simple},
    {"just the diff summary please",                 sm::Complexity::Simple},
    {"ok",                                           sm::Complexity::Simple},
    {"that worked, moving on",                       sm::Complexity::Simple},

    // ── the working middle (Implementation) ─────────────────────────────
    {"add a --verbose flag to the CLI and document it",   sm::Complexity::Simple},
    {"fix the null check in auth.cpp and rerun the tests", sm::Complexity::Simple},
    {"write a unit test for the retry backoff helper",    sm::Complexity::Simple},
    {"bump the dependency and fix the compile error",     sm::Complexity::Simple},
    {"apply the patch I described to the settings panel", sm::Complexity::Simple},
    {"ok continue",                                       sm::Complexity::Complex},
    {"it's still broken, try again",                      sm::Complexity::Standard},
    {"rename the helper and update its call sites",       sm::Complexity::Simple},
    {"extract that block into a function",                sm::Complexity::Simple},
    {"convert this loop to use the new iterator API",     sm::Complexity::Simple},
    {"add the missing include and rebuild",               sm::Complexity::Simple},

    // ── the hard tail (Strategic) ────────────────────────────────────────
    {"rename Foo to Bar across the repo and fix includes",      sm::Complexity::Simple},
    {"why does compaction fire twice on long sessions? root-cause it and propose a fix",
                                                                 sm::Complexity::Simple},
    {"review this diff for security issues in the auth flow, token refresh, and error handling paths",
                                                                 sm::Complexity::Complex},
    {"design a plugin system covering sandboxing, versioning, lifecycle hooks and a migration path",
                                                                 sm::Complexity::Simple},
    {"implement pagination in the model picker: keyboard nav, scroll anchoring, and persistence",
                                                                 sm::Complexity::Simple},
    {"the build fails on main.cpp; find the root cause, explain it, fix it, and verify with tests",
                                                                 sm::Complexity::Simple},
    {"refactor the config loader into two modules, update all call sites, and keep the wire format stable",
                                                                 sm::Complexity::Simple},
    {"compare the three storage backends, benchmark them, rank by latency, and write the tradeoff doc",
                                                                 sm::Complexity::Simple},
    {"continue",                                                 sm::Complexity::Complex},
    {"that approach won't scale — redesign it",                  sm::Complexity::Standard},
};

int tier_index(sm::Complexity c) { return static_cast<int>(c); }

double cost_of(const std::string& model) {
    if (model == "kimi-k3")        return kStratCost;
    if (model == "kimi-k2.7-code") return kImplCost;
    if (model == "glm-5.3-flash")  return kUtilCost;
    return 1.0;   // unknown = treat as mid
}

struct Tally {
    int tier[4] = {0, 0, 0, 0};
    int flagship_turns = 0;
    double cost = 0.0, baseline = 0.0;
    std::vector<std::pair<std::string, std::string>> mismatches; // text, actual
};

// Run the corpus through the EXACT production path: classify_turn (the
// context-aware composed classifier used by resolve_turn_routing) →
// role_for_complexity → resolve_role. Baseline = every turn on the flagship
// at its per-token price — i.e. the pre-Pareto behaviour.
Tally run_corpus(const std::vector<ModelInfo>& catalog,
                 const std::string& parent, const sm::RoleConfig& cfg) {
    Tally t;
    for (const Turn& turn : kCorpus) {
        const auto s = sm::classify_turn(turn.text, turn.prev, 0, 0);
        ++t.tier[tier_index(s.tier)];
        const auto prof = sm::resolve_role(sm::role_for_complexity(s.tier),
                                           parent, Effort::None, catalog, cfg);
        t.cost += cost_of(prof.model);
        t.baseline += kStratCost;
        if (prof.model == parent) ++t.flagship_turns;
    }
    return t;
}

} // namespace

TEST_CASE("pareto_distribution") {
    // ── 1. The mapping is exact, per tier ─────────────────────────────────
    CHECK(sm::role_for_complexity(sm::Complexity::Trivial)  == sm::ModelRole::Utility,
          "Trivial must route to Utility");
    CHECK(sm::role_for_complexity(sm::Complexity::Simple)   == sm::ModelRole::Utility,
          "Simple must route to Utility");
    CHECK(sm::role_for_complexity(sm::Complexity::Standard) == sm::ModelRole::Implementation,
          "Standard must route to Implementation (the working middle)");
    CHECK(sm::role_for_complexity(sm::Complexity::Complex)  == sm::ModelRole::Strategic,
          "Complex must route to Strategic (the expensive tail)");

    // ── 2. Tier resolution on the pinned ollama catalog ───────────────────
    // UNKNOWN ids default to Mid (catalog.hpp tier_for), so "kimi-k3" reads
    // as Mid — the resolver below still separates the three slots, but the
    // FLAGSHIP lane is not recognising the id. Worth a catalog hint later.
    CHECK(ModelCapabilities::tier_for("kimi-k3") == ModelCapabilities::Tier::Mid,
          "kimi-k3 is an unknown id → Mid tier (documented limitation)");
    CHECK(ModelCapabilities::tier_for("kimi-k2.7-code") == ModelCapabilities::Tier::Mid,
          "kimi-k2.7-code → Mid tier");
    CHECK(ModelCapabilities::tier_for("glm-5.3-flash") == ModelCapabilities::Tier::Cheap,
          "glm-5.3-flash must decode to the cheap lane");

    const std::vector<ModelInfo> catalog = {
        mi("kimi-k3"), mi("kimi-k2.7-code"), mi("glm-5.3-flash"),
    };
    const std::string parent = "kimi-k3";
    sm::RoleConfig cfg; cfg.enabled = true;

    const auto strat = sm::resolve_role(sm::ModelRole::Strategic, parent,
                                        Effort::None, catalog, cfg);
    const auto impl  = sm::resolve_role(sm::ModelRole::Implementation, parent,
                                        Effort::None, catalog, cfg);
    const auto util  = sm::resolve_role(sm::ModelRole::Utility, parent,
                                        Effort::None, catalog, cfg);
    CHECK(strat.model == parent,          "Strategic resolves to the parent");
    CHECK(impl.model  == "kimi-k2.7-code", "Implementation resolves to the mid model");
    CHECK(util.model  == "glm-5.3-flash",  "Utility resolves to the cheapest capable model");

    // ── 3. Safety: solo catalog must NOT regress ──────────────────────────
    {
        const std::vector<ModelInfo> solo = {mi("kimi-k3")};
        const auto i = sm::resolve_role(sm::ModelRole::Implementation, parent,
                                        Effort::None, solo, cfg);
        const auto u = sm::resolve_role(sm::ModelRole::Utility, parent,
                                        Effort::None, solo, cfg);
        CHECK(i.model == parent && u.model == parent,
              "single-model catalog: every role falls back to the parent");
    }

    // ── 4. THE BENCHMARK: distribution + cost over the corpus ─────────────
    const Tally t = run_corpus(catalog, parent, cfg);
    const int n = static_cast<int>(kCorpus.size());

    // MEASURE block — the numbers to read after any classifier retune.
    std::printf("\n[pareto] corpus n=%d  trivial=%d simple=%d standard=%d complex=%d\n",
                n, t.tier[0], t.tier[1], t.tier[2], t.tier[3]);
    std::printf("[pareto] flagship turns=%d/%d (%.0f%%)  cost=%.1f  baseline=%.1f  "
                "saving=%.2fx\n",
                t.flagship_turns, n, 100.0 * t.flagship_turns / n,
                t.cost, t.baseline, t.baseline / t.cost);

    // The Pareto window: flagship ≤ 35% of turns (the scarce tier), and the
    // cost model must be ≥ 1.8× cheaper than running every turn on the
    // flagship. Calibrated against THIS corpus (measured 40% on the earlier
    // hard-skewed probe; this corpus is realistic and lands lower).
    const double flagship_share = 100.0 * t.flagship_turns / n;
    const double saving         = t.baseline / t.cost;
    CHECK(flagship_share <= 35.0,
          "flagship must stay the scarce tier (Pareto tail), not the default");
    CHECK(saving >= 1.8,
          "cost model must beat the all-flagship baseline by the Pareto margin");
    CHECK(t.tier[1] + t.tier[2] >= n / 2,
          "the middle tiers (Utility+Implementation) must carry the bulk");
}