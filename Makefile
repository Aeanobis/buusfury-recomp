# Static recompilation of the Legacy of Goku games
#
#   make            build ./$(GAME)   (GAME=lgoku1 or lgoku2, default lgoku2)
#   make verify     run recompiled code against the interpreter and compare every frame
#   make profile    play a scripted session, record code the recompiler missed
#                   (run `make` again afterwards to compile it in)

GAME     ?= lgoku2
include games/$(GAME)/game.mk
SAVE_ID  ?= $(GAME)
GDIR     := games/$(GAME)
PATCHES  := $(sort $(wildcard $(GDIR)/patches/*.ips))
MODS     := $(sort $(wildcard $(GDIR)/mods/*.c))

MGBA     := deps/mgba
MGBA_BLD := deps/mgba-build
SDLROOT  := deps/sdlroot/usr

CC       := gcc
INC       = -I$(MGBA)/include -I$(MGBA_BLD)/include -Iruntime -I$(GEN) \
            -I$(SDLROOT)/include -I$(SDLROOT)/include/x86_64-linux-gnu
CFLAGS    = -O2 -g0 -std=gnu11 -fno-strict-aliasing -fwrapv $(INC) -D_GNU_SOURCE
# Generated code: thousands of tiny functions, optimise but don't spend forever
GENFLAGS  = $(CFLAGS) -fno-stack-protector -fomit-frame-pointer -w
XBRZ     := deps/xbrzscale/xbrz
NCNN     := deps/ncnn-build
NCNN_LIBS := $(NCNN)/src/libncnn.a $(NCNN)/glslang/glslang/libglslang.a $(NCNN)/glslang/SPIRV/libSPIRV.a \
            $(NCNN)/glslang/glslang/libMachineIndependent.a $(NCNN)/glslang/glslang/libGenericCodeGen.a \
            $(NCNN)/glslang/glslang/OSDependent/Unix/libOSDependent.a $(NCNN)/glslang/OGLCompilersDLL/libOGLCompiler.a \
            $(NCNN)/glslang/glslang/libglslang-default-resource-limits.a -l:libvulkan.so.1 -fopenmp
LIBS     := $(MGBA_BLD)/libmgba.a $(NCNN_LIBS) -l:libSDL2-2.0.so.0 -lz -lm -lpthread -lstdc++
# realesr-animevideov3-x4 keeps text, menus and dithered shading intact;
# realesrgan-x4plus-anime redraws everything as flat cartoon art and mangles them
AI_MODEL := $(CURDIR)/deps/realesrgan/models/realesr-animevideov3-x4

BUILD    := build/$(GAME)
GEN      := gen/$(GAME)
PROFILE  := $(GDIR)/profile/missed.txt
IWRAM    := $(GDIR)/profile/iwram.bin
ENTRIES  := $(GDIR)/entries.txt

GEN_STAMP := $(GEN)/.stamp
GEN_SRCS   = $(wildcard $(GEN)/rc_blocks_*.c) $(GEN)/rc_index.c
GEN_OBJS   = $(patsubst $(GEN)/%.c,$(BUILD)/%.o,$(GEN_SRCS))
MOD_OBJS  := $(patsubst $(GDIR)/mods/%.c,$(BUILD)/mod_%.o,$(MODS))

.PHONY: all clean verify profile link
all: $(GAME)

$(BUILD) $(GEN):
	mkdir -p $@

# The recompiler works from the patched, padded ROM. The executable does NOT
# contain it: the player's own ROM is read when the game starts (runtime/romload.c,
# same check, patches and padding), so a built game can be shared and still needs
# a copy of the cartridge. ROM_SHA1 may list several acceptable dumps.
$(BUILD)/rom.gba: $(PATCHES) $(GDIR)/game.mk | $(BUILD)
	@h=$$(sha1sum "$(ROM)" 2>/dev/null | cut -d' ' -f1); ok=0; for e in $(ROM_SHA1); do [ "$$h" = "$$e" ] && ok=1; done; \
	 [ $$ok = 1 ] || { echo "ROM missing or not the expected dump: $(ROM) (SHA-1 $${h:-none}; expected $(ROM_SHA1))"; exit 1; }
	python3 -I tools/ips.py $(if $(ROM_MB),--pad=$(ROM_MB)) "$(ROM)" $@ $(PATCHES)

# The project's own patches travel inside the executable (a table of their bytes)
$(BUILD)/patches.o: $(PATCHES) $(GDIR)/game.mk | $(BUILD)
	@{ printf '.section .data.rel.ro,"aw",@progbits\n.balign 8\n.global rcPatchTable\nrcPatchTable:\n'; \
	   n=0; for p in $(PATCHES) ""; do [ -n "$$p" ] || continue; printf '.quad p%d_s, p%d_e\n' $$n $$n; n=$$((n+1)); done; printf '.quad 0, 0\n'; \
	   printf '.section .rodata\n'; \
	   n=0; for p in $(PATCHES) ""; do [ -n "$$p" ] || continue; printf 'p%d_s: .incbin "%s"\np%d_e:\n' $$n "$$p" $$n; n=$$((n+1)); done; \
	   printf '.section .note.GNU-stack,"",@progbits\n'; } | $(CC) -c -x assembler - -o $@

$(BUILD)/game_config.h: $(GDIR)/game.mk Makefile | $(BUILD)
	$(file >$@,#define GAME_ID "$(GAME)")
	$(file >>$@,#define SAVE_ID "$(SAVE_ID)")
	$(file >>$@,#define GAME_TITLE "$(TITLE)")
	$(file >>$@,#define AI_MODEL "$(AI_MODEL)")
	$(file >>$@,#define ROM_SHA1 "$(ROM_SHA1)")
	$(file >>$@,#define ROM_MB $(if $(ROM_MB),$(ROM_MB),0))
	$(if $(filter SRAM,$(SAVE_MEMORY)),$(file >>$@,#define RC_FORCE_SRAM 1))

$(BUILD)/romload.o: runtime/romload.c runtime/romload.h $(BUILD)/game_config.h | $(BUILD)
	$(CC) $(CFLAGS) -Wall -include $(BUILD)/game_config.h -c $< -o $@

# Decode tables and always-inline instruction bodies, derived from mGBA's interpreter
$(GEN)/handler_tables.txt: tools/gen_tables.c | $(GEN)
	$(CC) $(INC) $< -o $(GEN)/gen_tables && ./$(GEN)/gen_tables > $@

$(GEN)/isa-thumb-inl.h: $(MGBA)/src/arm/isa-thumb.c | $(GEN)
	sed -e 's/static void _ThumbInstruction ## NAME/static inline __attribute__((always_inline)) void _ThumbInstruction ## NAME/' \
	    -e '/^const ThumbInstruction _thumbTable/,$$d' $< > $@

$(GEN)/isa-arm-inl.h: $(MGBA)/src/arm/isa-arm.c | $(GEN)
	sed -e 's/static void _ARMInstruction ## NAME/static inline __attribute__((always_inline)) void _ARMInstruction ## NAME/' \
	    -e 's/ATTRIBUTE_NOINLINE static/static inline/' \
	    -e '/^const ARMInstruction _armTable/,$$d' $< > $@

$(GEN_STAMP): tools/recomp.py $(GEN)/handler_tables.txt $(BUILD)/rom.gba $(wildcard $(PROFILE)) $(wildcard $(IWRAM)) $(wildcard $(ENTRIES))
	python3 -I tools/recomp.py $(BUILD)/rom.gba --tables $(GEN)/handler_tables.txt --out $(GEN) \
		--profile $(PROFILE) --entries $(ENTRIES) --iwram $(IWRAM) --bios $(MGBA)/src/gba/hle-bios.c
	touch $@

$(BUILD)/rc_%.o: $(GEN)/rc_%.c runtime/rc_rt.h $(GEN)/isa-thumb-inl.h $(GEN)/isa-arm-inl.h
	$(CC) $(GENFLAGS) -c $< -o $@

$(BUILD)/main.o: runtime/main.c runtime/rc_rt.h runtime/mod.h runtime/romload.h $(BUILD)/game_config.h | $(BUILD)
	$(CC) $(CFLAGS) -Wall -include $(BUILD)/game_config.h -c $< -o $@

# Picture upscaler (xBRZ)
$(BUILD)/upscale.o: runtime/upscale.cpp | $(BUILD)
	g++ -O2 -std=c++17 -I$(XBRZ) -c $< -o $@

# HD picture mode (neural network on the GPU)
$(BUILD)/ai_upscale.o: runtime/ai_upscale.cpp | $(BUILD)
	g++ -O2 -std=c++17 -I$(NCNN)/src -Ideps/ncnn/src -Ideps/vkroot/usr/include -c $< -o $@

$(BUILD)/xbrz.o: $(XBRZ)/xbrz.cpp | $(BUILD)
	g++ -O2 -std=c++17 -I$(XBRZ) -w -c $< -o $@

# Mods: every C file in games/<game>/mods/ is compiled in (see runtime/mod.h)
$(BUILD)/mod_%.o: $(GDIR)/mods/%.c runtime/mod.h $(wildcard $(GDIR)/mods/*.h) $(wildcard $(GDIR)/mods/generated/*) | $(BUILD)
	$(CC) $(CFLAGS) -Wall -c $< -o $@

# Two stages: the list of generated files is only known after the recompiler ran
# Relink when a mod is added or removed, not only when one changes
$(shell mkdir -p $(BUILD); echo "$(MODS)" | cmp -s - $(BUILD)/mods.list || echo "$(MODS)" > $(BUILD)/mods.list)

$(GAME): $(GEN_STAMP) $(BUILD)/main.o $(BUILD)/romload.o $(BUILD)/patches.o $(BUILD)/upscale.o $(BUILD)/xbrz.o $(BUILD)/ai_upscale.o $(MOD_OBJS) $(MGBA_BLD)/libmgba.a $(BUILD)/mods.list
	$(MAKE) --no-print-directory link
	@d=$${XDG_DATA_HOME:-$$HOME/.local/share}/$(SAVE_ID); f=$$d/$(SAVE_ID).gba; \
	 if [ ! -e "$$f" ] || [ -L "$$f" ]; then mkdir -p "$$d" && ln -sfn "$(ROM)" "$$f" && echo "./$(GAME) reads your ROM through $$f"; fi

link: $(GEN_OBJS)
	$(CC) -o $(GAME) $(BUILD)/main.o $(BUILD)/romload.o $(BUILD)/patches.o $(BUILD)/upscale.o $(BUILD)/xbrz.o $(BUILD)/ai_upscale.o $(MOD_OBJS) $(GEN_OBJS) $(LIBS)

FRAMES ?= 6000
verify: $(GAME)
	./$(GAME) --headless $(FRAMES) --hash $(BUILD)/hash-native.txt --no-mods
	./$(GAME) --headless $(FRAMES) --hash $(BUILD)/hash-interp.txt --interp
	@if cmp -s $(BUILD)/hash-native.txt $(BUILD)/hash-interp.txt; then \
		echo "verify: OK - $(FRAMES) frames identical (video, audio, CPU and RAM state)"; \
	else echo "verify: MISMATCH, first differing frame:"; diff $(BUILD)/hash-native.txt $(BUILD)/hash-interp.txt | head -3; exit 1; fi

profile: $(GAME)
	mkdir -p $(GDIR)/profile
	./$(GAME) --headless $(FRAMES) --no-mods --profile $(PROFILE) --dump-iwram $(IWRAM) --stats
	sort -u -o $(PROFILE) $(PROFILE)

clean:
	rm -rf $(BUILD) $(GEN) $(GAME)
