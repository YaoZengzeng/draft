#include <linux/module.h>
#include <linux/init.h>

#include <linux/kernel.h>	/* printk() */
#include <linux/types.h>
#include <linux/fs.h>

MODULE_LICENSE("GPL");

#define SCULL_NR_DEVS 4

int scull_major = 0;
int scull_minor = 0;

int scull_init_module(void) {
	int result;
	dev_t dev = 0;

	result = alloc_chrdev_region(&dev, scull_minor, SCULL_NR_DEVS, "scull");
	if (result < 0) {
		printk(KERN_WARNING "scull: can't allocate major number\n");
		return result;
	}
	scull_major = MAJOR(dev);
	printk("scull: major number is %d\n", scull_major);

	printk("scull: module init succeed\n");
	return 0; /* succeed */
}

void scull_cleanup_module(void) {
	dev_t devno = MKDEV(scull_major, scull_minor);

	unregister_chrdev_region(devno, SCULL_NR_DEVS);

	printk("scull: module clean up succeed\n");
}

module_init(scull_init_module);
module_exit(scull_cleanup_module);

