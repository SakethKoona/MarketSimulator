#include "logger.hpp"
#include <stdexcept>
#include <string>

std::string levelToString(Level level) {
    switch (level) {
    // case Level::DEBUG:
    //     return "DEBUG";
    case Level::INFO:
        return "INFO";
    case Level::TRACE:
        return "TRACE";
    case Level::WARNING:
        return "WARNING";
    case Level::ERROR:
        return "ERROR";
    }
}

Logger::Logger(const std::string &filename)
    : logFile_("logs/" + filename, std::ios::app) {
    if (!logFile_) {
        throw std::runtime_error("Could not find output log file");
    }
}

void Logger::log(Level level, const std::string &msg) {
    logFile_ << "[" << levelToString(level) << "]" << " " << msg << "\n";
}
