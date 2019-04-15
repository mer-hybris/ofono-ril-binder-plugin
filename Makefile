# -*- Mode: makefile-gmake -*-

.PHONY: clean all debug release pkgconfig
.PHONY: lib debug_lib release_lib
.PHONY: plugin debug_plugin release_plugin
.PHONY: install install-dev

#
# Required packages
#
# ofono.pc adds -export-symbols-regex linker option which doesn't  work
# on all platforms. 
#

LIB_LDPKGS = libgrilio libgbinder libgbinder-radio libglibutil gobject-2.0 glib-2.0
LIB_PKGS = $(LIB_LDPKGS)

PLUGIN_LDPKGS = libgrilio libglibutil
PLUGIN_PKGS = ofono $(PLUGIN_LDPKGS)

#
# Default target
#

all: debug release

#
# Plugin and library version
#

VERSION_MAJOR = 1
VERSION_MINOR = 0
VERSION_RELEASE = 3

# Version for pkg-config
PCVERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_RELEASE)

#
# Plugin name
#

PLUGIN_NAME = rilbinderplugin
PLUGIN = $(PLUGIN_NAME).so

#
# Library name
#

NAME = grilio-binder
LIB_NAME = lib$(NAME)
LIB_DEV_SYMLINK = $(LIB_NAME).so
LIB_SYMLINK1 = $(LIB_DEV_SYMLINK).$(VERSION_MAJOR)
LIB_SYMLINK2 = $(LIB_SYMLINK1).$(VERSION_MINOR)
LIB_SONAME = $(LIB_SYMLINK1)
LIB_SO = $(LIB_SONAME).$(VERSION_MINOR).$(VERSION_RELEASE)
LIB = $(LIB_NAME).so

#
# Sources
#

PLUGIN_SRC = \
  ril_binder_plugin.c

LIB_SRC = \
  ril_binder_radio.c

#
# Directories
#

SRC_DIR = src
PLUGIN_SRC_DIR = $(SRC_DIR)
LIB_SRC_DIR = $(SRC_DIR)
INCLUDE_DIR = include
BUILD_DIR = build

PLUGIN_BUILD_DIR = $(BUILD_DIR)/plugin
PLUGIN_DEBUG_BUILD_DIR = $(PLUGIN_BUILD_DIR)/debug
PLUGIN_RELEASE_BUILD_DIR = $(PLUGIN_BUILD_DIR)/release

DEBUG_PLUGIN = $(PLUGIN_DEBUG_BUILD_DIR)/$(PLUGIN)
RELEASE_PLUGIN = $(PLUGIN_RELEASE_BUILD_DIR)/$(PLUGIN)

LIB_BUILD_DIR = $(BUILD_DIR)/lib
LIB_DEBUG_BUILD_DIR = $(LIB_BUILD_DIR)/debug
LIB_RELEASE_BUILD_DIR = $(LIB_BUILD_DIR)/release

DEBUG_LIB = $(LIB_DEBUG_BUILD_DIR)/$(LIB_SO)
RELEASE_LIB = $(LIB_RELEASE_BUILD_DIR)/$(LIB_SO)
DEBUG_LINK = $(LIB_DEBUG_BUILD_DIR)/$(LIB_SYMLINK1)
RELEASE_LINK = $(LIB_RELEASE_BUILD_DIR)/$(LIB_SYMLINK1)

#
# Tools and flags
#

CC = $(CROSS_COMPILE)gcc
LD = $(CC)
WARNINGS = -Wall -Wstrict-aliasing -Wunused-result
INCLUDES = -I$(INCLUDE_DIR)
BASE_FLAGS = -fPIC
BASE_CFLAGS = $(CFLAGS) $(DEFINES) $(WARNINGS) $(INCLUDES)
DEBUG_FLAGS = -g
RELEASE_FLAGS =

PLUGIN_BASE_FLAGS = $(BASE_FLAGS) -fvisibility=hidden
PLUGIN_FULL_CFLAGS = $(PLUGIN_BASE_FLAGS) $(BASE_CFLAGS) \
  -MMD -MP $(shell pkg-config --cflags $(PLUGIN_PKGS))
PLUGIN_FULL_LDFLAGS = $(PLUGIN_BASE_FLAGS) $(LDFLAGS) -shared \
  $(shell pkg-config --libs $(PLUGIN_LDPKGS))

LIB_BASE_FLAGS = $(BASE_FLAGS)
LIB_FULL_CFLAGS = $(LIB_BASE_FLAGS) $(BASE_CFLAGS) \
  -MMD -MP $(shell pkg-config --cflags $(LIB_PKGS))
LIB_FULL_LDFLAGS = $(LIB_BASE_FLAGS) $(LDFLAGS) -shared \
  -Wl,-soname,$(LIB_SONAME) $(shell pkg-config --libs $(LIB_PKGS)) -lpthread

ifndef KEEP_SYMBOLS
KEEP_SYMBOLS = 0
endif

ifneq ($(KEEP_SYMBOLS),0)
RELEASE_FLAGS += -g
endif

PLUGIN_DEBUG_LDFLAGS = $(PLUGIN_FULL_LDFLAGS) $(DEBUG_FLAGS)
PLUGIN_RELEASE_LDFLAGS = $(PLUGIN_FULL_LDFLAGS) $(RELEASE_FLAGS)
PLUGIN_DEBUG_CFLAGS = $(PLUGIN_FULL_CFLAGS) $(DEBUG_FLAGS) -DDEBUG
PLUGIN_RELEASE_CFLAGS = $(PLUGIN_FULL_CFLAGS) $(RELEASE_FLAGS) -O2

LIB_DEBUG_LDFLAGS = $(LIB_FULL_LDFLAGS) $(DEBUG_FLAGS)
LIB_RELEASE_LDFLAGS = $(LIB_FULL_LDFLAGS) $(RELEASE_FLAGS)
LIB_DEBUG_CFLAGS = $(LIB_FULL_CFLAGS) $(DEBUG_FLAGS) -DDEBUG
LIB_RELEASE_CFLAGS = $(LIB_FULL_CFLAGS) $(RELEASE_FLAGS) -O2

#
# Files
#

PKGCONFIG = $(LIB_BUILD_DIR)/$(LIB_NAME).pc

PLUGIN_DEBUG_OBJS = $(PLUGIN_SRC:%.c=$(PLUGIN_DEBUG_BUILD_DIR)/%.o)
PLUGIN_RELEASE_OBJS = $(PLUGIN_SRC:%.c=$(PLUGIN_RELEASE_BUILD_DIR)/%.o)

LIB_DEBUG_OBJS = $(LIB_SRC:%.c=$(LIB_DEBUG_BUILD_DIR)/%.o)
LIB_RELEASE_OBJS = $(LIB_SRC:%.c=$(LIB_RELEASE_BUILD_DIR)/%.o)

DEBUG_OBJS = $(PLUGIN_DEBUG_OBJS) $(LIB_DEBUG_OBJS)
RELEASE_OBJS = $(PLUGIN_RELEASE_OBJS) $(LIB_RELEASE_OBJS)

#
# Dependencies
#

DEPS = \
  $(DEBUG_OBJS:%.o=%.d) \
  $(RELEASE_OBJS:%.o=%.d)
ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(DEPS)),)
-include $(DEPS)
endif
endif

$(PKGCONFIG): | $(LIB_BUILD_DIR)

$(LIB_DEBUG_OBJS): | $(LIB_DEBUG_BUILD_DIR)
$(LIB_RELEASE_OBJS): | $(LIB_RELEASE_BUILD_DIR)

$(PLUGIN_DEBUG_OBJS): | $(PLUGIN_DEBUG_BUILD_DIR)
$(PLUGIN_RELEASE_OBJS): | $(PLUGIN_RELEASE_BUILD_DIR)

#
# Rules
#

clean:
	rm -f *~ $(SRC_DIR)/*~
	rm -fr $(BUILD_DIR) RPMS installroot

lib: debug_lib release_lib

plugin: debug_plugin release_plugin

debug: debug_lib debug_plugin

release: release_lib release_plugin

debug_lib: $(DEBUG_LIB)

release_lib: $(RELEASE_LIB)

debug_plugin: $(DEBUG_PLUGIN)

release_plugin: $(RELEASE_PLUGIN)

pkgconfig: $(PKGCONFIG)

$(LIB_BUILD_DIR):
	mkdir -p $@

$(LIB_DEBUG_BUILD_DIR):
	mkdir -p $@

$(LIB_RELEASE_BUILD_DIR):
	mkdir -p $@

$(PLUGIN_DEBUG_BUILD_DIR):
	mkdir -p $@

$(PLUGIN_RELEASE_BUILD_DIR):
	mkdir -p $@

$(LIB_DEBUG_BUILD_DIR)/%.o : $(LIB_SRC_DIR)/%.c
	$(CC) -c $(LIB_DEBUG_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(LIB_RELEASE_BUILD_DIR)/%.o : $(LIB_SRC_DIR)/%.c
	$(CC) -c $(LIB_RELEASE_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(PLUGIN_DEBUG_BUILD_DIR)/%.o : $(PLUGIN_SRC_DIR)/%.c
	$(CC) -c $(PLUGIN_DEBUG_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(PLUGIN_RELEASE_BUILD_DIR)/%.o : $(PLUGIN_SRC_DIR)/%.c
	$(CC) -c $(PLUGIN_RELEASE_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(DEBUG_LIB): $(LIB_DEBUG_OBJS)
	$(LD) $^ $(LIB_DEBUG_LDFLAGS) -o $@
	ln -sf $(LIB_SO) $(DEBUG_LINK)

$(RELEASE_LIB): $(LIB_RELEASE_OBJS)
	$(LD) $^ $(LIB_RELEASE_LDFLAGS) -o $@
	ln -sf $(LIB_SO) $(RELEASE_LINK)
ifeq ($(KEEP_SYMBOLS),0)
	strip $@
endif

$(DEBUG_PLUGIN): $(PLUGIN_DEBUG_OBJS) $(DEBUG_LIB)
	$(LD) $^ $(PLUGIN_DEBUG_LDFLAGS) -o $@

$(RELEASE_PLUGIN): $(PLUGIN_RELEASE_OBJS) $(RELEASE_LIB)
	$(LD) $^ $(PLUGIN_RELEASE_LDFLAGS) -o $@
ifeq ($(KEEP_SYMBOLS),0)
	strip $@
endif

$(PKGCONFIG): $(LIB_NAME).pc.in Makefile
	sed -e 's/\[version\]/'$(PCVERSION)/g $< > $@

#
# Install
#

INSTALL = install
INSTALL_DIRS = $(INSTALL) -d
INSTALL_LIBS = $(INSTALL) -m 755
INSTALL_FILES = $(INSTALL) -m 644

INSTALL_LIB_DIR = $(DESTDIR)/usr/lib
INSTALL_PLUGIN_DIR = $(DESTDIR)/usr/lib/ofono/plugins
INSTALL_INCLUDE_DIR = $(DESTDIR)/usr/include/$(NAME)
INSTALL_PKGCONFIG_DIR = $(DESTDIR)/usr/lib/pkgconfig

install: $(INSTALL_LIB_DIR) $(INSTALL_PLUGIN_DIR)
	$(INSTALL_LIBS) $(RELEASE_PLUGIN) $(INSTALL_PLUGIN_DIR)
	$(INSTALL_LIBS) $(RELEASE_LIB) $(INSTALL_LIB_DIR)
	ln -sf $(LIB_SO) $(INSTALL_LIB_DIR)/$(LIB_SYMLINK2)
	ln -sf $(LIB_SYMLINK2) $(INSTALL_LIB_DIR)/$(LIB_SYMLINK1)

install-dev: install $(INSTALL_INCLUDE_DIR) $(INSTALL_PKGCONFIG_DIR)
	$(INSTALL_FILES) $(INCLUDE_DIR)/*.h $(INSTALL_INCLUDE_DIR)
	$(INSTALL_FILES) $(PKGCONFIG) $(INSTALL_PKGCONFIG_DIR)
	ln -sf $(LIB_SYMLINK1) $(INSTALL_LIB_DIR)/$(LIB_DEV_SYMLINK)

$(INSTALL_LIB_DIR):
	$(INSTALL_DIRS) $@

$(INSTALL_PLUGIN_DIR):
	$(INSTALL_DIRS) $@

$(INSTALL_INCLUDE_DIR):
	$(INSTALL_DIRS) $@

$(INSTALL_PKGCONFIG_DIR):
	$(INSTALL_DIRS) $@
