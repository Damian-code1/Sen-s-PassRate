#pragma once

#include "Data.hpp"
#include <Geode/ui/Popup.hpp>

namespace senrate {

enum class Tab { Overview = 0, Parts, Runs, Starts, Deaths, Tips, Count };

class StatsPopup : public geode::Popup {
public:
    // preferredStart < 0 picks the start position with the most runs;
    // tab / window default to the last ones the player used
    static StatsPopup* create(GJGameLevel* level, int preferredStart, std::optional<Tab> tab, std::optional<Window> window);
    static void open(GJGameLevel* level, int preferredStart,
        std::optional<Tab> tab = std::nullopt, std::optional<Window> window = std::nullopt);

protected:
    ~StatsPopup() override;
    bool init(GJGameLevel* level, int preferredStart, std::optional<Tab> tab, std::optional<Window> window);

    void collectStarts(int preferred);
    int currentStart() const;
    void refresh();
    void refreshHeader();

    void buildOverview(Analysis const& a);
    void buildParts(Analysis const& a);
    void buildRuns(std::vector<Run> const& runs);
    void buildStarts(std::vector<Run> const& runs);
    void buildDeaths(Analysis const& a);
    void buildTips();

    void onPrevStart(cocos2d::CCObject*);
    void onNextStart(cocos2d::CCObject*);
    void onWindow(cocos2d::CCObject* sender);
    void onTab(cocos2d::CCObject* sender);
    void onViewStart(cocos2d::CCObject* sender);
    void onRunsScope(cocos2d::CCObject*);
    void onSettings(cocos2d::CCObject*);
    void onDeleteSession(cocos2d::CCObject*);
    void onDeleteAll(cocos2d::CCObject*);

    GJGameLevel* m_level = nullptr;
    std::vector<int> m_starts;
    size_t m_startIdx = 0;
    Window m_window = Window::AllTime;
    Tab m_tab = Tab::Overview;
    bool m_runsAllStarts = false;

    cocos2d::CCNode* m_content = nullptr;
    cocos2d::CCLabelBMFont* m_startLabel = nullptr;
    cocos2d::CCLabelBMFont* m_summaryLabel = nullptr;
};

}
