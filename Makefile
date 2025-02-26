CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread -lssl -lcrypto

TARGET = server
SRC = server.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)