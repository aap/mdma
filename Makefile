# mdma — PS2 DMA/VIF/GIF packet library
#
#   make test    build and run the host unit tests (default)
#   make lib     build libmdma.a for the host
#   make ee      build libmdma.a for the EE with freesce's ee-gcc
#
# The host build needs nothing and covers everything except §9 (kicking).

SRC := mdma.c mdmadis.c
HDR := mdma.h mdmaplat.h

CC       ?= cc
CFLAGS   ?= -g -O1
WARN     := -Wall -Wextra -Wno-unused-parameter
# Host builds use freesce's headers as the stand-in for the SCE ones, so the
# tests exercise the exact SCE_VIF1_SET_* / SCE_GIF_* constants the EE uses.
# Two roots, because they are two different axes. FREESCE is the SDK vintage --
# headers, libraries, crt0, app.cmd -- and there is one per SDK version, so it
# can point at an install or straight at a git worktree:
#
#	make ee
#	make ee FREESCE=~/src/freesce_24        # a different vintage
#
# FREESCE_GCC deliberately does not derive from it: a source worktree has
# headers but no compiler, and the compiler is not SDK-versioned anyway.
FREESCE     ?= /usr/local/freesce
FREESCE_GCC ?= /usr/local/freesce/ee/gcc

HOSTFLAGS := -I. -I$(FREESCE)/ee/include
HOSTOBJ  := $(SRC:%.c=build/host/%.o)

.PHONY: test lib ee clean

test: build/host/test
	@build/host/test

lib: build/host/libmdma.a

build/host/libmdma.a: $(HOSTOBJ)
	ar rcs $@ $^

build/host/test: test/test.c build/host/libmdma.a $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARN) $(HOSTFLAGS) -o $@ test/test.c build/host/libmdma.a

build/host/%.o: %.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARN) $(HOSTFLAGS) -c $< -o $@

# ---- EE ------------------------------------------------------------------
EECC    ?= $(FREESCE_GCC)/bin/ee-gcc
EEAR    ?= $(FREESCE_GCC)/bin/ee-ar
EEINC   ?= -I$(FREESCE)/ee/include
EEFLAGS ?= -O2 -fno-common -DNDEBUG
EEOBJ   := $(SRC:%.c=build/ee/%.o)

ee: build/ee/libmdma.a

build/ee/libmdma.a: $(EEOBJ)
	$(EEAR) rcs $@ $^

build/ee/%.o: %.c $(HDR)
	@mkdir -p $(@D)
	$(EECC) $(EEFLAGS) $(EEINC) -I. -c $< -o $@

clean:
	rm -rf build
