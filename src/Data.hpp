#pragma once

#include <Geode/Geode.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace senrate {

// End percent of a run that reached the end of the level.
constexpr int COMPLETE = 100;

enum RunFlag : uint8_t {
    RUN_NOCLIP = 1 << 0,        // played with noclip (ended on a hit, or began after one)
    RUN_CONTINUATION = 1 << 1,  // noclip run that began right after a hit (e.g. the 27-60 after a 25-27)
    RUN_PRACTICE = 1 << 2,      // practice mode
};

struct Run {
    uint8_t start = 0;    // percent the run began at
    uint8_t end = 0;      // percent it ended at (100 = completed)
    uint8_t origin = 0;   // percent the attempt began at (== start unless continuation)
    uint8_t flags = 0;
    uint32_t session = 0; // id of the game session it was played in (0 = imported)

    bool completed() const { return end >= COMPLETE; }
    bool noclip() const { return flags & RUN_NOCLIP; }
    bool continuation() const { return flags & RUN_CONTINUATION; }
};

// Id of the current game session (a session lasts until the game is closed)
uint32_t currentSession();

struct LevelStats {
    std::string key;
    std::string name;
    std::vector<Run> runs; // oldest first
};

// Which runs a view looks at
enum class Window { AllTime = 0, Session = 1, Recent = 2 };
enum class NoclipMode { CountAll, FirstHit, Ignore };

struct Settings {
    int partSize;
    int recentWindow;
    int minSamples;
    NoclipMode noclip;
    static Settings get();
};

// Runs that count in the given window (respecting the noclip setting), oldest first
std::vector<Run> selectRuns(LevelStats const& level, Window window, Settings const& settings);

// Percents where attempts began, sorted
std::vector<int> originsOf(std::vector<Run> const& runs);

enum class PartStatus { NoData, Uncertain, Weak, Shaky, Consistent };

struct PartStats {
    int from = 0;          // inclusive
    int to = 0;            // exclusive (100 = up to the completion)
    int entered = 0;       // runs (from any start <= from) that reached `from`
    int passed = 0;        // of those, runs that got past `to`
    int deaths = 0;        // deaths inside this part
    int freshEntered = 0;  // ...from runs that began close to this part (practice)
    int freshPassed = 0;
    int deepEntered = 0;   // ...from runs that began far before it (long runs)
    int deepPassed = 0;
    float skill = -1.f;    // estimated pass rate when you practice it (-1 = unknown)
    float pressure = -1.f; // estimated pass rate deep into long runs (-1 = unknown)
    PartStatus status = PartStatus::NoData;

    float rate() const { return entered > 0 ? float(passed) / float(entered) : 0.f; }
    float freshRate() const { return freshEntered > 0 ? float(freshPassed) / float(freshEntered) : 0.f; }
    float deepRate() const { return deepEntered > 0 ? float(deepPassed) / float(deepEntered) : 0.f; }
};

struct Analysis {
    int start = 0;
    int runs = 0;                  // runs that began exactly at `start`
    int completions = 0;           // 100%s from `start`
    float passRate = 0.f;          // completions / runs
    float avgEnd = 0.f;            // average percent reached
    float estimatedRate = 0.f;     // product of every part's pass rate (uses all your practice)
    bool estimateComplete = false; // every part had data
    int currentStreak = 0;
    int bestStreak = 0;
    std::vector<PartStats> parts;
    std::map<int, int> deaths;     // death percent -> count, runs from `start`
    float partTarget = 0.9f;       // per-part pass rate needed for a 25% chance of a full run
    std::vector<size_t> walls;     // parts holding you back, worst first
    std::vector<size_t> chokes;    // parts you pass practicing but fail deep into runs
    std::optional<size_t> deadliestPart;
    int consistentFrom = -1;       // every part from here to the end is consistent
    int masteredFrom = -1;         // earliest start pos (after `start`) you complete consistently
    int masteredRuns = 0;
    int masteredCompletions = 0;

    std::optional<size_t> worstPart() const {
        return walls.empty() ? std::nullopt : std::optional<size_t>(walls.front());
    }
};

Analysis analyze(std::vector<Run> const& runs, int start, Settings const& settings);

// Advice text formatted for MDTextArea
std::string buildTips(LevelStats const& level, int start, Window window, Settings const& settings);

// Runs grouped by start-end, best (furthest) first
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
    // Removes this session's runs of the level; returns how many were removed
    size_t deleteSession(GJGameLevel* level);
    // Removes the whole history of the level
    void reset(GJGameLevel* level);
    void save(std::string const& key);
    void saveAll();

private:
    std::filesystem::path pathFor(std::string const& key) const;
    LevelStats load(std::string const& key) const;

    std::map<std::string, LevelStats> m_levels;
    std::map<std::string, int> m_unsaved;
};

// Shared state between the PlayLayer hooks and the UI
struct RunState {
    int currentStart = -1;          // start percent of the current attempt
    // last finished attempt (for the level complete screen)
    int lastOrigin = -1;
    bool completionRecorded = false;
    int lastCompletionStart = -1;
    bool lastAttemptNoclip = false; // noclip saved you at least once
    std::vector<Run> lastAttempt;   // runs recorded during that attempt
};
RunState& runState();

// Localisation helper: tr("English", "Español")
std::string tr(char const* en, char const* es);
std::string pct(float v01, int decimals = 1);

}
