CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread
OPENCV   := $(shell pkg-config --cflags --libs opencv4)

SRC := $(wildcard src/*.cpp)
HDR := $(wildcard src/*.hpp)
BIN := build/fpmatch
IMG ?= data/101_1.tif

# make        -> compile only
# make run    -> compile if needed, then run on IMG
# make run IMG=data/102_3.tif   -> run on a different print
# make clean  -> delete build/ and out/

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN) $(OPENCV)

run: $(BIN)
	./$(BIN) $(IMG)

clean:
	rm -rf build out

.PHONY: all run clean