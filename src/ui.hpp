#pragma once
// fpmatch terminal UI: banner, stage lines with timing, progress bars,
// status text and a blinking final screen.
//
// Use it from src/image_processing.cpp with:  #include "ui.hpp"
// Everything lives in namespace ui, so call things like ui::banner().
//
// It turns itself off when output isn't a terminal (piped to a file), and
// you can force it off with ui::enabled = false (fpeval will want that).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace ui {

// ---- settings -------------------------------------------------------------

// Off automatically when stdout isn't a terminal.
inline bool enabled = isatty(STDOUT_FILENO);

// Set false if the block characters show up as garbage in your terminal.
inline bool unicode = true;

// Width of the stage name column, so bars line up.
inline int nameWidth = 14;

// Every stage stays on screen at least this long, so fast stages don't just
// flash by. eval sets it to 0 because it runs hundreds of prints.
inline int minStageMs = 250;

// ---- colors (ANSI escape codes) --------------------------------------------

constexpr const char* RESET  = "\033[0m";
constexpr const char* BOLD   = "\033[1m";
constexpr const char* DIM    = "\033[90m";
constexpr const char* RED    = "\033[31m";
constexpr const char* GREEN  = "\033[32m";
constexpr const char* YELLOW = "\033[33m";
constexpr const char* CYAN   = "\033[36m";
constexpr const char* CLEAR_LINE = "\r\033[2K";

// ---- small helpers ---------------------------------------------------------

// 4812 -> "4,812"
inline std::string num(long long n) {
    std::string s = std::to_string(n < 0 ? -n : n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, ",");
    return n < 0 ? "-" + s : s;
}

// 127.4321 -> "127.43"
inline std::string fixed(double v, int decimals = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(decimals) << v;
    return out.str();
}

inline std::string pad(const std::string& s, int width) {
    if (static_cast<int>(s.size()) >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

inline std::string repeat(const std::string& s, int count) {
    std::string out;
    for (int i = 0; i < count; i++) out += s;
    return out;
}

// Prints text one character at a time, like a typewriter.
inline void typeOut(const std::string& text, int msPerChar = 12) {
    if (!enabled) { std::cout << text; return; }
    for (char c : text) {
        std::cout << c << std::flush;
        // Don't pause on the middle bytes of multi-byte characters like █
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
            std::this_thread::sleep_for(std::chrono::milliseconds(msPerChar));
    }
}

// ---- one-line messages -----------------------------------------------------

inline void banner() {
    if (!enabled) return;
    std::string block = unicode ? "███" : "###";
    std::string rule  = unicode ? "─" : "-";
    std::cout << "\n " << BOLD << CYAN;
    typeOut(block + " FPMATCH " + block);
    std::cout << RESET << BOLD;
    typeOut("  FORENSIC PRINT ANALYSIS");
    std::cout << RESET << DIM << "  v0.1" << RESET << "\n";
    std::cout << " " << DIM << repeat(rule, 47) << RESET << "\n";
}

// Gray, indented detail line under a stage, like "mean 0.000  std 1.000".
inline void info(const std::string& text) {
    if (!enabled) return;
    std::cout << "     " << DIM << text << RESET << "\n";
}

inline void success(const std::string& text) {
    if (!enabled) return;
    std::cout << " " << GREEN << BOLD << "[+] " << text << RESET << "\n";
}

inline void warn(const std::string& text) {
    if (!enabled) return;
    std::cout << " " << YELLOW << "[!] " << text << RESET << "\n";
}

// ---- stages ----------------------------------------------------------------
//
// Make a Stage right before a step and call done() right after:
//
//     ui::Stage s("Normalizing");
//     normalize(img);
//     s.done("mean 0.000  std 1.000");
//
// It prints " [*] Normalizing ..." when created, then rewrites that same line
// as " [+] Normalizing  OK  4 ms  <your text>" on done(). If an exception
// skips done(), it rewrites the line as FAILED in red.
//
// While a Stage is alive, your image functions can call ui::progress(...) or
// ui::status(...) without knowing anything about the Stage.

class Stage;
inline Stage* active = nullptr;

class Stage {
public:
    explicit Stage(std::string name)
        : name_(std::move(name)), t0_(std::chrono::steady_clock::now()), prev_(active) {
        active = this;
        draw(std::string(DIM) + "..." + RESET);
    }

    ~Stage() {
        if (!finished_) finish(false, "");
        active = prev_;
    }

    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    // Redraws the bar. Only redraws when the percent changes, so calling it
    // once per row is cheap.
    void progress(long long current, long long total) {
        if (!enabled || total <= 0) return;
        int pct = static_cast<int>(current * 100 / total);
        if (pct == lastPct_) return;
        lastPct_ = pct;
        const int width = 24;
        int filled = pct * width / 100;
        std::string on  = unicode ? "█" : "#";
        std::string off = unicode ? "░" : "-";
        std::string bar = std::string(CYAN) + repeat(on, filled) + DIM +
                          repeat(off, width - filled) + RESET;
        std::string pctText = std::to_string(pct) + "%";
        draw("[" + bar + "] " + pad(pctText, 4));
    }

    // Replaces the bar with text, for steps with no known total
    // (like thinning: "pass 3   removed 4,812 px").
    void status(const std::string& text) {
        draw(std::string(DIM) + text + RESET);
    }

    void done(const std::string& detail = "") { finish(true, detail); }

    long long elapsedMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0_).count();
    }

private:
    void draw(const std::string& tail) {
        if (!enabled) return;
        std::cout << CLEAR_LINE << " " << CYAN << "[*]" << RESET << " "
                  << pad(name_, nameWidth) << tail << std::flush;
    }

    void finish(bool ok, const std::string& detail) {
        finished_ = true;
        if (!enabled) return;
        // wait out the rest of minStageMs so the "..." line can be seen
        long long left = minStageMs - elapsedMs();
        if (ok && left > 0) std::this_thread::sleep_for(std::chrono::milliseconds(left));
        std::cout << CLEAR_LINE << " ";
        if (ok) {
            std::cout << GREEN << "[+]" << RESET << " " << pad(name_, nameWidth)
                      << GREEN << BOLD << "OK" << RESET << "  "
                      << DIM << pad(num(elapsedMs()) + " ms", 9) << RESET;
            if (!detail.empty()) std::cout << "  " << detail;
        } else {
            std::cout << RED << "[x]" << RESET << " " << pad(name_, nameWidth)
                      << RED << BOLD << "FAILED" << RESET;
        }
        std::cout << "\n" << std::flush;
    }

    std::string name_;
    std::chrono::steady_clock::time_point t0_;
    Stage* prev_;
    int lastPct_ = -1;
    bool finished_ = false;
};

// Call these from inside your image functions. They do nothing when no
// Stage is active, so the functions still work without the UI.
inline void progress(long long current, long long total) {
    if (active) active->progress(current, total);
}

inline void status(const std::string& text) {
    if (active) active->status(text);
}

// ---- questions and answers ---------------------------------------------------
//
// These print even when the UI is off, because they're the actual results
// (or a question the user has to answer). They just skip the colors then.

// Colors only when the UI is on, so piped output stays plain text.
inline std::string paint(const char* color, const std::string& text) {
    if (!enabled) return text;
    return std::string(color) + text + RESET;
}

// Asks a question and returns the answer typed in, without the Enter.
// Returns an empty string if input has run out (like a closed pipe).
inline std::string ask(const std::string& question) {
    std::cout << " " << paint(CYAN, "[?]") << " " << question << " " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) return "";
    return answer;
}

// Shows a list of choices like " [a] Catalogue this print" and returns the
// letter picked. Keeps asking until it gets a letter from the list. Returns
// 'q' if input runs out, so a closed pipe can't loop forever.
inline char menu(const std::string& title, const std::string& letters,
                 const std::string* labels) {
    std::string rule = unicode ? "─" : "-";
    std::cout << "\n " << paint(DIM, repeat(rule, 47)) << "\n";
    std::cout << " " << paint(BOLD, title) << "\n";
    for (size_t i = 0; i < letters.size(); i++) {
        std::cout << "  " << paint(CYAN, std::string("[") + letters[i] + "]") << " " << labels[i] << "\n";
    }
    while (true) {
        std::cout << " " << paint(CYAN, ">") << " " << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer)) return 'q';
        if (answer.size() == 1 && letters.find(answer[0]) != std::string::npos) {
            return answer[0];
        }
        std::cout << " " << paint(YELLOW, "[!] pick one of: " + letters) << "\n";
    }
}

// A big MATCH or NO MATCH line with the score after it.
inline void verdict(bool match, const std::string& text) {
    if (match) std::cout << "\n " << paint(GREEN, BOLD + std::string(" MATCH ")) << "     " << text << "\n";
    else       std::cout << "\n " << paint(RED, BOLD + std::string(" NO MATCH ")) << "  " << text << "\n";
}

// One row of a table. Each cell is padded to its width so columns line up.
// Pass header = true for the title row, which prints bold.
inline void tableRow(const std::string* cells, const int* widths, int count, bool header = false) {
    std::string line;
    for (int i = 0; i < count; i++) line += pad(cells[i], widths[i]) + "  ";
    if (header) std::cout << "  " << paint(BOLD, line) << "\n";
    else        std::cout << "  " << line << "\n";
}

// A plain line of text that always prints, for results like the EER.
inline void line(const std::string& text) {
    std::cout << " " << text << "\n";
}

// ---- final screen ------------------------------------------------------------
//
// Prints a final line with a blinking SUCCESS (or FAILED) badge and waits for
// the user to press Enter. The blinking runs on a second thread while the
// main thread waits for input, then the line settles to a solid badge.
// When the UI is off (output going to a file, or fpeval), it just prints the
// line and returns without waiting.

inline void finalScreen(bool ok, const std::string& text) {
    const char* color = ok ? GREEN : RED;
    std::string badge = ok ? " SUCCESS " : " FAILED ";

    if (!enabled) {
        std::cout << badge << " " << text << "\n";
        return;
    }

    auto drawLine = [&](bool lit) {
        std::cout << CLEAR_LINE << " ";
        if (lit) std::cout << BOLD << "\033[7m" << color << badge << RESET;   // inverted badge
        else     std::cout << DIM << std::string(badge.size(), ' ') << RESET;
        std::cout << "  " << text << DIM << "   [press ENTER to exit]" << RESET << std::flush;
    };

    std::cout << "\n";
    std::atomic<bool> stop{false};
    std::thread blinker([&] {
        bool lit = true;
        while (!stop) {
            drawLine(lit);
            lit = !lit;
            for (int i = 0; i < 10 && !stop; i++)        // 500 ms, checked often
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    std::string ignored;
    std::getline(std::cin, ignored);   // waits for Enter
    stop = true;
    blinker.join();

    std::cout << "\033[1A";            // Enter moved the cursor down, go back up
    drawLine(true);
    std::cout << "\n\n" << std::flush;
}

} // namespace ui