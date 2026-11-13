CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter -fopenmp
LDFLAGS ?= -fopenmp

SRC := src/common.h src/json.h src/sql.h src/store.h src/engine.h \
       src/json.cpp src/sql.cpp src/store.cpp src/engine.cpp src/main.cpp

minivectordb: $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ src/json.cpp src/sql.cpp src/store.cpp src/engine.cpp src/main.cpp $(LDFLAGS)

clean:
	rm -f minivectordb

.PHONY: clean
