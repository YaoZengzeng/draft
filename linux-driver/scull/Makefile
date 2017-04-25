obj-m := scull.o

KERNSRC = /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KERNSRC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNSRC) M=$(PWD) clean

