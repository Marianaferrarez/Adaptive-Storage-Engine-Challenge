CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude -Ithird_party

BIN := engine
SRCS := src/engine.cpp
HEADERS := include/engine.hpp

.PHONY: all clean test

all: $(BIN)

$(BIN): src/main.cpp $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) src/main.cpp $(SRCS) -o $(BIN)

crash_test: experiments/crash_recovery_test.cpp $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) experiments/crash_recovery_test.cpp $(SRCS) -o crash_test

smoke_test: experiments/smoke_test.cpp $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) experiments/smoke_test.cpp $(SRCS) -o smoke_test

benchmark: experiments/benchmark.cpp $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) experiments/benchmark.cpp $(SRCS) -o benchmark

test: smoke_test crash_test
	./smoke_test
	./crash_test

clean:
	rm -f $(BIN) crash_test smoke_test benchmark
	rm -rf data
