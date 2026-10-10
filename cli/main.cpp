#ifdef _WIN32
#define NOMINMAX
#endif

#include <utils/config.h>
#include <utils/logger.h>

#include <CLI/CLI.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "mini_json.h"
#include "sql_runner.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
//  Terminal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace color {

const char* RESET = "\033[0m";
const char* BOLD = "\033[1m";

const char* PINK = "\033[38;5;213m";
const char* PINK_BRIGHT = "\033[38;5;219m";
const char* HOT_PINK = "\033[38;5;205m";
const char* MAGENTA = "\033[38;5;200m";
const char* WHITE_SHINE = "\033[1;97m";

const char* GREEN = "\033[38;5;114m";
const char* RED = "\033[38;5;203m";
const char* YELLOW = "\033[38;5;221m";

const char* DIM = "\033[2m";
const char* CYAN = "\033[38;5;117m";
const char* GRAY = "\033[38;5;245m";

// Used when stdout is not a terminal (pipes, CI logs, redirected scripts).
void Disable() {
    RESET = BOLD = PINK = PINK_BRIGHT = HOT_PINK = MAGENTA = WHITE_SHINE = "";
    GREEN = RED = YELLOW = DIM = CYAN = GRAY = "";
}

}  // namespace color

namespace {

bool g_anim = true;  // animations on/off (off when piped or --no-anim)

void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool StdoutIsTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

int TerminalWidth() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
#else
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
#endif
    return 80;
}

void HideCursor() {
    std::cout << "\033[?25l" << std::flush;
}
void ShowCursor() {
    std::cout << "\033[?25h" << std::flush;
}

}  // namespace

void EnableAnsiColors() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, mode);
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  Project metadata: phases.json + queries.json
// ─────────────────────────────────────────────────────────────────────────────

struct Task {
    std::string id, title, status;  // status: done | in_progress | todo
    int progress = -1;              // optional 0..100 for in_progress tasks

    double Fraction() const {
        if (status == "done")
            return 1.0;
        if (progress >= 0)
            return std::min(progress, 100) / 100.0;
        return status == "in_progress" ? 0.5 : 0.0;
    }
};

struct Phase {
    std::string id, name, state, note;  // state: done | in_progress | next | todo | optional
    std::vector<Task> tasks;

    double Fraction() const {
        if (tasks.empty())
            return state == "done" ? 1.0 : 0.0;
        double sum = 0;
        for (const auto& t : tasks)
            sum += t.Fraction();
        return sum / static_cast<double>(tasks.size());
    }
    int Percent() const {
        return static_cast<int>(Fraction() * 100.0 + 0.5);
    }
};

struct Stage {
    std::string phase, milestone, label, state;
};

struct Feature {
    std::string id, title, status, phase, task, example, note;
    std::vector<std::vector<std::string>> patterns;  // token sequences, upper-case
    bool Available() const {
        return status == "available";
    }
};

struct ProjectData {
    std::string project = "AxolotlDB", version;
    Stage stage;
    std::vector<Phase> phases;
    std::vector<Feature> features;
    bool phases_loaded = false;
    bool queries_loaded = false;
    std::string problem;  // human-readable load failure, empty when fine
};

namespace {

std::string ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + p.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> SplitWords(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string w;
    while (is >> w) {
        std::transform(w.begin(), w.end(), w.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        out.push_back(w);
    }
    return out;
}

void LoadPhases(const fs::path& path, ProjectData& d) {
    const auto doc = minijson::Parse(ReadFile(path));
    d.project = doc.Str("project", "AxolotlDB");
    d.version = doc.Str("version");
    if (const auto* st = doc.Find("stage")) {
        d.stage = {st->Str("phase"), st->Str("milestone"), st->Str("label"), st->Str("state")};
    }
    for (const auto& pj : doc.List("phases")) {
        Phase p;
        p.id = pj.Str("id");
        p.name = pj.Str("name");
        p.state = pj.Str("state", "todo");
        p.note = pj.Str("note");
        for (const auto& tj : pj.List("tasks")) {
            Task t;
            t.id = tj.Str("id");
            t.title = tj.Str("title");
            t.status = tj.Str("status", "todo");
            t.progress = static_cast<int>(tj.Num("progress", -1));
            p.tasks.push_back(std::move(t));
        }
        d.phases.push_back(std::move(p));
    }
    d.phases_loaded = true;
}

void LoadQueries(const fs::path& path, ProjectData& d) {
    const auto doc = minijson::Parse(ReadFile(path));
    for (const auto& fj : doc.List("features")) {
        Feature f;
        f.id = fj.Str("id");
        f.title = fj.Str("title");
        f.status = fj.Str("status", "planned");
        f.phase = fj.Str("phase");
        f.task = fj.Str("task");
        f.example = fj.Str("example");
        f.note = fj.Str("note");
        for (const auto& kw : fj.List("keywords")) {
            auto words = SplitWords(kw.string);
            if (!words.empty()) {
                f.patterns.push_back(std::move(words));
            }
        }
        d.features.push_back(std::move(f));
    }
    d.queries_loaded = true;
}

// Looks for the two JSON files next to the CLI, in the working directory, or in
// the source tree layout (build/bin/ -> cli/data/).
fs::path FindMetaDir(const std::string& explicit_dir, const char* argv0) {
    std::vector<fs::path> candidates;
    if (!explicit_dir.empty()) {
        candidates.emplace_back(explicit_dir);
    }
    candidates.emplace_back("cli/data");
    candidates.emplace_back("data");
    std::error_code ec;
    const fs::path exe_dir = fs::weakly_canonical(fs::path(argv0), ec).parent_path();
    if (!ec) {
        candidates.push_back(exe_dir / "data");
        candidates.push_back(exe_dir / ".." / "cli" / "data");
        candidates.push_back(exe_dir / ".." / ".." / "cli" / "data");
    }
    for (const auto& c : candidates) {
        if (fs::exists(c / "phases.json") && fs::exists(c / "queries.json")) {
            return c;
        }
    }
    return {};
}

void LoadProjectData(const fs::path& dir, ProjectData& d) {
    if (dir.empty()) {
        d.problem = "phases.json / queries.json not found (use --meta-dir)";
        return;
    }
    try {
        LoadPhases(dir / "phases.json", d);
    } catch (const std::exception& e) {
        d.problem = std::string("phases.json: ") + e.what();
    }
    try {
        LoadQueries(dir / "queries.json", d);
    } catch (const std::exception& e) {
        d.problem += (d.problem.empty() ? "" : "; ") + std::string("queries.json: ") + e.what();
    }
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  SQL gate: what this build is allowed to run, decided by queries.json
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Upper-cased word tokens; '(' kept as its own token; string literals, quoted
// identifiers and comments are skipped so keywords inside them never match.
std::vector<std::string> SqlTokens(const std::string& sql) {
    std::vector<std::string> out;
    const size_t n = sql.size();
    for (size_t i = 0; i < n;) {
        const char c = sql[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
            while (i < n && sql[i] != '\n')
                ++i;
        } else if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
            const size_t end = sql.find("*/", i + 2);
            i = end == std::string::npos ? n : end + 2;
        } else if (c == '\'' || c == '"') {
            const char quote = c;
            ++i;
            while (i < n) {
                if (sql[i] == quote) {
                    if (i + 1 < n && sql[i + 1] == quote) {  // doubled quote escape
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                ++i;
            }
            out.emplace_back("?");
        } else if (c == '(') {
            out.emplace_back("(");
            ++i;
        } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            std::string w;
            while (i < n && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) {
                w += static_cast<char>(std::toupper(static_cast<unsigned char>(sql[i])));
                ++i;
            }
            out.push_back(std::move(w));
        } else {
            ++i;
        }
    }
    return out;
}

bool ContainsSequence(const std::vector<std::string>& tokens,
                      const std::vector<std::string>& pattern) {
    if (pattern.empty() || tokens.size() < pattern.size()) {
        return false;
    }
    for (size_t i = 0; i + pattern.size() <= tokens.size(); ++i) {
        if (std::equal(pattern.begin(), pattern.end(), tokens.begin() + static_cast<long>(i))) {
            return true;
        }
    }
    return false;
}

struct Gate {
    std::vector<const Feature*> hits;
    std::vector<const Feature*> blockers;  // hits that are not available
};

Gate CheckGate(const ProjectData& d, const std::string& sql) {
    Gate g;
    const auto tokens = SqlTokens(sql);
    for (const auto& f : d.features) {
        for (const auto& pattern : f.patterns) {
            if (ContainsSequence(tokens, pattern)) {
                g.hits.push_back(&f);
                if (!f.Available()) {
                    g.blockers.push_back(&f);
                }
                break;
            }
        }
    }
    return g;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Banner
// ─────────────────────────────────────────────────────────────────────────────

const std::vector<std::string> AXOLOTL_BANNER = {

    R"(                                                                                            )",
    R"(    ⢀⠀⠀⠀⠀⠀⠀⠀⣀⣀⡤⠤⠤⠤⣄⣀⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀                   _____                .__          __  ________      )",
    R"( ⠀⢴⠋⠙⠳⣤⡀⣠⠖⠋⠁⠀⠀⠀⠀⠀⠀⠀⠉⠓⠤⡀⣠⡴⠟⠛⣷                  /  _  \  ___  ___ ___ |  |   _____/  |_\______ \  __ )",
    R"(  ⠀⠈⠳⢤⣀⢈⠟⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠘⢏⣀⣠⡴⠋⠀⠀               /  /_\  \ \  \/  //   \|  |  /  _ \   __\|    |  \|  |)",
    R"(  ⣠⣤⣄⣄⣉⡏⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠈⣇⣡⣤⡴⣦⣀              /    |    \ >    <|  |  \  |_(  (_) )  |  |    `   \  |)",
    R"(  ⢹⣅⡀⠀⠀⠈⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⡏⠁⠀⢀⣠⠏             \____|__  //__/\_ \___  /____/\____/|__| /_______  /__|)",
    R"(  ⠈⠙⠉⢋⣉⣇⠀⠀⣾⣷⠄⠀⠀⠀⠀⠀⠀⠀⢴⣿⡆⠀⢀⣿⡙⠋⠋⠀⠀                     \/       \/   \/                         \/    )",
    R"(   ⢤⠶⠋⠉⢈⣦⡀⠈⠉⠀⠀⠀⠉⠉⠉⠀⠀⠀⠉⠀⣠⣎⠈⠉⠛⢷⡀⠀                                                                    )",
    R"(   ⠻⣤⣤⠶⠋⠀⠈⠑⠠⠄⣀⣀⣀⣀⣀⣀⡀⠤⠐⠉⠀⠈⠻⠶⠖⠶⠃⠀              Vector Database Engine v0.1.0                        )",
    R"(   ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢠⠃⠤⡀⠀⡠⠤⢱⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀                                                                    )",
    R"(   ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⡎⠀⠉⠁⠀⠉⠉⠀⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀                                                                    )",
    R"(   ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⡇⢀⠤⡀⠀⡠⢄⢀⠇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀                                                                    )",
    R"(   ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠘⠢⣀⣀⣀⣈⡠⠊⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀                                                                    )",
    R"(                  ⡌⡇⠀⣎⠇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀                                                                     )",
    R"(             ⠀⠀⠀⠀ ⢡⡇⣸⠜⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀                                                                    )",
    R"(                    ⠓⠉⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀                                                                    )",

};

inline int Utf8CharLen(unsigned char lead) {
    if (lead < 0x80)
        return 1;
    if ((lead & 0xE0) == 0xC0)
        return 2;
    if ((lead & 0xF0) == 0xE0)
        return 3;
    if ((lead & 0xF8) == 0xF0)
        return 4;
    return 1;
}

struct Utf8Char {
    std::string ch;
    int col;
};

std::vector<Utf8Char> SplitUtf8(const std::string& line) {
    std::vector<Utf8Char> chars;
    int col = 0;
    for (size_t i = 0; i < line.size();) {
        int len = Utf8CharLen(static_cast<unsigned char>(line[i]));
        if (i + len > line.size())
            len = 1;
        chars.push_back({line.substr(i, len), col});
        i += len;
        ++col;
    }
    return chars;
}

size_t VisibleWidth(const std::string& s) {
    return SplitUtf8(s).size();
}

std::string PadRight(const std::string& s, size_t width) {
    const size_t w = VisibleWidth(s);
    return w >= width ? s : s + std::string(width - w, ' ');
}

std::string Truncate(const std::string& s, size_t max_width) {
    auto chars = SplitUtf8(s);
    if (chars.size() <= max_width)
        return s;
    std::string out;
    for (size_t i = 0; i + 1 < max_width; ++i)
        out += chars[i].ch;
    return out + "…";
}

void PrintBannerFrame(const std::vector<std::string>& banner, int shine_col, int shine_width) {
    for (const auto& line : banner) {
        auto chars = SplitUtf8(line);
        for (const auto& uc : chars) {
            if (uc.ch == " ") {
                std::cout << ' ';
                continue;
            }

            int dist = uc.col - shine_col;
            if (dist >= 0 && dist < shine_width) {
                int center_dist = std::abs(dist - shine_width / 2);
                if (center_dist <= 1) {
                    std::cout << color::WHITE_SHINE << uc.ch << color::RESET;
                } else if (center_dist <= 3) {
                    std::cout << color::PINK_BRIGHT << uc.ch << color::RESET;
                } else {
                    std::cout << color::PINK << uc.ch << color::RESET;
                }
            } else {
                if (uc.col % 7 == 0) {
                    std::cout << color::HOT_PINK << uc.ch << color::RESET;
                } else if (uc.col % 11 == 0) {
                    std::cout << color::MAGENTA << uc.ch << color::RESET;
                } else {
                    std::cout << color::PINK << uc.ch << color::RESET;
                }
            }
        }
        std::cout << '\n';
    }
}

void PrintStaticBanner(const std::vector<std::string>& banner) {
    PrintBannerFrame(banner, -1000, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Swimming axolotl
// ─────────────────────────────────────────────────────────────────────────────

// The prompt axolotl swims across the terminal: it blinks, wiggles its tail, and
// leaves bubbles behind it over a rippling water line. It moves one column per
// frame, so the pace is set only by delay_ms (bigger = slower).
void Swim(int delay_ms = 55) {
    if (!g_anim) {
        return;
    }
    const int kRows = 3;           // bubbles, axolotl, ripples
    const int kSwimmerWidth = 12;  // tail + face, with slack for wide glyphs
    const int width = std::min(TerminalWidth(), 100) - 1;

    HideCursor();
    for (int i = 0; i < kRows; ++i) {
        std::cout << '\n';
    }

    int frame = 0;
    for (int x = 0; x + kSwimmerWidth < width; ++x, ++frame) {
        std::string bubbles(static_cast<size_t>(width), ' ');
        for (int k = 1; k <= 3; ++k) {
            const int bx = x - 4 * k;
            if (bx >= 0) {
                bubbles[static_cast<size_t>(bx)] = ((frame / 4 + k) % 2 == 0) ? 'o' : '.';
            }
        }

        const bool blink = frame % 28 >= 26;
        const char* face = blink ? "꒰- - -꒱" : "꒰• - •꒱";
        const char* tail = (frame / 4) % 2 == 0 ? "~~" : "-~";
        const std::string swimmer = std::string(static_cast<size_t>(x), ' ') + tail + face;

        std::string ripple(static_cast<size_t>(width), ' ');
        for (int i = 0; i < width; ++i) {
            if (((i - frame / 3) % 7 + 7) % 7 == 0) {
                ripple[static_cast<size_t>(i)] = '~';
            }
        }

        std::cout << "\033[" << kRows << "A";
        std::cout << '\r' << color::CYAN << bubbles << color::RESET << "\033[K\n";
        std::cout << '\r' << color::PINK << color::BOLD << swimmer << color::RESET << "\033[K\n";
        std::cout << '\r' << color::DIM << color::CYAN << ripple << color::RESET << "\033[K\n";
        std::cout.flush();
        SleepMs(delay_ms);
    }

    // Wipe the pond so the screen stays clean.
    std::cout << "\033[" << kRows << "A";
    for (int r = 0; r < kRows; ++r) {
        std::cout << "\r\033[K\n";
    }
    std::cout << "\033[" << kRows << "A";
    ShowCursor();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Prompt, modes, panels
// ─────────────────────────────────────────────────────────────────────────────

// user:  placeholder, input is echoed (module not built yet)
// admin: placeholder, input is echoed (module not built yet)
// dev:   everything built so far: SQL, roadmap, queries, demo data, animations
enum class Mode { USER, ADMIN, DEV };

const char* ModeColor(Mode m) {
    switch (m) {
        case Mode::ADMIN:
            return color::RED;
        case Mode::DEV:
            return color::YELLOW;
        default:
            return color::GREEN;
    }
}
const char* ModeName(Mode m) {
    switch (m) {
        case Mode::ADMIN:
            return "admin";
        case Mode::DEV:
            return "dev";
        default:
            return "user";
    }
}
const char* ModeFace(Mode m) {
    switch (m) {
        case Mode::ADMIN:
            return "ò - ó";
        case Mode::DEV:
            return "◉ - ◉";
        default:
            return "• - •";
    }
}

void PrintPrompt(Mode m) {
    const char* c = ModeColor(m);
    std::cout << c << color::BOLD << "꒰" << ModeFace(m) << "꒱" << color::RESET << c << "> "
              << color::RESET;
    std::cout.flush();
}

void PrintContinuation(Mode m) {
    std::cout << ModeColor(m) << "   ...> " << color::RESET;
    std::cout.flush();
}

void Blink(Mode m, const std::string& text) {
    const char* c = ModeColor(m);
    auto draw = [&](const char* face) {
        std::cout << '\r' << "  " << c << color::BOLD << "꒰" << face << "꒱" << color::RESET
                  << color::GRAY << "  " << text << color::RESET << "\033[K";
        std::cout.flush();
    };
    if (g_anim) {
        draw("- - -");
        SleepMs(160);
    }
    draw(ModeFace(m));
    std::cout << '\n';
}

void PanelTop(const std::string& title) {
    std::cout << color::PINK << "  ╭─ " << color::BOLD << title << color::RESET << color::PINK
              << " " << color::DIM;
    const int fill = 62 - static_cast<int>(VisibleWidth(title));
    for (int i = 0; i < fill; ++i) {
        std::cout << "─";
    }
    std::cout << color::RESET << '\n';
}
void PanelBottom() {
    std::cout << color::PINK << "  ╰" << color::DIM;
    for (int i = 0; i < 64; ++i) {
        std::cout << "─";
    }
    std::cout << color::RESET << '\n';
}
std::ostream& PanelLine() {
    return std::cout << color::PINK << "  │ " << color::RESET;
}

const char* PhaseIcon(const std::string& state) {
    if (state == "done")
        return "✔ ";
    if (state == "in_progress")
        return "▶ ";
    if (state == "optional")
        return "○ ";
    return "· ";
}
const char* StateColor(const std::string& state) {
    if (state == "done")
        return color::GREEN;
    if (state == "in_progress")
        return color::YELLOW;
    return color::GRAY;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screens
// ─────────────────────────────────────────────────────────────────────────────

void PrintStageLine(const ProjectData& d) {
    if (!d.phases_loaded) {
        return;
    }
    PanelLine() << color::GRAY << "stage     " << color::YELLOW << color::BOLD << d.stage.phase
                << " · " << d.stage.milestone << color::RESET << color::GRAY << "  "
                << d.stage.label << color::RESET << '\n';
}

void PrintBasicWelcome(Mode m) {
    std::cout << '\n';
    PanelTop("AxolotlDB · interactive console");
    PanelLine() << color::GRAY << "mode      " << ModeColor(m) << color::BOLD << ModeName(m)
                << color::RESET << '\n';
    PanelLine() << color::GRAY << "note      " << color::YELLOW
                << "this mode is a placeholder: input is echoed back" << color::RESET << '\n';
    PanelLine() << color::GRAY << "tip       " << color::CYAN << ":dev" << color::GRAY
                << " runs SQL and shows the roadmap, " << color::CYAN << ":help" << color::GRAY
                << " lists commands" << color::RESET << '\n';
    PanelBottom();
    std::cout << '\n';
}

void PrintDevWelcome(const ProjectData& d, const fs::path& data_dir, bool engine_ok) {
    size_t available = 0, planned = 0;
    for (const auto& f : d.features)
        (f.Available() ? available : planned)++;

    std::cout << '\n';
    PanelTop("AxolotlDB · dev mode");
    PanelLine() << color::GRAY << "about     " << color::RESET
                << "The engineering mode: everything built so far, behind one switch." << '\n';
    PanelLine() << color::GRAY << "contains  " << color::GREEN << "✔ " << color::RESET
                << "SQL on the embedded engine, limited to what queries.json allows" << '\n';
    PanelLine() << "          " << color::GREEN << "✔ " << color::RESET
                << "roadmap and task list from phases.json  " << color::CYAN << "(:status, :tasks)"
                << color::RESET << '\n';
    PanelLine() << "          " << color::GREEN << "✔ " << color::RESET
                << "catalogue of available and planned SQL  " << color::CYAN << "(:queries)"
                << color::RESET << '\n';
    PanelLine() << "          " << color::GREEN << "✔ " << color::RESET
                << "demo data, since INSERT is not an executor yet  " << color::CYAN
                << "(:seed, :tables)" << color::RESET << '\n';
    PanelLine() << "          " << color::GREEN << "✔ " << color::RESET << "axolotl animations  "
                << color::CYAN << "(:swim)" << color::RESET << '\n';
    PanelLine() << color::DIM << color::GRAY
                << "          user and admin modes stay placeholders until their modules exist."
                << color::RESET << '\n';
    PanelLine() << '\n';
    PanelLine() << color::GRAY << "engine    " << (engine_ok ? color::GREEN : color::RED)
                << (engine_ok ? "open" : "not available") << color::RESET << color::GRAY << "  "
                << (engine_ok ? data_dir.string() : "SQL is disabled") << color::RESET << '\n';
    PrintStageLine(d);
    if (d.queries_loaded) {
        PanelLine() << color::GRAY << "sql       " << color::GREEN << available << " features"
                    << color::GRAY << " available, " << color::YELLOW << planned << color::GRAY
                    << " planned" << color::RESET << '\n';
    }
    PanelLine() << color::GRAY << "tip       " << color::CYAN << ":seed" << color::GRAY
                << " loads demo data, " << color::CYAN << ":queries" << color::GRAY
                << " shows what runs, " << color::CYAN << ":help" << color::GRAY << " for more"
                << color::RESET << '\n';
    if (!d.problem.empty()) {
        PanelLine() << color::RED << "warning   " << d.problem << color::RESET << '\n';
    }
    PanelBottom();
    std::cout << '\n';
}

void PrintHelp(Mode m) {
    std::cout << '\n';
    PanelTop("commands");
    auto row = [](const char* c, const char* cmd, const char* desc) {
        PanelLine() << c << cmd << color::GRAY << desc << color::RESET << '\n';
    };
    row(color::GREEN, ":user      ", "user mode   (placeholder, echo only)");
    row(color::RED, ":admin     ", "admin mode  (placeholder, echo only)");
    row(color::YELLOW, ":dev       ", "dev mode: SQL, roadmap, demo data  (alias :debug)");
    row(color::CYAN, ":clear     ", "clear screen and show the banner");
    row(color::CYAN, ":help      ", "show this message");
    row(color::CYAN, ":exit      ", "leave the console  (:quit, :q)");
    PanelLine() << '\n';

    const bool dev = m == Mode::DEV;
    const char* c = dev ? color::PINK_BRIGHT : color::GRAY;
    PanelLine() << color::DIM << (dev ? "dev mode commands" : "dev mode commands (type :dev first)")
                << color::RESET << '\n';
    row(c, ":queries   ", "SQL that works today, and what is planned");
    row(c, ":status    ", "roadmap progress, phase by phase");
    row(c, ":tasks [P4]", " tasks of a phase (default: the current stage)");
    row(c, ":seed      ", "create the demo `users` table (12 rows)");
    row(c, ":tables    ", "list tables");
    row(c, ":swim      ", "watch the axolotl swim by");
    PanelLine() << color::DIM << color::GRAY
                << "SQL ends with ';' and may span lines.  \\c or :cancel drops a half-typed "
                   "statement."
                << color::RESET << '\n';
    PanelBottom();
    std::cout << '\n';
}

const char* BarColor(int percent) {
    if (percent >= 100)
        return color::GREEN;
    if (percent > 0)
        return color::YELLOW;
    return color::GRAY;
}

void PrintStatus(const ProjectData& d) {
    if (!d.phases_loaded) {
        std::cout << color::RED << "  phases.json is not loaded. " << d.problem << color::RESET
                  << '\n';
        return;
    }
    const int kCells = 24;

    std::cout << '\n';
    PanelTop("roadmap");
    PrintStageLine(d);
    PanelLine() << '\n';

    double total = 0;
    for (const auto& p : d.phases) {
        total += p.Fraction();
        const int percent = p.Percent();
        const int filled = percent * kCells / 100;

        PanelLine() << StateColor(p.state) << PhaseIcon(p.state) << color::BOLD << PadRight(p.id, 4)
                    << color::RESET << color::GRAY << PadRight(p.name, 25) << color::RESET;

        std::cout << BarColor(percent);
        for (int i = 0; i < kCells; ++i) {
            std::cout << (i < filled ? "█" : "░");
            if (g_anim && i < filled) {
                std::cout.flush();
                SleepMs(5);
            }
        }
        std::cout << color::RESET << color::GRAY << " " << (percent < 100 ? " " : "")
                  << (percent < 10 ? " " : "") << percent << "%" << color::RESET;
        if (!p.note.empty() && p.state != "done") {
            std::cout << color::DIM << "  " << Truncate(p.note, 44) << color::RESET;
        }
        std::cout << '\n';
    }

    const int overall =
        static_cast<int>(total / static_cast<double>(d.phases.size()) * 100.0 + 0.5);
    PanelLine() << '\n';
    PanelLine() << color::PINK_BRIGHT << color::BOLD << "overall " << overall << "%" << color::RESET
                << color::GRAY << "   (:tasks <phase> for the checklist)" << color::RESET << '\n';
    PanelBottom();
    std::cout << '\n';
}

void PrintTasks(const ProjectData& d, std::string wanted) {
    if (!d.phases_loaded) {
        std::cout << color::RED << "  phases.json is not loaded. " << d.problem << color::RESET
                  << '\n';
        return;
    }
    if (wanted.empty()) {
        wanted = d.stage.phase;
    }
    std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    const Phase* phase = nullptr;
    for (const auto& p : d.phases) {
        if (p.id == wanted)
            phase = &p;
    }
    if (phase == nullptr) {
        std::cout << color::RED << "  no phase named " << wanted << color::RESET << color::GRAY
                  << "  (try P0 … P10)" << color::RESET << '\n';
        return;
    }

    std::cout << '\n';
    PanelTop(phase->id + " · " + phase->name);
    for (const auto& t : phase->tasks) {
        const char* c = StateColor(t.status);
        PanelLine() << c << PhaseIcon(t.status) << color::BOLD << PadRight(t.id, 5) << color::RESET
                    << (t.status == "done" ? color::GRAY : color::RESET) << t.title << color::RESET;
        if (t.status == "in_progress" && t.progress >= 0) {
            std::cout << color::YELLOW << "  " << t.progress << "%" << color::RESET;
        }
        std::cout << '\n';
    }
    if (!phase->note.empty()) {
        PanelLine() << '\n';
        PanelLine() << color::DIM << color::GRAY << phase->note << color::RESET << '\n';
    }
    PanelBottom();
    std::cout << '\n';
}

void PrintQueries(const ProjectData& d) {
    if (!d.queries_loaded) {
        std::cout << color::RED << "  queries.json is not loaded, so SQL is disabled. " << d.problem
                  << color::RESET << '\n';
        return;
    }
    std::cout << '\n';
    PanelTop("available now");
    for (const auto& f : d.features) {
        if (!f.Available())
            continue;
        PanelLine() << color::GREEN << "✔ " << color::RESET << f.title << '\n';
        PanelLine() << "    " << color::CYAN << f.example << color::RESET << '\n';
    }
    PanelBottom();
    std::cout << '\n';

    PanelTop("planned");
    for (const auto& f : d.features) {
        if (f.Available())
            continue;
        PanelLine() << color::YELLOW << "· " << color::RESET << PadRight(f.title, 34) << color::GRAY
                    << f.phase << " " << f.task << color::RESET;
        if (!f.note.empty()) {
            std::cout << color::DIM << "  " << Truncate(f.note, 40) << color::RESET;
        }
        std::cout << '\n';
    }
    PanelBottom();
    std::cout << '\n';
}

void PrintGoodbye(bool animate) {
    if (animate) {
        Swim(35);
    }
    std::cout << color::PINK << color::BOLD << "  Goodbye! " << color::RESET << color::PINK
              << "The axolotl waves a tiny hand " << color::BOLD << "꒰• - •꒱/~" << color::RESET
              << "\n\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Running SQL and printing results
// ─────────────────────────────────────────────────────────────────────────────

void PrintSqlError(const SqlError& e, const std::string& sql) {
    std::cout << color::RED << color::BOLD << "  ✖ " << e.kind << color::RESET << color::RED << ": "
              << e.message << color::RESET << '\n';
    if (e.line > 0) {
        std::istringstream lines(sql);
        std::string text;
        for (int i = 1; i <= e.line && std::getline(lines, text); ++i) {
        }
        std::cout << color::GRAY << "    " << e.line << " │ " << color::RESET << text << '\n';
        const int col = std::max(1, e.column);
        std::cout << color::GRAY << "    " << std::string(std::to_string(e.line).size(), ' ')
                  << " │ " << color::RESET << std::string(static_cast<size_t>(col - 1), ' ')
                  << color::RED << color::BOLD << "^" << color::RESET << '\n';
    }
}

void PrintResult(const QueryResult& r) {
    const size_t kMaxRows = 200;
    const size_t kMaxCell = 40;

    if (r.columns.empty()) {
        std::cout << color::GRAY << "  ok  (" << r.elapsed_ms << " ms)" << color::RESET << '\n';
        return;
    }

    const size_t shown = std::min(r.rows.size(), kMaxRows);
    std::vector<size_t> width(r.columns.size());
    for (size_t c = 0; c < r.columns.size(); ++c) {
        width[c] = std::min(VisibleWidth(r.columns[c]), kMaxCell);
    }
    for (size_t i = 0; i < shown; ++i) {
        for (size_t c = 0; c < r.columns.size() && c < r.rows[i].size(); ++c) {
            width[c] = std::max(width[c], std::min(VisibleWidth(r.rows[i][c]), kMaxCell));
        }
    }

    std::cout << '\n' << "  ";
    for (size_t c = 0; c < r.columns.size(); ++c) {
        std::cout << color::PINK_BRIGHT << color::BOLD
                  << PadRight(Truncate(r.columns[c], kMaxCell), width[c]) << color::RESET
                  << (c + 1 < r.columns.size() ? "  " : "");
    }
    std::cout << '\n' << "  " << color::DIM;
    for (size_t c = 0; c < r.columns.size(); ++c) {
        for (size_t i = 0; i < width[c]; ++i)
            std::cout << "─";
        std::cout << (c + 1 < r.columns.size() ? "──" : "");
    }
    std::cout << color::RESET << '\n';

    for (size_t i = 0; i < shown; ++i) {
        std::cout << "  ";
        for (size_t c = 0; c < r.columns.size(); ++c) {
            const std::string& cell = c < r.rows[i].size() ? r.rows[i][c] : std::string();
            const bool is_null = cell == "NULL";
            std::cout << (is_null ? color::DIM : "") << PadRight(Truncate(cell, kMaxCell), width[c])
                      << color::RESET << (c + 1 < r.columns.size() ? "  " : "");
        }
        std::cout << '\n';
    }

    std::cout << '\n'
              << color::GRAY << "  (" << r.rows.size() << (r.rows.size() == 1 ? " row" : " rows")
              << " · " << r.elapsed_ms << " ms)" << color::RESET;
    if (r.rows.size() > shown) {
        std::cout << color::DIM << "  showing the first " << shown << color::RESET;
    }
    std::cout << "\n\n";
}

// Returns true if the statement was handed to the engine.
void RunSql(const std::string& sql, const ProjectData& data, SqlRunner* runner) {
    if (runner == nullptr) {
        std::cout << color::RED << "  ✖ the engine is not open, so SQL is disabled." << color::RESET
                  << '\n';
        return;
    }
    if (!data.queries_loaded) {
        std::cout << color::RED
                  << "  ✖ queries.json is not loaded, so the CLI cannot tell what is safe to run."
                  << color::RESET << '\n';
        return;
    }

    const Gate gate = CheckGate(data, sql);

    if (gate.hits.empty()) {
        std::cout << color::RED << "  ✖ not a statement this build recognises." << color::RESET
                  << color::GRAY << "  :queries lists what works." << color::RESET << '\n';
        return;
    }
    if (!gate.blockers.empty()) {
        std::cout << color::YELLOW << color::BOLD << "  ✖ not available yet in this build"
                  << color::RESET << '\n';
        for (const auto* f : gate.blockers) {
            std::cout << color::YELLOW << "    " << PadRight(f->title, 34) << color::RESET
                      << color::GRAY << "planned: " << f->phase << " · " << f->task << color::RESET;
            if (!f->note.empty())
                std::cout << color::DIM << "  " << f->note << color::RESET;
            std::cout << '\n';
        }
        std::cout << color::GRAY << "    :queries shows what runs today." << color::RESET << '\n';
        return;
    }

    const QueryResult result = runner->Execute(sql);
    if (result.error) {
        PrintSqlError(*result.error, sql);
        return;
    }
    PrintResult(result);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Config helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

AxolotlDB::utils::LogLevel ParseLogLevel(const std::string& raw_level) {
    std::string level = raw_level;
    std::transform(level.begin(), level.end(), level.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    using AxolotlDB::utils::LogLevel;
    if (level == "trace")
        return LogLevel::kTrace;
    if (level == "debug")
        return LogLevel::kDebug;
    if (level == "info")
        return LogLevel::kInfo;
    if (level == "warn")
        return LogLevel::kWarn;
    if (level == "error")
        return LogLevel::kError;
    if (level == "critical")
        return LogLevel::kCritical;
    if (level == "off")
        return LogLevel::kOff;
    throw std::invalid_argument("invalid log level: " + raw_level);
}

void ClearAndShowBanner() {
    std::cout << "\033[2J\033[H";
    std::cout << '\n';
    PrintStaticBanner(AXOLOTL_BANNER);
    std::cout << '\n';
}

std::string Trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    const auto last = s.find_last_not_of(" \t\r\n");
    s.erase(last == std::string::npos ? 0 : last + 1);
    return s;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    EnableAnsiColors();

    std::string config_path;
    std::string log_level_override;
    std::string data_dir_arg = "AxolotlDB_data";
    std::string meta_dir_arg;
    bool no_anim = false;
    bool start_dev = false;

    CLI::App app{"AxolotlDB CLI (embedded)"};
    app.add_option("-c,--config", config_path, "Path to YAML config file");
    app.add_option("--log-level", log_level_override,
                   "Override logging.level (trace|debug|info|warn|error|critical|off)");
    app.add_option("-d,--data-dir", data_dir_arg, "Directory holding the database files (dev mode)")
        ->capture_default_str();
    app.add_option("--meta-dir", meta_dir_arg,
                   "Directory with phases.json and queries.json (dev mode)");
    app.add_flag("--dev", start_dev, "Start in dev mode");
    app.add_flag("--no-anim", no_anim, "Disable animations");

    CLI11_PARSE(app, argc, argv);

    // Piped output (scripts, CI) gets plain text: no escapes, no sleeps.
    const bool tty = StdoutIsTty();
    g_anim = tty && !no_anim;
    if (!tty) {
        color::Disable();
    }

    try {
        using AxolotlDB::utils::ConfigManager;

        ConfigManager::LoadDefaults();
        if (!config_path.empty()) {
            ConfigManager::LoadFromFile(config_path);
        }
        ConfigManager::ApplyEnvOverrides();
        if (!log_level_override.empty()) {
            ConfigManager::OverrideLogLevel(log_level_override);
        }

        const auto& cfg = ConfigManager::Get();
        const auto log_level = ParseLogLevel(cfg.logging.level);

        const fs::path log_path(cfg.logging.file);
        if (!log_path.parent_path().empty()) {
            fs::create_directories(log_path.parent_path());
        }

        // Resolve everything relative BEFORE SqlRunner changes the working directory.
        const fs::path data_dir = fs::absolute(data_dir_arg);
        const fs::path meta_dir = FindMetaDir(meta_dir_arg, argv[0]);
        const fs::path abs_meta = meta_dir.empty() ? meta_dir : fs::absolute(meta_dir);

        AxolotlDB::utils::Logger::Init(
            "AxolotlDB-cli", fs::absolute(log_path).string(), log_level, cfg.logging.async,
            static_cast<size_t>(cfg.logging.max_file_size_mb) * 1024U * 1024U,
            static_cast<size_t>(cfg.logging.max_files));

        if (tty) {
            ClearAndShowBanner();
        } else {
            PrintStaticBanner(AXOLOTL_BANNER);
        }

        // Dev-mode state. Nothing here is touched until the first :dev, so the other
        // modes never open the storage engine or read the JSON files.
        ProjectData data;
        std::unique_ptr<SqlRunner> runner;
        bool dev_ready = false;
        Mode mode = Mode::USER;

        auto enter_dev = [&] {
            if (!dev_ready) {
                LoadProjectData(abs_meta, data);
                try {
                    runner = std::make_unique<SqlRunner>(data_dir);
                } catch (const std::exception& ex) {
                    data.problem +=
                        (data.problem.empty() ? "" : "; ") + std::string("engine: ") + ex.what();
                    LOG_ERROR("engine failed to open: {}", ex.what());
                }
                dev_ready = true;
            }
            mode = Mode::DEV;
            Swim();
            PrintDevWelcome(data, data_dir, runner != nullptr);
            Blink(mode, runner ? "dev mode ready" : "dev mode ready (SQL disabled)");
            std::cout << '\n';
        };

        if (start_dev) {
            enter_dev();
        } else {
            PrintBasicWelcome(mode);
            Blink(mode, "ready");
            std::cout << '\n';
        }

        LOG_INFO("AxolotlDB CLI started, data dir {}", data_dir.string());

        std::string buffer;  // dev mode: statement being typed
        bool goodbye = false;
        bool echo_hint_shown = false;
        std::string input;

        auto prompt = [&] {
            if (buffer.empty())
                PrintPrompt(mode);
            else
                PrintContinuation(mode);
        };

        auto need_dev = [&](const char* what) {
            if (mode == Mode::DEV) {
                return true;
            }
            std::cout << color::YELLOW << "  " << what << " belongs to dev mode." << color::RESET
                      << color::GRAY << "  type " << color::CYAN << ":dev" << color::GRAY
                      << " to enable it" << color::RESET << '\n';
            return false;
        };

        auto handle_command = [&](const std::string& line) {
            std::istringstream is(line);
            std::string cmd, arg;
            is >> cmd >> arg;

            if (cmd == ":exit" || cmd == ":quit" || cmd == ":q") {
                std::cout << '\n';
                PrintGoodbye(mode == Mode::DEV);
                goodbye = true;
            } else if (cmd == ":help" || cmd == ":h") {
                PrintHelp(mode);
            } else if (cmd == ":user" || cmd == ":admin") {
                mode = cmd == ":user" ? Mode::USER : Mode::ADMIN;
                Blink(mode, std::string("switched to ") + ModeName(mode) + " mode");
            } else if (cmd == ":dev" || cmd == ":debug") {
                enter_dev();
            } else if (cmd == ":clear" || cmd == ":cls") {
                ClearAndShowBanner();
                if (mode == Mode::DEV)
                    PrintDevWelcome(data, data_dir, runner != nullptr);
                else
                    PrintBasicWelcome(mode);
            } else if (cmd == ":status" || cmd == ":roadmap") {
                if (need_dev("the roadmap"))
                    PrintStatus(data);
            } else if (cmd == ":tasks") {
                if (need_dev("the task list"))
                    PrintTasks(data, arg);
            } else if (cmd == ":queries" || cmd == ":sql") {
                if (need_dev("the query list"))
                    PrintQueries(data);
            } else if (cmd == ":swim") {
                if (need_dev("the swim"))
                    Swim();
            } else if (cmd == ":seed") {
                if (!need_dev("demo data")) {
                } else if (!runner) {
                    std::cout << color::RED << "  ✖ the engine is not open." << color::RESET
                              << '\n';
                } else {
                    try {
                        std::cout << color::GREEN << "  ✔ " << runner->SeedDemo() << color::RESET
                                  << '\n';
                        std::cout << color::GRAY << "    try: " << color::CYAN
                                  << "SELECT name, score FROM users WHERE age > 30;" << color::RESET
                                  << '\n';
                    } catch (const std::exception& ex) {
                        std::cout << color::RED << "  ✖ seed failed: " << ex.what() << color::RESET
                                  << '\n';
                    }
                }
            } else if (cmd == ":tables") {
                if (!need_dev("the table list")) {
                } else if (!runner) {
                    std::cout << color::RED << "  ✖ the engine is not open." << color::RESET
                              << '\n';
                } else {
                    try {
                        const auto names = runner->TableNames();
                        if (names.empty())
                            std::cout << color::GRAY << "  (no tables, try :seed)" << color::RESET
                                      << '\n';
                        for (const auto& n : names)
                            std::cout << "  " << color::CYAN << n << color::RESET << '\n';
                    } catch (const std::exception& ex) {
                        std::cout << color::RED << "  ✖ " << ex.what() << color::RESET << '\n';
                    }
                }
            } else {
                std::cout << color::RED << "  unknown command " << color::BOLD << cmd
                          << color::RESET << color::GRAY << "  (try :help)" << color::RESET << '\n';
            }
        };

        prompt();
        while (!goodbye && std::getline(std::cin, input)) {
            const std::string line = Trim(input);

            // user / admin: placeholders. Commands work, everything else is echoed.
            if (mode != Mode::DEV) {
                if (line.empty()) {
                    prompt();
                    continue;
                }
                if (line[0] == ':') {
                    handle_command(line);
                    if (!goodbye)
                        prompt();
                    continue;
                }
                std::cout << ModeColor(mode) << "  → " << color::RESET << input << '\n';
                if (!echo_hint_shown) {
                    std::cout << color::DIM << color::GRAY << "    echo only: the "
                              << ModeName(mode)
                              << " module is not built yet. SQL lives in dev mode (:dev)"
                              << color::RESET << '\n';
                    echo_hint_shown = true;
                }
                LOG_DEBUG("echo [{}]: {}", ModeName(mode), input);
                prompt();
                continue;
            }

            // dev: commands, multi-line SQL ended by ';'
            if (buffer.empty()) {
                if (line.empty()) {
                    prompt();
                    continue;
                }
                if (line[0] == ':') {
                    handle_command(line);
                    if (!goodbye)
                        prompt();
                    continue;
                }
            } else if (line == ":cancel" || line == "\\c") {
                buffer.clear();
                std::cout << color::GRAY << "  statement discarded" << color::RESET << '\n';
                prompt();
                continue;
            }

            if (line == "\\c" && buffer.empty()) {
                prompt();
                continue;
            }

            buffer += (buffer.empty() ? "" : "\n") + input;

            if (!line.empty() && line.back() == ';') {
                const std::string sql = Trim(buffer);
                buffer.clear();
                LOG_DEBUG("sql: {}", sql);
                RunSql(sql, data, runner.get());
            }
            prompt();
        }

        ShowCursor();
        runner.reset();  // flushes every file
        AxolotlDB::utils::Logger::Shutdown();
        return 0;
    } catch (const std::exception& ex) {
        ShowCursor();
        std::cerr << color::RED << color::BOLD << "Fatal error: " << color::RESET << color::RED
                  << ex.what() << color::RESET << std::endl;
        return 1;
    }
}