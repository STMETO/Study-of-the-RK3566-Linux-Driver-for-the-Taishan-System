// SPDX-License-Identifier: GPL-2.0
/*
 * gpio3a1_cli.c - userspace command line tool for /dev/gpio3a1
 *
 *   gpio3a1_cli get          read the current level
 *   gpio3a1_cli set 0|1      drive the pin low / high
 *   gpio3a1_cli toggle       invert the current level
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEV_PATH	"/dev/gpio3a1"

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s get | set <0|1> | toggle\n", prog);
}

static int read_level(int fd)
{
	char buf[8];
	ssize_t n;

	n = read(fd, buf, sizeof(buf) - 1);
	if (n < 0) {
		perror("read");
		return -1;
	}
	buf[n] = '\0';
	printf("GPIO3_A1 = %s", buf);
	return 0;
}

int main(int argc, char **argv)
{
	int fd;

	if (argc < 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	fd = open(DEV_PATH, O_RDWR);
	if (fd < 0) {
		perror("open " DEV_PATH);
		return EXIT_FAILURE;
	}

	if (!strcmp(argv[1], "get")) {
		if (read_level(fd) < 0) {
			close(fd);
			return EXIT_FAILURE;
		}
	} else if (!strcmp(argv[1], "set")) {
		if (argc < 3) {
			usage(argv[0]);
			close(fd);
			return EXIT_FAILURE;
		}
		if (write(fd, argv[2], strlen(argv[2])) < 0) {
			perror("write");
			close(fd);
			return EXIT_FAILURE;
		}
		printf("GPIO3_A1 <- %s\n", argv[2]);
	} else if (!strcmp(argv[1], "toggle")) {
		if (write(fd, "toggle", 6) < 0) {
			perror("write");
			close(fd);
			return EXIT_FAILURE;
		}
		if (read_level(fd) < 0) {
			close(fd);
			return EXIT_FAILURE;
		}
	} else {
		usage(argv[0]);
		close(fd);
		return EXIT_FAILURE;
	}

	close(fd);
	return EXIT_SUCCESS;
}
