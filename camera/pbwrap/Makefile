# pbwrap: OV13850 pBAA pre-processing V4L2 wrapper (out-of-tree module)
# build against the running 6.12.1-msm8916 kernel tree:
#   make KDIR=/path/to/linux-6.12.1-src modules
# (kernel tree must have been `make olddefconfig prepare` with the device
#  /proc/config.gz; BTF disabled in the *build* config is fine for ABI)
KDIR ?= $(HOME)/workspace/agent-workspace/kernel-extract/ksrc-6.12.1

ccflags-y := -O2 -I$(src)

obj-m := pbwrap.o

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm64 \
		CROSS_COMPILE=aarch64-linux-gnu- modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm64 \
		CROSS_COMPILE=aarch64-linux-gnu- clean

.PHONY: all modules clean
