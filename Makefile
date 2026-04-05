CXX := clang++
CXXFLAGS := -std=c++20 -O3 -Wall -Wextra -Wpedantic -pthread -Iinclude
LDFLAGS := -pthread

BUILD_DIR := build
SRC_DIR := src
BIN := $(BUILD_DIR)/amb_graph

SOURCES := $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))

.PHONY: all clean run sample demo benchmark-graph

all: $(BIN)

$(BIN): $(OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN) --graph data/sample_graph.edgelist --algorithm pagerank --iterations 15 --threads 4

sample: $(BIN)
	./$(BIN) --graph data/sample_graph.edgelist --algorithm bfs --source 0 --threads 4 --emit-trace web/trace.json

demo: $(BIN)
	python3 tools/server.py

benchmark-graph:
	python3 tools/generate_powerlaw_graph.py --output data/powerlaw_graph.edgelist --vertices 4096 --fanout 12

clean:
	rm -rf $(BUILD_DIR) web/trace.json web/benchmark.json
