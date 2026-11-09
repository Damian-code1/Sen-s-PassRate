#include "StatsPopup.hpp"

#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/MDTextArea.hpp>
#include <Geode/ui/NineSlice.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace senrate {

namespace {
    constexpr float WIDTH = 460.f;
    constexpr float HEIGHT = 300.f;
    constexpr float PANEL_X = 15.f;
    constexpr float PANEL_Y = 34.f;
    constexpr float PANEL_W = WIDTH - 30.f;
    constexpr float PANEL_H = 156.f;

    ccColor3B const GREEN = { 120, 255, 140 };
    ccColor3B const GRAY = { 190, 190, 210 };

    CCLabelBMFont* label(std::string const& text, char const* font, float scale, CCPoint pos, CCPoint anchor = { .5f, .5f }) {
        auto lbl = CCLabelBMFont::create(text.c_str(), font);
        lbl->setScale(scale);
        lbl->setAnchorPoint(anchor);
        lbl->setPosition(pos);
        return lbl;
    }

    void limitWidth(CCLabelBMFont* lbl, float maxWidth) {
        float w = lbl->getContentSize().width * lbl->getScale();
        if (w > maxWidth && w > 0.f) lbl->setScale(lbl->getScale() * maxWidth / w);
    }

    CCLayerColor* rect(ccColor3B color, GLubyte opacity, float x, float y, float w, float h) {
        auto r = CCLayerColor::create({ color.r, color.g, color.b, opacity }, std::max(w, 0.f), std::max(h, 0.f));
        r->setPosition({ x, y });
        return r;
    }

    CCMenu* menuIn(CCNode* parent, CCSize size) {
        auto menu = CCMenu::create();
        menu->setPosition({ 0, 0 });
        menu->setContentSize(size);
        parent->addChild(menu, 5);
        return menu;
    }

    std::string windowName(Window w) {
        switch (w) {
            case Window::Session: return tr("Session", "Sesion");
            case Window::Recent: return tr("Recent", "Recientes");
            default: return tr("All-time", "Total");
        }
    }

    std::string tabName(Tab t) {
        switch (t) {
            case Tab::Overview: return tr("Overview", "Resumen");
            case Tab::Parts: return tr("Parts", "Partes");
            case Tab::Runs: return "Runs";
            case Tab::Starts: return tr("Starts", "Startpos");
            case Tab::Deaths: return tr("Deaths", "Muertes");
            default: return tr("Tips", "Consejos");
        }
    }

    // Small text for a rate that may not have data
    std::string rateText(int passed, int entered) {
        return entered > 0 ? pct(float(passed) / float(entered), 0) : "-";
    }
}

StatsPopup* StatsPopup::create(GJGameLevel* level, int preferredStart, std::optional<Tab> tab, std::optional<Window> window) {
    auto ret = new StatsPopup();
    if (ret->init(level, preferredStart, tab, window)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

void StatsPopup::open(GJGameLevel* level, int preferredStart, std::optional<Tab> tab, std::optional<Window> window) {
    if (!level) return;
    if (auto popup = create(level, preferredStart, tab, window)) popup->show();
}

StatsPopup::~StatsPopup() {
    CC_SAFE_RELEASE(m_level);
}

bool StatsPopup::init(GJGameLevel* level, int preferredStart, std::optional<Tab> tab, std::optional<Window> window) {
    if (!Popup::init(WIDTH, HEIGHT, "GJ_square02.png")) return false;
    m_level = level;
    CC_SAFE_RETAIN(m_level);

    auto mod = Mod::get();
    m_tab = tab.value_or(static_cast<Tab>(std::clamp(mod->getSavedValue<int>("last-tab", 0), 0, static_cast<int>(Tab::Count) - 1)));
    m_window = window.value_or(static_cast<Window>(std::clamp(mod->getSavedValue<int>("last-window", 0), 0, 2)));
    m_runsAllStarts = mod->getSavedValue<bool>("runs-all-starts", false);

    this->setTitle("SenRate", "goldFont.fnt", .85f, 18.f);
    if (m_title) {
        if (auto logo = CCSprite::create("senrate_logo.png"_spr)) {
            logo->setScale(26.f / logo->getContentSize().width);
            float titleW = m_title->getContentSize().width * m_title->getScale();
            logo->setPosition(m_title->getPosition() - CCPoint(titleW / 2 + 18.f, 0.f));
            m_mainLayer->addChild(logo);
        }
    }

    auto name = label(std::string(level->m_levelName), "bigFont.fnt", .38f, { WIDTH / 2, HEIGHT - 38.f });
    limitWidth(name, 300.f);
    name->setColor({ 200, 230, 255 });
    m_mainLayer->addChild(name);

    collectStarts(preferredStart);

    // --- start selector ---------------------------------------------------
    auto selector = menuIn(m_mainLayer, { WIDTH, HEIGHT });

    auto prevSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
    prevSpr->setScale(.45f);
    auto prevBtn = CCMenuItemSpriteExtra::create(prevSpr, this, menu_selector(StatsPopup::onPrevStart));
    prevBtn->setPosition({ 30.f, HEIGHT - 64.f });
    selector->addChild(prevBtn);

    auto nextSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
    nextSpr->setScale(.45f);
    nextSpr->setFlipX(true);
    auto nextBtn = CCMenuItemSpriteExtra::create(nextSpr, this, menu_selector(StatsPopup::onNextStart));
    nextBtn->setPosition({ 190.f, HEIGHT - 64.f });
    selector->addChild(nextBtn);

    m_startLabel = label("", "bigFont.fnt", .5f, { 110.f, HEIGHT - 59.f });
    m_mainLayer->addChild(m_startLabel, 2);
    m_summaryLabel = label("", "chatFont.fnt", .6f, { 110.f, HEIGHT - 74.f });
    m_mainLayer->addChild(m_summaryLabel, 2);

    // --- content panel ----------------------------------------------------
    auto panel = NineSlice::create("square02_001.png");
    panel->setContentSize({ PANEL_W, PANEL_H });
    panel->setAnchorPoint({ 0, 0 });
    panel->setPosition({ PANEL_X, PANEL_Y });
    panel->setOpacity(110);
    m_mainLayer->addChild(panel);

    m_content = CCNode::create();
    m_content->setContentSize({ PANEL_W, PANEL_H });
    m_content->setPosition({ PANEL_X, PANEL_Y });
    m_mainLayer->addChild(m_content, 3);

    // --- footer -----------------------------------------------------------
    auto credits = label("SenRate by Sen", "goldFont.fnt", .5f, { WIDTH / 2 + 10.f, 17.f });
    m_mainLayer->addChild(credits);
    auto version = label(mod->getVersion().toVString(), "chatFont.fnt", .45f, { WIDTH / 2 + 66.f, 14.f }, { 0.f, .5f });
    version->setOpacity(150);
    m_mainLayer->addChild(version);

    auto footer = menuIn(m_mainLayer, { WIDTH, HEIGHT });

    // gray X: this session / red X: the whole history
    auto makeX = [&](CircleBaseColor color, SEL_MenuHandler callback, float x, std::string const& text) {
        auto spr = CircleButtonSprite::createWithSprite("senrate_x.png"_spr, .9f, color, CircleBaseSize::Tiny);
        spr->setScale(22.f / std::max(1.f, spr->getContentSize().width));
        auto btn = CCMenuItemSpriteExtra::create(spr, this, callback);
        btn->setPosition({ x, 18.f });
        footer->addChild(btn);
        auto lbl = label(text, "bigFont.fnt", .3f, { x + 14.f, 18.f }, { 0.f, .5f });
        m_mainLayer->addChild(lbl, 2);
    };
    makeX(CircleBaseColor::Gray, menu_selector(StatsPopup::onDeleteSession), 24.f, tr("Session", "Sesion"));
    makeX(CircleBaseColor::Red, menu_selector(StatsPopup::onDeleteAll), 100.f, tr("All", "Todo"));

    auto gear = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
    gear->setScale(.5f);
    auto gearBtn = CCMenuItemSpriteExtra::create(gear, this, menu_selector(StatsPopup::onSettings));
    gearBtn->setPosition({ WIDTH - 24.f, 18.f });
    footer->addChild(gearBtn);

    refresh();
    return true;
}

void StatsPopup::collectStarts(int preferred) {
    auto settings = Settings::get();
    auto runs = selectRuns(Manager::get().level(m_level), Window::AllTime, settings);
    auto origins = originsOf(runs);
    std::set<int> starts(origins.begin(), origins.end());
    if (preferred >= 0) starts.insert(preferred);
    if (starts.empty()) starts.insert(0);
    m_starts.assign(starts.begin(), starts.end());

    int pick = preferred;
    if (pick < 0) {
        std::map<int, int> perStart;
        for (auto const& r : runs) perStart[r.start]++;
        int best = -1;
        for (int s : m_starts) {
            if (perStart[s] > best) {
                best = perStart[s];
                pick = s;
            }
        }
    }
    auto it = std::find(m_starts.begin(), m_starts.end(), pick);
    m_startIdx = it == m_starts.end() ? 0 : static_cast<size_t>(it - m_starts.begin());
}

int StatsPopup::currentStart() const {
    return m_starts.empty() ? 0 : m_starts[std::min(m_startIdx, m_starts.size() - 1)];
}

void StatsPopup::refreshHeader() {
    m_startLabel->setString(fmt::format(fmt::runtime(tr("From {}%", "Desde {}%")), currentStart()).c_str());
    m_startLabel->setScale(.5f);
    limitWidth(m_startLabel, 130.f);

    // rebuild the toggles so the selected ones are highlighted
    if (auto old = m_mainLayer->getChildByID("senrate-toggles")) old->removeFromParent();
    auto menu = menuIn(m_mainLayer, { WIDTH, HEIGHT });
    menu->setID("senrate-toggles");

    for (int i = 0; i < 3; i++) {
        auto w = static_cast<Window>(i);
        auto spr = ButtonSprite::create(windowName(w).c_str(), 62, true, "bigFont.fnt",
            w == m_window ? "GJ_button_02.png" : "GJ_button_04.png", 24.f, .5f);
        spr->setScale(.62f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(StatsPopup::onWindow));
        btn->setTag(i);
        btn->setPosition({ 270.f + i * 64.f, HEIGHT - 64.f });
        menu->addChild(btn);
    }

    int tabs = static_cast<int>(Tab::Count);
    for (int i = 0; i < tabs; i++) {
        auto t = static_cast<Tab>(i);
        auto spr = ButtonSprite::create(tabName(t).c_str(), 92, true, "bigFont.fnt",
            t == m_tab ? "GJ_button_01.png" : "GJ_button_05.png", 26.f, .5f);
        spr->setScale(.66f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(StatsPopup::onTab));
        btn->setTag(i);
        btn->setPosition({ WIDTH / 2 + (i - (tabs - 1) / 2.f) * 70.f, HEIGHT - 94.f });
        menu->addChild(btn);
    }
}

void StatsPopup::refresh() {
    refreshHeader();
    m_content->removeAllChildren();

    auto settings = Settings::get();
    auto& stats = Manager::get().level(m_level);
    auto runs = selectRuns(stats, m_window, settings);
    auto a = analyze(runs, currentStart(), settings);

    m_summaryLabel->setString(fmt::format(fmt::runtime(tr("{} runs | {} x 100%", "{} runs | {} x 100%")), a.runs, a.completions).c_str());
    m_summaryLabel->setColor(a.completions > 0 ? GREEN : ccColor3B{ 220, 220, 220 });

    if (runs.empty()) {
        m_content->addChild(label(
            m_window == Window::Session
                ? tr("No runs this session yet.", "Todavia no hiciste runs en esta sesion.")
                : tr("No runs yet. Go play!", "Todavia no hay runs. A jugar!"),
            "bigFont.fnt", .45f, { PANEL_W / 2, PANEL_H / 2 }
        ));
    }
    else {
        switch (m_tab) {
            case Tab::Overview: buildOverview(a); break;
            case Tab::Parts: buildParts(a); break;
            case Tab::Runs: buildRuns(runs); break;
            case Tab::Starts: buildStarts(runs); break;
            case Tab::Deaths: buildDeaths(a); break;
            default: buildTips(); break;
        }
    }
    handleTouchPriority(this);
}

// ---------------------------------------------------------------------------
// Overview
// ---------------------------------------------------------------------------

void StatsPopup::buildOverview(Analysis const& a) {
    struct Tile { std::string value; std::string caption; ccColor3B color; };
    std::vector<Tile> tiles = {
        { std::to_string(a.completions), tr("100%s from here", "100% desde aca"), GREEN },
        { std::to_string(a.runs), tr("Runs", "Runs"), { 255, 255, 255 } },
        { a.runs ? pct(a.passRate) : "-", tr("Pass rate", "Pass rate"), a.runs ? rateColor(a.passRate) : GRAY },
        { a.estimateComplete ? pct(a.estimatedRate, 2) : "?", tr("Est. chance", "Chance estimada"), a.estimateComplete ? rateColor(a.estimatedRate * 2.f) : GRAY },
        { a.runs ? fmt::format("{:.1f}%", a.avgEnd) : "-", tr("Avg. reached", "Promedio"), { 140, 210, 255 } },
        { fmt::format("{} / {}", a.currentStreak, a.bestStreak), tr("Streak / best", "Racha / mejor"), { 255, 210, 90 } },
    };

    float tileW = 108.f;
    float topY = PANEL_H - 20.f;
    for (size_t i = 0; i < tiles.size(); i++) {
        float x = 62.f + (i % 3) * tileW;
        float y = topY - (i / 3) * 42.f;
        auto value = label(tiles[i].value, "bigFont.fnt", .6f, { x, y });
        limitWidth(value, tileW - 10.f);
        value->setColor(tiles[i].color);
        m_content->addChild(value);
        auto caption = label(tiles[i].caption, "chatFont.fnt", .6f, { x, y - 16.f });
        limitWidth(caption, tileW - 6.f);
        caption->setColor(GRAY);
        m_content->addChild(caption);
    }

    // Consistency grade
    float gradeRate = a.estimateComplete ? a.estimatedRate : a.passRate;
    auto grade = label(gradeFor(gradeRate), "bigFont.fnt", 1.5f, { PANEL_W - 55.f, topY - 13.f });
    grade->setColor(rateColor(gradeRate * 2.f));
    m_content->addChild(grade);
    m_content->addChild(label(tr("Consistency", "Consistencia"), "goldFont.fnt", .5f, { PANEL_W - 55.f, topY - 48.f }));

    // Pass rate per part
    float chartX = 12.f, chartY = 14.f, chartW = PANEL_W - 24.f, chartH = 36.f;
    auto chartTitle = label(
        fmt::format(fmt::runtime(tr("Pass rate per part ({}%)", "Pass rate por parte ({}%)")), Settings::get().partSize),
        "chatFont.fnt", .55f, { chartX, chartY + chartH + 8.f }, { 0.f, .5f }
    );
    chartTitle->setColor(GRAY);
    m_content->addChild(chartTitle);

    if (a.parts.empty()) return;
    auto worst = a.worstPart();
    float span = float(COMPLETE - a.start);
    m_content->addChild(rect({ 0, 0, 0 }, 70, chartX, chartY, chartW, chartH));
    for (size_t i = 0; i < a.parts.size(); i++) {
        auto const& p = a.parts[i];
        float x = chartX + chartW * float(p.from - a.start) / span;
        float w = chartW * float(p.to - p.from) / span - 1.f;
        if (p.entered == 0) {
            m_content->addChild(rect({ 120, 120, 120 }, 120, x, chartY, w, 2.f));
            continue;
        }
        float h = std::max(2.f, chartH * p.rate());
        m_content->addChild(rect(rateColor(p.rate()), 230, x, chartY, w, h));
        if (worst && *worst == i) m_content->addChild(rect({ 255, 255, 255 }, 255, x, chartY + chartH + 1.f, w, 2.f));
    }
    m_content->addChild(label(fmt::format("{}%", a.start), "chatFont.fnt", .45f, { chartX, chartY - 6.f }, { 0.f, .5f }));
    m_content->addChild(label("100%", "chatFont.fnt", .45f, { chartX + chartW, chartY - 6.f }, { 1.f, .5f }));

    CCLabelBMFont* verdict;
    if (worst) {
        auto const& w = a.parts[*worst];
        verdict = label(fmt::format(fmt::runtime(tr("worst: {}-{}% ({})", "peor: {}-{}% ({})")), w.from, w.to, pct(w.skill)),
            "chatFont.fnt", .45f, { chartX + chartW / 2, chartY - 6.f });
        verdict->setColor({ 255, 120, 110 });
    }
    else {
        verdict = label(tr("no weak parts", "sin partes debiles"), "chatFont.fnt", .45f, { chartX + chartW / 2, chartY - 6.f });
        verdict->setColor(GREEN);
    }
    m_content->addChild(verdict);
}

// ---------------------------------------------------------------------------
// Parts
// ---------------------------------------------------------------------------

void StatsPopup::buildParts(Analysis const& a) {
    constexpr float ROW_H = 20.f;
    float listH = PANEL_H - 22.f;

    struct Col { float x; char const* en; char const* es; };
    Col cols[] = {
        { 10, "Part", "Parte" }, { 74, "Pass rate", "Pass rate" }, { 200, "Practice", "Practica" },
        { 260, "Long runs", "En runs" }, { 322, "Deaths", "Muertes" }, { 374, "Status", "Estado" },
    };
    for (auto const& c : cols) {
        auto h = label(tr(c.en, c.es), "goldFont.fnt", .4f, { c.x, PANEL_H - 11.f }, { 0.f, .5f });
        limitWidth(h, 58.f);
        m_content->addChild(h);
    }

    auto scroll = ScrollLayer::create({ PANEL_W, listH });
    m_content->addChild(scroll);
    float total = std::max(listH, ROW_H * a.parts.size());
    scroll->m_contentLayer->setContentSize({ PANEL_W, total });

    auto worst = a.worstPart();
    for (size_t i = 0; i < a.parts.size(); i++) {
        auto const& p = a.parts[i];
        float y = total - ROW_H * (i + 1);
        auto row = CCNode::create();
        row->setPosition({ 0, y });
        row->setContentSize({ PANEL_W, ROW_H });
        scroll->m_contentLayer->addChild(row);

        bool isWorst = worst && *worst == i;
        bool isChoke = std::find(a.chokes.begin(), a.chokes.end(), i) != a.chokes.end();
        if (i % 2 == 0) row->addChild(rect({ 255, 255, 255 }, 14, 4.f, 0.f, PANEL_W - 8.f, ROW_H));
        if (isWorst) row->addChild(rect({ 255, 60, 60 }, 45, 4.f, 0.f, PANEL_W - 8.f, ROW_H));
        else if (isChoke) row->addChild(rect(NOCLIP_COLOR, 35, 4.f, 0.f, PANEL_W - 8.f, ROW_H));

        auto name = label(fmt::format("{}-{}%", p.from, p.to), "bigFont.fnt", .32f, { 10.f, ROW_H / 2 }, { 0.f, .5f });
        if (isWorst) name->setColor({ 255, 130, 120 });
        row->addChild(name);

        float barX = 74.f, barW = 80.f, barH = 8.f;
        row->addChild(rect({ 0, 0, 0 }, 110, barX, ROW_H / 2 - barH / 2, barW, barH));
        if (p.entered > 0) {
            row->addChild(rect(rateColor(p.rate()), 255, barX, ROW_H / 2 - barH / 2, barW * p.rate(), barH));
            auto rateLbl = label(pct(p.rate(), 0), "bigFont.fnt", .28f, { barX + barW + 5.f, ROW_H / 2 }, { 0.f, .5f });
            rateLbl->setColor(rateColor(p.rate()));
            row->addChild(rateLbl);
        }

        auto contextLabel = [&](int passed, int entered, float x) {
            auto lbl = label(entered > 0 ? fmt::format("{} ({})", rateText(passed, entered), entered) : "-",
                "chatFont.fnt", .5f, { x, ROW_H / 2 }, { 0.f, .5f });
            if (entered > 0) lbl->setColor(rateColor(float(passed) / float(entered)));
            else lbl->setOpacity(120);
            row->addChild(lbl);
        };
        contextLabel(p.freshPassed, p.freshEntered, 200.f);
        contextLabel(p.deepPassed, p.deepEntered, 260.f);

        auto deaths = label(std::to_string(p.deaths), "chatFont.fnt", .55f, { 322.f, ROW_H / 2 }, { 0.f, .5f });
        if (a.deadliestPart && *a.deadliestPart == i) deaths->setColor({ 255, 110, 100 });
        row->addChild(deaths);

        std::string tag;
        ccColor3B tagColor = GRAY;
        if (isWorst) { tag = tr("WORST", "PEOR"); tagColor = { 255, 90, 80 }; }
        else if (isChoke) { tag = "CHOKE"; tagColor = NOCLIP_COLOR; }
        else switch (p.status) {
            case PartStatus::Weak: tag = tr("WEAK", "DEBIL"); tagColor = { 255, 110, 90 }; break;
            case PartStatus::Shaky: tag = tr("SHAKY", "FLOJA"); tagColor = { 255, 190, 90 }; break;
            case PartStatus::Consistent: tag = "OK"; tagColor = GREEN; break;
            case PartStatus::Uncertain: tag = "?"; break;
            default: tag = "-"; break;
        }
        auto tagLbl = label(tag, "bigFont.fnt", .26f, { 374.f, ROW_H / 2 }, { 0.f, .5f });
        limitWidth(tagLbl, PANEL_W - 380.f);
        tagLbl->setColor(tagColor);
        row->addChild(tagLbl);
    }
    scroll->scrollToTop();
}

// ---------------------------------------------------------------------------
// Runs
// ---------------------------------------------------------------------------

void StatsPopup::buildRuns(std::vector<Run> const& runs) {
    constexpr float ROW_H = 20.f;
    float listH = PANEL_H - 24.f;
    int start = currentStart();
    auto groups = groupRuns(runs, m_runsAllStarts ? std::nullopt : std::optional<int>(start));

    m_content->addChild(label("Run", "goldFont.fnt", .4f, { 140.f, PANEL_H - 12.f }, { 0.f, .5f }));
    m_content->addChild(label(tr("Times", "Veces"), "goldFont.fnt", .4f, { 232.f, PANEL_H - 12.f }, { 0.f, .5f }));
    auto ncHeader = label("Noclip", "goldFont.fnt", .4f, { 290.f, PANEL_H - 12.f }, { 0.f, .5f });
    ncHeader->setColor(NOCLIP_COLOR);
    m_content->addChild(ncHeader);

    auto menu = menuIn(m_content, { PANEL_W, PANEL_H });
    auto scopeSpr = ButtonSprite::create(
        (m_runsAllStarts ? tr("All starts", "Todas") : fmt::format(fmt::runtime(tr("From {}%", "Desde {}%")), start)).c_str(),
        80, true, "bigFont.fnt", "GJ_button_04.png", 24.f, .5f);
    scopeSpr->setScale(.55f);
    auto scopeBtn = CCMenuItemSpriteExtra::create(scopeSpr, this, menu_selector(StatsPopup::onRunsScope));
    scopeBtn->setPosition({ PANEL_W - 34.f, PANEL_H - 12.f });
    menu->addChild(scopeBtn);

    if (groups.empty()) {
        m_content->addChild(label(tr("No runs from this start.", "No hay runs desde este start."),
            "bigFont.fnt", .4f, { PANEL_W / 2, listH / 2 }));
        return;
    }

    auto scroll = ScrollLayer::create({ PANEL_W, listH });
    m_content->addChild(scroll);
    float total = std::max(listH, ROW_H * groups.size());
    scroll->m_contentLayer->setContentSize({ PANEL_W, total });

    for (size_t i = 0; i < groups.size(); i++) {
        auto const& g = groups[i];
        float y = total - ROW_H * (i + 1);
        auto row = CCNode::create();
        row->setPosition({ 0, y });
        row->setContentSize({ PANEL_W, ROW_H });
        scroll->m_contentLayer->addChild(row);
        if (i % 2 == 0) row->addChild(rect({ 255, 255, 255 }, 14, 4.f, 0.f, PANEL_W - 8.f, ROW_H));

        bool completed = g.end >= COMPLETE;
        ccColor3B color = completed ? GREEN : g.normal == 0 ? NOCLIP_COLOR : ccColor3B{ 255, 175, 90 };

        // where the run happened in the level (0-100%)
        float barX = 10.f, barW = 120.f, barH = 7.f;
        row->addChild(rect({ 0, 0, 0 }, 110, barX, ROW_H / 2 - barH / 2, barW, barH));
        float x0 = barX + barW * g.start / 100.f;
        float x1 = barX + barW * g.end / 100.f;
        row->addChild(rect(color, 255, x0, ROW_H / 2 - barH / 2, std::max(1.5f, x1 - x0), barH));

        auto name = label(runName(g.start, g.end), "bigFont.fnt", .36f, { 140.f, ROW_H / 2 }, { 0.f, .5f });
        name->setColor(completed ? GREEN : ccColor3B{ 255, 255, 255 });
        row->addChild(name);

        auto normal = label(g.normal ? fmt::format("x{}", g.normal) : "-", "bigFont.fnt", .32f, { 232.f, ROW_H / 2 }, { 0.f, .5f });
        if (!g.normal) normal->setOpacity(110);
        row->addChild(normal);

        auto noclip = label(g.noclip ? fmt::format("x{}", g.noclip) : "-", "bigFont.fnt", .32f, { 290.f, ROW_H / 2 }, { 0.f, .5f });
        if (g.noclip) noclip->setColor(NOCLIP_COLOR);
        else noclip->setOpacity(110);
        row->addChild(noclip);
    }
    scroll->scrollToTop();
}

// ---------------------------------------------------------------------------
// Starts
// ---------------------------------------------------------------------------

void StatsPopup::buildStarts(std::vector<Run> const& runs) {
    constexpr float ROW_H = 24.f;
    float listH = PANEL_H - 22.f;
    auto settings = Settings::get();

    struct Col { float x; char const* en; char const* es; };
    Col cols[] = { { 12, "Start", "Inicio" }, { 100, "Runs", "Runs" }, { 160, "100%s", "100%s" }, { 222, "Pass rate", "Pass rate" }, { 300, "Streak", "Racha" } };
    for (auto const& c : cols) {
        m_content->addChild(label(tr(c.en, c.es), "goldFont.fnt", .45f, { c.x, PANEL_H - 11.f }, { 0.f, .5f }));
    }

    auto scroll = ScrollLayer::create({ PANEL_W, listH });
    m_content->addChild(scroll);
    float total = std::max(listH, ROW_H * m_starts.size());
    scroll->m_contentLayer->setContentSize({ PANEL_W, total });

    for (size_t i = 0; i < m_starts.size(); i++) {
        int start = m_starts[i];
        auto a = analyze(runs, start, settings);
        float y = total - ROW_H * (i + 1);

        auto row = CCNode::create();
        row->setPosition({ 0, y });
        row->setContentSize({ PANEL_W, ROW_H });
        scroll->m_contentLayer->addChild(row);

        if (i == m_startIdx) row->addChild(rect({ 90, 200, 255 }, 50, 4.f, 1.f, PANEL_W - 8.f, ROW_H - 2.f));
        else if (i % 2 == 0) row->addChild(rect({ 255, 255, 255 }, 14, 4.f, 1.f, PANEL_W - 8.f, ROW_H - 2.f));

        row->addChild(label(fmt::format("{}%", start), "bigFont.fnt", .4f, { 12.f, ROW_H / 2 }, { 0.f, .5f }));
        row->addChild(label(std::to_string(a.runs), "chatFont.fnt", .65f, { 100.f, ROW_H / 2 }, { 0.f, .5f }));
        auto comps = label(std::to_string(a.completions), "bigFont.fnt", .4f, { 160.f, ROW_H / 2 }, { 0.f, .5f });
        comps->setColor(a.completions ? GREEN : ccColor3B{ 200, 200, 200 });
        row->addChild(comps);
        auto rate = label(a.runs ? pct(a.passRate) : "-", "bigFont.fnt", .35f, { 222.f, ROW_H / 2 }, { 0.f, .5f });
        if (a.runs) rate->setColor(rateColor(a.passRate));
        row->addChild(rate);
        row->addChild(label(fmt::format("{} / {}", a.currentStreak, a.bestStreak), "chatFont.fnt", .6f, { 300.f, ROW_H / 2 }, { 0.f, .5f }));

        auto menu = menuIn(row, { PANEL_W, ROW_H });
        auto spr = ButtonSprite::create(tr("View", "Ver").c_str(), 40, true, "bigFont.fnt", "GJ_button_01.png", 22.f, .5f);
        spr->setScale(.55f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(StatsPopup::onViewStart));
        btn->setTag(static_cast<int>(i));
        btn->setPosition({ PANEL_W - 30.f, ROW_H / 2 });
        menu->addChild(btn);
    }
    scroll->scrollToTop();
}

// ---------------------------------------------------------------------------
// Deaths histogram
// ---------------------------------------------------------------------------

void StatsPopup::buildDeaths(Analysis const& a) {
    float chartX = 14.f, chartY = 20.f, chartW = PANEL_W - 28.f, chartH = PANEL_H - 66.f;

    std::vector<std::pair<int, int>> sorted(a.deaths.begin(), a.deaths.end());
    std::sort(sorted.begin(), sorted.end(), [](auto const& x, auto const& y) { return x.second > y.second; });
    std::string top = tr("Most deaths: ", "Mas muertes: ");
    for (size_t i = 0; i < std::min<size_t>(5, sorted.size()); i++) {
        top += fmt::format("{}{}% (x{})", i ? "  " : "", sorted[i].first, sorted[i].second);
    }
    if (sorted.empty()) top = tr("No deaths recorded from this start.", "No hay muertes registradas desde este start.");
    auto topLbl = label(top, "chatFont.fnt", .6f, { PANEL_W / 2, PANEL_H - 14.f });
    limitWidth(topLbl, PANEL_W - 20.f);
    topLbl->setColor({ 255, 170, 160 });
    m_content->addChild(topLbl);

    m_content->addChild(rect({ 0, 0, 0 }, 70, chartX, chartY, chartW, chartH));
    int maxDeaths = 1;
    for (auto const& [_, c] : a.deaths) maxDeaths = std::max(maxDeaths, c);
    int span = std::max(1, COMPLETE - a.start);
    float barW = chartW / float(span);
    for (auto const& [p, c] : a.deaths) {
        float t = float(c) / float(maxDeaths);
        float x = chartX + barW * float(p - a.start);
        ccColor3B col = { 255, static_cast<GLubyte>(200 - 150 * t), static_cast<GLubyte>(90 - 60 * t) };
        m_content->addChild(rect(col, 240, x, chartY, std::max(1.f, barW - (barW > 3.f ? 1.f : 0.f)), std::max(1.5f, chartH * t)));
    }
    for (int p = (a.start / 10) * 10; p <= COMPLETE; p += 10) {
        if (p < a.start) continue;
        float x = chartX + barW * float(p - a.start);
        m_content->addChild(rect({ 255, 255, 255 }, 40, x, chartY, 1.f, chartH));
        m_content->addChild(label(fmt::format("{}", p), "chatFont.fnt", .45f, { x, chartY - 7.f }));
    }
    auto maxLbl = label(fmt::format("max {}", maxDeaths), "chatFont.fnt", .45f, { chartX + 2.f, chartY + chartH - 6.f }, { 0.f, .5f });
    maxLbl->setOpacity(160);
    m_content->addChild(maxLbl);
}

// ---------------------------------------------------------------------------
// Tips
// ---------------------------------------------------------------------------

void StatsPopup::buildTips() {
    auto text = senrate::buildTips(Manager::get().level(m_level), currentStart(), m_window, Settings::get());
    auto area = MDTextArea::create(text, { PANEL_W - 10.f, PANEL_H - 8.f });
    area->ignoreAnchorPointForPosition(false);
    area->setAnchorPoint({ .5f, .5f });
    area->setPosition({ PANEL_W / 2, PANEL_H / 2 });
    m_content->addChild(area);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void StatsPopup::onPrevStart(CCObject*) {
    if (m_starts.empty()) return;
    m_startIdx = m_startIdx == 0 ? m_starts.size() - 1 : m_startIdx - 1;
    refresh();
}

void StatsPopup::onNextStart(CCObject*) {
    if (m_starts.empty()) return;
    m_startIdx = (m_startIdx + 1) % m_starts.size();
    refresh();
}

void StatsPopup::onWindow(CCObject* sender) {
    m_window = static_cast<Window>(static_cast<CCNode*>(sender)->getTag());
    Mod::get()->setSavedValue<int>("last-window", static_cast<int>(m_window));
    refresh();
}

void StatsPopup::onTab(CCObject* sender) {
    m_tab = static_cast<Tab>(static_cast<CCNode*>(sender)->getTag());
    Mod::get()->setSavedValue<int>("last-tab", static_cast<int>(m_tab));
    refresh();
}

void StatsPopup::onViewStart(CCObject* sender) {
    m_startIdx = static_cast<size_t>(static_cast<CCNode*>(sender)->getTag());
    m_tab = Tab::Overview;
    refresh();
}

void StatsPopup::onRunsScope(CCObject*) {
    m_runsAllStarts = !m_runsAllStarts;
    Mod::get()->setSavedValue<bool>("runs-all-starts", m_runsAllStarts);
    refresh();
}

void StatsPopup::onSettings(CCObject*) {
    openSettingsPopup(Mod::get());
}

void StatsPopup::onDeleteSession(CCObject*) {
    auto const& stats = Manager::get().level(m_level);
    auto session = currentSession();
    auto count = std::count_if(stats.runs.begin(), stats.runs.end(), [&](Run const& r) { return r.session == session; });
    if (count == 0) {
        Notification::create(tr("No runs this session on this level.", "No hay runs de esta sesion en este nivel."), NotificationIcon::Info)->show();
        return;
    }
    Ref<StatsPopup> self = this;
    createQuickPopup(
        tr("Delete session", "Borrar sesion").c_str(),
        fmt::format(fmt::runtime(tr(
            "Delete the <cy>{}</c> runs you did on this level <cy>this session</c>? They're also removed from the all-time stats.",
            "Borrar las <cy>{}</c> runs que hiciste en este nivel <cy>en esta sesion</c>? Tambien se quitan de las estadisticas totales."
        )), count),
        tr("Cancel", "Cancelar").c_str(), tr("Delete", "Borrar").c_str(),
        [self](FLAlertLayer*, bool confirm) {
            if (!confirm) return;
            Manager::get().deleteSession(self->m_level);
            self->collectStarts(self->currentStart());
            self->refresh();
        }
    );
}

void StatsPopup::onDeleteAll(CCObject*) {
    auto count = Manager::get().level(m_level).runs.size();
    Ref<StatsPopup> self = this;
    createQuickPopup(
        tr("Delete everything", "Borrar todo").c_str(),
        fmt::format(fmt::runtime(tr(
            "Delete the <cr>whole history</c> of this level ({} runs)? This can't be undone.",
            "Borrar <cr>todo el historial</c> de este nivel ({} runs)? No se puede deshacer."
        )), count),
        tr("Cancel", "Cancelar").c_str(), tr("Delete", "Borrar").c_str(),
        [self](FLAlertLayer*, bool confirm) {
            if (!confirm) return;
            Manager::get().reset(self->m_level);
            self->collectStarts(-1);
            self->refresh();
        }
    );
}

}
