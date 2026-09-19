CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra $(shell fltk-config --cxxflags)
LDFLAGS = $(shell fltk-config --ldflags) -lcurl -ltag -lfltk_images

TARGET = musicDL
SRC = main.cpp

$(TARGET) : $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
