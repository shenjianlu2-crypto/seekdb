CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=

SRC := src/common.h src/sql.cpp src/store.cpp src/engine.cpp src/main.cpp

minivectordb: $(SRC) src/sql.h src/store.h src/engine.h
	$(CXX) $(CXXFLAGS) -o $@ src/sql.cpp src/store.cpp src/engine.cpp src/main.cpp $(LDFLAGS)

clean:
	rm -f minivectordb

.PHONY: clean