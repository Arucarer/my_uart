obj-m += my_uart.o

my_uart-objs := my_uart_core.o \
                my_uart_port.o \
                my_uart_proc.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean