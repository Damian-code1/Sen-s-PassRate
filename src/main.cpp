#include "Data.hpp"
#include "StatsPopup.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GJListLayer.hpp>
#include <Geode/modify/AppDelegate.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/NineSlice.hpp>
#include <Geode/ui/Notification.hpp>
#include <cmath>

using namespace geode::prelude;
using namespace senrate;

namespace {
    ccColor3B const GREEN = { 120, 255, 140 };
    ccColor3B const GRAY = { 190, 190, 210 };
    ccColor3B const WHITE = { 255, 255, 255 };

    bool setting(char const* key) {
        return Mod::get()->getSettingValue<bool>(key);
    }

    CCMenuItemSpriteExtra* makeButton(CCObject* target, SEL_MenuHandler callback, float scale) {
        auto spr = CircleButtonSprite::createWithSprite("senrate_btn.png"_spr, .95f, CircleBaseColor::DarkPurple, CircleBaseSize::Medium);
        spr->setScale(scale);
        auto btn = CCMenuItemSpriteExtra::create(spr, target, callback);
        btn->setID("senrate-button"_spr);
        return btn;
    }

    void notify(std::string const& text, NotificationIcon icon) {
        Notification::create(text, icon, 2.5f)->show();
    }

    // Noclip hits closer than this (in game seconds) are the same hazard
    constexpr double CONTACT_GRACE = 0.2;

    // Game time in seconds, used to tell apart separate noclip hits
    double g_gameClock = 0.0;

    struct Totals {
        int runs = 0;
        int completions = 0;
        int streak = 0;
    };

    // Counted all-time runs that began at `start`
    Totals totalsFrom(GJGameLevel* level, int start) {
        Totals t;
        for (auto const& r : selectRuns(Manager::get().level(level), Window::AllTime, Settings::get())) {
            if (r.start != start || r.continuation()) continue;
            t.runs++;
            if (r.completed()) {
                t.completions++;
                t.streak++;
            }
            else t.streak = 0;
        }
        return t;
    }
}

// ---------------------------------------------------------------------------
// Run tracking
// ---------------------------------------------------------------------------

class $modify(SRGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        if (dt > 0.f && dt < 1.f) g_gameClock += dt;
        GJBaseGameLayer::update(dt);
    }
};

class $modify(SRPlayLayer, PlayLayer) {
    struct Fields {
        int origin = -1;            // where the attempt began (-1 = not tracked)
        int segStart = -1;          // where the current run began
        bool active = false;        // a run is being tracked right now
        bool continuation = false;  // the current run began after a noclip hit
        bool noclipHit = false;     // noclip saved the player this attempt
        double lastHitClock = -1e9; // game time of the last noclip hit
        std::vector<Run> attempt;   // runs recorded during this attempt
        // only tell the player once per level visit
        bool noclipNotified = false;
    };

    static void onModify(auto& self) {
        // run before noclip hacks so we can tell when a hit didn't kill the player
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::First);
    }

    bool shouldTrack() {
        if (!m_level || m_level->isPlatformer()) return false;
        if (m_isPracticeMode && !setting("track-practice")) return false;
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        f->active = shouldTrack();
        f->origin = f->segStart = f->active ? std::clamp(this->getCurrentPercentInt(), 0, 99) : -1;
        f->continuation = false;
        f->noclipHit = false;
        f->lastHitClock = -1e9;
        f->attempt.clear();
        runState().currentStart = f->origin;
    }

    // Ends the current run at `end` and records it; returns whether it counted
    bool finishRun(int end, bool noclip) {
        auto f = m_fields.self();
        if (!f->active) return false;
        f->active = false;
        // e.g. switched to practice mode in the middle of the run
        if (!this->shouldTrack()) return false;

        Run run;
        run.start = static_cast<uint8_t>(f->segStart);
        run.end = static_cast<uint8_t>(std::clamp(end, f->segStart, COMPLETE));
        run.origin = static_cast<uint8_t>(f->origin);
        run.flags = (noclip || f->continuation ? RUN_NOCLIP : 0)
            | (f->continuation ? RUN_CONTINUATION : 0)
            | (m_isPracticeMode ? RUN_PRACTICE : 0);
        Manager::get().addRun(m_level, run);
        f->attempt.push_back(run);
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        bool wasDead = m_playerDied;
        PlayLayer::destroyPlayer(player, object);
        if (object == m_anticheatSpike || wasDead) return;

        auto f = m_fields.self();
        if (f->origin < 0) return; // this attempt isn't tracked
        int percent = std::clamp(this->getCurrentPercentInt(), 0, 99);

        if (m_playerDied) {
            this->finishRun(percent, false);
            return;
        }

        // The hit didn't kill the player: another mod's noclip swallowed it
        double now = g_gameClock;
        bool sameContact = now - f->lastHitClock <= CONTACT_GRACE;
        f->lastHitClock = now;
        auto mode = Settings::get().noclip;

        if (sameContact) {
            // still touching the same hazard: the next run begins once it's over
            if (f->active && f->continuation) f->segStart = std::max(f->segStart, percent);
            return;
        }

        f->noclipHit = true;
        if (!f->noclipNotified) {
            f->noclipNotified = true;
            switch (mode) {
                case NoclipMode::CountAll:
                    notify(fmt::format(fmt::runtime(tr(
                        "Noclip: every hit ends a run and starts a new one ({}-{}, {}-...)",
                        "Noclip: cada golpe corta la run y arranca otra ({}-{}, {}-...)"
                    )), f->segStart, percent, percent), NotificationIcon::Info);
                    break;
                case NoclipMode::FirstHit:
                    notify(fmt::format(fmt::runtime(tr(
                        "Noclip: this run counts as {}-{} (your first hit)",
                        "Noclip: esta run cuenta como {}-{} (tu primer golpe)"
                    )), f->segStart, percent), NotificationIcon::Info);
                    break;
                default:
                    notify(tr("Noclip detected: these runs are NOT counted (see settings)",
                              "Noclip detectado: estas runs NO cuentan (ver ajustes)"), NotificationIcon::Warning);
                    break;
            }
        }

        switch (mode) {
            case NoclipMode::Ignore:
                f->active = false;
                break;
            case NoclipMode::FirstHit:
                this->finishRun(percent, true);
                break;
            case NoclipMode::CountAll:
                // the run ends where it would have without noclip...
                this->finishRun(percent, true);
                // ...and a new one starts right here
                if (this->shouldTrack()) {
                    f->segStart = percent;
                    f->continuation = true;
                    f->active = true;
                }
                break;
        }
    }

    void levelComplete() {
        auto f = m_fields.self();
        auto& rs = runState();
        int segStart = f->segStart;
        bool continuation = f->continuation;
        rs.completionRecorded = this->finishRun(COMPLETE, false);
        rs.lastOrigin = f->origin;
        rs.lastCompletionStart = segStart;
        rs.lastAttemptNoclip = f->noclipHit;
        rs.lastAttempt = f->attempt;

        if (rs.completionRecorded && !continuation && setting("completion-toast")) {
            auto t = totalsFrom(m_level, segStart);
            auto icon = CCSprite::create("senrate_logo.png"_spr);
            if (icon) icon->setScale(28.f / icon->getContentSize().width);
            Notification::create(
                fmt::format(fmt::runtime(tr("100% from {}%: x{} | pass rate {}", "100% desde {}%: x{} | pass rate {}")),
                    segStart, t.completions, pct(float(t.completions) / float(std::max(1, t.runs)))),
                icon, 3.f
            )->show();
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        Manager::get().saveAll();
        runState().currentStart = -1;
        PlayLayer::onQuit();
    }
};

class $modify(AppDelegate) {
    void trySaveGame(bool force) {
        Manager::get().saveAll();
        AppDelegate::trySaveGame(force);
    }
};

// ---------------------------------------------------------------------------
// Pause menu
// ---------------------------------------------------------------------------

class $modify(SRPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!setting("pause-button")) return;
        auto menu = this->getChildByID("right-button-menu");
        if (!menu) return;
        menu->addChild(makeButton(this, menu_selector(SRPauseLayer::onSenRate), .62f));
        menu->updateLayout();
    }

    void onSenRate(CCObject*) {
        if (auto pl = PlayLayer::get()) StatsPopup::open(pl->m_level, runState().currentStart);
    }
};

// ---------------------------------------------------------------------------
// Level pages
// ---------------------------------------------------------------------------

class $modify(SRLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        if (!setting("level-button")) return true;
        if (auto menu = this->getChildByID("left-side-menu")) {
            menu->addChild(makeButton(this, menu_selector(SRLevelInfoLayer::onSenRate), .8f));
            menu->updateLayout();
        }
        return true;
    }

    void onSenRate(CCObject*) {
        StatsPopup::open(m_level, -1);
    }
};

class $modify(SREditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;
        if (!setting("level-button")) return true;
        if (auto menu = this->getChildByID("level-actions-menu")) {
            menu->addChild(makeButton(this, menu_selector(SREditLevelLayer::onSenRate), .8f));
            menu->updateLayout();
        }
        return true;
    }

    void onSenRate(CCObject*) {
        StatsPopup::open(m_level, -1);
    }
};

// ---------------------------------------------------------------------------
// Level complete screen
// ---------------------------------------------------------------------------

class $modify(SREndLevelLayer, EndLevelLayer) {
    void customSetup() {
        EndLevelLayer::customSetup();
        if (!setting("end-screen") || !m_playLayer) return;
        if (runState().lastOrigin < 0) return; // the attempt wasn't tracked (e.g. practice)

        bool panel = this->addRunsPanel();
        auto text = this->headline(!panel);
        if (!text.empty()) this->addHeadline(text);
        handleTouchPriority(this);
    }

    std::string headline(bool listAttempt) {
        auto& rs = runState();
        if (rs.lastAttemptNoclip) {
            std::string text;
            switch (Settings::get().noclip) {
                case NoclipMode::CountAll:
                    text = fmt::format(fmt::runtime(tr("Noclip: {} runs counted this attempt", "Noclip: {} runs contadas en este intento")), rs.lastAttempt.size());
                    break;
                case NoclipMode::FirstHit:
                    text = rs.lastAttempt.empty()
                        ? tr("Noclip: this attempt wasn't counted", "Noclip: este intento no se conto")
                        : fmt::format(fmt::runtime(tr("Noclip: counted as {} (first hit)", "Noclip: conto como {} (primer golpe)")),
                            runName(rs.lastAttempt.front().start, rs.lastAttempt.front().end));
                    break;
                default:
                    text = tr("Noclip: this attempt wasn't counted", "Noclip: este intento no se conto");
                    break;
            }
            if (listAttempt && rs.lastAttempt.size() > 1) {
                text += " |";
                for (size_t i = 0; i < std::min<size_t>(rs.lastAttempt.size(), 6); i++) {
                    text += " " + runName(rs.lastAttempt[i].start, rs.lastAttempt[i].end);
                }
                if (rs.lastAttempt.size() > 6) text += " ...";
            }
            return text;
        }

        if (rs.completionRecorded) {
            auto t = totalsFrom(m_playLayer->m_level, rs.lastCompletionStart);
            float rate = float(t.completions) / float(std::max(1, t.runs));
            std::string text = rs.lastCompletionStart == 0
                ? fmt::format(fmt::runtime(tr("Full 100% x{}  |  pass rate {} ({}/{})", "100% completo x{}  |  pass rate {} ({}/{})")),
                    t.completions, pct(rate), t.completions, t.runs)
                : fmt::format(fmt::runtime(tr("100% from {}%: x{}  |  pass rate {} ({}/{})", "100% desde {}%: x{}  |  pass rate {} ({}/{})")),
                    rs.lastCompletionStart, t.completions, pct(rate), t.completions, t.runs);
            if (t.streak > 1) text += fmt::format(fmt::runtime(tr("  |  streak {}", "  |  racha {}")), t.streak);
            return text;
        }
        return "";
    }

    // One-line summary at the bottom of the screen
    void addHeadline(std::string const& text) {
        auto win = CCDirector::get()->getWinSize();
        auto node = CCNode::create();
        node->setID("senrate-summary"_spr);
        node->setPosition({ win.width / 2, 19.f });
        m_mainLayer->addChild(node, 20);

        auto lbl = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
        lbl->setScale(.38f);
        float lblW = lbl->getContentSize().width * lbl->getScale();
        if (lblW > 360.f) {
            lbl->setScale(lbl->getScale() * 360.f / lblW);
            lblW = 360.f;
        }
        lbl->setColor(runState().lastAttemptNoclip ? NOCLIP_COLOR : GREEN);

        float pillW = lblW + 50.f;
        auto bg = NineSlice::create("square02_001.png");
        bg->setContentSize({ pillW / .5f, 26.f / .5f });
        bg->setScale(.5f);
        bg->setOpacity(170);
        node->addChild(bg);

        lbl->setPosition({ 14.f, 0.f });
        node->addChild(lbl);

        auto menu = CCMenu::create();
        menu->setPosition({ 0, 0 });
        node->addChild(menu);
        auto btn = makeButton(this, menu_selector(SREndLevelLayer::onSenRate), .42f);
        btn->setPosition({ -pillW / 2 + 16.f, 0.f });
        menu->addChild(btn);
    }

    // The runs of this attempt and this session, beside the end screen panel
    bool addRunsPanel() {
        auto& rs = runState();
        auto win = CCDirector::get()->getWinSize();
        auto sessionRuns = selectRuns(Manager::get().level(m_playLayer->m_level), Window::Session, Settings::get());
        auto groups = groupRuns(sessionRuns);
        if (groups.empty() && rs.lastAttempt.empty()) return false;

        // Find the big background panel of the end screen
        CCRect panel(win.width / 2 - 190.f, 25.f, 380.f, win.height - 50.f);
        float bestArea = 0.f;
        if (auto children = m_mainLayer->getChildren()) {
            for (auto child : CCArrayExt<CCNode*>(children)) {
                if (!child->isVisible()) continue;
                bool background = typeinfo_cast<cocos2d::extension::CCScale9Sprite*>(child)
                    || typeinfo_cast<NineSlice*>(child) || typeinfo_cast<GJListLayer*>(child);
                if (!background) continue;
                auto box = child->boundingBox();
                float area = box.size.width * box.size.height;
                if (area > bestArea && box.size.width < win.width * .95f) {
                    bestArea = area;
                    panel = box;
                }
            }
        }

        // Pick the side away from the end screen's side buttons
        float leftSpace = panel.getMinX();
        float rightSpace = win.width - panel.getMaxX();
        bool right = rightSpace >= leftSpace;
        if (m_sideMenu && m_sideMenu->getChildrenCount() > 0) {
            float sum = 0.f;
            int n = 0;
            for (auto child : CCArrayExt<CCNode*>(m_sideMenu->getChildren())) {
                if (!child->isVisible()) continue;
                sum += m_sideMenu->convertToWorldSpace(child->getPosition()).x;
                n++;
            }
            if (n > 0) right = sum / n < win.width / 2;
        }
        constexpr float MIN_WIDTH = 78.f;
        float space = right ? rightSpace : leftSpace;
        if (space < MIN_WIDTH + 8.f) {
            right = !right;
            space = right ? rightSpace : leftSpace;
            if (space < MIN_WIDTH + 8.f) return false;
        }
        float width = std::min(124.f, space - 8.f);
        float left = right ? panel.getMaxX() + (space - width) / 2 : (space - width) / 2;

        constexpr float PAD = 6.f, TITLE_H = 18.f, HEADER_H = 13.f, LINE_H = 11.f, BUTTON_H = 24.f;
        float maxHeight = std::max(90.f, panel.size.height - 10.f);

        struct Line { std::string text; ccColor3B color; bool header; };
        std::vector<Line> lines;
        float used = 0.f;
        auto header = [&](std::string text) {
            lines.push_back({ std::move(text), GRAY, true });
            used += HEADER_H;
        };
        auto line = [&](std::string text, ccColor3B color) {
            lines.push_back({ std::move(text), color, false });
            used += LINE_H;
        };

        if (!rs.lastAttempt.empty()) {
            header(tr("This attempt", "Este intento"));
            size_t shown = std::min<size_t>(rs.lastAttempt.size(), 6);
            for (size_t i = 0; i < shown; i++) {
                auto const& r = rs.lastAttempt[i];
                line(runName(r.start, r.end) + (r.noclip() ? " nc" : ""), r.completed() ? GREEN : r.noclip() ? NOCLIP_COLOR : WHITE);
            }
            if (rs.lastAttempt.size() > shown) line(fmt::format("+{}", rs.lastAttempt.size() - shown), GRAY);
        }
        if (!groups.empty()) {
            header(tr("This session", "Esta sesion"));
            float room = maxHeight - 2 * PAD - TITLE_H - BUTTON_H - used;
            size_t fit = static_cast<size_t>(std::max(1.f, std::floor(room / LINE_H)));
            for (size_t i = 0; i < groups.size(); i++) {
                if (i + 1 == fit && groups.size() > fit) {
                    line(fmt::format(fmt::runtime(tr("+{} more", "+{} mas")), groups.size() - i), GRAY);
                    break;
                }
                auto const& g = groups[i];
                std::string text = runName(g.start, g.end);
                if (g.total() > 1) text += fmt::format(" x{}", g.total());
                if (g.noclip && !g.normal) text += " nc";
                else if (g.noclip) text += fmt::format(" ({} nc)", g.noclip);
                line(text, g.end >= COMPLETE ? GREEN : g.normal == 0 ? NOCLIP_COLOR : WHITE);
            }
        }

        float height = 2 * PAD + TITLE_H + used + BUTTON_H;
        auto node = CCNode::create();
        node->setID("senrate-runs-panel"_spr);
        node->setContentSize({ width, height });
        node->setPosition({ left, panel.getMidY() - height / 2 });
        m_mainLayer->addChild(node, 30);

        auto bg = NineSlice::create("square02_001.png");
        bg->setScale(.5f);
        bg->setContentSize({ width / .5f, height / .5f });
        bg->setAnchorPoint({ 0, 0 });
        bg->setOpacity(165);
        node->addChild(bg);

        float y = height - PAD;
        auto title = CCLabelBMFont::create("SenRate", "goldFont.fnt");
        title->setScale(.45f);
        title->setPosition({ width / 2, y - TITLE_H / 2 });
        node->addChild(title);
        y -= TITLE_H;

        for (auto const& l : lines) {
            float h = l.header ? HEADER_H : LINE_H;
            auto lbl = CCLabelBMFont::create(l.text.c_str(), l.header ? "chatFont.fnt" : "bigFont.fnt");
            lbl->setScale(l.header ? .5f : .28f);
            lbl->setAnchorPoint({ 0.f, .5f });
            lbl->setPosition({ PAD + (l.header ? 0.f : 3.f), y - h / 2 });
            lbl->setColor(l.color);
            float maxW = width - 2 * PAD - 3.f;
            float w = lbl->getContentSize().width * lbl->getScale();
            if (w > maxW) lbl->setScale(lbl->getScale() * maxW / w);
            node->addChild(lbl);
            y -= h;
        }

        auto menu = CCMenu::create();
        menu->setPosition({ 0, 0 });
        node->addChild(menu);
        auto spr = ButtonSprite::create(tr("See all", "Ver todo").c_str(), static_cast<int>((width - 12.f) / .6f), true,
            "bigFont.fnt", "GJ_button_01.png", 28.f, .55f);
        spr->setScale(.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(SREndLevelLayer::onSeeRuns));
        btn->setPosition({ width / 2, PAD + BUTTON_H / 2 - 2.f });
        menu->addChild(btn);
        return true;
    }

    void onSenRate(CCObject*) {
        if (m_playLayer) StatsPopup::open(m_playLayer->m_level, runState().lastOrigin);
    }

    void onSeeRuns(CCObject*) {
        if (m_playLayer) StatsPopup::open(m_playLayer->m_level, runState().lastOrigin, Tab::Runs, Window::Session);
    }
};
