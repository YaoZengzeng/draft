#include <linux/module.h>
#include <linux/init.h>

#include <linux/kernel.h>	/* printk() */
#include <linux/types.h>
#include <linux/fs.h>		/* everything...*/
#include <linux/cdev.h>
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <asm/uaccess.h>	/* copy_*_user */

MODULE_LICENSE("GPL");

#define SCULL_NR_DEVS 4
/* The bare device is a variable-length region of memory.
 * Use a linked list of indirect blocks.
 *
 * "scull_dev->data" points to an array of pointers, each
 * pointer refers to a memory area of SCULL_QUANTUM bytes.
 *
 * The array (quantum-set) is SCULL_QSET long
 */
#define SCULL_QUANTUM 4000
#define SCULL_QSET 1000

// Representation of scull quantum sets
struct scull_qset {
	void **data;
	struct scull_qset *next;
};

struct scull_dev {
	struct scull_qset *data; // Pointer to first quantum set
	int quantum;		// the current quantum size
	int qset;		// the current arrary size
	unsigned long size;	// amount of data stored here
	struct cdev	cdev;	// Char device structure
};

struct scull_dev *scull_devices;	// Allocated in scull_init_module

int scull_major = 0;
int scull_minor = 0;
int scull_quantum = SCULL_QUANTUM;
int scull_qset = SCULL_QSET;

void scull_cleanup_module(void);

// Follow the list
struct scull_qset *scull_follow(struct scull_dev *dev, int n) {
	struct scull_qset *qs = dev->data;

	// Allocate first qset explicitly if need be
	if (!qs) {
		qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
		if (qs == NULL) {
			return NULL;
		}
		memset(qs, 0, sizeof(struct scull_qset));
	}

	// The follow the list
	while(n--) {
		if (!qs->next) {
			qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
			if (qs->next == NULL) {
				return NULL;
			}
			memset(qs->next, 0, sizeof(struct scull_qset));
		}
		qs = qs->next;
	}
	
	return qs;
}

// Empty out the scull device
int scull_trim(struct scull_dev *dev) {
	struct scull_qset *next, *dptr;
	int qset = dev->qset;	/* "dev" is not-null */
	int i;

	for (dptr = dev->data; dptr; dptr = next) {
		if (dptr->data) {
			for (i = 0; i < qset; i++) {
				kfree(dptr->data[i]);
			}
			kfree(dptr->data);
			dptr->data = NULL;
		}
		next = dptr->next;
		kfree(dptr);
	}
	dev->size = 0;
	dev->quantum = scull_quantum;
	dev->qset = scull_qset;
	dev->data = NULL;
	return 0;
}

int scull_open(struct inode *inode, struct file *flip) {
	struct scull_dev *dev;	// device information
	
	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	flip->private_data = dev;

	// Now trim to 0 the length of the devices if open was right only
	if ( (flip->f_flags & O_ACCMODE) == O_WRONLY) {
		scull_trim(dev);
	}

	printk("scull: open successfully\n");

	return 0;
}

int scull_release(struct inode *inode, struct file *flip) {
	return 0;
}

ssize_t scull_read(struct file *filp, char __user *buf, size_t count,
		loff_t *f_pos) {
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum, qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
	ssize_t retval = 0;
	
	if (*f_pos >= dev->size) {
		goto out;
	}
	if (*f_pos + count > dev->size) {
		count = dev->size - *f_pos;
	}

	// find listitem, qset index, and offset in the quantum
	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;

	// follow the list up to the right position
	dptr = scull_follow(dev, item);

	if (dptr == NULL || !dptr->data || !dptr->data[s_pos]) {
		goto out;
	}

	// read only up to the end of this quantum
	if (count > quantum - q_pos) {
		count = quantum - q_pos;
	}

	if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
		retval = -EFAULT;
		goto out;
	}
	*f_pos += count;
	retval = count;

	printk("scull: read successfully\n");
out:
	return retval;
}

ssize_t scull_write(struct file *filp, const char __user *buf, size_t count,
		loff_t *f_pos) {
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum, qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
	ssize_t retval = -ENOMEM;

	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;

	dptr = scull_follow(dev, item);
	if (dptr == NULL) {
		goto out;
	}
	if (!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
		if (!dptr->data) {
			goto out;
		}
		memset(dptr->data, 0, qset * sizeof(char *));
	}
	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
		if (!dptr->data[s_pos]) {
			goto out;
		}
	}
	// Write only up to the end of this quantum
	if (count > quantum - q_pos) {
		count = quantum - q_pos;
	}

	if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;

	// Update the size
	if (dev->size < *f_pos) {
		dev->size = *f_pos;
	}
	
	printk("scull: write successfully\n");
out:
	return retval;
}

struct file_operations scull_fops = {
	.owner	= THIS_MODULE,
	.open	= scull_open,
	.release = scull_release,
	.read  = scull_read,
	.write = scull_write,
};

// Set up the char_dev structure for this device
static void scull_setup_cdev(struct scull_dev *dev, int index) {
	int err, devno = MKDEV(scull_major, scull_minor + index);

	cdev_init(&dev->cdev, &scull_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_fops;
	err = cdev_add(&dev->cdev, devno, 1);
	// Fail gracefully if need be
	if (err) {
		printk(KERN_NOTICE "Error %d adding scull%d", err, index);
	}
}

int scull_init_module(void) {
	int result, i;
	dev_t dev = 0;

	result = alloc_chrdev_region(&dev, scull_minor, SCULL_NR_DEVS, "scull");
	if (result < 0) {
		printk(KERN_WARNING "scull: can't allocate major number\n");
		return result;
	}
	scull_major = MAJOR(dev);
	printk("scull: major number is %d\n", scull_major);

	scull_devices = kmalloc(SCULL_NR_DEVS*sizeof(struct scull_dev), GFP_KERNEL);
	if (!scull_devices) {
		result = -ENOMEM;
		goto fail;
	}
	memset(scull_devices, 0, SCULL_NR_DEVS * sizeof(struct scull_dev));

	for (i = 0; i < SCULL_NR_DEVS; i++) {
		scull_devices[i].quantum = scull_quantum;
		scull_devices[i].qset = scull_qset;
		scull_setup_cdev(&scull_devices[i], i);
	}
	printk("scull: module init succeed\n");
	return 0; /* succeed */

fail:
	scull_cleanup_module();
	return result;
}

void scull_cleanup_module(void) {
	dev_t devno = MKDEV(scull_major, scull_minor);

	unregister_chrdev_region(devno, SCULL_NR_DEVS);

	printk("scull: module clean up succeed\n");
}

module_init(scull_init_module);
module_exit(scull_cleanup_module);

