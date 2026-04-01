CXX = g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wno-unused-parameter
LDFLAGS = -lncursesw

TARGET = ttop
SRCS = ttop.cpp
PREFIX = /usr

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install:
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(TARGET) $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)