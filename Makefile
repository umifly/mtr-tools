CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Isrc -Wall
SYSTEM_LIBS = -pthread

all: mtr-tools test_analyzer

mtr-tools: src/main.cpp src/core/prober.cpp src/core/prober_linux.cpp src/core/geo_asn.cpp src/core/analyzer.cpp src/tui/tui_app.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(SYSTEM_LIBS)

test_analyzer: tests/test_analyzer.cpp src/core/geo_asn.cpp src/core/analyzer.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(SYSTEM_LIBS)

test: test_analyzer
	./test_analyzer

clean:
	rm -f mtr-tools test_analyzer *.o

.PHONY: all test clean
