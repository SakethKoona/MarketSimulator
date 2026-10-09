# Top-level driver. CMake builds the C++ (engine, gateway, plugins, tests);
# cargo builds feedviz. `make demo` runs everything in tmux.
BUILD ?= build
CMAKE_BUILD_TYPE ?= Release
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

.PHONY: all cpp rust test test-cpp test-rust demo clean format capture

all: cpp rust

cpp:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) >/dev/null
	cmake --build $(BUILD) -j $(JOBS)

rust:
	cd clients/feedviz && cargo build --release

test: test-cpp test-rust

test-cpp: cpp
	cd $(BUILD) && ctest --output-on-failure

test-rust:
	cd clients/feedviz && cargo test --release

demo: all
	scripts/demo.sh

# Record a feed capture for feedviz's golden test and replay mode.
capture: cpp
	$(BUILD)/gateway/exchange_server configs/default.json --rate 3000 --seconds 3 --quiet \
	  --capture clients/feedviz/testdata/capture.bin

format:
	find engine gateway -name '*.cpp' -o -name '*.hpp' -o -name '*.h' | grep -v third_party | xargs clang-format -i
	cd clients/feedviz && cargo fmt

clean:
	rm -rf $(BUILD)
	cd clients/feedviz && cargo clean
