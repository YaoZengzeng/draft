#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <linux/sockios.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/signal.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/errqueue.h>

#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

void main_loop(int icmp_sock, char *packet, int packlen);
int parse_reply(struct msghdr *msg, int cc, void *addr, struct timeval *tv);
int gather_statistics(char *ptr, int cc, unsigned short seq, int hops,
				int csfailed, struct timeval *tv, char *from);
int pinger(void);
uint16_t in_cksum(uint16_t *addr, int len, uint16_t cksum);

#define DEFDATALEN	(64 - 8)	/* default data length */
#define MAXIPLEN	60
#define MAXICMPLEN	76

struct sockaddr_in whereto;	/* who to ping */
int icmp_sock;		/* socket file descriptor */
char *hostname;

int datalen = DEFDATALEN;

int main(int argc, char **argv) {
	int packlen;
	char *target;
	char *packet;

	icmp_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (icmp_sock < 0) {
		perror("ping: icmp open socket");
		return 1;
	}

	if (argc < 2) {
		fprintf(stderr, "ping: destination address needed\n");
		return 1;
	}

	target = argv[1];
	bzero((char *)&whereto, sizeof(whereto));

	whereto.sin_family = AF_INET;

	if (inet_aton(target, &whereto.sin_addr) == 1){
		hostname = target;
	} else {
		fprintf(stderr, "ping: inet_aton %s failed\n", target);
		return 1;
	}

	packlen = datalen + MAXIPLEN + MAXICMPLEN;
	if (!(packet = (char *)malloc(packlen))) {
		fprintf(stderr, "ping: out of memory.\n");
		return 1;
	}

	printf("PING %s (%s) ", hostname, inet_ntoa(whereto.sin_addr));
	printf("%d(%d) bytes of data.\n", datalen, datalen + 8 + 20);

	main_loop(icmp_sock, packet, packlen);
}

void main_loop(int icmp_sock, char *packet, int packlen) {
	char addrbuf[128];
	char ans_data[4096];
	struct iovec iov;
	struct msghdr msg;
	struct cmsghdr *c;
	int cc;

	iov.iov_base = packet;

	for (;;) {
		pinger();

		for (;;) {
			struct timeval *recv_timep = NULL;
			struct timeval recv_time;

			iov.iov_len = packlen;
			memset(&msg, 0, sizeof(msg));
			msg.msg_name = addrbuf;
			msg.msg_namelen = sizeof(addrbuf);
			msg.msg_iov = &iov;
			msg.msg_iovlen = 1;
			msg.msg_control = ans_data;
			msg.msg_controllen = sizeof(ans_data);

			cc = recvmsg(icmp_sock, &msg, 0);
			if (cc < 0) {
				fprintf(stderr, "ping: recvmsg failed\n");
				return;
			} 

			parse_reply(&msg, cc, addrbuf, recv_timep);
			break;
		}

		sleep(1);
	}
}

int parse_reply(struct msghdr *msg, int cc, void *addr, struct timeval *tv) {
	struct sockaddr_in *from = addr;
	char *buf = msg->msg_iov->iov_base;
	struct icmphdr *icp;
	struct iphdr *ip;
	int hlen;

	// Check the IP header
	ip = (struct iphdr *)buf;
	hlen = ip->ihl * 4;
	if (cc < hlen + 8 || ip->ihl < 5) {
		fprintf(stderr, "ping: packet too short from %s\n", inet_ntoa(from->sin_addr));
		return 1;
	}

	// Now the ICMP part
	cc -= hlen;
	icp = (struct icmphdr *)(buf + hlen);

	if (icp->type == ICMP_ECHOREPLY) {
		printf("%d bytes from %s: icmp_seq=%u ttl=%d\n", cc,inet_ntoa(from->sin_addr),
							ntohs(icp->un.echo.sequence), ip->ttl);
	} else {
		fprintf(stderr, "ping: not process type other than ICMP_ECHOREPLY\n");
		return 1;
	}

	return 0;
}

/*
 * Compose and transmit an ICMP ECHO REQUEST packet. The IP packet
 * will be added by the kernel. The ID field is our UNIX process ID,
 * and the sequence number is an ascending integer. The first 8 bytes
 * of the data portion are used to hold an UNIX "timeval" struct in VAX
 * byte-order, to compute the round-trip time.
 */
int pinger(void) {
	static ntransmitted = 1;
	char outpack[256];
	struct icmphdr *icp;
	int i, cc;

	icp = (struct icmphdr *)outpack;
	icp->type = ICMP_ECHO;
	icp->code = 0;
	icp->checksum = 0;
	icp->un.echo.sequence = htons(ntransmitted);
	ntransmitted++;

	cc = datalen + 8;

	icp->checksum = in_cksum((uint16_t *)icp, cc, 0);

	struct iovec iov = {outpack, cc};
	struct msghdr m = { &whereto, sizeof(whereto),
				&iov, 1, 0, 0, 0};

	i = sendmsg(icmp_sock, &m, 0);
	if (i <= 0) {
		perror("sendmsg failed\n");
	}

	return (cc == i ? 0 : i);
}

uint16_t in_cksum(uint16_t *addr, int len, uint16_t cksum) {
	int sum = cksum;
	int nleft = len;
	uint16_t *w = addr, answer;

	while(nleft > 1) {
		sum += *w++;
		nleft -= 2;
	}

	if (nleft == 1) {
		sum += htons(*(char *)w << 8);
	}

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	answer = ~sum;

	return answer;
}
