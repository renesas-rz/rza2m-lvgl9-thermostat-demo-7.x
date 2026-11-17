# Smart-thermostat LVGL 9 demo for RZ/A2M Linux BSP, with D/AVE 2D (step 2)
# Build inside the BSP container after:  export ROOTDIR=$(pwd) ; source ./setup_env.sh

CC      ?= gcc
ifneq ($(CROSS_COMPILE),)
CC      := $(CROSS_COMPILE)gcc
endif
ARCH_FLAGS ?= -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
ifeq ($(CROSS_COMPILE),)
ARCH_FLAGS :=
endif

DAVE_INC := -Idave_driver/inc -Idave_driver/linux -Idave_driver/linux/driver_l1 \
            -Idave_driver/linux/memmgr -Idave_driver/linux/kernel

CFLAGS  ?= -O2
CFLAGS  += $(ARCH_FLAGS) -std=gnu99 -Wno-error=incompatible-pointer-types -I. -Ilvgl $(DAVE_INC) -DLV_CONF_INCLUDE_SIMPLE \
           -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections -lm -lpthread

# D/AVE 2D user-space library (talks to /dev/dave2d), built as Renesas ships it
DAVE_SRCS := $(wildcard dave_driver/code/*.c) $(wildcard dave_driver/linux/*.c) \
             $(wildcard dave_driver/linux/memmgr/*.c) $(wildcard dave_driver/linux/driver_l1/*.c)
DAVE_CFLAGS := -O2 $(ARCH_FLAGS) $(DAVE_INC) -w

SRCS := main.c $(wildcard thermostat/*.c) $(wildcard thermostat/fonts/*.c) a2m_disp.c a2m_texcache.c \
        $(shell find fonts -type f -name '*.c' -not -path '*/preview-*') \
        $(shell find lvgl/src -type f -name '*.c')
SRCS += thermostat/edge264/src/edge264.c
build/thermostat/edge264/src/edge264.o: CFLAGS += -std=gnu11 -flax-vector-conversions -Wno-override-init -w -O3

OBJS := $(patsubst %.c,build/%.o,$(SRCS)) $(patsubst %.c,build/%.o,$(DAVE_SRCS))
APP  := thermostat_a2m

all: $(APP)

build/dave_driver/%.o: dave_driver/%.c
	@mkdir -p $(dir $@)
	@echo CC $<
	@$(CC) $(DAVE_CFLAGS) -c $< -o $@

build/%.o: %.c lv_conf.h
	@mkdir -p $(dir $@)
	@echo CC $<
	@$(CC) $(CFLAGS) -c $< -o $@

$(APP): $(OBJS)
	@echo LD $@
	@$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)
	@ls -l $@

install: $(APP)
	@test -n "$(BUILDROOT_DIR)" || (echo "BUILDROOT_DIR not set: source setup_env.sh first" && exit 1)
	mkdir -p $(BUILDROOT_DIR)/output/rootfs_overlay/root/bin
	cp $(APP) $(BUILDROOT_DIR)/output/rootfs_overlay/root/bin/
	@echo "Now run ./build.sh buildroot to repack the rootfs"

clean:
	rm -rf build $(APP)

.PHONY: all install clean
