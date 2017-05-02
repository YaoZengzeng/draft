#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <linux/ioctl.h>

// Use 'k' as magic number
#define SCULL_IOC_MAGIC 'k'

#define SCULL_IOCRESET  _IO(SCULL_IOC_MAGIC, 0)

#define SCULL_IOCTQUANTUM       _IO(SCULL_IOC_MAGIC, 1)
#define SCULL_IOCTQSET          _IO(SCULL_IOC_MAGIC, 2)
#define SCULL_IOCQQUANTUM       _IO(SCULL_IOC_MAGIC, 3)
#define SCULL_IOCQQSET          _IO(SCULL_IOC_MAGIC, 4)
#define SCULL_IOCHQUANTUM       _IO(SCULL_IOC_MAGIC, 5)
#define SCULL_IOCHQSET          _IO(SCULL_IOC_MAGIC, 6)


int main() {
	int fd, cmd, quantum;

	if ((fd = open("/dev/scull0", O_WRONLY)) == -1) {
		perror("open failed");
		return -1;
	}

	cmd = SCULL_IOCHQUANTUM;
	quantum = 8000;
	quantum = ioctl(fd, SCULL_IOCHQUANTUM, quantum);
	if (quantum < 0) {
		perror("ioctl failed");
		return -1;
	}
	printf("old quantum is %d\n", quantum);
		
	return 0;	
}

