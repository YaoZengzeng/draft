#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>

#include <asm/hardirq.h>

// Use these as data pointers, to implement four files in one function
enum jit_file {
	JIT_BUSY,
	JIT_SCHED,
	JIT_QUEUE,
	JIT_SCHEDTO
};

int delay = HZ;	// The default delay, expressed in jiffies

// This function prints one line of data, after sleeping one second
// It can sleep in different ways, accoring to the data pointer
int jit_fn(char *buf, char **start, off_t offset,
		int len, int *eof, void *data) {
	unsigned long j0, j1;
	wait_queue_head_t wait;

	init_waitqueue_head(&wait);
	j0 = jiffies;
	j1 = j0 + delay;

	switch((long)data) {
		case JIT_BUSY:
			while(time_before(jiffies, j1)) {
				cpu_relax();
			}
			break;
		case JIT_SCHED:
			while(time_before(jiffies, j1)) {
				schedule();
			}
			break;
		case JIT_QUEUE:
			wait_event_interruptible_timeout(wait, 0, delay);
			break;
		case JIT_SCHEDTO:
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(delay);
			break;
	}
	j1 = jiffies; // Actual value after we delayed

	len = sprintf(buf, "%9li %9li\n", j0, j1);
	*start = buf;

	return len;
}

// This file, on the other hand, returns the current time forever
int jit_currentime(char *buf, char **start, off_t offset,
		int len, int *eof, void *data) {
	struct timeval tv1;
	struct timespec tv2;
	unsigned long j1;
	u64 j2;

	j1 = jiffies;
	j2 = get_jiffies_64();
	do_gettimeofday(&tv1);
	tv2 = current_kernel_time();

	len = 0;
	len += sprintf(buf, "0x%08lx 0x%016Lx %10i.%06i\n"
			"%40i.%09i\n",
			j1, j2,
			(int) tv1.tv_sec, (int) tv1.tv_usec,
			(int) tv2.tv_sec, (int) tv2.tv_nsec);
	//*start = buf;
	*eof = 1;

	return len;
}

// The timer example follows
int tdelay = 10;

// This data structure used as "data" for the timer and tasklet functions
struct jit_data {
	struct timer_list timer;
	wait_queue_head_t wait;
	unsigned long prevjiffies;
	unsigned char *buf;
	int loops;
};
#define JIT_ASYNC_LOOPS 5

void jit_timer_fn(unsigned long arg) {
	struct jit_data *data = (struct jit_data *)arg;
	unsigned long j = jiffies;
	data->buf += sprintf(data->buf, "%9li  %3li     %i    %6i   %i   %s\n",
			j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);
	if (--data->loops) {
		data->timer.expires += tdelay;
		data->prevjiffies = j;
		add_timer(&data->timer);
	} else {
		wake_up_interruptible(&data->wait);
	}
}

int jit_timer(char *buf, char **start, off_t offset,
	int len, int *eof, void *unused_data) {
	struct jit_data *data;
	char *buf2 = buf;
	unsigned long j = jiffies;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		return -ENOMEM;
	}
	init_timer(&data->timer);
	init_waitqueue_head(&data->wait);

	// Write the first lines in the buffer
	buf2 += sprintf(buf2, "   time   delta   inirq    pid   cpu command\n");
	buf2 += sprintf(buf2, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);

	// Fill the data for our timer function
	data->prevjiffies = j;
	data->buf = buf2;
	data->loops = JIT_ASYNC_LOOPS;

	// Register the timer
	data->timer.data = (unsigned long)data;
	data->timer.function = jit_timer_fn;
	data->timer.expires = j + tdelay;
	add_timer(&data->timer);

	// Wait for the buffer to fill
	wait_event_interruptible(data->wait, !data->loops);
	if (signal_pending(current)) {
		return -ERESTARTSYS;
	}
	buf2 = data->buf;
	kfree(data);
	*eof = 1;

	return buf2 - buf;
}

int __init jit_init(void) {
	create_proc_read_entry("currentime", 0, NULL, jit_currentime, NULL);
	create_proc_read_entry("jitbusy", 0, NULL, jit_fn, (void *)JIT_BUSY);
	create_proc_read_entry("jitsched", 0, NULL, jit_fn, (void *)JIT_SCHED);
	create_proc_read_entry("jitqueue", 0, NULL, jit_fn, (void *)JIT_QUEUE);
	create_proc_read_entry("jitschedto", 0, NULL, jit_fn, (void *)JIT_SCHEDTO);
	create_proc_read_entry("jitimer", 0, NULL, jit_timer, NULL);

	return 0;
}

void __exit jit_cleanup(void) {
	remove_proc_entry("currentime", NULL);
	remove_proc_entry("jitbusy", NULL);
	remove_proc_entry("jitsched", NULL);
	remove_proc_entry("jitqueue", NULL);
	remove_proc_entry("jitschedto", NULL);

	remove_proc_entry("jitimer", NULL);
}

module_init(jit_init);
module_exit(jit_cleanup);


