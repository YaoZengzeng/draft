#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <errno.h>
#include <stdarg.h>

#define PORT 		12345
#define IFNAME 		"tun0"
#define REMOTEIP 	"49.51.33.48"

#define CLIENT		0
#define SERVER		1

#define BUFSIZE 	2000

/*
 * tun_alloc: allocates or reconnects to a tun device.
 * The caller must reserve enough space in *dev.
 */
int tun_alloc(char *dev, int flags) {
	struct ifreq ifr;
	int fd, err;

	if ((fd = open("/dev/net/tun", O_RDWR)) < 0) {
		printf("open /dev/net/tun failed\n");
		return fd;
	}

	memset(&ifr, 0, sizeof(ifr));

	ifr.ifr_flags = flags;

	if (*dev) {
		strncpy(ifr.ifr_name, dev, IFNAMSIZ);
	}

	if ((err = ioctl(fd, TUNSETIFF, (void *)&ifr))) {
		printf("ioctl(TUNSETIFF) failed\n");
		close(fd);
		return err;
	}

	strcpy(dev, ifr.ifr_name);

	return fd;
}


// ensure we read exactly n bytes and puts them into "buf"
int read_n(int fd, char *buf, int n) {
	int nr, left = n;

	while (left > 0) {
		nr = read(fd, buf, left);
		if (nr <= 0) {
			return nr;
		}
		left -= nr;
		buf += nr;
	}

	return n;
}

int main(int argc, char *argv[]) {
	int tap_fd, option;
	int flags = IFF_TUN;
	char if_name[IFNAMSIZ] = IFNAME;
	char buffer[BUFSIZE];
	struct sockaddr_in local, remote;
	char remote_ip[16] = REMOTEIP;		/* dotted quad IP string */
	unsigned short int port = PORT;
	int sock_fd, net_fd, optval = 1;
	int maxfd;
	int cliserv = -1;	/* must be specified on cmd line */
	socklen_t remotelen;
	uint16_t nw, nr, l;
	unsigned long int tap2net = 0, net2tap = 0;

	/* Check command line options */
	while((option = getopt(argc, argv, "s:c")) > 0) {
		switch(option) {
			case 's':
				cliserv = SERVER;
				break;
			case 'c':
				cliserv = CLIENT;
				break;
		}
	}

	/* initialize tun interface */
	if ((tap_fd = tun_alloc(if_name, flags | IFF_NO_PI)) < 0) {
		printf("failed to connect to tun interface %s\n", if_name);
		exit(1);
	}

	printf("successfully connect to interface %s\n", if_name);

	if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		printf("socket() failed\n");
		exit(1);
	}

	if (cliserv == CLIENT) {
		/* Client, try to connect to server */

		/* assign the destination address */
		memset(&remote, 0, sizeof(remote));
		remote.sin_family = AF_INET;
		remote.sin_addr.s_addr = inet_addr(remote_ip);
		remote.sin_port = htons(port);

		/* connection request */
		if (connect(sock_fd, (struct sockaddr*) &remote, sizeof(remote)) < 0) {
			printf("connect() failed\n");
			exit(1);
		}

		net_fd = sock_fd;
		printf("client connect to server already\n");
	} else {
		/* Server, wait for connections */

		/* avoid EADDRINUSE error on bind() */
		if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&optval, sizeof(optval)) < 0) {
			printf("setsockopt() failed\n");
			exit(1);
		}

		memset(&local, 0, sizeof(local));
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_ANY);
		local.sin_port = htons(port);
		if (bind(sock_fd, (struct sockaddr*) &local, sizeof(local)) < 0) {
			printf("bind() failed\n");
			exit(0);
		}

		if (listen(sock_fd, 5) < 0) {
			printf("listen() failed\n");
			exit(1);
		}

		/* wait for connection request */
		remotelen = sizeof(remote);
		memset(&remote, 0, remotelen);
		if ((net_fd = accept(sock_fd, (struct sockaddr*)&remote, &remotelen)) < 0) {
			printf("accept() failed\n");
			exit(1);
		}

		printf("server: client connect from %s\n", inet_ntoa(remote.sin_addr));
	}

	/* use select() to handle two descriptors at once */
	maxfd = (tap_fd > net_fd) ? tap_fd : net_fd;

	while(1) {
		int ret;
		fd_set	rd_set;

		FD_ZERO(&rd_set);
		FD_SET(tap_fd, &rd_set), FD_SET(net_fd, &rd_set);

		ret = select(maxfd + 1, &rd_set, NULL, NULL, NULL);

		if (ret < 0 && errno == EINTR) {
			continue;
		}

		if (ret < 0) {
			printf("select() failed\n");
			exit(1);
		}

		if (FD_ISSET(tap_fd, &rd_set)) {
			/* data from tun: just read it and write it to the network */
			if ((nr = read(tap_fd, buffer, BUFSIZE)) < 0) {
				printf("read from tap_fd failed\n");
				exit(1);
			}


			tap2net++;
			printf("tap to net %lu: read %d bytes from the tap interface\n", tap2net, nr);

			/* write length + packet */
			l = htons(nr);
			if ((nw = write(net_fd, (char *)&l, sizeof(l))) < 0) {
				printf("write length to net_fd failed\n");
				exit(1);
			}
			if ((nw = write(net_fd, buffer, nr)) < 0) {
				printf("write buffer to net_fd failed\n");
				exit(1);
			}
			printf("tap 2 net %lu: write %d bytes to the network\n", tap2net, nw);
		}

		if (FD_ISSET(net_fd, &rd_set)) {
			/* data from the network: read it and write it to the tun interface.
			 * we need the length first, and then the packet */

			// Read length
			nr = read_n(net_fd, (char *)&l, sizeof(l));
			if (nr == 0) {
				break;
			}

			net2tap++;

			/* read packet */
			nr = read_n(net_fd, buffer, ntohs(l));
			printf("net to tap %lu: read %d bytes from the network\n", net2tap, nr);

			/* now buffer[] contains a full packet or frame, write it into the tun interface */
			if ((nw = write(tap_fd, buffer, nr)) < 0) {
				printf("write to tap_fd failed\n");
				exit(1);
			}
			printf("net to tap %lu: write %d bytes to the tun interface\n", net2tap, nw);
		}
	}

	return 0;
}
