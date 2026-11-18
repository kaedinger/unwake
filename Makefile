##############################################################
# unwake - build system (same shape as Unspin's)
# Produces the .txz package that the .plg installs
##############################################################

PLUGIN   := unwake
PLG_FILE := unwake.plg
VERSION  := $(shell date +%Y.%m.%d)
PKG_DIR  := pkg
BUILD    := build/$(PLUGIN)-$(VERSION)

# libfuse 3.17+ (kernel FUSE passthrough), linked statically: Unraid needn't ship it
LIBFUSE_VER    := 3.17.4
LIBFUSE_PREFIX ?= $(CURDIR)/build/libfuse3
LIBFUSE_LIBDIR := $(firstword $(wildcard $(LIBFUSE_PREFIX)/lib/x86_64-linux-gnu $(LIBFUSE_PREFIX)/lib64 $(LIBFUSE_PREFIX)/lib))

CXX      := g++
CXXFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -std=c++17 \
            -I$(LIBFUSE_PREFIX)/include/fuse3 -DUNWAKE_VERSION='"$(VERSION)"'
LDFLAGS  := -L$(LIBFUSE_LIBDIR) -Wl,-Bstatic -lfuse3 -Wl,-Bdynamic -lpthread -ldl

SRC := $(wildcard src/*.cpp)

.PHONY: all libfuse build-daemon package clean update-plg update-plg-hash install-local

all: build-daemon package

# downloads, patches (patches/: in-place upgrade) and builds libfuse into build/libfuse3
# (needs meson and ninja). A patch change needs `rm -rf build/libfuse3` to take effect.
libfuse:
	@if grep -q fuse_session_handover_import "$(LIBFUSE_PREFIX)/include/fuse3/fuse_lowlevel.h" 2>/dev/null; then echo "✓ libfuse $(LIBFUSE_VER) present"; else \
	  set -e; mkdir -p build; cd build; \
	  curl -fsSL -o fuse.tar.gz https://github.com/libfuse/libfuse/releases/download/fuse-$(LIBFUSE_VER)/fuse-$(LIBFUSE_VER).tar.gz; \
	  rm -rf fuse-$(LIBFUSE_VER); tar xzf fuse.tar.gz; cd fuse-$(LIBFUSE_VER); \
	  for p in $(CURDIR)/patches/libfuse-$(LIBFUSE_VER)-*.patch; do patch -p1 -s < $$p; done; \
	  meson setup b --default-library=static -Dexamples=false -Dutils=false -Dtests=false \
	        --buildtype=release --prefix=$(LIBFUSE_PREFIX) >/dev/null; \
	  ninja -C b install >/dev/null; echo "✓ libfuse $(LIBFUSE_VER) built"; fi

build-daemon: libfuse
	@echo "→ Compiling unwaked"
	@mkdir -p usr/local/sbin
	@$(MAKE) --no-print-directory usr/local/sbin/unwaked LIBFUSE_LIBDIR=$(firstword $(wildcard $(LIBFUSE_PREFIX)/lib/x86_64-linux-gnu $(LIBFUSE_PREFIX)/lib64 $(LIBFUSE_PREFIX)/lib))
	@echo "✓ unwaked compiled"

usr/local/sbin/unwaked: $(SRC) $(wildcard src/*.h)
	@$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)
	@chmod +x $@

package:
	@echo "→ Building $(PLUGIN)-$(VERSION)"
	@rm -rf $(BUILD)
	@mkdir -p $(BUILD)
	@rsync -a usr/ $(BUILD)/usr/
	@rsync -a etc/ $(BUILD)/etc/
	@rsync -a install/ $(BUILD)/install/
	@chmod +x $(BUILD)/usr/local/sbin/unwaked $(BUILD)/etc/rc.d/rc.unwake
	@chmod +x $(BUILD)/usr/local/emhttp/plugins/unwake/event/*
	@mkdir -p $(PKG_DIR)
	@cd $(BUILD) && \
	  tar --create --xz --owner=0 --group=0 --numeric-owner \
	      --file=../../$(PKG_DIR)/$(PLUGIN)-$(VERSION)-x86_64-1.txz \
	      install/ usr/ etc/
	@echo "✓ Package: $(PKG_DIR)/$(PLUGIN)-$(VERSION)-x86_64-1.txz"
	@$(MAKE) --no-print-directory update-plg-hash VERSION=$(VERSION)

update-plg:
	@sed -i "s|<!ENTITY version.*|<!ENTITY version   \"$(VERSION)\">|" $(PLG_FILE)
	@echo "✓ Updated .plg version to $(VERSION)"

update-plg-hash:
	@HASH=$$(sha256sum $(PKG_DIR)/$(PLUGIN)-$(VERSION)-x86_64-1.txz | cut -d' ' -f1); \
	sed -i "s|<SHA256>.*</SHA256>|<SHA256>$$HASH</SHA256>|" $(PLG_FILE); \
	echo "✓ Updated .plg SHA256 to $$HASH"

clean:
	@rm -rf build/$(PLUGIN)-* $(PKG_DIR) usr/local/sbin/unwaked
	@echo "✓ Cleaned build artifacts"

# Quick local test on a running Unraid system: make install-local HOST=tower
install-local:
	scp $(PKG_DIR)/$(PLUGIN)-$(VERSION)-x86_64-1.txz root@$(HOST):/tmp/
	ssh root@$(HOST) "installpkg /tmp/$(PLUGIN)-$(VERSION)-x86_64-1.txz"
