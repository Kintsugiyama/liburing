/* SPDX-License-Identifier: MIT */
/*
 * Description: test SQPOLL with IORING_SETUP_ATTACH_WQ
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include "liburing.h"

#define FILE_SIZE	(128 * 1024 * 1024)
#define BS		4096
#define BUFFERS		64

#define NR_RINGS	4

static struct iovec *vecs;

static int create_buffers(void)
{
	int i;

	vecs = malloc(BUFFERS * sizeof(struct iovec));
	for (i = 0; i < BUFFERS; i++) {
		if (posix_memalign(&vecs[i].iov_base, BS, BS))
			return 1;
		vecs[i].iov_len = BS;
	}

	return 0;
}

static int create_file(const char *file)
{
	ssize_t ret;
	char *buf;
	int fd;

	buf = malloc(FILE_SIZE);
	memset(buf, 0xaa, FILE_SIZE);

	fd = open(file, O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		perror("open file");
		return 1;
	}
	ret = write(fd, buf, FILE_SIZE);
	close(fd);
	return ret != FILE_SIZE;
}

static int wait_io(struct io_uring *ring, int nr_ios)
{
	struct io_uring_cqe *cqe;

	while (nr_ios) {
		io_uring_wait_cqe(ring, &cqe);
		io_uring_cqe_seen(ring, cqe);
		nr_ios--;
	}

	return 0;
}

static int queue_io(struct io_uring *ring, const char *fname, int nr_ios)
{
	unsigned long off;
	int fd, ret, i;

	fd = open(fname, O_RDONLY | O_DIRECT);
	if (fd < 0) {
		perror("open");
		return -1;
	}

	i = 0;
	off = 0;
	while (nr_ios) {
		struct io_uring_sqe *sqe;

		sqe = io_uring_get_sqe(ring);
		if (!sqe)
			break;
		io_uring_prep_read(sqe, fd, vecs[i].iov_base, vecs[i].iov_len, off);
		nr_ios--;
		i++;
		off += BS;
	}

	ret = io_uring_submit(ring);
	close(fd);
	return ret;
}

int main(int argc, char *argv[])
{
	struct io_uring rings[NR_RINGS];
	int rets[NR_RINGS];
	unsigned long ios;
	int i, ret;
	char *fname;

	if (argc > 1) {
		fname = argv[1];
	} else {
		fname = ".basic-rw";
		if (create_file(fname)) {
			fprintf(stderr, "file creation failed\n");
			goto err;
		}
	}

	if (create_buffers()) {
		fprintf(stderr, "file creation failed\n");
		goto err;
	}

	for (i = 0; i < NR_RINGS; i++) {
		struct io_uring_params p = { };

		p.flags = IORING_SETUP_SQPOLL;
		if (i) {
			p.wq_fd = rings[0].ring_fd;
			p.flags |= IORING_SETUP_ATTACH_WQ;
		}
		ret = io_uring_queue_init_params(BUFFERS, &rings[i], &p);
		if (ret) {
			fprintf(stderr, "queue_init: %d/%d\n", ret, i);
			goto err;
		}
	}

	ios = 0;
	while (ios < (FILE_SIZE / BS)) {
		for (i = 0; i < NR_RINGS; i++) {
			ret = queue_io(&rings[i], fname, BUFFERS);
			if (ret < 0)
				goto err;
			rets[i] = ret;
		}
		for (i = 0; i < NR_RINGS; i++)
			wait_io(&rings[i], rets[i]);
		ios += BUFFERS;
	}

	return 0;
err:
	if (fname != argv[1])
		unlink(fname);
	return 1;
}