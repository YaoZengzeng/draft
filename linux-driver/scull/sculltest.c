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
	int n, flags;
	char buf[1024];

	if ((fd = open("/dev/scull0", O_WRONLY)) == -1) {
		perror("open scull failed");
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

	close(fd);

	if ((fd = open("/dev/scullpipe0", O_RDONLY)) == -1) {
		perror("open scullpipe failed\n");
		return -1;
	}

	flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	while( (n = read(fd, buf, sizeof(buf))) == -1) {
		perror("read failed");
		sleep(1);
	}
	printf("%s\n", buf);

	return 0;	
}

