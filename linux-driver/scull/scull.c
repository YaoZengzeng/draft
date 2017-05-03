#include <linux/module.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/types.h>
#include <linux/fs.h>		/* everything...*/
#include <linux/cdev.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/seq_file.h>
#include <linux/ioctl.h>
#include <asm/uaccess.h>	/* copy_*_user */

MODULE_LICENSE("GPL");

#define SCULL_NR_DEVS 4
#define SCULL_P_NR_DEVS 4
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

#define SCULL_P_BUFFER 4000

/*
 * Ioctl definitions
 */
// Use 'k' as magic number
#define SCULL_IOC_MAGIC	'k'

#define SCULL_IOCRESET	_IO(SCULL_IOC_MAGIC, 0)

/*
 * T means "Tell" directly with the argument value
 * Q means "Query": response is on the return value
 * H means "SHift": switch T and Q atomatically
 */
#define SCULL_IOCTQUANTUM	_IO(SCULL_IOC_MAGIC, 1)
#define SCULL_IOCTQSET		_IO(SCULL_IOC_MAGIC, 2)
#define SCULL_IOCQQUANTUM	_IO(SCULL_IOC_MAGIC, 3)
#define SCULL_IOCQQSET		_IO(SCULL_IOC_MAGIC, 4)
#define SCULL_IOCHQUANTUM	_IO(SCULL_IOC_MAGIC, 5)
#define SCULL_IOCHQSET		_IO(SCULL_IOC_MAGIC, 6)

#define SCULL_IOC_MAXNR 6

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

struct scull_pipe {
	wait_queue_head_t inq, outq;	// read and write queues
	char *buffer, *end;		// begin of buf, end of buf
	int buffersize;			// used in pointer arithmetic
	char *rp, *wp;			// where to read, where to write
	int nreaders, nwriters;		// number of openings for r/w
	struct semaphore sem;		// mutual exclusion semaphore
	struct cdev cdev;		// char device structure
};

struct scull_dev *scull_devices;	// Allocated in scull_init_module

int scull_major = 0;
int scull_minor = 0;
int scull_quantum = SCULL_QUANTUM;
int scull_qset = SCULL_QSET;

int scull_p_nr_devs = SCULL_P_NR_DEVS;	// number of pipe devices
int scull_p_buffer = SCULL_P_BUFFER;	// buffer size
dev_t scull_p_devno;

struct scull_pipe *scull_p_devices;

ssize_t scull_p_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
	struct scull_pipe *dev = filp->private_data;

	if (down_interruptible(&dev->sem)) {
		return -ERESTARTSYS;
	}

	// The while loop tests the buffer with the device semaphore held.
	// If there is data there, we know we can return it to the user 
	// immediately without sleeping, so the entire body of the loop is
	// skipped.
	while(dev->rp == dev->wp) { // Nothing to read
		up(&dev->sem); // Release the lock

		// Return if the user has requested non-blocking I/O
		if (filp->f_flags & O_NONBLOCK) {
			return -EAGAIN;
		}

		// Otherwise go to sleep
		printk("scullpipe: reading go to sleep");

		// Something has awakened us but we do not know that.
		// One possibility is that the process received a signal.
		// The if statement that contains the wait_event_interruptible call
		// checks for this case.This statement ensures the proper and
		// expected reaction to signals, which could have been responsible
		// for waking up the process. If a signal has arrived and it has
		// not been blocked by the process, the proper behavior is to let
		// upper layers of the kernel handle that event. To this end, the
		// driver returns -ERESTARTSYS to the caller; this value is used
		// internally by the virtual filesystem (VFS) layer, which either
		// restart the system call or returns -EINTR to user space.
		if (wait_event_interruptible(dev->inq, (dev->rp != dev->wp)))	{
			return -ERESTARTSYS;	// signal: tell the fs layer to handle it
		}

		// However, even in the absence of a signal, we do not yet know
		// sure that there is data there for the taking. Somebody else
		// could have been waiting for data as well, and they might
		// win the race and get the data first. So we must get the
		// semaphore again; only then can we test the read buffer again
		// (in the while loop) and truly know that we can return the
		// data in the buffer to the user.
		if (down_interruptible(&dev->sem)) {
			return -ERESTARTSYS;
		}
	}

	// We know that the semaphore is held and the buffer contains data
	// that we can use. We can now read the data
	if (dev->wp > dev->rp) {
		count = min(count, (size_t)(dev->wp - dev->rp));
	} else {
		// the write point has wrapped, return data up to dev->end
		count = min(count, (size_t)(dev->end - dev->rp));
	}

	if (copy_to_user(buf, dev->rp, count)) {
		up(&dev->sem);
		return -EFAULT;
	}

	dev->rp += count;
	if (dev->rp == dev->end) {
		// wrapped
		dev->rp = dev->buffer;
	}

	up(&dev->sem);

	// finally, awaken any writers and return
	wake_up_interruptible(&dev->outq);
	printk("scullpipe: read %li bytes\n", (long)count);

	return count;
}

// How much space is free
int spacefree(struct scull_pipe *dev) {
	if (dev->rp == dev->wp) {
		return dev->buffersize - 1;
	}
	return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}

// Wait for space for writing; caller must hold device semaphore. On
// error the semaphore will be released before returning
int scull_getwritespace(struct scull_pipe *dev, struct file *filp) {
	while(spacefree(dev) == 0) { // full
		DEFINE_WAIT(wait);

		up(&dev->sem);
		if (filp->f_flags & O_NONBLOCK) {
			return -EAGAIN;
		}
		printk("writing: going to sleep\n");

		prepare_to_wait(&dev->outq, &wait, TASK_INTERRUPTIBLE);
		if (spacefree(dev) == 0) {
			schedule();
		}

		finish_wait(&dev->outq, &wait);

		// Signal: tell the fs layer to handle it
		if (signal_pending(current)) {
			return -ERESTARTSYS;
		}
		if (down_interruptible(&dev->sem)) {
			return -ERESTARTSYS;
		}
	}

	return 0;
}

ssize_t scull_p_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_ops) {
	struct scull_pipe *dev = filp->private_data;
	int result;

	if (down_interruptible(&dev->sem)) {
		return -ERESTARTSYS;
	}

	// Make sure there's space to write
	result = scull_getwritespace(dev, filp);
	if (result) {
		// scull_getwritespace called up(&dev->sem)
		return result;
	}

	// ok, space there, accept something
	count = min(count, (size_t)spacefree(dev));
	if (dev->wp >= dev->rp) {
		count = min(count, (size_t)(dev->end - dev->wp)); // to end-of-buffer
	} else {
		// the write pointer has wrapped, fill up to rp-1
		count = min(count, (size_t)(dev->rp - dev->wp - 1));
	}
	printk("scullpipe: accept %li bytes\n", (long)count);

	if (copy_from_user(dev->wp, buf, count)) {
		up(&dev->sem);
		return -EFAULT;
	}
	dev->wp += count;
	if (dev->wp == dev->end) {
		dev->wp = dev->buffer; // wrapped
	}
	up(&dev->sem);

	// finally, awake any reader
	wake_up_interruptible(&dev->inq);	// blocked in read() and select()
	printk("scullpipe: write %li bytes\n", (long)count);

	return count;
}

int scull_p_open(struct inode *inode, struct file *filp) {
	struct scull_pipe *dev;

	dev = container_of(inode->i_cdev, struct scull_pipe, cdev);
	filp->private_data = dev;

	if (down_interruptible(&dev->sem)) {
		return -ERESTARTSYS;
	}
	if (!dev->buffer) {
		// allocate the buffer
		dev->buffer = kmalloc(scull_p_buffer, GFP_KERNEL);
		if (!dev->buffer) {
			up(&dev->sem);
			return -ENOMEM;
		}
	}
	dev->buffersize = scull_p_buffer;
	dev->end = dev->buffer + dev->buffersize;
	dev->rp = dev->wp = dev->buffer; // rd and wr from the beginning

	if (filp->f_mode & FMODE_READ) {
		dev->nreaders++;
	}
	if (filp->f_mode & FMODE_WRITE) {
		dev->nwriters++;
	}
	up(&dev->sem);

	printk("scullpipe: open successfully\n");

	return nonseekable_open(inode, filp);
}

// The file operations for the pipe device
struct file_operations scull_pipe_fops = {
	.owner	=	THIS_MODULE,
	.open	= 	scull_p_open,
	.read	=	scull_p_read,
	.write	=	scull_p_write,
};

// Set up a cdev entry
void scull_p_setup_cdev(struct scull_pipe *dev, int index) {
	int err, devno = scull_p_devno + index;

	cdev_init(&dev->cdev, &scull_pipe_fops);
	dev->cdev.owner = THIS_MODULE;
	err = cdev_add(&dev->cdev, devno, 1);
	// Fail gracefully if need be
	if (err) {
		printk("Error %d adding scullpipe%d", err, index);
	}
}

// Initialize the pipe devs; return how many we did
int scull_p_init(dev_t firstdev) {
	int i, result;

	result = register_chrdev_region(firstdev, scull_p_nr_devs, "scullp");
	if (result < 0) {
		printk("Unable to get scullp region, error %d\n", result);
		return 0;
	}
	scull_p_devno = firstdev;
	scull_p_devices = kmalloc(scull_p_nr_devs * sizeof(struct scull_pipe), GFP_KERNEL);
	if (scull_p_devices == NULL) {
		unregister_chrdev_region(firstdev, scull_p_nr_devs);
		return 0;
	}
	memset(scull_p_devices, 0, scull_p_nr_devs * sizeof(struct scull_pipe));
	for (i = 0; i < scull_p_nr_devs; i++) {
		init_waitqueue_head(&(scull_p_devices[i].inq));
		init_waitqueue_head(&(scull_p_devices[i].outq));
		sema_init(&scull_p_devices[i].sem, 1);
		scull_p_setup_cdev(scull_p_devices + i, i);
	}

	// create_proc_read_entry("scullpipe", 0, NULL, scull_read_p_mem, NULL);
	return scull_p_nr_devs;
}

// This is called by cleanup_module or on failure.
// It is required to never fail, even if nothing was initialized first
void scull_p_cleanup(void) {
	int i;

	if (!scull_p_devices) {
		return;	// Nothing else to release
	}

	for (i = 0; i < scull_p_nr_devs; i++) {
		cdev_del(&scull_p_devices[i].cdev);
		kfree(scull_p_devices[i].buffer);
	}
	kfree(scull_p_devices);
	unregister_chrdev_region(scull_p_devno, scull_p_nr_devs);
	scull_p_devices = NULL;
}

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

// The ioctl() implementation
long scull_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
	int err = 0, tmp;
	int retval = 0;

	// extract the type and number bitfields, and don't decode
	// wrong cmds: return ENOTTY (inappropriate ioctl) before access_ok()
	if (_IOC_TYPE(cmd) != SCULL_IOC_MAGIC) {
		return -ENOTTY;
	}
	if (_IOC_NR(cmd) > SCULL_IOC_MAXNR) {
		return -ENOTTY;
	}

	// the direction is a bitmask, and VERIFY_WRITE catches R/W
	// transfers. `Type` is a user-oriented, while
	// access_ok is kernel-oriented, so the concept of "read" and
	// "write" is reserved
	if (_IOC_DIR(cmd) & _IOC_READ) {
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	} else if (_IOC_DIR(cmd) & _IOC_WRITE) {
		err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	}
	if (err) {
		return -EFAULT;
	}
	
	switch (cmd) {
	case SCULL_IOCRESET:
		scull_quantum = SCULL_QUANTUM;
		scull_qset = SCULL_QSET;
		break;
	
	case SCULL_IOCTQUANTUM: /* Tell: arg is the value */
		if (!capable(CAP_SYS_ADMIN)) {
			return -EPERM;
		}
		scull_quantum = arg;
		break;
	
	case SCULL_IOCQQUANTUM: /* Query: return it (it's positive) */
		return scull_quantum;
	
	case SCULL_IOCHQUANTUM: /* sHift: like Tell + Query */
		if (!capable(CAP_SYS_ADMIN)) {
			return -EPERM;
		}
		tmp = scull_quantum;
		scull_quantum = arg;
		return tmp;
	
	case SCULL_IOCTQSET:
		if (!capable(CAP_SYS_ADMIN)) {
			return -EPERM;
		}
		scull_qset = arg;
		break;

	case SCULL_IOCQQSET:
		return scull_qset;

	case SCULL_IOCHQSET:
		if (!capable(CAP_SYS_ADMIN)) {
			return -EPERM;
		}
		tmp= scull_qset;
		scull_qset = arg;
		return tmp;

	default: /* redundant, as cmd was checked against MAXNR */
		return -ENOTTY;
		
	}
	return retval;
}

struct file_operations scull_fops = {
	.owner	= THIS_MODULE,
	.open	= scull_open,
	.release = scull_release,
	.read  = scull_read,
	.write = scull_write,
	.unlocked_ioctl = scull_ioctl,
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

// The proc filesystem: function to read and entry
int scull_read_procmem(char *buf, char **start, off_t offset,
			int count, int *eof, void *data) {
	int i, len = 0;

	for (i = 0; i < SCULL_NR_DEVS; i++) {
		struct scull_dev *d = &scull_devices[i];
		
		len += sprintf(buf + len, "\nDevice %i: qset %i, q %i, sz %li\n", i, d->qset, d->quantum, d->size);
	}
	*eof = 1;

	return len;
}

// Here are our sequence iteration methods. Our "postion" is
// simply the device number
static void *scull_seq_start(struct seq_file *s, loff_t *pos) {
	if (*pos >= SCULL_NR_DEVS) {
		return NULL;
	}
	return scull_devices + *pos;
}

static void *scull_seq_next(struct seq_file *s, void *v, loff_t *pos) {
	(*pos)++;
	if (*pos >= SCULL_NR_DEVS) {
		return NULL;
	}
	return scull_devices + *pos;
}

static void scull_seq_stop(struct seq_file *s, void *v) {
	// Actually, there's nothing to do here
}

static int scull_seq_show(struct seq_file *s, void *v) {
	struct scull_dev *dev = (struct scull_dev *) v;
	struct scull_qset *d;
	int i;

	seq_printf(s, "\nDevice %i: qset %i, q %i, sz %li\n",
		(int) (dev - scull_devices), dev->qset,
		dev->quantum, dev->size);
	for (d = dev->data; d; d = d->next) { // scan the list
		seq_printf(s, " item at %p, qset at %p\n", d, d->data);
		if (d->data && !d->next) {
			for (i = 0; i < dev->qset; i++) {
				if (d->data[i]) {
					seq_printf(s, " % 4i: %8p\n", i, d->data[i]);
				}
			}
		}
	}

	return 0;
}

// Tie the sequence operators up
static struct seq_operations scull_seq_ops = {
	.start	= scull_seq_start,
	.next	= scull_seq_next,
	.stop	= scull_seq_stop,
	.show	= scull_seq_show
};

// Now to implement the /proc file we need only make an open
// method which set up the sequence operators
static int scull_proc_open(struct inode *inode, struct file *file) {
	return seq_open(file, &scull_seq_ops);
}

// Create a set of file operations for our proc file
static struct file_operations scull_proc_ops = {
	.owner	= THIS_MODULE,
	.open	= scull_proc_open,
	.read	= seq_read,
	.llseek = seq_lseek,
	.release = seq_release
};

// Acutally create (and remove) the /proc file(s)
static void scull_create_proc(void) {
	struct proc_dir_entry *entry;
	create_proc_read_entry("scullmem", 0 /* default mode */,
				NULL /* parent dir */, scull_read_procmem,
				NULL /* client data */);
	entry = create_proc_entry("scullseq", 0, NULL);
	if (entry) {
		entry->proc_fops = &scull_proc_ops;
	}
}

static void scull_remove_proc(void) {
	// No problem if it was not registered
	remove_proc_entry("scullmem", NULL /* Parent dir */);
	remove_proc_entry("scullseq", NULL);
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

	// At this point call the init funciton for any friend device
	dev = MKDEV(scull_major, scull_minor + SCULL_NR_DEVS);
	dev += scull_p_init(dev);
	
	scull_create_proc();

	printk("scull: module init succeed\n");
	return 0; /* succeed */

fail:
	scull_cleanup_module();
	return result;
}

void scull_cleanup_module(void) {
	dev_t devno = MKDEV(scull_major, scull_minor);

	scull_remove_proc();

	unregister_chrdev_region(devno, SCULL_NR_DEVS);

	// call the cleanup functions for friend devices
	scull_p_cleanup();

	printk("scull: module clean up succeed\n");
}

module_init(scull_init_module);
module_exit(scull_cleanup_module);

