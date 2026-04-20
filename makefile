CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -O2
RAYLIB_FLAGS := $(shell pkg-config --cflags --libs raylib)
LIBS := $(RAYLIB_FLAGS) -lpthread -ldl -lm

SRC_CPP := main.cpp
OBJ := $(SRC_CPP:.cpp=.o)

TARGET := visualizer_demo

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)
