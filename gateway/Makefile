# Gateway, protocol and feed code. Compiles the engine sources from
# ../limit_order_book directly so the engine Makefile stays untouched.
CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -O2 -g
LOB       = ../limit_order_book
INCLUDES  = -Iinclude -I$(LOB)/include -I$(LOB)/third_party
BUILD     = build

ENGINE_SRC = $(LOB)/src/orderbook.cpp $(LOB)/src/engine.cpp $(LOB)/src/arena.cpp \
             $(LOB)/src/logger.cpp $(LOB)/src/exchange.cpp $(LOB)/src/sequencer.cpp \
             $(LOB)/src/events.cpp
ENGINE_OBJ = $(patsubst $(LOB)/src/%.cpp,$(BUILD)/engine/%.o,$(ENGINE_SRC))

GATEWAY_SRC = src/feed_publisher.cpp src/boe_server.cpp src/ingress/core.cpp src/ingress/plugin.cpp
GATEWAY_OBJ = $(patsubst src/%.cpp,$(BUILD)/%.o,$(GATEWAY_SRC))

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  SHLIB_EXT = dylib
  SHLIB_FLAGS = -dynamiclib
else
  SHLIB_EXT = so
  SHLIB_FLAGS = -shared
  LDLIBS += -ldl
endif
PLUGINS = $(BUILD)/jsonl_ingress.$(SHLIB_EXT)

TESTS = $(BUILD)/protocol_test $(BUILD)/spsc_ring_test $(BUILD)/mpmc_ring_test $(BUILD)/feed_reconstruct_test
BINS  = $(BUILD)/exchange_server $(BUILD)/feed_dump $(BUILD)/flowgen $(PLUGINS)

all: $(BINS) test

$(BUILD) $(BUILD)/engine:
	@mkdir -p $@

$(BUILD)/engine/%.o: $(LOB)/src/%.cpp | $(BUILD)/engine
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -c $< -o $@

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -c $< -o $@

$(BUILD)/jsonl_ingress.$(SHLIB_EXT): plugins/jsonl_ingress.cpp include/ingress/api.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -fPIC $(SHLIB_FLAGS) -Iinclude $< -o $@

$(BUILD)/exchange_server: src/exchange_server.cpp $(GATEWAY_OBJ) $(ENGINE_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LDLIBS)

$(BUILD)/feed_dump: tools/feed_dump.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(BUILD)/flowgen: tools/flowgen.cpp include/protocol/boe.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(BUILD)/feed_reconstruct_test: tests/feed_reconstruct_test.cpp $(ENGINE_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(BUILD)/%_test: tests/%_test.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done
	@./$(BUILD)/feed_reconstruct_test 100000 3

clean:
	rm -rf $(BUILD)

-include $(BUILD)/*.d $(BUILD)/engine/*.d $(BUILD)/ingress/*.d
.PHONY: all test clean
