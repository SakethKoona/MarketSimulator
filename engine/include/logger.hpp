#pragma once

#include <chrono>
#include <fstream>
#include <string>

enum class Level {
    INFO,
    TRACE,
    WARNING,
    ERROR,
};

std::string levelToString(Level level);

class Logger {
  public:
    Logger(const std::string &filename);

    // Delete the copy and move constructors
    Logger(const Logger &) = delete;
    Logger(Logger &&) = delete;

    // Delete assignment
    Logger &operator=(const Logger &) = delete;
    Logger &&operator=(Logger &&) = delete;

    void log(Level level, const std::string &msg);

    template <typename Func> void TimeFunction(Func f) {
        auto start = std::chrono::steady_clock::now();
        f();

        auto end = std::chrono::steady_clock::now();
        auto duration =
            duration_cast<std::chrono::milliseconds>(end - start).count();

        log(Level::INFO, "Elapsed Time: " + std::to_string(duration) + "ms");
    }

  private:
    std::ofstream logFile_;
};
