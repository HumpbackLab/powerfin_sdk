/*
 * Standalone AT7456E/MAX7456 font programmer for Linux spidev.
 *
 * Stop every other driver using the OSD SPI device before running this tool
 * (for PX4: "atxxxx stop").
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEVICE          "/dev/spidev1.1"
#define DEFAULT_SPEED_HZ        2000000U
#define DEFAULT_TIMEOUT_MS      100U

#define FONT_CHAR_COUNT         256U
#define MCM_BYTES_PER_CHAR      64U
#define VISIBLE_BYTES_PER_CHAR  54U

#define REG_VM0_W               0x00U
#define REG_VM0_R               0x80U
#define REG_CMM_W               0x08U
#define REG_CMAH_W              0x09U
#define REG_CMAL_W              0x0aU
#define REG_CMDI_W              0x0bU
#define REG_STAT_R              0xa0U
#define REG_CMDO_R              0xc0U
#define REG_OSDM_R              0x8cU

#define CMM_SHADOW_TO_NVM       0xa0U
#define CMM_NVM_TO_SHADOW       0x50U
#define STAT_NVM_BUSY           0x20U
#define OSDM_RESET_VALUE        0x1bU
#define VM0_ENABLE_OSD          0x08U

struct programmer {
	int fd;
	uint32_t speed_hz;
	uint32_t timeout_ms;
	uint8_t saved_vm0;
	bool saved_vm0_valid;
	bool display_disabled;
};

static volatile sig_atomic_t stop_requested;

static void signal_handler(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static uint64_t monotonic_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}

	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static int spi_transfer(struct programmer *p, const uint8_t *tx, uint8_t *rx, size_t length)
{
	struct spi_ioc_transfer transfer;
	int ret;

	memset(&transfer, 0, sizeof(transfer));
	transfer.tx_buf = (uintptr_t)tx;
	transfer.rx_buf = (uintptr_t)rx;
	transfer.len = (uint32_t)length;
	transfer.speed_hz = p->speed_hz;
	transfer.bits_per_word = 8;

	ret = ioctl(p->fd, SPI_IOC_MESSAGE(1), &transfer);
	if (ret != (int)length) {
		if (ret >= 0) {
			errno = EIO;
		}
		return -1;
	}

	return 0;
}

static int write_reg(struct programmer *p, uint8_t reg, uint8_t value)
{
	uint8_t tx[2] = {reg, value};
	uint8_t rx[2] = {0, 0};

	return spi_transfer(p, tx, rx, sizeof(tx));
}

static int read_reg(struct programmer *p, uint8_t reg, uint8_t *value)
{
	uint8_t tx[2] = {reg, 0xffU};
	uint8_t rx[2] = {0, 0};

	if (spi_transfer(p, tx, rx, sizeof(tx)) != 0) {
		return -1;
	}

	*value = rx[1];
	return 0;
}

static int wait_nvm_ready(struct programmer *p)
{
	const uint64_t start = monotonic_ms();
	const struct timespec pause_time = {.tv_sec = 0, .tv_nsec = 100000L};
	uint8_t status;

	for (;;) {
		if (read_reg(p, REG_STAT_R, &status) != 0) {
			return -1;
		}
		if ((status & STAT_NVM_BUSY) == 0U) {
			return 0;
		}
		if (monotonic_ms() - start >= p->timeout_ms) {
			errno = ETIMEDOUT;
			return -1;
		}
		(void)nanosleep(&pause_time, NULL);
	}
}

static int disable_display(struct programmer *p)
{
	if (read_reg(p, REG_VM0_R, &p->saved_vm0) != 0) {
		return -1;
	}
	p->saved_vm0_valid = true;

	if (write_reg(p, REG_VM0_W, (uint8_t)(p->saved_vm0 & ~VM0_ENABLE_OSD)) != 0) {
		return -1;
	}
	p->display_disabled = true;
	return 0;
}

static int restore_display(struct programmer *p)
{
	if (!p->display_disabled || !p->saved_vm0_valid) {
		return 0;
	}
	if (write_reg(p, REG_VM0_W, p->saved_vm0) != 0) {
		return -1;
	}
	p->display_disabled = false;
	return 0;
}

static int read_character(struct programmer *p, uint8_t address,
			  uint8_t data[VISIBLE_BYTES_PER_CHAR])
{
	unsigned int i;

	if (write_reg(p, REG_CMAH_W, address) != 0 ||
	    write_reg(p, REG_CMM_W, CMM_NVM_TO_SHADOW) != 0 ||
	    wait_nvm_ready(p) != 0) {
		return -1;
	}

	for (i = 0; i < VISIBLE_BYTES_PER_CHAR; ++i) {
		if (write_reg(p, REG_CMAL_W, (uint8_t)i) != 0 ||
		    read_reg(p, REG_CMDO_R, &data[i]) != 0) {
			return -1;
		}
	}
	return 0;
}

static int write_character(struct programmer *p, uint8_t address,
			   const uint8_t data[VISIBLE_BYTES_PER_CHAR])
{
	unsigned int i;

	if (write_reg(p, REG_CMAH_W, address) != 0) {
		return -1;
	}

	for (i = 0; i < VISIBLE_BYTES_PER_CHAR; ++i) {
		if (stop_requested) {
			errno = EINTR;
			return -1;
		}
		if (write_reg(p, REG_CMAL_W, (uint8_t)i) != 0 ||
		    write_reg(p, REG_CMDI_W, data[i]) != 0) {
			return -1;
		}
	}

	if (write_reg(p, REG_CMM_W, CMM_SHADOW_TO_NVM) != 0) {
		return -1;
	}

	/* Once programming starts, always wait for NVM to become idle. */
	return wait_nvm_ready(p);
}

static void trim_line(char *line)
{
	size_t length = strlen(line);

	while (length > 0U && (line[length - 1U] == '\n' ||
			       line[length - 1U] == '\r' ||
			       line[length - 1U] == ' ' ||
			       line[length - 1U] == '\t')) {
		line[--length] = '\0';
	}
}

static int parse_bit_line(const char *line, uint8_t *value)
{
	unsigned int i;
	uint8_t result = 0;

	if (strlen(line) != 8U) {
		return -1;
	}
	for (i = 0; i < 8U; ++i) {
		if (line[i] != '0' && line[i] != '1') {
			return -1;
		}
		result = (uint8_t)((result << 1U) | (uint8_t)(line[i] - '0'));
	}
	*value = result;
	return 0;
}

static int load_mcm(const char *path,
		    uint8_t font[FONT_CHAR_COUNT][MCM_BYTES_PER_CHAR])
{
	FILE *file = fopen(path, "r");
	char line[128];
	unsigned int byte_index = 0;
	unsigned int line_number = 1;
	int result = -1;

	if (file == NULL) {
		return -1;
	}
	if (fgets(line, sizeof(line), file) == NULL) {
		fprintf(stderr, "%s: empty file\n", path);
		errno = EINVAL;
		goto out;
	}
	trim_line(line);
	if (strcmp(line, "MAX7456") != 0) {
		fprintf(stderr, "%s:1: expected MAX7456 header\n", path);
		errno = EINVAL;
		goto out;
	}

	while (fgets(line, sizeof(line), file) != NULL) {
		uint8_t value;
		++line_number;
		trim_line(line);
		if (line[0] == '\0') {
			continue;
		}
		if (byte_index >= FONT_CHAR_COUNT * MCM_BYTES_PER_CHAR) {
			fprintf(stderr, "%s:%u: too much font data\n", path, line_number);
			errno = EINVAL;
			goto out;
		}
		if (parse_bit_line(line, &value) != 0) {
			fprintf(stderr, "%s:%u: expected eight binary digits\n",
				path, line_number);
			errno = EINVAL;
			goto out;
		}
		font[byte_index / MCM_BYTES_PER_CHAR]
		    [byte_index % MCM_BYTES_PER_CHAR] = value;
		++byte_index;
	}

	if (ferror(file)) {
		goto out;
	}
	if (byte_index != FONT_CHAR_COUNT * MCM_BYTES_PER_CHAR) {
		fprintf(stderr, "%s: expected %u data bytes, got %u\n", path,
			FONT_CHAR_COUNT * MCM_BYTES_PER_CHAR, byte_index);
		errno = EINVAL;
		goto out;
	}
	result = 0;

out:
	{
		int saved_errno = errno;
		(void)fclose(file);
		errno = saved_errno;
	}
	return result;
}

static int parse_u32(const char *text, uint32_t minimum, uint32_t maximum,
		     uint32_t *result)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 0);
	if (errno != 0 || *text == '\0' || *end != '\0' ||
	    value < minimum || value > maximum) {
		errno = EINVAL;
		return -1;
	}
	*result = (uint32_t)value;
	return 0;
}

static int open_programmer(struct programmer *p, const char *device, bool force)
{
	uint8_t mode = SPI_MODE_0;
	uint8_t bits = 8;
	uint8_t osdm;

	p->fd = open(device, O_RDWR | O_CLOEXEC);
	if (p->fd < 0) {
		return -1;
	}
	if (flock(p->fd, LOCK_EX | LOCK_NB) != 0) {
		fprintf(stderr, "%s is locked by another font programmer\n", device);
		return -1;
	}
	if (ioctl(p->fd, SPI_IOC_WR_MODE, &mode) != 0 ||
	    ioctl(p->fd, SPI_IOC_WR_BITS_PER_WORD, &bits) != 0 ||
	    ioctl(p->fd, SPI_IOC_WR_MAX_SPEED_HZ, &p->speed_hz) != 0) {
		return -1;
	}
	if (read_reg(p, REG_OSDM_R, &osdm) != 0) {
		return -1;
	}
	if (osdm != OSDM_RESET_VALUE && !force) {
		fprintf(stderr,
			"probe failed: OSDM=0x%02x, expected 0x%02x\n"
			"Check the device and stop atxxxx; use --force only for a known clone.\n",
			osdm, OSDM_RESET_VALUE);
		errno = EPROTO;
		return -1;
	}
	if (osdm != OSDM_RESET_VALUE) {
		fprintf(stderr, "warning: ignoring unexpected OSDM value 0x%02x\n", osdm);
	}
	return 0;
}

static int verify_character(struct programmer *p, uint8_t address,
			    const uint8_t expected[VISIBLE_BYTES_PER_CHAR])
{
	uint8_t actual[VISIBLE_BYTES_PER_CHAR];
	unsigned int i;

	if (read_character(p, address, actual) != 0) {
		return -1;
	}
	for (i = 0; i < VISIBLE_BYTES_PER_CHAR; ++i) {
		if (actual[i] != expected[i]) {
			fprintf(stderr,
				"character 0x%02x byte %u: read 0x%02x, expected 0x%02x\n",
				address, i, actual[i], expected[i]);
			errno = EILSEQ;
			return -1;
		}
	}
	return 0;
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage:\n"
		"  %s [options] write FONT.mcm\n"
		"  %s [options] verify FONT.mcm\n"
		"  %s [options] write-char FONT.mcm ADDRESS\n"
		"  %s [options] verify-char FONT.mcm ADDRESS\n\n"
		"Options:\n"
		"  -d, --device PATH   spidev node (default: %s)\n"
		"  -s, --speed HZ      SPI clock (default: %u)\n"
		"  -t, --timeout MS    NVM busy timeout (default: %u)\n"
		"  -y, --yes           required for NVM writes\n"
		"  -n, --no-verify     skip readback after writing\n"
		"  -f, --force         ignore unexpected OSDM probe value\n"
		"  -h, --help          show this help\n\n"
		"Before writing: disarm, remove propellers, use stable power, and\n"
		"stop every OSD driver (PX4: atxxxx stop).\n",
		program, program, program, program, DEFAULT_DEVICE,
		DEFAULT_SPEED_HZ, DEFAULT_TIMEOUT_MS);
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{"device", required_argument, NULL, 'd'},
		{"speed", required_argument, NULL, 's'},
		{"timeout", required_argument, NULL, 't'},
		{"yes", no_argument, NULL, 'y'},
		{"no-verify", no_argument, NULL, 'n'},
		{"force", no_argument, NULL, 'f'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};
	struct programmer p = {
		.fd = -1,
		.speed_hz = DEFAULT_SPEED_HZ,
		.timeout_ms = DEFAULT_TIMEOUT_MS
	};
	uint8_t font[FONT_CHAR_COUNT][MCM_BYTES_PER_CHAR];
	const char *device = DEFAULT_DEVICE;
	const char *command;
	const char *font_path;
	uint32_t parsed;
	unsigned int first = 0;
	unsigned int last = FONT_CHAR_COUNT - 1U;
	unsigned int address;
	bool write_operation;
	bool verify_after_write = true;
	bool assume_yes = false;
	bool force = false;
	int option;
	int result = EXIT_FAILURE;

	while ((option = getopt_long(argc, argv, "d:s:t:ynfh", options, NULL)) != -1) {
		switch (option) {
		case 'd':
			device = optarg;
			break;
		case 's':
			if (parse_u32(optarg, 1, UINT32_MAX, &p.speed_hz) != 0) {
				fprintf(stderr, "invalid SPI speed: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 't':
			if (parse_u32(optarg, 1, UINT32_MAX, &p.timeout_ms) != 0) {
				fprintf(stderr, "invalid timeout: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'y':
			assume_yes = true;
			break;
		case 'n':
			verify_after_write = false;
			break;
		case 'f':
			force = true;
			break;
		case 'h':
			usage(stdout, argv[0]);
			return EXIT_SUCCESS;
		default:
			usage(stderr, argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (argc - optind < 2) {
		usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}
	command = argv[optind++];
	font_path = argv[optind++];
	write_operation = strcmp(command, "write") == 0 ||
			  strcmp(command, "write-char") == 0;

	if (!write_operation && strcmp(command, "verify") != 0 &&
	    strcmp(command, "verify-char") != 0) {
		fprintf(stderr, "unknown command: %s\n", command);
		return EXIT_FAILURE;
	}

	if (strcmp(command, "write-char") == 0 ||
	    strcmp(command, "verify-char") == 0) {
		if (argc - optind != 1 ||
		    parse_u32(argv[optind], 0, FONT_CHAR_COUNT - 1U, &parsed) != 0) {
			fprintf(stderr, "ADDRESS from 0 to 255 is required\n");
			return EXIT_FAILURE;
		}
		first = parsed;
		last = parsed;
	} else if (argc != optind) {
		fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
		return EXIT_FAILURE;
	}

	if (write_operation && !assume_yes) {
		fprintf(stderr,
			"refusing to modify OSD NVM without --yes\n"
			"Stop atxxxx, disarm, remove propellers and provide stable power first.\n");
		return EXIT_FAILURE;
	}
	if (load_mcm(font_path, font) != 0) {
		fprintf(stderr, "cannot load %s: %s\n", font_path, strerror(errno));
		return EXIT_FAILURE;
	}
	if (signal(SIGINT, signal_handler) == SIG_ERR ||
	    signal(SIGTERM, signal_handler) == SIG_ERR) {
		perror("signal");
		return EXIT_FAILURE;
	}
	if (open_programmer(&p, device, force) != 0) {
		fprintf(stderr, "cannot use %s: %s\n", device, strerror(errno));
		goto out;
	}
	if (disable_display(&p) != 0) {
		fprintf(stderr, "cannot disable OSD: %s\n", strerror(errno));
		goto out;
	}

	printf("%s %s characters 0x%02x..0x%02x at %u Hz\n",
	       write_operation ? "Writing" : "Verifying", device, first, last,
	       p.speed_hz);

	for (address = first; address <= last; ++address) {
		if (stop_requested) {
			errno = EINTR;
			fprintf(stderr, "interrupted before character 0x%02x\n", address);
			goto out;
		}
		if (write_operation &&
		    write_character(&p, (uint8_t)address, font[address]) != 0) {
			fprintf(stderr, "cannot write character 0x%02x: %s\n",
				address, strerror(errno));
			goto out;
		}
		if ((!write_operation || verify_after_write) &&
		    verify_character(&p, (uint8_t)address, font[address]) != 0) {
			fprintf(stderr, "cannot verify character 0x%02x: %s\n",
				address, strerror(errno));
			goto out;
		}
		if (first != last && ((address + 1U) % 16U == 0U || address == last)) {
			printf("  %u/%u characters complete\n", address - first + 1U,
			       last - first + 1U);
			(void)fflush(stdout);
		}
	}

	printf("Success: %u character%s %s%s.\n", last - first + 1U,
	       first == last ? "" : "s", write_operation ? "written" : "verified",
	       write_operation && verify_after_write ? " and verified" : "");
	result = EXIT_SUCCESS;

out:
	if (p.fd >= 0) {
		if (restore_display(&p) != 0) {
			fprintf(stderr, "warning: cannot restore VM0: %s\n", strerror(errno));
			result = EXIT_FAILURE;
		}
		(void)close(p.fd);
	}
	return result;
}
