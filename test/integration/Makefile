# ─────────────────────────────────────────────────────────────────────
#  Integration test Makefile for RoboControl
#
#  Targets:
#    all              Build all test binaries
#    relay_lossy      Relay server with configurable packet loss
#    signal_server    Minimal signaling server
#    robot_sender     Robot process (sends 2× H.264 streams)
#    remote_receiver  Remote control process (receives + validates)
#    run              Run the full integration test suite
#    clean            Remove build artifacts
#
#  Dependencies: libssl-dev
# ─────────────────────────────────────────────────────────────────────

CC       ?= gcc
CSTD     := -std=c11
CFLAGS   := -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE $(CSTD) -Wall -Wextra \
            -O2 -g -I. -I../../include
LDFLAGS  := -lm

SRCDIR   := .
BINDIR   := ../../build/integration

# Source files
RELAY_SRC    := relay_lossy.c
SIGNAL_SRC   := signal_server.c
ROBOT_SRC    := robot_sender.c
REMOTE_SRC   := remote_receiver.c

# Binary targets
RELAY_BIN    := $(BINDIR)/relay_lossy
SIGNAL_BIN   := $(BINDIR)/signal_server
ROBOT_BIN    := $(BINDIR)/robot_sender
REMOTE_BIN   := $(BINDIR)/remote_receiver

BINS := $(RELAY_BIN) $(SIGNAL_BIN) $(ROBOT_BIN) $(REMOTE_BIN)

.PHONY: all clean run

all: $(BINS)

$(BINDIR):
	mkdir -p $(BINDIR)

$(RELAY_BIN): $(RELAY_SRC) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $(RELAY_SRC) $(LDFLAGS)

$(SIGNAL_BIN): $(SIGNAL_SRC) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $(SIGNAL_SRC) $(LDFLAGS)

$(ROBOT_BIN): $(ROBOT_SRC) pacing.h h264_gen.h | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $(ROBOT_SRC) $(LDFLAGS)

$(REMOTE_BIN): $(REMOTE_SRC) latency_stats.h h264_gen.h | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $(REMOTE_SRC) $(LDFLAGS)

run: all
	./run_integration.sh

clean:
	rm -rf $(BINDIR)
	rm -f *_report.txt
