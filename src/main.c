/*
** Copyright (C) 2022-2023 Arseny Vakhrushev <arseny.vakhrushev@me.com>
**
** This firmware is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** This firmware is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this firmware. If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <err.h>
#include "common.h"

#define VERSION "1.0"

#define CMD_PROBE  0
#define CMD_INFO   1
#define CMD_READ   2
#define CMD_WRITE  3
#define CMD_UPDATE 4
#define CMD_SETWRP 5

#define RES_OK    0
#define RES_ERROR 1

int delay;

static const char *device = "/dev/ttyUSB0";
static const char *filename;
static int force, boot, setwrp, version;

static int parseargs(int argc, char *argv[]) {
	int opt;
	while ((opt = getopt(argc, argv, "d:fBP:Vvh?")) != -1) {
		switch (opt) {
			case 'd':
				device = optarg;
				break;
			case 'f':
				if (force) delay = 1;
				force = 1;
				break;
			case 'B':
				boot = 1;
				break;
			case 'P':
				if (optarg[1]) goto error;
				switch (optarg[0]) {
					case '0':
						setwrp = 0x33;
						break;
					case '1':
						setwrp = 0x44;
						break;
					case '2':
						setwrp = 0x55;
						break;
					default:
						goto error;
				}
				break;
			case 'V':
			case 'v':
				version = 1;
				break;
			case 'h':
			case '?':
			default:
				return 0;
		}
	}
	argc -= optind;
	argv += optind;
	if (!argc) return !force;
	if (argc > 1) return 0;
	filename = argv[0];
	return 1;
error:
	warnx("invalid argument '%s'", argv[optind - 1]);
	return 0;
}

static void checkres(int res, int val, const char *msg) {
	if (res == val || force) return;
	errx(1, "%s (result %d, expected %d)", msg, res, val);
}

static void recvack(int fd, const char *msg) {
	checkres(recvval(fd), RES_OK, msg);
}

static size_t maxlen(size_t pos, size_t size) {
	size_t len = size - pos;
	if (len > 1024) len = 1024;
	return len;
}

int main(int argc, char *argv[]) {
	if (!parseargs(argc, argv)) {
		fprintf(stderr,
			"Usage: %s [options] [<image>]\n"
			"  <image>      Binary image filename for update.\n"
			"Options:\n"
			"  -d <device>  Serial device name.\n"
			"  -f           Forced mode (specify twice to add delay after each transmitted byte).\n"
			"  -B           Update bootloader.\n"
			"  -P <level>   Set write protection (0-off, 1-bootloader, 2-full).\n"
			"  -V           Print version.\n"
			"ESC info is printed when no operation specified.\n",
			argv[0]);
		return 1;
	}
	if (version) {
		printf("ESCape32-Update " VERSION "\n");
		return 0;
	}
	uint8_t data[61440] = {0xff};
	size_t size = 0;
	if (filename) {
		FILE *f = fopen(filename, "r");
		if (!f || (!(size = fread(data, 1, sizeof data, f)) && ferror(f))) err(1, "%s", filename);
		fclose(f);
		if (!(size = (size + 3) & ~3)) errx(1, "%s: Empty data", filename);
	}
	printf("Probing bootloader via '%s'...\n", device);
	int fd = openserial(device);
	for (int i = 0; !force || i < 20; ++i) {
		if (force) printf("%4d\r", 20 - i);
		else printf("%3c\r", "-\\|/"[i & 3]);
		fflush(stdout);
		sendval(fd, CMD_PROBE);
		if (recvval(fd) == RES_OK) break;
	}
	if (filename) {
		if (boot) {
			if (size > 4096) errx(1, "%s: Image too big", filename);
			if (!(size & 1023) && size != 4096) size += 4; // Ensure last block marker
			printf("Updating bootloader...\n");
			sendval(fd, CMD_UPDATE);
			for (size_t pos = 0; pos < size; pos += 1024) {
				printf("%4zu%%\r", pos * 100 / size);
				fflush(stdout);
				senddata(fd, data + pos, maxlen(pos, size));
				recvack(fd, "Error writing data");
			}
			recvack(fd, "Update failed"); // Wait for ACK after reboot
		} else {
			printf("Updating firmware...\n");
			for (size_t pos = 0; pos < size; pos += 1024) {
				printf("%4zu%%\r", pos * 100 / size);
				fflush(stdout);
				sendval(fd, CMD_WRITE);
				sendval(fd, pos / 1024); // Block number
				senddata(fd, data + pos, maxlen(pos, size));
				recvack(fd, "Error writing data");
			}
		}
		printf("Done!\n");
	}
	if (setwrp) {
		printf("Setting write protection...\n");
		sendval(fd, CMD_SETWRP);
		sendval(fd, setwrp);
		recvack(fd, "Operation failed");
		printf("Done!\n");
	} else if (!filename && !force) {
		printf("Fetching ESCape32 info...\n");
		sendval(fd, CMD_INFO);
		checkres(recvdata(fd, data), 32, "Error reading data");
		printf("Bootloader revision %d\n", data[0]);
		sendval(fd, CMD_READ);
		sendval(fd, 0); // First block
		sendval(fd, 4); // (4+1)*4=20 bytes
		checkres(recvdata(fd, data), 20, "Error reading data");
		if (*(uint16_t *)data == 0x32ea) printf("Firmware revision %d [%s]\n", data[2], data + 4);
		else printf("Firmware not installed!\n");
	}
	close(fd);
	return 0;
}
