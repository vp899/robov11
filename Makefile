# ============================================================================
#  RoboControl — Production build system
# ============================================================================
#
#  Targets:
#    all        Build everything (lib, relay, signaling, auth, billing)
#    lib        Build the core static/shared library
#    relay      Build the relay server binary
#    signaling  Build the signaling server binary
#    auth       Build the auth service binary
#    billing    Build the billing service binary
#    test       Build and run the test suite
#    clean      Remove all build artefacts
#    install    Install binaries to PREFIX/bin
#
#  Configurations:
#    make                   — release build (-O2)
#    make DEBUG=1           — debug build (-O0 -g -fsanitize=address)
#    make V=1               — verbose output
#
# ============================================================================

# --- Toolchain ---------------------------------------------------------------
CC        ?= gcc
AR        ?= ar
INSTALL   ?= install
MKDIR     ?= mkdir -p

# --- Directories -------------------------------------------------------------
SRCDIR    := src
INCDIR    := include
BUILDDIR  := build
BINDIR    := $(BUILDDIR)/bin
LIBDIR    := $(BUILDDIR)/lib
OBJDIR    := $(BUILDDIR)/obj

PREFIX    ?= /usr/local

# --- Source files ------------------------------------------------------------
CORE_SRCS := $(SRCDIR)/core/rc_proto.c      \
             $(SRCDIR)/core/rc_conn.c       \
             $(SRCDIR)/core/rc_crypto.c     \
             $(SRCDIR)/core/rc_handshake.c  \
             $(SRCDIR)/core/rc_congestion.c \
             $(SRCDIR)/core/rc_retransmit.c \
             $(SRCDIR)/core/rc_p2p.c        \
             $(SRCDIR)/core/rc_relay.c      \
             $(SRCDIR)/core/rc_epoll.c      \
             $(SRCDIR)/stubs/hiredis_stub.c

RELAY_SRCS    := $(SRCDIR)/relay/relay_server.c
SIGNALING_SRCS := $(SRCDIR)/signaling/signaling_server.c
AUTH_SRCS     := $(SRCDIR)/auth/auth_service.c
BILLING_SRCS  := $(SRCDIR)/billing/billing_service.c
TEST_SRCS     := $(wildcard test/test_*.c)

# --- Object files ------------------------------------------------------------
CORE_OBJS     := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(CORE_SRCS))
RELAY_OBJS    := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(RELAY_SRCS))
SIGNALING_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SIGNALING_SRCS))
AUTH_OBJS     := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(AUTH_SRCS))
BILLING_OBJS  := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(BILLING_SRCS))
TEST_OBJS     := $(patsubst %.c,$(OBJDIR)/%.o,$(TEST_SRCS))

# --- Libraries ---------------------------------------------------------------
STATIC_LIB    := $(LIBDIR)/librobocontrol.a
SHARED_LIB    := $(LIBDIR)/librobocontrol.so

LIBS          := -lssl -lcrypto -lpthread
TEST_LIBS     := $(LIBS)

# --- Compiler flags ----------------------------------------------------------
CSTD        := -std=c11
WARNINGS    := -Wall -Wextra -Werror -Wpedantic               \
               -Wshadow -Wformat=2 -Wformat-security           \
               -Wdouble-promotion -Wnull-dereference           \
               -Wstrict-prototypes -Wmissing-prototypes        \
               -Wold-style-definition -Wjump-misses-init

INCLUDES    := -I$(INCDIR) -I$(SRCDIR) -I/usr/include/node

# Release flags
REL_CFLAGS  := -O2 -DNDEBUG -D_FORTIFY_SOURCE=2
REL_LDFLAGS := -Wl,-z,relro,-z,now -Wl,-O1

# Debug flags
DBG_CFLAGS  := -O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
DBG_LDFLAGS := -fsanitize=address,undefined

# PIC for shared library
PIC_FLAG    := -fPIC

# --- Select configuration ----------------------------------------------------
ifdef DEBUG
  OPT_CFLAGS  := $(DBG_CFLAGS)
  OPT_LDFLAGS := $(DBG_LDFLAGS)
  BUILD_TYPE  := debug
else
  OPT_CFLAGS  := $(REL_CFLAGS)
  OPT_LDFLAGS := $(REL_LDFLAGS)
  BUILD_TYPE  := release
endif

CFLAGS  := -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE $(CSTD) $(WARNINGS) $(INCLUDES) $(OPT_CFLAGS) $(PIC_FLAG)
TEST_CFLAGS := $(INCLUDES) -Itest -O0 -g3 -fno-omit-frame-pointer $(PIC_FLAG)
LDFLAGS := $(OPT_LDFLAGS)

# --- Verbose output ----------------------------------------------------------
ifndef V
  QUIET_CC   := @echo "  CC      $@"
  QUIET_AR   := @echo "  AR      $@"
  QUIET_LD   := @echo "  LD      $@"
  QUIET_LINK := @echo "  LINK    $@"
endif

# =============================================================================
#  Targets
# =============================================================================

.PHONY: all lib relay signaling auth billing test clean install info

all: lib relay signaling auth billing

# --- Core library (static + shared) ------------------------------------------
lib: $(STATIC_LIB) $(SHARED_LIB)

$(STATIC_LIB): $(CORE_OBJS) | $(LIBDIR)
	$(QUIET_AR)
	$(AR) rcs $@ $(CORE_OBJS)

$(SHARED_LIB): $(CORE_OBJS) | $(LIBDIR)
	$(QUIET_LINK)
	$(CC) -shared $(LDFLAGS) -o $@ $(CORE_OBJS) $(LIBS)

# --- Binaries ----------------------------------------------------------------
relay: $(BINDIR)/rc_relay

$(BINDIR)/rc_relay: $(RELAY_OBJS) $(STATIC_LIB) | $(BINDIR)
	$(QUIET_LD)
	$(CC) $(LDFLAGS) -o $@ $(RELAY_OBJS) $(STATIC_LIB) $(LIBS)

signaling: $(BINDIR)/rc_signaling

$(BINDIR)/rc_signaling: $(SIGNALING_OBJS) $(STATIC_LIB) | $(BINDIR)
	$(QUIET_LD)
	$(CC) $(LDFLAGS) -o $@ $(SIGNALING_OBJS) $(STATIC_LIB) $(LIBS)

auth: $(BINDIR)/rc_auth

$(BINDIR)/rc_auth: $(AUTH_OBJS) $(STATIC_LIB) | $(BINDIR)
	$(QUIET_LD)
	$(CC) $(LDFLAGS) -o $@ $(AUTH_OBJS) $(STATIC_LIB) $(LIBS)

billing: $(BINDIR)/rc_billing

$(BINDIR)/rc_billing: $(BILLING_OBJS) $(STATIC_LIB) | $(BINDIR)
	$(QUIET_LD)
	$(CC) $(LDFLAGS) -o $@ $(BILLING_OBJS) $(STATIC_LIB) $(LIBS)

# --- Tests -------------------------------------------------------------------
TEST_BINS := $(addprefix $(BINDIR)/,$(patsubst %.c,%,$(notdir $(TEST_SRCS))))

test: $(TEST_BINS)
	@echo "=== Running tests ==="
	@for t in $(TEST_BINS); do echo "  Running $$t..."; $$t || exit 1; done

$(BINDIR)/test_%: $(OBJDIR)/test/test_%.o $(STATIC_LIB) | $(BINDIR)
	$(QUIET_LD)
	$(CC) $(LDFLAGS) -o $@ $< $(STATIC_LIB) $(TEST_LIBS)

# --- Object compilation ------------------------------------------------------
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(QUIET_CC)
	@$(MKDIR) $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(QUIET_CC)
	@$(MKDIR) $(dir $@)
	$(if $(findstring test/,$<),$(CC) $(TEST_CFLAGS) -c $< -o $@,$(CC) $(CFLAGS) -c $< -o $@)

# --- Directory creation ------------------------------------------------------
$(OBJDIR) $(LIBDIR) $(BINDIR):
	$(MKDIR) $@

# --- Install -----------------------------------------------------------------
install: all
	$(MKDIR) $(DESTDIR)$(PREFIX)/bin
	$(MKDIR) $(DESTDIR)$(PREFIX)/lib
	$(MKDIR) $(DESTDIR)$(PREFIX)/include/robocontrol
	$(INSTALL) -m 0755 $(BINDIR)/rc_relay     $(DESTDIR)$(PREFIX)/bin/
	$(INSTALL) -m 0755 $(BINDIR)/rc_signaling $(DESTDIR)$(PREFIX)/bin/
	$(INSTALL) -m 0755 $(BINDIR)/rc_auth      $(DESTDIR)$(PREFIX)/bin/
	$(INSTALL) -m 0755 $(BINDIR)/rc_billing   $(DESTDIR)$(PREFIX)/bin/
	$(INSTALL) -m 0644 $(STATIC_LIB)          $(DESTDIR)$(PREFIX)/lib/
	$(INSTALL) -m 0755 $(SHARED_LIB)          $(DESTDIR)$(PREFIX)/lib/
	$(INSTALL) -m 0644 $(INCDIR)/*.h          $(DESTDIR)$(PREFIX)/include/robocontrol/
	ldconfig $(DESTDIR)$(PREFIX)/lib 2>/dev/null || true
	@echo "Installed to $(DESTDIR)$(PREFIX)"

# --- Clean -------------------------------------------------------------------
clean:
	rm -rf $(BUILDDIR)
	@echo "Build directory cleaned."

# --- Info --------------------------------------------------------------------
info:
	@echo "  CC         = $(CC)"
	@echo "  CFLAGS     = $(CFLAGS)"
	@echo "  LDFLAGS    = $(LDFLAGS)"
	@echo "  LIBS       = $(LIBS)"
	@echo "  BUILD_TYPE = $(BUILD_TYPE)"
	@echo "  PREFIX     = $(PREFIX)"
	@echo "  SRCDIR     = $(SRCDIR)"
	@echo "  CORE_SRCS  = $(CORE_SRCS)"
