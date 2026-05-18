CXXFLAGS ?= -O2
WARNFLAGS = -Wall -Wextra -Wpedantic -Wno-c++11-narrowing -Wno-gnu-string-literal-operator-template
CXXFLAGS += -shared -fPIC -std=c++2b $(WARNFLAGS)

INCLUDES = `pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon`
LIBS = `pkg-config --libs libsystemd`

SRC = src/main.cpp
TARGET = hyprcaffeine.so

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LIBS)

clean:
	rm -f ./$(TARGET)

install: $(TARGET)
	install -Dm644 $(TARGET) ~/.hyprplugins/hyprcaffeine/$(TARGET)

uninstall:
	rm -f ~/.hyprplugins/hyprcaffeine/$(TARGET)

.PHONY: all clean install uninstall
