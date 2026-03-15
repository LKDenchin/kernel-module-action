# 替换你的源文件名称
obj-m += tcp-pixie.o

# 使用 KDIR 变量，如果未定义，则默认使用当前系统的构建路径
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
