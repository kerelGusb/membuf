obj-m += membuf.o
membuf-objs := src/membuf.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

TEST = test_membuf

all: module test

module:
	make -C $(KDIR) M=$(PWD) modules

test: $(TEST)

$(TEST): src/test_membuf.c
	gcc -pthread -o $(TEST) src/test_membuf.c

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f $(TEST)