# # Compiler and flags
# CXX = g++
# CXXFLAGS = -std=c++23 -Wall -Wextra -O2 -g
# INCLUDES = -Iinclude -Ithird_party
#
# DEBUGFLAGS = -std=c++23 -Wall -Wextra -O0 -g -DDEBUG
#
# # Directories
# SRC_DIR = src
# INCLUDE_DIR = include
# BUILD_DIR = build
# BIN_DIR = bin
# LOG_DIR = logs
#
# # Source files
# SOURCES = $(SRC_DIR)/main.cpp \
#           $(SRC_DIR)/orderbook.cpp \
#           $(SRC_DIR)/engine.cpp \
# 		  $(SRC_DIR)/arena.cpp \
# 			$(SRC_DIR)/logger.cpp
#
# #Object files
# OBJECTS = $(SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
#
# # Target executable
# TARGET = $(BIN_DIR)/main
#
# # Default target
# all: directories $(TARGET)
#
# debug: CXXFLAGS = $(DEBUGFLAGS)
# debug: clean all
#
# # Create necessary directories
# directories:
# 	@mkdir -p $(BUILD_DIR)
# 	@mkdir -p $(BIN_DIR)
# 	@mkdir -p ${LOG_DIR}
#
# # Link object files to create executable
# $(TARGET): $(OBJECTS)
# 	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $(TARGET)
# 	@echo "Build complete: $(TARGET)"
#
# # Compile source files to object files
# $(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
# 	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
#
# # Clean build artifacts
# clean:
# 	rm -rf $(BUILD_DIR) $(BIN_DIR)
# 	@echo "Clean complete"
#
# # Run the program
# run: $(TARGET)
# 	./$(TARGET)
#
# # Rebuild everything
# rebuild: clean all
#
# # Debug build
# debug: CXXFLAGS += -g -DDEBUG
# debug: clean all
#
# clear:
# 	rm -rf logs
# 	@echo "Cleared All Logs"
#
# .PHONY: all clean run rebuild debug directories

# =========================
# Compiler
# =========================
CXX = clang++

# =========================
# Flags
# =========================
COMMON_FLAGS  = -std=c++23 -Wall -Wextra
DEBUG_FLAGS   = $(COMMON_FLAGS) -O0 -g -DDEBUG
RELEASE_FLAGS = $(COMMON_FLAGS) -O2

# Default to release
CXXFLAGS = $(RELEASE_FLAGS)

INCLUDES = -Iinclude -Ithird_party

# =========================
# Directories
# =========================
SRC_DIR     = src
INCLUDE_DIR = include
BUILD_DIR   = build
BIN_DIR     = bin
LOG_DIR     = logs

# =========================
# Sources / Objects
# =========================
SOURCES = \
	$(SRC_DIR)/main.cpp \
	$(SRC_DIR)/orderbook.cpp \
	$(SRC_DIR)/engine.cpp \
	$(SRC_DIR)/arena.cpp \
	$(SRC_DIR)/logger.cpp

OBJECTS = $(SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)

# =========================
# Target
# =========================
TARGET = $(BIN_DIR)/main

# =========================
# Targets
# =========================

all: directories $(TARGET)

# Debug build
debug: CXXFLAGS = $(DEBUG_FLAGS)
debug: clean all

# Create directories
directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(LOG_DIR)

# Link
$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $(TARGET)
	@echo "Build complete: $(TARGET)"

# Compile
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Run
run: $(TARGET)
	./$(TARGET)

# Clean
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Clean complete"

# Rebuild
rebuild: clean all

# Clear logs
clear:
	rm -rf $(LOG_DIR)
	@echo "Cleared all logs"

.PHONY: all debug clean run rebuild directories clear
