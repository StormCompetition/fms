CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic

storm_robot: storm_robot.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f storm_robot

.PHONY: clean
