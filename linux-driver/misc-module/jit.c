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

int __init jit_init(void) {
	create_proc_read_entry("currentime", 0, NULL, jit_currentime, NULL);
	create_proc_read_entry("jitbusy", 0, NULL, jit_fn, (void *)JIT_BUSY);
	create_proc_read_entry("jitsched", 0, NULL, jit_fn, (void *)JIT_SCHED);
	create_proc_read_entry("jitqueue", 0, NULL, jit_fn, (void *)JIT_QUEUE);
	create_proc_read_entry("jitschedto", 0, NULL, jit_fn, (void *)JIT_SCHEDTO);

	return 0;
}

void __exit jit_cleanup(void) {
	remove_proc_entry("currentime", NULL);
}

module_init(jit_init);
module_exit(jit_cleanup);


