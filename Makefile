CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += -Isrc

BUILD   := build
BIN     := $(BUILD)/engine
SRCS    := src/engine.cpp
HEADERS := src/engine.hpp

.PHONY: all test unit clean zip

all: $(BIN)

$(BIN): src/main.cpp $(SRCS) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) src/main.cpp $(SRCS) -o $@

$(BUILD)/unit_tests: tests/unit_tests.cpp $(SRCS) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) tests/unit_tests.cpp $(SRCS) -o $@

$(BUILD)/benchmark: experiments/benchmark.cpp $(SRCS) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) experiments/benchmark.cpp $(SRCS) -o $@

unit: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests

# Suíte completa (unitários + workloads + persistência + corrupção + contrato).
test: $(BIN) $(BUILD)/unit_tests
	bash tests/run_tests.sh

zip:
	bash tests/make_zip.sh

clean:
	rm -rf $(BUILD)
