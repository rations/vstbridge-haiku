# Makefile for yabridgectl
# Dependencies are fetched automatically with 'make deps' or on first build.
# Requirements: g++ (C++20), git
#
# The GUI target differs per platform. On Haiku it is the native Interface Kit
# front-end in src/haiku (binary 'VSTBridge'); everywhere else it is the GTK3
# one in src/gui (binary 'vstbridgectl-gtk'). The CLI is the same on both.

UNAME_S := $(shell uname -s)

CXX      ?= g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra $(CXXFLAGS)

ifeq ($(UNAME_S),Haiku)
# Haiku has no libdl — dlopen and friends live in libroot, and there is no
# /boot/system/develop/lib/libdl.a to link against. Passing -ldl here only ever
# worked because of a hand-made stub archive in the non-packaged tree.
LDFLAGS  := -lpthread $(LDFLAGS)
else
LDFLAGS  := -ldl -lpthread $(LDFLAGS)
endif

BUILD_DIR := build
DEP_DIR   := deps

CLI11_URL        := https://github.com/CLIUtils/CLI11.git
CLI11_TAG        := v2.4.2
NLOHMANN_URL     := https://github.com/nlohmann/json.git
NLOHMANN_TAG     := v3.11.3
TOMLPP_URL       := https://github.com/marzer/tomlplusplus.git
TOMLPP_TAG       := v3.4.0

INCLUDES := \
	-Isrc \
	-I$(DEP_DIR)/cli11/include \
	-I$(DEP_DIR)/nlohmann/single_include \
	-I$(DEP_DIR)/tomlplusplus/include

SRCS := \
	src/main.cpp \
	src/config.cpp \
	src/files.cpp \
	src/util.cpp \
	src/symbols.cpp \
	src/vst3_moduleinfo.cpp \
	src/actions.cpp \
	src/actions/blacklist.cpp \
	src/plugin_config.cpp

OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))

TARGET := $(BUILD_DIR)/vstbridgectl

# Object files shared by the CLI and whichever GUI is being built.
CORE_OBJS := \
	$(BUILD_DIR)/config.o \
	$(BUILD_DIR)/files.o \
	$(BUILD_DIR)/util.o \
	$(BUILD_DIR)/symbols.o \
	$(BUILD_DIR)/vst3_moduleinfo.o \
	$(BUILD_DIR)/actions.o \
	$(BUILD_DIR)/actions/blacklist.o \
	$(BUILD_DIR)/plugin_config.o

# ─── GUI: Haiku (Interface Kit) ───────────────────────────────────────────────

# -lbe for the Interface/Application kits, -ltracker for BFilePanel, which
# libbe does not export.
HAIKU_GUI_LIBS   := -lbe -ltracker
HAIKU_GUI_OBJS   := \
	$(BUILD_DIR)/haiku/main.o \
	$(BUILD_DIR)/haiku/VstbridgeApp.o \
	$(BUILD_DIR)/haiku/MainWindow.o \
	$(BUILD_DIR)/haiku/ActionRunner.o \
	$(BUILD_DIR)/haiku/OutputView.o \
	$(CORE_OBJS)
HAIKU_GUI_TARGET := $(BUILD_DIR)/VSTBridge
HAIKU_RDEF       := VSTBridge.rdef

# ─── GUI: GTK3 (everything else) ──────────────────────────────────────────────

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs   gtk+-3.0 2>/dev/null)

GTK_GUI_OBJS := \
	$(BUILD_DIR)/gui_main.o \
	$(BUILD_DIR)/gui/app_window.o \
	$(CORE_OBJS)

GTK_GUI_TARGET := $(BUILD_DIR)/vstbridgectl-gtk

ifeq ($(UNAME_S),Haiku)
GUI_TARGET := $(HAIKU_GUI_TARGET)
else
GUI_TARGET := $(GTK_GUI_TARGET)
endif

.PHONY: all deps clean install

all: deps $(TARGET) $(GUI_TARGET)

# ─── Dependency fetch ─────────────────────────────────────────────────────────

$(DEP_DIR)/cli11/.git:
	git clone --depth 1 --branch $(CLI11_TAG) $(CLI11_URL) $(DEP_DIR)/cli11

$(DEP_DIR)/nlohmann/.git:
	git clone --depth 1 --branch $(NLOHMANN_TAG) $(NLOHMANN_URL) $(DEP_DIR)/nlohmann

$(DEP_DIR)/tomlplusplus/.git:
	git clone --depth 1 --branch $(TOMLPP_TAG) $(TOMLPP_URL) $(DEP_DIR)/tomlplusplus

deps: $(DEP_DIR)/cli11/.git $(DEP_DIR)/nlohmann/.git $(DEP_DIR)/tomlplusplus/.git

# ─── Build rules ──────────────────────────────────────────────────────────────

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR) $(BUILD_DIR)/actions deps
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/actions/%.o: src/actions/%.cpp | $(BUILD_DIR)/actions deps
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/actions:
	mkdir -p $@

$(BUILD_DIR)/gui:
	mkdir -p $@

$(BUILD_DIR)/haiku:
	mkdir -p $@

# ─── Haiku GUI build rules ────────────────────────────────────────────────────

$(BUILD_DIR)/haiku/%.o: src/haiku/%.cpp | $(BUILD_DIR)/haiku deps
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Link, then compile the resource definitions and attach them to the binary, the
# same order Haiku's own makefile-engine uses. mimeset stamps the file type from
# the attached app_signature, which is what puts the app in Deskbar's list.
$(HAIKU_GUI_TARGET): $(HAIKU_GUI_OBJS) $(HAIKU_RDEF)
	$(CXX) $(CXXFLAGS) -o $@ $(HAIKU_GUI_OBJS) $(LDFLAGS) $(HAIKU_GUI_LIBS)
	rc -o $(BUILD_DIR)/VSTBridge.rsrc $(HAIKU_RDEF)
	xres -o $@ $(BUILD_DIR)/VSTBridge.rsrc
	mimeset -f $@

# ─── GTK GUI build rules (need GTK_CFLAGS) ────────────────────────────────────

$(BUILD_DIR)/gui_main.o: src/gui_main.cpp | $(BUILD_DIR) deps
	$(CXX) $(CXXFLAGS) $(GTK_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/gui/app_window.o: src/gui/app_window.cpp | $(BUILD_DIR)/gui deps
	$(CXX) $(CXXFLAGS) $(GTK_CFLAGS) $(INCLUDES) -c $< -o $@

$(GTK_GUI_TARGET): $(GTK_GUI_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(GTK_LIBS)

# ─── Install / clean ──────────────────────────────────────────────────────────

ifeq ($(UNAME_S),Haiku)
# Installation on Haiku is the .hpkg built by packaging/make-hpkg.sh, which
# places the GUI in apps/ and the CLI in bin/. There is no install-into-PREFIX
# equivalent, so this target only exists to say so.
install:
	@echo "On Haiku, build and install the package instead:"
	@echo "    sh ../../packaging/make-hpkg.sh"
	@false
else
install: $(TARGET) $(GTK_GUI_TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/vstbridgectl
	install -Dm755 $(GTK_GUI_TARGET) $(DESTDIR)$(PREFIX)/bin/vstbridgectl-gtk
endif

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf $(DEP_DIR)
