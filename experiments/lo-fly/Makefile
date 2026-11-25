# SPDX-License-Identifier: AGPL-3.0-only
# lo-fly for Melodee (README.md). Builds into build/lo-fly/ from lo-fly's dsp/ at a commit (LOFLY_REV, default
# HEAD), or from a copy of its working tree (LOFLY_REV=WORKTREE).
#   make            the MPC Sample's lo-fly folder (aarch64 Linux, glibc 2.28, through zig) and the Mac bundle
#   make mpc        build/lo-fly/mpc/lo-fly/: lo-fly.clap and an empty Presets folder
#   make mac        build/lo-fly/mac/lo-fly.clap (a macOS bundle, for clap-validator)
#   make screens    build/lo-fly/screens/*.png: the panel's pages, drawn by the plug-in itself
#   make bench      render and draw times on this machine
#   make presets    the presets preset discovery finds, each loaded and played
#   make validate   clap-validator on the Mac bundle
ROOT := ../..
LOFLY_DIR ?= $(ROOT)/../lo-fly
LOFLY_REV ?= HEAD
OUT ?= $(ROOT)/build/lo-fly
CLAP_INCLUDE ?= $(ROOT)/build/deps/clap/include
# Spleen (BSD 2-Clause), from its GitHub release: `make fonts`
FONT_DIR ?= $(OUT)/fonts
SPLEEN := 2.3.0
PYTHON ?= python3
ZIG ?= zig
CXX_MAC ?= clang++
VALIDATOR ?= $(LOFLY_DIR)/.tools/clap-validator/bin/clap-validator

ifeq ($(LOFLY_REV),WORKTREE)
# The working tree's dsp/ as it is now, copied again on every make (times kept, so only changed files rebuild).
SRC := $(OUT)/lofly-worktree
DSP := $(basename $(notdir $(wildcard $(LOFLY_DIR)/dsp/src/*.cpp)))
EXPORT_WHEN := FORCE
else
REV := $(shell git -C $(LOFLY_DIR) rev-parse --short=12 $(LOFLY_REV))
SRC := $(OUT)/lofly-$(REV)
DSP := $(basename $(notdir $(filter %.cpp,$(shell git -C $(LOFLY_DIR) ls-tree --name-only $(REV) dsp/src/))))
EXPORT_WHEN :=
endif
OWN := plugin panel

INCLUDES := -I$(SRC)/dsp/include -I$(ROOT)/sdk/melodee-screen -I$(CLAP_INCLUDE) -I$(OUT)/gen
FLAGS := -std=c++20 -O2 -MMD -MP -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -DNDEBUG $(INCLUDES)
WARN := -Wall -Wextra -Wno-unused-parameter
MPC_FLAGS := -target aarch64-linux-gnu.2.28 -mcpu=cortex_a55 -Wno-nullability-completeness

MAC_OBJ := $(addprefix $(OUT)/mac/obj/,$(addsuffix .o,$(DSP) $(OWN)))
MPC_OBJ := $(addprefix $(OUT)/mpc/obj/,$(addsuffix .o,$(DSP) $(OWN)))
BUNDLE := $(OUT)/mac/lo-fly.clap

all: mpc mac
mpc: $(OUT)/mpc/lo-fly/lo-fly.clap
mac: $(BUNDLE)/Contents/MacOS/lo-fly $(BUNDLE)/Contents/Info.plist

$(SRC)/.exported: $(EXPORT_WHEN)
	@test -d $(LOFLY_DIR)/dsp || (echo "lo-fly not found at $(LOFLY_DIR)"; exit 1)
	mkdir -p $(SRC)
ifeq ($(LOFLY_REV),WORKTREE)
	rsync -a --delete $(LOFLY_DIR)/dsp/ $(SRC)/dsp/
else
	git -C $(LOFLY_DIR) archive $(REV) dsp | tar -x -C $(SRC)
endif
	touch $@
$(SRC)/dsp/src/%.cpp: | $(SRC)/.exported ;
.PRECIOUS: $(SRC)/dsp/src/%.cpp

$(OUT)/gen/fonts.h: fonts.py $(FONT_DIR)/spleen-6x12.otf
	$(PYTHON) fonts.py $(FONT_DIR) $@

fonts: $(FONT_DIR)/spleen-6x12.otf
$(FONT_DIR)/spleen-6x12.otf:
	@mkdir -p $(FONT_DIR)/spleen
	curl -sfL -o $(FONT_DIR)/spleen/spleen-$(SPLEEN).tar.gz https://github.com/fcambus/spleen/releases/download/$(SPLEEN)/spleen-$(SPLEEN).tar.gz
	tar -xzf $(FONT_DIR)/spleen/spleen-$(SPLEEN).tar.gz -C $(FONT_DIR)/spleen spleen-$(SPLEEN)/spleen-6x12.otf spleen-$(SPLEEN)/spleen-8x16.otf \
		spleen-$(SPLEEN)/spleen-12x24.otf spleen-$(SPLEEN)/LICENSE
	cp $(FONT_DIR)/spleen/spleen-$(SPLEEN)/spleen-*.otf $(FONT_DIR)/
	cp $(FONT_DIR)/spleen/spleen-$(SPLEEN)/LICENSE $(FONT_DIR)/Spleen-LICENSE.txt

HEADERS := store.h panel.h $(ROOT)/sdk/melodee-screen/melodee_screen.h

$(OUT)/mac/obj/%.o: $(SRC)/dsp/src/%.cpp | $(SRC)/.exported
	@mkdir -p $(@D)
	$(CXX_MAC) $(FLAGS) -c $< -o $@
$(OUT)/mac/obj/%.o: %.cpp $(HEADERS) $(OUT)/gen/fonts.h | $(SRC)/.exported
	@mkdir -p $(@D)
	$(CXX_MAC) $(FLAGS) $(WARN) -c $< -o $@
$(OUT)/mpc/obj/%.o: $(SRC)/dsp/src/%.cpp | $(SRC)/.exported
	@mkdir -p $(@D)
	$(ZIG) c++ $(MPC_FLAGS) $(FLAGS) -c $< -o $@
$(OUT)/mpc/obj/%.o: %.cpp $(HEADERS) $(OUT)/gen/fonts.h | $(SRC)/.exported
	@mkdir -p $(@D)
	$(ZIG) c++ $(MPC_FLAGS) $(FLAGS) $(WARN) -c $< -o $@

$(OUT)/mpc/lo-fly/lo-fly.clap: $(MPC_OBJ) lo-fly.map $(OUT)/mpc/lo-fly/FONTS.txt
	@mkdir -p $(@D)/Presets
	$(ZIG) c++ $(MPC_FLAGS) -shared -Wl,--version-script=lo-fly.map -Wl,-s $(MPC_OBJ) -o $@
# The panel's type is drawn from Spleen: its licence goes with the plug-in.
$(OUT)/mpc/lo-fly/FONTS.txt: $(FONT_DIR)/spleen-6x12.otf
	@mkdir -p $(@D)
	{ echo "lo-fly's panel draws its text from Spleen, under the BSD 2-Clause License."; \
	  echo; cat $(FONT_DIR)/Spleen-LICENSE.txt; } > $@

$(BUNDLE)/Contents/MacOS/lo-fly: $(MAC_OBJ)
	@mkdir -p $(@D)
	$(CXX_MAC) -bundle $(MAC_OBJ) -o $@
$(BUNDLE)/Contents/Info.plist: Info.plist
	@mkdir -p $(@D)
	cp Info.plist $@

$(OUT)/mac/preview: preview.cpp $(MAC_OBJ)
	$(CXX_MAC) $(FLAGS) $(WARN) preview.cpp $(MAC_OBJ) -o $@

screens: $(OUT)/mac/preview
	@mkdir -p $(OUT)/screens
	$(OUT)/mac/preview $(OUT)/screens
bench: $(OUT)/mac/preview
	$(OUT)/mac/preview --bench
presets: $(OUT)/mac/preview
	$(OUT)/mac/preview --presets
validate: mac
	$(VALIDATOR) validate $(BUNDLE)

clean:
	rm -rf $(OUT)

-include $(MAC_OBJ:.o=.d) $(MPC_OBJ:.o=.d)
# The compiler writes the .d files; make never builds one.
%.d: ;

FORCE:
.PHONY: all mpc mac fonts screens bench presets validate clean FORCE
