#include "Data.hpp"

#include <Geode/utils/file.hpp>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <numeric>
#include <set>

using namespace geode::prelude;

namespace senrate {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string tr(char const* en, char const* es) {
    // read every call so changing the setting applies immediately
    bool spanish = Mod::get()->getSettingValue<std::string>("language") != "English";
    if (!spanish) return en;

    // GD's bitmap fonts don't have accented characters
    static std::pair<char const*, char const*> const replacements[] = {
        { "á", "a" }, { "é", "e" }, { "í", "i" }, { "ó", "o" }, { "ú", "u" },
        { "Á", "A" }, { "É", "E" }, { "Í", "I" }, { "Ó", "O" }, { "Ú", "U" },
        { "ñ", "n" }, { "Ñ", "N" }, { "¡", "" }, { "¿", "" }, { "ü", "u" },
    };
    std::string out = es;
    for (auto const& [from, to] : replacements) {
        std::string_view f = from;
        for (size_t pos = out.find(f); pos != std::string::npos; pos = out.find(f, pos)) {
            out.replace(pos, f.size(), to);
            pos += std::string_view(to).size();
        }
    }
    return out;
}

std::string pct(float v01, int decimals) {
    if (!std::isfinite(v01)) v01 = 0.f;
    return fmt::format("{:.{}f}%", v01 * 100.f, decimals);
}

std::string runName(int start, int end) {
    return fmt::format("{}-{}", start, end);
}

std::string gradeFor(float rate) {
    if (rate >= 0.50f) return "S";
    if (rate >= 0.25f) return "A";
    if (rate >= 0.10f) return "B";
    if (rate >= 0.03f) return "C";
    if (rate > 0.f) return "D";
    return "-";
}

ccColor3B rateColor(float r) {
    r = std::clamp(r, 0.f, 1.f);
    // red -> yellow -> green
    if (r < 0.5f) {
        float t = r / 0.5f;
        return { 255, static_cast<GLubyte>(70 + 150 * t), 60 };
    }
    float t = (r - 0.5f) / 0.5f;
    return { static_cast<GLubyte>(255 - 175 * t), static_cast<GLubyte>(220 + 30 * t), static_cast<GLubyte>(60 + 40 * t) };
}

uint32_t currentSession() {
    static uint32_t const id = static_cast<uint32_t>(std::max<long long>(1, static_cast<long long>(std::time(nullptr))));
    return id;
}

Settings Settings::get() {
    auto mod = Mod::get();
    auto noclip = mod->getSettingValue<std::string>("noclip-mode");
    Settings s;
    s.partSize = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("part-size"), 1, 50));
    s.recentWindow = static_cast<int>(std::max<int64_t>(1, mod->getSettingValue<int64_t>("recent-window")));
    s.minSamples = static_cast<int>(std::max<int64_t>(1, mod->getSettingValue<int64_t>("min-samples")));
    s.noclip = noclip == "Ignore" ? NoclipMode::Ignore
        : noclip == "Count until first hit" ? NoclipMode::FirstHit
        : NoclipMode::CountAll;
    return s;
}

RunState& runState() {
    static RunState state;
    return state;
}

// ---------------------------------------------------------------------------
// Selecting & grouping runs
// ---------------------------------------------------------------------------

static bool counts(Run const& r, Settings const& s) {
    if (!r.noclip()) return true;
    switch (s.noclip) {
        case NoclipMode::Ignore: return false;
        case NoclipMode::FirstHit: return !r.continuation();
        default: return true;
    }
}

std::vector<Run> selectRuns(LevelStats const& level, Window window, Settings const& s) {
    std::vector<Run> out;
    if (window == Window::Recent) {
        // the latest N runs of every start position
        std::map<int, int> taken;
        for (auto it = level.runs.rbegin(); it != level.runs.rend(); ++it) {
            if (!counts(*it, s)) continue;
            if (taken[it->start]++ >= s.recentWindow) continue;
            out.push_back(*it);
        }
        std::reverse(out.begin(), out.end());
        return out;
    }
    out.reserve(level.runs.size());
    for (auto const& r : level.runs) {
        if (!counts(r, s)) continue;
        if (window == Window::Session && r.session != currentSession()) continue;
        out.push_back(r);
    }
    return out;
}

std::vector<int> originsOf(std::vector<Run> const& runs) {
    std::set<int> origins;
    for (auto const& r : runs) origins.insert(r.origin);
    return { origins.begin(), origins.end() };
}

std::vector<RunGroup> groupRuns(std::vector<Run> const& runs, std::optional<int> origin) {
    std::map<std::pair<int, int>, RunGroup> groups;
    for (auto const& r : runs) {
        if (origin && r.origin != *origin) continue;
        auto& g = groups[{ r.start, r.end }];
        g.start = r.start;
        g.end = r.end;
        (r.noclip() ? g.noclip : g.normal)++;
    }
    std::vector<RunGroup> out;
    out.reserve(groups.size());
    for (auto const& [_, g] : groups) out.push_back(g);
    // furthest runs first, then the ones that began earlier
    std::sort(out.begin(), out.end(), [](RunGroup const& x, RunGroup const& y) {
        if (x.end != y.end) return x.end > y.end;
        return x.start < y.start;
    });
    return out;
}

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

namespace {
    using Ends = std::map<int, int>;     // end percent -> count
    using Dataset = std::map<int, Ends>; // start percent -> ends

    // Pseudo-runs added to every rate so a lucky/unlucky handful of runs
    // doesn't decide the verdict on its own
    constexpr float PRIOR_WEIGHT = 2.f;
    // Runs needed before a practice / long-run rate is trusted
    constexpr int MIN_CONTEXT = 3;
    // A part is a choke when long runs pass it this much less than practice
    constexpr float CHOKE_GAP = 0.2f;
    // Below this you fail the part more than you'd like even in practice
    constexpr float WEAK_RATE = 0.6f;

    int reached(Ends const& ends, int percent) {
        int total = 0;
        for (auto it = ends.lower_bound(percent); it != ends.end(); ++it) total += it->second;
        return total;
    }

    int completionsOf(Ends const& ends) {
        auto it = ends.find(COMPLETE);
        return it == ends.end() ? 0 : it->second;
    }

    int totalOf(Ends const& ends) {
        int total = 0;
        for (auto const& [_, c] : ends) total += c;
        return total;
    }
}

Analysis analyze(std::vector<Run> const& runs, int start, Settings const& s) {
    Analysis a;
    a.start = start;
    int const partSize = std::max(1, s.partSize);
    // Runs that began at most this far before a part count as practicing it
    int const freshRange = std::max(10, partSize * 2);

    Dataset data;
    for (auto const& r : runs) {
        int key = r.start;
        if (r.continuation()) {
            // A noclip run that began right after a hit starts in the middle of
            // the obstacle that hit you, so it only counts from the next percent
            key = r.start + 1;
            if (key > 99 || (!r.completed() && r.end < key)) continue;
        }
        data[key][r.end]++;
    }

    // --- Runs from exactly this start --------------------------------------
    if (auto it = data.find(start); it != data.end()) {
        long long sum = 0;
        for (auto const& [end, count] : it->second) {
            a.runs += count;
            sum += static_cast<long long>(end) * count;
            if (end >= COMPLETE) a.completions += count;
            else a.deaths[end] += count;
        }
        if (a.runs > 0) {
            a.passRate = float(a.completions) / float(a.runs);
            a.avgEnd = float(sum) / float(a.runs);
        }
    }

    int streak = 0;
    for (auto const& r : runs) {
        if (r.start != start || r.continuation()) continue;
        streak = r.completed() ? streak + 1 : 0;
        a.bestStreak = std::max(a.bestStreak, streak);
    }
    a.currentStreak = streak;

    // --- Parts, aligned to multiples of partSize ---------------------------
    for (int from = start; from < COMPLETE;) {
        int to = std::min(COMPLETE, (from / partSize + 1) * partSize);
        PartStats part;
        part.from = from;
        part.to = to;
        for (auto const& [st, ends] : data) {
            if (st > from) break; // runs that began later never entered this part
            int entered = reached(ends, from);
            if (entered == 0) continue;
            int passed = to >= COMPLETE ? completionsOf(ends) : reached(ends, to);
            part.entered += entered;
            part.passed += passed;
            for (auto it = ends.lower_bound(from); it != ends.end() && it->first < to; ++it) {
                part.deaths += it->second;
            }
            if (from - st <= freshRange) {
                part.freshEntered += entered;
                part.freshPassed += passed;
            }
            else {
                part.deepEntered += entered;
                part.deepPassed += passed;
            }
        }
        a.parts.push_back(part);
        from = to;
    }

    // --- Skill / pressure estimates ----------------------------------------
    long long totalPassed = 0, totalEntered = 0;
    for (auto const& p : a.parts) {
        totalPassed += p.passed;
        totalEntered += p.entered;
    }
    float prior = totalEntered > 0 ? std::clamp(float(totalPassed) / float(totalEntered), 0.5f, 0.97f) : 0.85f;
    auto shrink = [&](int passed, int entered) {
        return (float(passed) + PRIOR_WEIGHT * prior) / (float(entered) + PRIOR_WEIGHT);
    };

    // Pass rate every part needs for a 25% chance of a full run from `start`
    size_t partCount = std::max<size_t>(1, a.parts.size());
    a.partTarget = std::clamp(std::pow(0.25f, 1.f / float(partCount)), 0.6f, 0.95f);

    a.estimateComplete = !a.parts.empty();
    float estimate = 1.f;
    for (size_t i = 0; i < a.parts.size(); i++) {
        auto& p = a.parts[i];
        if (p.deaths > 0 && (!a.deadliestPart || p.deaths > a.parts[*a.deadliestPart].deaths)) {
            a.deadliestPart = i;
        }
        if (p.entered == 0) {
            a.estimateComplete = false;
            p.status = PartStatus::NoData;
            continue;
        }
        estimate *= p.rate();

        // Skill = how well you do the part when you're actually playing it.
        // Long runs can only make it look worse (nerves), never better, so
        // the best of practice-only and overall data is your real level.
        p.skill = shrink(p.passed, p.entered);
        if (p.freshEntered >= MIN_CONTEXT) p.skill = std::max(p.skill, shrink(p.freshPassed, p.freshEntered));
        if (p.deepEntered >= MIN_CONTEXT) p.pressure = shrink(p.deepPassed, p.deepEntered);

        if (p.entered < s.minSamples) p.status = PartStatus::Uncertain;
        else if (p.skill >= a.partTarget) p.status = PartStatus::Consistent;
        else if (p.skill >= WEAK_RATE) p.status = PartStatus::Shaky;
        else p.status = PartStatus::Weak;
    }
    a.estimatedRate = a.estimateComplete ? estimate : 0.f;

    for (size_t i = 0; i < a.parts.size(); i++) {
        auto const& p = a.parts[i];
        if (p.status == PartStatus::Weak || p.status == PartStatus::Shaky) a.walls.push_back(i);
        if (p.freshEntered >= MIN_CONTEXT && p.pressure >= 0.f) {
            float practice = shrink(p.freshPassed, p.freshEntered);
            if (p.pressure < practice - CHOKE_GAP) a.chokes.push_back(i);
        }
    }
    std::sort(a.walls.begin(), a.walls.end(), [&](size_t x, size_t y) { return a.parts[x].skill < a.parts[y].skill; });
    std::sort(a.chokes.begin(), a.chokes.end(), [&](size_t x, size_t y) {
        auto gap = [&](PartStats const& p) { return shrink(p.freshPassed, p.freshEntered) - p.pressure; };
        return gap(a.parts[x]) > gap(a.parts[y]);
    });

    for (size_t i = a.parts.size(); i-- > 0;) {
        if (a.parts[i].status != PartStatus::Consistent) break;
        a.consistentFrom = a.parts[i].from;
    }

    // Earliest later start pos you already complete consistently (e.g. 87-100 x5)
    for (auto const& [st, ends] : data) {
        if (st <= start) continue;
        int total = totalOf(ends);
        int comps = completionsOf(ends);
        if (comps >= 3 && float(comps) / float(total) >= 0.6f) {
            a.masteredFrom = st;
            a.masteredRuns = total;
            a.masteredCompletions = comps;
            break;
        }
    }
    return a;
}

// ---------------------------------------------------------------------------
// Tips
// ---------------------------------------------------------------------------

static std::string partName(PartStats const& p) {
    return fmt::format("{}-{}%", p.from, p.to);
}

static std::string colored(float rate, std::string const& text) {
    char const* tag = rate >= 0.8f ? "<cg>" : rate >= 0.5f ? "<cy>" : rate >= 0.3f ? "<co>" : "<cr>";
    return tag + text + "</c>";
}

std::string buildTips(LevelStats const& level, int start, Window window, Settings const& s) {
    auto runs = selectRuns(level, window, s);
    auto a = analyze(runs, start, s);

    bool anyData = std::any_of(a.parts.begin(), a.parts.end(), [](auto const& p) { return p.entered > 0; });
    if (a.runs == 0 && !anyData) {
        return fmt::format(fmt::runtime(tr(
            "## No data yet\n\nPlay some runs from <cy>{}%</c> and come back: SenRate will tell you which parts are holding you back.",
            "## Todavía no hay datos\n\nHacé algunas runs desde <cy>{}%</c> y volvé: SenRate te va a decir qué partes te están frenando."
        )), start);
    }

    std::string out;
    auto add = [&](std::string const& line) { out += line + "\n\n"; };

    // --- Summary -----------------------------------------------------------
    add(fmt::format(fmt::runtime(tr("## From {}%", "## Desde {}%")), start));
    if (a.runs > 0) {
        add(fmt::format(fmt::runtime(tr(
            "You reached 100% in <cg>{}</c> of <cy>{}</c> runs: pass rate {}.",
            "Llegaste al 100% en <cg>{}</c> de <cy>{}</c> runs: pass rate {}."
        )), a.completions, a.runs, colored(a.passRate, pct(a.passRate))));
    }
    if (a.estimateComplete && a.estimatedRate > 0.f) {
        add(fmt::format(fmt::runtime(tr(
            "Combining all your practice (every start pos), your chance of a 100% from here is about {} - roughly <cy>1 every {}</c> attempts. Consistency grade: <cy>{}</c>.",
            "Combinando toda tu práctica (todos los startpos), tu chance de hacer 100% desde acá es de {} - más o menos <cy>1 cada {}</c> intentos. Nota de consistencia: <cy>{}</c>."
        )), colored(a.estimatedRate, pct(a.estimatedRate, 2)),
            static_cast<int>(std::ceil(1.f / a.estimatedRate)), gradeFor(a.estimatedRate)));
    }

    // --- What you already master -------------------------------------------
    if (a.masteredFrom > start) {
        add(fmt::format(fmt::runtime(tr(
            "<cg>You already master {}-100</c>: you completed it {} of {} times ({}). The end isn't your problem.",
            "<cg>Ya dominás {}-100</c>: lo completaste {} de {} veces ({}). El final no es tu problema."
        )), a.masteredFrom, a.masteredCompletions, a.masteredRuns,
            pct(float(a.masteredCompletions) / float(std::max(1, a.masteredRuns)))));
    }
    else if (a.consistentFrom > start) {
        add(fmt::format(fmt::runtime(tr(
            "You pass every part from <cg>{}%</c> to the end consistently.",
            "Pasás todas las partes desde <cg>{}%</c> hasta el final de forma consistente."
        )), a.consistentFrom));
    }

    // --- Walls -------------------------------------------------------------
    if (!a.walls.empty()) {
        auto const& w = a.parts[a.walls.front()];
        add(fmt::format(fmt::runtime(tr("### Your worst part: <cr>{}</c>", "### Tu peor parte: <cr>{}</c>")), partName(w)));
        std::string line;
        if (w.freshEntered >= MIN_CONTEXT) {
            line = fmt::format(fmt::runtime(tr(
                "Even practicing it (runs that begin close to it) you only pass it {} ({}/{})",
                "Incluso practicándola (runs que empiezan cerca) la pasás solo el {} ({}/{})"
            )), colored(w.freshRate(), pct(w.freshRate())), w.freshPassed, w.freshEntered);
            if (w.deepEntered >= MIN_CONTEXT) {
                line += fmt::format(fmt::runtime(tr(", and {} in long runs ({}/{})", ", y el {} en runs largas ({}/{})")),
                    colored(w.deepRate(), pct(w.deepRate())), w.deepPassed, w.deepEntered);
            }
        }
        else {
            line = fmt::format(fmt::runtime(tr("You pass it {} of the time ({}/{})", "La pasás el {} de las veces ({}/{})")),
                colored(w.rate(), pct(w.rate())), w.passed, w.entered);
        }
        line += fmt::format(fmt::runtime(tr(", with <cr>{}</c> deaths there.", ", con <cr>{}</c> muertes ahí.")), w.deaths);
        if (w.rate() > 0.f && w.rate() < 1.f) {
            line += fmt::format(fmt::runtime(tr(
                " Making it consistent would multiply your chance of a 100% by <cg>x{:.1f}</c>.",
                " Si la hacés consistente, tu chance de 100% se multiplica por <cg>x{:.1f}</c>."
            )), 1.f / w.rate());
        }
        add(line);

        if (a.walls.size() > 1) {
            std::string list = tr("**Also work on:**\n", "**También practicá:**\n");
            for (size_t i = 1; i < std::min<size_t>(a.walls.size(), 4); i++) {
                auto const& p = a.parts[a.walls[i]];
                list += fmt::format("\n- {}: {} ({}/{})", partName(p), colored(p.skill, pct(p.skill)), p.passed, p.entered);
            }
            add(list);
        }
    }
    else if (anyData) {
        bool allConsistent = std::all_of(a.parts.begin(), a.parts.end(), [](auto const& p) { return p.status == PartStatus::Consistent; });
        add(allConsistent
            ? tr("<cg>No weak parts:</c> you pass every part often enough for a good chance of a full run.",
                 "<cg>No tenés partes débiles:</c> pasás cada parte lo suficiente para tener buena chance de una run completa.")
            : tr("None of the parts with enough runs is weak.",
                 "Ninguna de las partes con suficientes runs es débil."));
    }

    // --- Chokes ------------------------------------------------------------
    if (!a.chokes.empty()) {
        add(tr("### Nerves (choke)", "### Nervios (choke)"));
        std::string list;
        for (size_t i = 0; i < std::min<size_t>(a.chokes.size(), 3); i++) {
            auto const& p = a.parts[a.chokes[i]];
            list += fmt::format(fmt::runtime(tr(
                "\n- {}: practicing {} ({}/{}), in long runs <cr>{}</c> ({}/{})",
                "\n- {}: practicando {} ({}/{}), en runs largas <cr>{}</c> ({}/{})"
            )), partName(p), pct(p.freshRate()), p.freshPassed, p.freshEntered, pct(p.deepRate()), p.deepPassed, p.deepEntered);
        }
        add(list);
        add(tr(
            "It's not a lack of skill: you can do it, but the pressure gets you. Do more long runs and breathe before you get there.",
            "No es falta de skill: la sabés hacer, pero te gana la presión. Hacé más runs largas y respirá antes de llegar."
        ));
    }

    // --- Deadliest percent -------------------------------------------------
    if (!a.deaths.empty()) {
        auto top = std::max_element(a.deaths.begin(), a.deaths.end(), [](auto const& x, auto const& y) { return x.second < y.second; });
        add(fmt::format(fmt::runtime(tr(
            "Most deaths from this start happen at <cr>{}%</c> ({} deaths, {} of your runs).",
            "La mayoría de tus muertes desde este start son en <cr>{}%</c> ({} muertes, {} de tus runs)."
        )), top->first, top->second, pct(float(top->second) / float(std::max(1, a.runs)))));
    }

    // --- Trend -------------------------------------------------------------
    if (window != Window::Session) {
        std::vector<bool> own;
        for (auto const& r : selectRuns(level, Window::AllTime, s)) {
            if (r.start == start) own.push_back(r.completed());
        }
        size_t w = static_cast<size_t>(s.recentWindow);
        if (own.size() >= w + std::max<size_t>(1, w / 2)) {
            auto rateOf = [&](size_t from, size_t to) {
                size_t c = 0;
                for (size_t i = from; i < to; i++) c += own[i];
                return float(c) / float(std::max<size_t>(1, to - from));
            };
            size_t n = own.size();
            size_t prevFrom = n >= 2 * w ? n - 2 * w : 0;
            float recent = rateOf(n - w, n);
            float prev = rateOf(prevFrom, n - w);
            float diff = recent - prev;
            std::string trend = std::abs(diff) < 0.02f
                ? tr("<cy>stable</c>", "<cy>estable</c>")
                : diff > 0 ? fmt::format("<cg>+{}</c>", pct(diff)) : fmt::format("<cr>-{}</c>", pct(-diff));
            add(fmt::format(fmt::runtime(tr(
                "**Trend** (last {} runs vs the ones before): pass rate {} -> {} ({}).",
                "**Tendencia** (últimas {} runs vs las anteriores): pass rate {} -> {} ({})."
            )), w, pct(prev), pct(recent), trend));
            if (diff < -0.05f) {
                add(tr("You're doing worse than before - maybe take a short break.",
                       "Estás jugando peor que antes - capaz te conviene un descanso corto."));
            }
        }
    }

    // --- Missing data ------------------------------------------------------
    std::optional<std::pair<int, int>> missing;
    for (auto const& p : a.parts) {
        bool lacking = p.entered < s.minSamples;
        if (lacking && !missing) missing = std::make_pair(p.from, p.to);
        else if (lacking && missing && missing->second == p.from) missing->second = p.to;
        else if (!lacking && missing) break;
    }
    if (missing) {
        add(fmt::format(fmt::runtime(tr(
            "Not enough runs between <cy>{}%</c> and <cy>{}%</c>. Do some runs from a start pos there so SenRate can rate that zone.",
            "Hay pocas runs entre <cy>{}%</c> y <cy>{}%</c>. Hacé runs desde un startpos ahí así SenRate puede evaluar esa zona."
        )), missing->first, missing->second));
    }

    // --- Practice plan -----------------------------------------------------
    add(tr("### Practice plan", "### Plan de práctica"));
    if (a.runs >= 10 && a.passRate >= 0.5f) {
        if (start > 0) {
            add(fmt::format(fmt::runtime(tr(
                "You're consistent from {}% ({}). Move your start pos earlier, around <cy>{}%</c>.",
                "Ya sos consistente desde {}% ({}). Mové tu startpos más atrás, a <cy>{}%</c> más o menos."
            )), start, pct(a.passRate), std::max(0, start - 10)));
        }
        else {
            add(tr("You complete the level from 0% half the time or more. Go get that verify!",
                   "Completás el nivel desde 0% la mitad de las veces o más. ¡Andá a buscar ese verify!"));
        }
    }
    else if (!a.walls.empty()) {
        auto const& w = a.parts[a.walls.front()];
        add(fmt::format(fmt::runtime(tr(
            "1. Put a start pos at <cy>{}%</c> and grind {} until you pass it 8/10 times.\n2. Then do runs from {}% to connect it with the rest.\n3. Go back to runs from {}% and check this menu again.",
            "1. Poné un startpos en <cy>{}%</c> y practicá {} hasta pasarla 8 de 10 veces.\n2. Después hacé runs desde {}% para conectarla con el resto.\n3. Volvé a las runs desde {}% y revisá este menú de nuevo."
        )), w.from, partName(w), std::max(start, w.from - s.partSize * 2), start));
    }
    else if (!a.chokes.empty()) {
        auto const& c = a.parts[a.chokes.front()];
        add(fmt::format(fmt::runtime(tr(
            "Your skill is there. Do more runs from {}% (or earlier) so arriving at {} under pressure becomes normal.",
            "El skill ya lo tenés. Hacé más runs desde {}% (o más atrás) para que llegar a {} con presión se vuelva normal."
        )), start, partName(c)));
    }
    else if (a.masteredFrom > start) {
        add(fmt::format(fmt::runtime(tr(
            "Keep extending backwards: do runs from around <cy>{}%</c> until you complete those too.",
            "Seguí extendiendo hacia atrás: hacé runs desde <cy>{}%</c> más o menos hasta completarlas también."
        )), std::max(start, a.masteredFrom - 10)));
    }
    else if (missing) {
        add(fmt::format(fmt::runtime(tr(
            "Do runs with start positions between {}% and {}% to unlock a personalised plan.",
            "Hacé runs con startpos entre {}% y {}% para desbloquear un plan personalizado."
        )), missing->first, missing->second));
    }
    else {
        add(tr("Keep playing to unlock a personalised plan.", "Seguí jugando para desbloquear un plan personalizado."));
    }

    return out;
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

namespace {
    // start (7 bits) | end (7 bits) | origin (7 bits) | flags (4 bits)
    int64_t pack(Run const& r) {
        return int64_t(r.start) | (int64_t(r.end) << 7) | (int64_t(r.origin) << 14) | (int64_t(r.flags & 0xF) << 21);
    }

    Run unpack(int64_t v) {
        Run r;
        r.start = static_cast<uint8_t>(std::min<int64_t>(v & 0x7F, 99));
        r.end = static_cast<uint8_t>(std::clamp<int64_t>((v >> 7) & 0x7F, r.start, COMPLETE));
        r.origin = static_cast<uint8_t>(std::min<int64_t>((v >> 14) & 0x7F, r.start));
        r.flags = static_cast<uint8_t>((v >> 21) & 0xF);
        return r;
    }

    constexpr int SAVE_EVERY = 25;
}

Manager& Manager::get() {
    static Manager instance;
    return instance;
}

std::string Manager::keyFor(GJGameLevel* level) {
    if (!level) return "unknown";
    switch (level->m_levelType) {
        case GJLevelType::Editor: {
            // local levels have no ID: use a sanitised name + a hash of it
            std::string name = level->m_levelName;
            std::string clean;
            for (char c : name) clean += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
            if (clean.size() > 32) clean.resize(32);
            return fmt::format("local_{}_{:08x}", clean, static_cast<uint32_t>(std::hash<std::string>{}(name)));
        }
        case GJLevelType::Main:
            return fmt::format("main_{}", level->m_levelID.value());
        default:
            return fmt::format("online_{}", level->m_levelID.value());
    }
}

std::filesystem::path Manager::pathFor(std::string const& key) const {
    return Mod::get()->getSaveDir() / "levels" / (key + ".json");
}

LevelStats Manager::load(std::string const& key) const {
    LevelStats stats;
    stats.key = key;
    auto path = pathFor(key);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return stats;

    auto res = utils::file::readJson(path);
    if (!res) {
        log::warn("Couldn't read {}: {}", key, res.unwrapErr());
        return stats;
    }
    auto json = std::move(res).unwrap();
    stats.name = json["name"].asString().unwrapOr("");

    // --- current format: every run, plus run-length encoded session ids ---
    if (json["runs"].isArray()) {
        size_t count = json["runs"].size();
        std::vector<uint32_t> sessions;
        sessions.reserve(count);
        if (json["sessions"].isArray()) {
            for (auto const& s : json["sessions"]) {
                if (!s.isArray() || s.size() < 2) continue;
                auto id = static_cast<uint32_t>(s[0].asInt().unwrapOr(0));
                auto n = static_cast<size_t>(std::max<int64_t>(0, s[1].asInt().unwrapOr(0)));
                n = std::min(n, count - sessions.size());
                sessions.insert(sessions.end(), n, id);
            }
        }
        stats.runs.reserve(count);
        size_t i = 0;
        for (auto const& v : json["runs"]) {
            Run r = unpack(v.asInt().unwrapOr(0));
            r.session = i < sessions.size() ? sessions[i] : 0;
            stats.runs.push_back(r);
            i++;
        }
        return stats;
    }

    // --- v1.0 / v1.1 format: totals + capped history per start -------------
    if (json["starts"].isArray()) {
        for (auto const& s : json["starts"]) {
            int start = static_cast<int>(std::clamp<int64_t>(s["s"].asInt().unwrapOr(0), 0, 99));
            int comps = static_cast<int>(s["c"].asInt().unwrapOr(0));
            std::map<int, int> deaths;
            if (s["d"].isArray()) {
                for (auto const& pair : s["d"]) {
                    if (!pair.isArray() || pair.size() < 2) continue;
                    deaths[static_cast<int>(pair[0].asInt().unwrapOr(0))] += static_cast<int>(pair[1].asInt().unwrapOr(0));
                }
            }
            std::vector<int> history;
            if (s["h"].isArray()) {
                for (auto const& h : s["h"]) {
                    history.push_back(static_cast<int>(std::clamp<int64_t>(h.asInt().unwrapOr(start), start, COMPLETE)));
                }
            }
            std::map<int, int> inHistory;
            int compsInHistory = 0;
            for (int h : history) {
                if (h >= COMPLETE) compsInHistory++;
                else inHistory[h]++;
            }
            auto push = [&](int end) {
                Run r;
                r.start = r.origin = static_cast<uint8_t>(start);
                r.end = static_cast<uint8_t>(std::clamp(end, start, COMPLETE));
                stats.runs.push_back(r);
            };
            // runs older than the stored history only existed in the totals
            for (auto const& [p, c] : deaths) {
                for (int k = inHistory[p]; k < c; k++) push(p);
            }
            for (int k = compsInHistory; k < comps; k++) push(COMPLETE);
            for (int h : history) push(h);
        }
        log::info("Migrated {} runs of {} to the new format", stats.runs.size(), key);
    }
    return stats;
}

LevelStats& Manager::level(GJGameLevel* level) {
    auto key = keyFor(level);
    auto it = m_levels.find(key);
    if (it == m_levels.end()) {
        it = m_levels.emplace(key, load(key)).first;
    }
    if (level) it->second.name = level->m_levelName;
    return it->second;
}

void Manager::addRun(GJGameLevel* lvl, Run run) {
    auto& stats = level(lvl);
    run.session = currentSession();
    stats.runs.push_back(run);
    if (++m_unsaved[stats.key] >= SAVE_EVERY || run.completed()) save(stats.key);
}

size_t Manager::deleteSession(GJGameLevel* lvl) {
    auto& stats = level(lvl);
    auto session = currentSession();
    size_t before = stats.runs.size();
    std::erase_if(stats.runs, [&](Run const& r) { return r.session == session; });
    size_t removed = before - stats.runs.size();
    save(stats.key);
    return removed;
}

void Manager::reset(GJGameLevel* lvl) {
    auto key = keyFor(lvl);
    m_levels[key] = LevelStats{ key, lvl ? std::string(lvl->m_levelName) : "" };
    std::error_code ec;
    std::filesystem::remove(pathFor(key), ec);
    m_unsaved[key] = 0;
}

void Manager::save(std::string const& key) {
    auto it = m_levels.find(key);
    if (it == m_levels.end()) return;
    auto const& stats = it->second;

    // written by hand: much faster than building a JSON tree for thousands of runs
    std::string out;
    out.reserve(96 + stats.runs.size() * 9);
    out += "{\"version\":2,\"name\":";
    out += matjson::Value(stats.name).dump(matjson::NO_INDENTATION);
    out += ",\"runs\":[";
    for (size_t i = 0; i < stats.runs.size(); i++) {
        if (i) out += ',';
        out += std::to_string(pack(stats.runs[i]));
    }
    out += "],\"sessions\":[";
    for (size_t i = 0; i < stats.runs.size();) {
        size_t j = i;
        while (j < stats.runs.size() && stats.runs[j].session == stats.runs[i].session) j++;
        if (i) out += ',';
        out += fmt::format("[{},{}]", stats.runs[i].session, j - i);
        i = j;
    }
    out += "]}";

    auto path = pathFor(key);
    (void)utils::file::createDirectoryAll(path.parent_path());
    if (auto res = utils::file::writeStringSafe(path, out); !res) {
        log::error("Failed to save {}: {}", key, res.unwrapErr());
    }
    m_unsaved[key] = 0;
}

void Manager::saveAll() {
    for (auto const& [key, count] : m_unsaved) {
        if (count > 0) save(key);
    }
}

}
