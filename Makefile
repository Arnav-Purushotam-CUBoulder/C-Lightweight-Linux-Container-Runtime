# ==============================================================================
# Makefile — linuc container runtime
# ==============================================================================
# Targets:
#   make          — build the linuc binary
#   make clean    — remove build artifacts
#   make test     — run integration tests  (requires root / sudo)
#   make bench    — run benchmarks         (requires root)
#   make install  — install to /usr/local/bin
# ==============================================================================

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g
LDFLAGS  := -lcap   # libcap for capability management

SRCDIR   := src
BENCHDIR := bench
BUILDDIR := build

# Source files
SRCS := $(SRCDIR)/cgroup.cpp   \
        $(SRCDIR)/caps.cpp     \
        $(SRCDIR)/runtime.cpp  \
        $(SRCDIR)/supervisor.cpp \
        $(SRCDIR)/main.cpp     \
        $(BENCHDIR)/bench.cpp

OBJS := $(patsubst %.cpp, $(BUILDDIR)/%.o, $(SRCS))

TARGET := $(BUILDDIR)/linuc

.PHONY: all clean test bench install

all: $(TARGET)

# ── Link ──────────────────────────────────────────────────────────────────────
$(TARGET): $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@"

# ── Compile ───────────────────────────────────────────────────────────────────
$(BUILDDIR)/$(SRCDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -c $< -o $@

$(BUILDDIR)/$(BENCHDIR)/%.o: $(BENCHDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -c $< -o $@

# ── Test ─────────────────────────────────────────────────────────────────────
test: $(TARGET)
	@echo "Running integration tests..."
	@bash tests/test.sh $(TARGET)

# ── Bench ────────────────────────────────────────────────────────────────────
bench: $(TARGET)
	@echo "Running benchmarks (needs root for cgroup tests)..."
	sudo $(TARGET) bench --iterations 20 | tee bench_results.ndjson
	@echo "Results saved to bench_results.ndjson"
	@echo "Tip: pipe through 'jq .' for pretty printing"

# ── Install ───────────────────────────────────────────────────────────────────
install: $(TARGET)
	sudo install -m 755 $(TARGET) /usr/local/bin/linuc
	@echo "Installed to /usr/local/bin/linuc"

# ── Clean ────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILDDIR) bench_results.ndjson
