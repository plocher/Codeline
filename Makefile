# One command builds everything and runs every functional scenario on a host:
#
#     make check
#
# scenarios/*.cpp  each is a program with its own main(); built into build/
# scenarios/*.py   each is run from the repository root through uv, which
#                  installs what pyproject.toml lists on first use
#
# Everything under src/ is header-only C++17 for now, except the host's main
# (src/codeline_host.cpp), the one program linked with libmosquitto.
#
# build/<Station>.station  a station's chart and plant, written from the project
#                          file by tools/station_file.py; scenarios and the host
#                          read it

CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -g
INCLUDES := -I src -I scenarios
PYTHON   ?= uv run --quiet python

HEADERS      := $(shell find src scenarios -name '*.h')
CPP_SOURCES  := $(wildcard scenarios/*.cpp)
CPP_PROGRAMS := $(patsubst scenarios/%.cpp,build/%,$(CPP_SOURCES))
PY_SCENARIOS := $(wildcard scenarios/*.py)
STATIONS     := build/Corporal.station build/Sargent.station build/Luchessa.station
PROJECT      := tools/spcoast.layout.json

# libmosquitto, for build/codeline_host only (Homebrew's prefix by default).
MOSQUITTO_PREFIX ?= /opt/homebrew
MOSQUITTO_CFLAGS ?= -I$(MOSQUITTO_PREFIX)/include
MOSQUITTO_LIBS   ?= -L$(MOSQUITTO_PREFIX)/lib -lmosquitto

.PHONY: check build clean

check: build
	@fail=0; \
	for p in $(CPP_PROGRAMS); do echo "$$p"; $$p || fail=1; done; \
	for s in $(PY_SCENARIOS); do echo "$$s"; $(PYTHON) $$s || fail=1; done; \
	if [ $$fail -eq 0 ]; then echo "ALL SCENARIOS PASSED"; else echo "SCENARIOS FAILED"; exit 1; fi

build: $(STATIONS) $(CPP_PROGRAMS) build/codeline_host

build/%.station: tools/station_file.py tools/codeline_chart.py $(PROJECT)
	@mkdir -p build
	$(PYTHON) tools/station_file.py $(PROJECT) --station $* > $@.tmp && mv $@.tmp $@

build/codeline_host: src/codeline_host.cpp $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(MOSQUITTO_CFLAGS) $< -o $@ $(MOSQUITTO_LIBS)

build/%: scenarios/%.cpp $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

clean:
	rm -rf build
