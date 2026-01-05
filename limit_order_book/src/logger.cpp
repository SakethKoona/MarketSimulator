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

// TODO: too tired but add some sort of os make dirs check for the logs folder
// Or we could just add it into the makefile so it's always there
Logger::Logger(const std::string &filename) : logFile_("logs/" + filename) {
    if (!logFile_) {
        throw std::runtime_error("Could not find output log file");
    }
}

void Logger::log(Level level, const std::string &msg) {
    logFile_ << "[" << levelToString(level) << "]" << " " << msg << "\n";
}
