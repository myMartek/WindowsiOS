CC = clang
CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
OBJCFLAGS = -fobjc-arc -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
B = build

SLIRP_CFLAGS = $(shell PATH=/opt/homebrew/bin:$$PATH pkg-config --cflags slirp)
SLIRP_LIBS = $(shell PATH=/opt/homebrew/bin:$$PATH pkg-config --libs slirp)

CORE = $(B)/classify.o $(B)/operands.o $(B)/translate.o $(B)/dispatch.o $(B)/mmu.o $(B)/memop.o \
       $(B)/exception.o $(B)/sysreg.o $(B)/machine.o
HOST = $(B)/jit_region.o $(B)/enter.o $(B)/disk.o
ENGINE = $(CORE) $(HOST)

DEV = $(B)/acpi.o $(B)/fb_display.o $(B)/fb_metal.o $(B)/fdt.o $(B)/fw_cfg.o $(B)/gicv3.o $(B)/hda.o \
      $(B)/nvme.o $(B)/pcie.o $(B)/pflash.o $(B)/pl011.o $(B)/pl031.o $(B)/pl061.o $(B)/timer.o \
      $(B)/virtio.o $(B)/virtio_blk.o $(B)/virtio_input.o $(B)/virtio_net.o $(B)/virtio_snd.o \
      $(B)/winvirt.o $(B)/xhci.o

VM_HOST = $(B)/winvm.o $(B)/net_slirp.o $(B)/share.o $(B)/audio_mac.o $(B)/hvsparse.o

MAC_FRAMEWORKS = -framework Cocoa -framework Foundation -framework QuartzCore -framework Metal \
                 -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers \
                 -framework AudioToolbox

CORE_TESTS = $(B)/test_classify $(B)/test_jit_region $(B)/test_operands $(B)/test_jit $(B)/test_mem \
             $(B)/test_mmu $(B)/test_exc

all: $(CORE_TESTS) $(B)/hvsparse $(B)/winvm

# mmu.c: no SIMD (hv_tlb_refill runs with the guest's SIMD registers live).
$(B)/mmu.o: core/mmu.c core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -mgeneral-regs-only -c $< -o $@
$(B)/net_slirp.o: host/net_slirp.c host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(CFLAGS) $(SLIRP_CFLAGS) -c $< -o $@
$(B)/fb_metal.o: devices/fb_metal.m devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(OBJCFLAGS) -c $< -o $@
$(B)/main_mac.o: host/main_mac.m host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(OBJCFLAGS) -c $< -o $@
$(B)/%.o: core/%.c core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.c host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.S | $(B)
	$(CC) -c $< -o $@
$(B)/%.o: devices/%.c devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/winvm: $(B)/main_mac.o $(ENGINE) $(DEV) $(VM_HOST)
	$(CC) $(B)/main_mac.o $(ENGINE) $(DEV) $(VM_HOST) $(SLIRP_LIBS) $(MAC_FRAMEWORKS) -o $@

$(B)/test_classify: tests/test_classify.c tests/classify_cases.s $(B)/classify.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_jit_region: tests/test_jit_region.c $(B)/jit_region.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_operands: tests/test_operands.c tests/operand_cases.s $(B)/operands.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_mmu: tests/test_mmu.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_mmu.c $(ENGINE) -o $@

$(B)/hvsparse: tools/hvsparse_main.c host/hvsparse.c host/hvsparse.h | $(B)
	$(CC) $(CFLAGS) tools/hvsparse_main.c host/hvsparse.c -o $@

$(B)/test_%: tests/test_%.c tests/%_programs.S tests/harness.S tests/harness.h $(ENGINE)
	$(CC) $(CFLAGS) -Itests tests/test_$*.c tests/$*_programs.S tests/harness.S $(ENGINE) -o $@

test-core: $(CORE_TESTS)
	@ulimit -s 65520; for t in $(CORE_TESTS); do echo "== $$t"; $$t || exit 1; done

$(B):
	mkdir -p $(B)

# ---- iOS static library target: build/ios/libwinvm.a ----
IOS_SDK = $(shell xcrun --sdk iphoneos --show-sdk-path)
IOS_CC = xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -isysroot $(IOS_SDK)
IOS_SLIRP_INC ?= ios/deps/include/slirp
IOS_CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices -I$(IOS_SLIRP_INC)
IOS_OBJCFLAGS = -fobjc-arc -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
IB = build/ios

IOS_C_SRC = $(wildcard core/*.c) \
            host/jit_region.c host/disk.c host/winvm.c host/net_slirp.c host/share.c \
            host/audio_mac.c host/hvsparse.c host/prof_ios.c \
            $(wildcard devices/*.c)
IOS_OBJ = $(patsubst %.c,$(IB)/%.o,$(IOS_C_SRC)) $(IB)/host/enter.o $(IB)/devices/fb_metal.o

$(IB)/core/mmu.o: core/mmu.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -mgeneral-regs-only -c $< -o $@
$(IB)/%.o: %.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -c $< -o $@
$(IB)/devices/fb_metal.o: devices/fb_metal.m devices/*.h core/*.h host/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_OBJCFLAGS) -c $< -o $@
$(IB)/host/enter.o: host/enter.S
	@mkdir -p $(dir $@)
	$(IOS_CC) -c $< -o $@
$(IB)/libwinvm.a: $(IOS_OBJ)
	rm -f $@
	xcrun --sdk iphoneos libtool -static -o $@ $(IOS_OBJ)

ios-lib: $(IB)/libwinvm.a

clean:
	rm -rf $(B)

.PHONY: all test-core ios-lib clean

# ---- visionOS (visionmac spike): build/<platform>/libwinvm.a, one object with only the hv_winvm_* calls visible ----
# The Windows engine is the macOS one's sibling (Henge) with 4 KB pages and another machine: the same names, other
# layouts. In one app the two must not see each other, so everything here is linked into one relocatable object
# whose other symbols are local.
#   make xros-lib                         -> build/xros/libwinvm.a          (Apple Vision Pro)
#   make xros-lib XR_PLATFORM=xrsimulator -> build/xrsimulator/libwinvm.a   (its simulator)
XR_PLATFORM ?= xros
XR_MINVER ?= 2.0
XR_TRIPLE = arm64-apple-xros$(XR_MINVER)$(if $(filter xrsimulator,$(XR_PLATFORM)),-simulator,)
XR_SDK = $(shell xcrun --sdk $(XR_PLATFORM) --show-sdk-path)
XR_CC = xcrun --sdk $(XR_PLATFORM) clang -target $(XR_TRIPLE) -isysroot $(XR_SDK)
XR_SLIRP_INC ?= ../Henge/xros/deps/$(XR_PLATFORM)/include/slirp
XR_WIM ?= xros/deps/$(XR_PLATFORM)
# -fno-common: a variable without an initialiser (hv_knob) is a definition, not a common one, which ld -r leaves visible.
XR_CFLAGS = -std=c11 -Wall -Wextra -O2 -g -fno-common -Icore -Ihost -Idevices -I$(XR_SLIRP_INC)
XR_OBJCFLAGS = -fobjc-arc -Wall -Wextra -O2 -g -fno-common -Icore -Ihost -Idevices
XB = build/$(XR_PLATFORM)
# Not host/prof_ios.c: it sets a variable only its own app defines, and here that name is the other engine's.
XR_OBJ = $(patsubst %.c,$(XB)/%.o,$(filter-out host/prof_ios.c,$(IOS_C_SRC))) $(XB)/host/enter.o $(XB)/devices/fb_metal.o
XR_MEDIA_OBJ = $(XB)/xros/winmedia/winmedia.o $(XB)/xros/winmedia/fatfs/ff.o $(XB)/xros/winmedia/fatfs/ffsystem.o $(XB)/xros/winmedia/fatfs/ffunicode.o

$(XB)/core/mmu.o: core/mmu.c core/*.h host/*.h
	@mkdir -p $(dir $@)
	$(XR_CC) $(XR_CFLAGS) -mgeneral-regs-only -c $< -o $@
$(XB)/xros/winmedia/%.o: xros/winmedia/%.c xros/winmedia/*.h xros/winmedia/fatfs/*.h
	@mkdir -p $(dir $@)
	$(XR_CC) -std=c11 -O2 -g -Wall -Wno-unused-parameter -Ixros/winmedia -I$(XR_WIM)/include -c $< -o $@
$(XB)/%.o: %.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(XR_CC) $(XR_CFLAGS) -c $< -o $@
$(XB)/devices/fb_metal.o: devices/fb_metal.m devices/*.h core/*.h host/*.h
	@mkdir -p $(dir $@)
	$(XR_CC) $(XR_OBJCFLAGS) -c $< -o $@
$(XB)/host/enter.o: host/enter.S
	@mkdir -p $(dir $@)
	$(XR_CC) -c $< -o $@
$(XB)/winvm_all.o: $(XR_OBJ) xros/winvm.exports
	xcrun --sdk $(XR_PLATFORM) ld -r -arch arm64 -exported_symbols_list xros/winvm.exports $(XR_OBJ) -o $@
$(XB)/winmedia_all.o: $(XR_MEDIA_OBJ) xros/winmedia.exports
	xcrun --sdk $(XR_PLATFORM) ld -r -arch arm64 -exported_symbols_list xros/winmedia.exports $(XR_MEDIA_OBJ) -o $@
$(XB)/libwinvm.a: $(XB)/winvm_all.o $(XB)/winmedia_all.o
	rm -f $@
	xcrun --sdk $(XR_PLATFORM) libtool -static -o $@ $^
xros-lib: $(XB)/libwinvm.a
.PHONY: xros-lib

# visionmac: the PC built, run and taken down several times in one process (tests/test_cycle.c).
$(B)/test_cycle: tests/test_cycle.c $(ENGINE) $(DEV) $(VM_HOST)
	$(CC) $(CFLAGS) tests/test_cycle.c $(ENGINE) $(DEV) $(VM_HOST) $(SLIRP_LIBS) $(MAC_FRAMEWORKS) -o $@
