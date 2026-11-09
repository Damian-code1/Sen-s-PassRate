#pragma once

#include <Geode/Geode.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace senrate {
constexpr int COMPLETE = 100;

enum RunFlag : uint8_t {
    RUN_NOCLIP = 1 << 0,
    RUN_CONTINUATION = 1 << 1,
    RUN_PRACTICE = 1 << 2,
};

struct Run {
    uint8_t start = 0;
    uint8_t end = 0;
    uint8_t origin = 0;
    uint8_t flags = 0;
    uint32_t session = 0;

    bool completed() const { return end >= COMPLETE; }
    bool noclip() const { return flags & RUN_NOCLIP; }
    bool continuation() const { return flags & RUN_CONTINUATION; }
};

uint32_t currentSession();

struct LevelStats {
    std::string key;
    std::string name;
    std::vector<Run> runs;
};

enum class Window { AllTime = 0, Session = 1, Recent = 2 };
enum class NoclipMode { CountAll, FirstHit, Ignore };

struct Settings {
    int partSize;
    int recentWindow;
    int minSamples;
    NoclipMode noclip;
    static Settings get();
};

std::vector<Run> selectRuns(LevelStats const& level, Window window, Settings const& settings);

std::vector<int> originsOf(std::vector<Run> const& runs);

enum class PartStatus { NoData, Uncertain, Weak, Shaky, Consistent };

struct PartStats {
    int from = 0;
    int to = 0;
    int entered = 0;
    int passed = 0;
    int deaths = 0;
    int freshEntered = 0;
    int freshPassed = 0;
    int deepEntered = 0;
    int deepPassed = 0;
    float skill = -1.f;
    float pressure = -1.f;
    PartStatus status = PartStatus::NoData;

    float rate() const { return entered > 0 ? float(passed) / float(entered) : 0.f; }
    float freshRate() const { return freshEntered > 0 ? float(freshPassed) / float(freshEntered) : 0.f; }
    float deepRate() const { return deepEntered > 0 ? float(deepPassed) / float(deepEntered) : 0.f; }
};

struct Analysis {
    int start = 0;
    int runs = 0;
    int completions = 0;
    float passRate = 0.f;
    float avgEnd = 0.f;
    float estimatedRate = 0.f;
    bool estimateComplete = false;
    int currentStreak = 0;
    int bestStreak = 0;
    std::vector<PartStats> parts;
    std::map<int, int> deaths;
    float partTarget = 0.9f;
    std::vector<size_t> walls;
    std::vector<size_t> chokes;
    std::optional<size_t> deadliestPart;
    int consistentFrom = -1;
    int masteredFrom = -1;
    int masteredRuns = 0;
    int masteredCompletions = 0;

    std::optional<size_t> worstPart() const {
        return walls.empty() ? std::nullopt : std::optional<size_t>(walls.front());
    }
};

Analysis analyze(std::vector<Run> const& runs, int start, Settings const& settings);

std::string buildTips(LevelStats const& level, int start, Window window, Settings const& settings);

struct RunGroup {
    int start = 0;
    int end = 0;
    int normal = 0;
    int noclip = 0;
    int total() const { return normal + noclip; }
};
std::vector<RunGroup> groupRuns(std::vector<Run> const& runs, std::optional<int> origin = std::nullopt);
std::string runName(int start, int end);

std::string gradeFor(float rate);
cocos2d::ccColor3B rateColor(float rate);
cocos2d::ccColor3B const NOCLIP_COLOR = { 205, 150, 255 };

class Manager {
public:
    static Manager& get();

    static std::string keyFor(GJGameLevel* level);

    LevelStats& level(GJGameLevel* level);
    void addRun(GJGameLevel* level, Run run);

    size_t deleteSession(GJGameLevel* level);

    void reset(GJGameLevel* level);
    void save(std::string const& key);
    void saveAll();

private:
    std::filesystem::path pathFor(std::string const& key) const;
    LevelStats load(std::string const& key) const;

    std::map<std::string, LevelStats> m_levels;
    std::map<std::string, int> m_unsaved;
};

struct RunState {
    int currentStart = -1;

    int lastOrigin = -1;
    bool completionRecorded = false;
    int lastCompletionStart = -1;
    bool lastAttemptNoclip = false;
    std::vector<Run> lastAttempt;
};
RunState& runState();

std::string tr(char const* en, char const* es);
std::string pct(float v01, int decimals = 1);

}
