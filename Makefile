obj-m += membuf.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

TEST = test_membuf

all: module test

module:
	make -C $(KDIR) M=$(PWD) modules

test: $(TEST)

$(TEST): test_membuf.c
	gcc -pthread -o $(TEST) test_membuf.c

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f $(TEST)