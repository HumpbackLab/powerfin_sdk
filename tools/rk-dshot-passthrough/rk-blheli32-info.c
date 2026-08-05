// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minimal BLHeli/BLHeli_32 bootloader information reader over the Rockchip
 * Flexbus DShot passthrough device.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define RK_DSHOT_IOCTL_BASE 'D'
#define RK_DSHOT_IOC_PASSTHROUGH_XFER \
	_IOWR(RK_DSHOT_IOCTL_BASE, 0x04, struct rk_dshot_passthrough_xfer)

#define RK_DSHOT_PASSTHROUGH_F_TX_INVERT (1U << 0)
#define RK_DSHOT_PASSTHROUGH_F_RX_INVERT (1U << 1)

#define DEFAULT_DSHOT_DEV "/dev/rk-flexbus-dshot"
#define DEFAULT_ESC_BAUD 19200
#define DEFAULT_OVERSAMPLE 16
#define DEFAULT_RX_WINDOW_MS 80

#define CMD_READ_FLASH_SIL 0x03
#define CMD_READ_FLASH_ATM 0x07
#define CMD_SET_ADDRESS 0xff

#define BR_SUCCESS 0x30
#define BR_ERROR_VERIFY 0xc0
#define BR_ERROR_COMMAND 0xc1
#define BR_ERROR_CRC 0xc2
#define BR_NONE 0xff

#define MAX_RAW_RX 512
#define MAX_READ_LEN 256

struct rk_dshot_passthrough_xfer {
	uint64_t tx_buf;
	uint64_t rx_buf;
	uint32_t tx_samples;
	uint32_t rx_samples;
	uint32_t sample_rate;
	uint32_t channel;
	uint32_t timeout_ms;
	uint32_t flags;
};

struct config {
	const char *dshot_dev;
	unsigned int esc_baud;
	unsigned int bit_us;
	unsigned int oversample;
	unsigned int rx_window_ms;
	unsigned int rx_skip_ms;
	unsigned int channel;
	unsigned int timeout_ms;
	unsigned int flash_addr;
	unsigned int read_len;
	unsigned int boot_high_ms;
	unsigned int boot_low_ms;
	unsigned int boot_zeros;
	uint32_t flags;
	int verbose;
};

struct blheli_device {
	uint8_t boot_tag;
	uint8_t signature_hi;
	uint8_t signature_lo;
	uint8_t boot_version;
	uint8_t boot_pages;
};

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [-d /dev/rk-flexbus-dshot] [-c 0..3]\n"
		"          [--esc-baud N] [--bit-us N] [--oversample N] [--rx-window-ms N]\n"
		"          [--rx-skip-ms N]\n"
		"          [--timeout-ms N] [--flash-addr N] [--read-len 0..256]\n"
		"          [--boot-high-ms N] [--boot-low-ms N] [--boot-zeros N]\n"
		"          [--tx-invert] [--rx-invert] [-v]\n",
		argv0);
}

static uint16_t crc16_update(uint16_t crc, uint8_t data)
{
	crc ^= data;
	for (unsigned int i = 0; i < 8; i++) {
		if (crc & 1)
			crc = (crc >> 1) ^ 0xa001;
		else
			crc >>= 1;
	}
	return crc;
}

static uint16_t crc16_buf(const uint8_t *buf, size_t len)
{
	uint16_t crc = 0;

	for (size_t i = 0; i < len; i++)
		crc = crc16_update(crc, buf[i]);

	return crc;
}

static unsigned int cfg_sample_rate(const struct config *cfg)
{
	if (cfg->bit_us)
		return (1000000U * cfg->oversample + cfg->bit_us / 2) / cfg->bit_us;

	return cfg->esc_baud * cfg->oversample;
}

static unsigned int cfg_rx_oversample(const struct config *cfg, unsigned int actual_sample_rate)
{
	if (cfg->bit_us)
		return (actual_sample_rate * cfg->bit_us + 500000U) / 1000000U;

	return (actual_sample_rate + cfg->esc_baud / 2) / cfg->esc_baud;
}

static const char *ack_name(uint8_t ack)
{
	switch (ack) {
	case BR_SUCCESS:
		return "SUCCESS";
	case BR_ERROR_VERIFY:
		return "ERROR_VERIFY";
	case BR_ERROR_COMMAND:
		return "ERROR_COMMAND";
	case BR_ERROR_CRC:
		return "ERROR_CRC";
	case BR_NONE:
		return "NONE";
	default:
		return "UNKNOWN";
	}
}

static size_t encode_blheli_suart(uint8_t *samples, const uint8_t *bytes, size_t len,
				  unsigned int oversample)
{
	size_t pos = 0;

	for (size_t i = 0; i < len; i++) {
		uint16_t frame = ((uint16_t)bytes[i] << 2) | 1U | (1U << 10);

			for (unsigned int bit = 0; bit < 11; bit++) {
				uint8_t level = (frame >> bit) & 1U;

				for (unsigned int sample = 0; sample < oversample; sample++)
					samples[pos++] = level;
			}
	}

	samples[pos++] = 1;

	return pos;
}

static size_t decode_blheli_suart(uint8_t *bytes, size_t max_len, const uint8_t *samples,
				  size_t sample_count, unsigned int oversample,
				  size_t base_offset, int trace)
{
	size_t out = 0;
	size_t i = 0;

	while (i + oversample * 10 < sample_count && out < max_len) {
		while (i < sample_count && samples[i])
			i++;
		if (i + oversample * 10 >= sample_count)
			break;

		size_t start = i;
		size_t sample_at = start + (oversample * 3 + 2) / 4;
		size_t stop_sample_at;
		uint16_t bitmask = 0;
		uint8_t value = 0;

		for (unsigned int bit = 0; bit < 10; bit++) {
			if (sample_at >= sample_count)
				return out;
			if (samples[sample_at])
				bitmask |= 1U << bit;
			if (bit == 9)
				stop_sample_at = sample_at;
			sample_at += oversample;
		}

		if (!(bitmask & 1U) && (bitmask & (1U << 9))) {
			value = (bitmask >> 1) & 0xff;
			bytes[out++] = value;
			if (trace)
				fprintf(stderr, "rx_byte start=%zu bitmask=0x%03x byte=0x%02x\n",
					base_offset + start, bitmask, value);
			i = stop_sample_at + 1;
		} else {
			i = start + 1;
		}
	}

	return out;
}

static void dump_sample_stats(const uint8_t *samples, size_t sample_count)
{
	size_t ones = 0;
	size_t edges = 0;
	unsigned int printed = 0;

	for (size_t i = 0; i < sample_count; i++) {
		ones += samples[i] ? 1 : 0;
		if (i && samples[i] != samples[i - 1])
			edges++;
	}

	fprintf(stderr,
		"rx_samples=%zu high=%.2f%% edges=%zu first=%u last=%u\n",
		sample_count, sample_count ? 100.0 * ones / sample_count : 0.0,
		edges, sample_count ? samples[0] : 0,
		sample_count ? samples[sample_count - 1] : 0);

	if (!edges)
		return;

	fprintf(stderr, "rx_edges=");
	for (size_t i = 1; i < sample_count && printed < 48; i++) {
		if (samples[i] != samples[i - 1]) {
			fprintf(stderr, "%s%zu:%u->%u", printed ? "," : "",
				i, samples[i - 1], samples[i]);
			printed++;
		}
	}
	fprintf(stderr, "\n");
}

static void dump_sample_stats_from(const char *label, const uint8_t *samples,
				   size_t sample_count, size_t offset)
{
	size_t ones = 0;
	size_t edges = 0;
	unsigned int printed = 0;

	if (offset > sample_count)
		offset = sample_count;

	for (size_t i = offset; i < sample_count; i++) {
		ones += samples[i] ? 1 : 0;
		if (i > offset && samples[i] != samples[i - 1])
			edges++;
	}

	fprintf(stderr,
		"%s offset=%zu samples=%zu high=%.2f%% edges=%zu first=%u last=%u\n",
		label, offset, sample_count - offset,
		sample_count > offset ? 100.0 * ones / (sample_count - offset) : 0.0,
		edges, sample_count > offset ? samples[offset] : 0,
		sample_count > offset ? samples[sample_count - 1] : 0);

	if (!edges)
		return;

	fprintf(stderr, "%s_edges=", label);
	for (size_t i = offset + 1; i < sample_count && printed < 48; i++) {
		if (samples[i] != samples[i - 1]) {
			fprintf(stderr, "%s%zu:%u->%u", printed ? "," : "",
				i, samples[i - 1], samples[i]);
			printed++;
		}
	}
	fprintf(stderr, "\n");
}

static int passthrough_exchange(int fd, const struct config *cfg,
				const uint8_t *tx, size_t tx_len,
				uint8_t *rx, size_t rx_len, size_t *decoded_len)
{
	unsigned int sample_rate = cfg_sample_rate(cfg);
	size_t tx_samples_len = tx_len * 11 * cfg->oversample + 1;
	size_t rx_window_samples = (sample_rate * cfg->rx_window_ms + 999) / 1000;
	size_t rx_lead_samples = tx_len ? tx_samples_len : 0;
	size_t rx_samples_len = rx_lead_samples + rx_window_samples;
	unsigned int rx_oversample;
	uint8_t *tx_samples = NULL;
	uint8_t *rx_samples = NULL;
	struct rk_dshot_passthrough_xfer xfer;
	int ret = -1;

	tx_samples = calloc(tx_samples_len ? tx_samples_len : 1, 1);
	rx_samples = calloc(rx_samples_len ? rx_samples_len : 1, 1);
	if (!tx_samples || !rx_samples) {
		errno = ENOMEM;
		goto out;
	}

	encode_blheli_suart(tx_samples, tx, tx_len, cfg->oversample);

	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf = (uintptr_t)tx_samples;
	xfer.rx_buf = (uintptr_t)rx_samples;
	xfer.tx_samples = tx_samples_len;
	xfer.rx_samples = rx_samples_len;
	xfer.sample_rate = sample_rate;
	xfer.channel = cfg->channel;
	xfer.timeout_ms = cfg->timeout_ms;
	xfer.flags = cfg->flags;

	if (ioctl(fd, RK_DSHOT_IOC_PASSTHROUGH_XFER, &xfer) < 0)
		goto out;

	rx_oversample = cfg_rx_oversample(cfg, xfer.sample_rate ?: sample_rate);
	if (rx_oversample < 8)
		rx_oversample = cfg->oversample;

	if (cfg->verbose && xfer.sample_rate != sample_rate)
		fprintf(stderr, "actual_sample_rate=%u rx_oversample=%u\n",
			xfer.sample_rate, rx_oversample);

	if (cfg->verbose)
		dump_sample_stats(rx_samples, rx_samples_len);

	size_t rx_skip_samples = rx_lead_samples + (sample_rate * cfg->rx_skip_ms + 999) / 1000;
	if (rx_skip_samples > rx_samples_len)
		rx_skip_samples = rx_samples_len;

	if (cfg->verbose && rx_skip_samples)
		dump_sample_stats_from("rx_decode", rx_samples, rx_samples_len, rx_skip_samples);

	*decoded_len = decode_blheli_suart(rx, rx_len, rx_samples + rx_skip_samples,
					   rx_samples_len - rx_skip_samples, rx_oversample,
					   rx_skip_samples, cfg->verbose);
	ret = 0;

out:
	free(rx_samples);
	free(tx_samples);
	return ret;
}

static int passthrough_tx_samples(int fd, const struct config *cfg,
				  const uint8_t *samples, size_t sample_count,
				  unsigned int sample_rate)
{
	struct rk_dshot_passthrough_xfer xfer;

	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf = (uintptr_t)samples;
	xfer.tx_samples = sample_count;
	xfer.sample_rate = sample_rate;
	xfer.channel = cfg->channel;
	xfer.timeout_ms = cfg->timeout_ms;

	return ioctl(fd, RK_DSHOT_IOC_PASSTHROUGH_XFER, &xfer);
}

static int drive_line_level(int fd, const struct config *cfg, unsigned int ms, uint8_t level)
{
	static const unsigned int sample_rate = 100000;
	uint8_t samples[65536];

	memset(samples, level ? 1 : 0, sizeof(samples));

	while (ms) {
		unsigned int chunk_ms = ms > 500 ? 500 : ms;
		size_t sample_count = (sample_rate * chunk_ms + 999) / 1000;

		if (sample_count > sizeof(samples))
			sample_count = sizeof(samples);

		if (passthrough_tx_samples(fd, cfg, samples, sample_count, sample_rate) < 0)
			return -1;

		ms -= chunk_ms;
	}

	return 0;
}

static void dump_bytes(const char *label, const uint8_t *buf, size_t len)
{
	fprintf(stderr, "%s:", label);
	for (size_t i = 0; i < len; i++)
		fprintf(stderr, " %02x", buf[i]);
	fprintf(stderr, "\n");
}

static int raw_xfer(int fd, const struct config *cfg, const uint8_t *tx, size_t tx_len,
		    uint8_t *rx, size_t rx_len, size_t min_rx, size_t *got)
{
	if (cfg->verbose)
		dump_bytes("tx", tx, tx_len);

	if (passthrough_exchange(fd, cfg, tx, tx_len, rx, rx_len, got) < 0)
		return -1;

	if (cfg->verbose)
		dump_bytes("rx", rx, *got);

	if (*got < min_rx) {
		errno = ETIMEDOUT;
		return -1;
	}

	return 0;
}

static int send_crc_cmd_expect_ack(int fd, const struct config *cfg,
				   const uint8_t *cmd, size_t cmd_len)
{
	uint8_t tx[16];
	uint8_t rx[MAX_RAW_RX];
	size_t got = 0;
	uint16_t crc;

	if (cmd_len + 2 > sizeof(tx)) {
		errno = EINVAL;
		return -1;
	}

	memcpy(tx, cmd, cmd_len);
	crc = crc16_buf(cmd, cmd_len);
	tx[cmd_len] = crc & 0xff;
	tx[cmd_len + 1] = crc >> 8;

	if (raw_xfer(fd, cfg, tx, cmd_len + 2, rx, sizeof(rx), 1, &got) < 0)
		return -1;

	if (rx[got - 1] != BR_SUCCESS) {
		fprintf(stderr, "bootloader ACK failed: 0x%02x (%s)\n",
			rx[got - 1], ack_name(rx[got - 1]));
		errno = EPROTO;
		return -1;
	}

	return 0;
}

static int connect_bootloader(int fd, const struct config *cfg, struct blheli_device *dev)
{
	static const uint8_t boot_tail[] = {
		0x0d, 'B', 'L', 'H', 'e', 'l', 'i', 0xf4, 0x7d
	};
	uint8_t boot_init[32];
	uint8_t rx[MAX_RAW_RX];
	size_t got = 0;
	size_t boot_len;

	if (cfg->boot_zeros > sizeof(boot_init) - sizeof(boot_tail)) {
		errno = EINVAL;
		return -1;
	}

	memset(boot_init, 0, cfg->boot_zeros);
	memcpy(boot_init + cfg->boot_zeros, boot_tail, sizeof(boot_tail));
	boot_len = cfg->boot_zeros + sizeof(boot_tail);

	for (unsigned int attempt = 1; attempt <= 3; attempt++) {
		if (cfg->verbose)
			fprintf(stderr, "connect attempt %u\n", attempt);

		if (raw_xfer(fd, cfg, boot_init, boot_len, rx, sizeof(rx), 9, &got) == 0)
			break;

		if (attempt == 3)
			return -1;

		usleep(10000);
	}

	if (got >= 9 && rx[1] == '7' && rx[2] == '1' && rx[8] == BR_SUCCESS) {
		if (rx[0] != '4')
			fprintf(stderr, "warning: bootloader first byte corrupted: 0x%02x\n", rx[0]);

		dev->boot_tag = rx[3];
		dev->signature_hi = rx[4];
		dev->signature_lo = rx[5];
		dev->boot_version = rx[6];
		dev->boot_pages = rx[7];
		return 0;
	}

	if (got >= 8 && rx[0] == '7' && rx[1] == '1' && rx[7] == BR_SUCCESS) {
		fprintf(stderr, "warning: bootloader first byte was missed\n");

		dev->boot_tag = rx[2];
		dev->signature_hi = rx[3];
		dev->signature_lo = rx[4];
		dev->boot_version = rx[5];
		dev->boot_pages = rx[6];
		return 0;
	}

	if (rx[0] != '4' || rx[1] != '7' || rx[2] != '1' || rx[8] != BR_SUCCESS) {
		fprintf(stderr, "unexpected bootloader response\n");
		errno = EPROTO;
		return -1;
	}

	dev->boot_tag = rx[3];
	dev->signature_hi = rx[4];
	dev->signature_lo = rx[5];
	dev->boot_version = rx[6];
	dev->boot_pages = rx[7];
	return 0;
}

static int set_address(int fd, const struct config *cfg, uint16_t addr)
{
	uint8_t cmd[] = { CMD_SET_ADDRESS, 0, addr >> 8, addr & 0xff };

	if (addr == 0xffff)
		return 0;

	return send_crc_cmd_expect_ack(fd, cfg, cmd, sizeof(cmd));
}

static int read_flash(int fd, const struct config *cfg, uint8_t read_cmd,
		      uint16_t addr, uint8_t *buf, unsigned int len)
{
	uint8_t tx[4];
	uint8_t rx[MAX_RAW_RX];
	size_t got = 0;
	size_t data_len = len ? len : 256;
	uint16_t expected_crc;
	uint16_t rx_crc;

	if (data_len > MAX_READ_LEN) {
		errno = EINVAL;
		return -1;
	}

	if (set_address(fd, cfg, addr) < 0)
		return -1;

	tx[0] = read_cmd;
	tx[1] = len & 0xff;
	expected_crc = crc16_buf(tx, 2);
	tx[2] = expected_crc & 0xff;
	tx[3] = expected_crc >> 8;

	if (raw_xfer(fd, cfg, tx, sizeof(tx), rx, sizeof(rx), data_len + 3, &got) < 0)
		return -1;

	if (rx[data_len + 2] != BR_SUCCESS) {
		fprintf(stderr, "read ACK failed: 0x%02x (%s)\n",
			rx[data_len + 2], ack_name(rx[data_len + 2]));
		errno = EPROTO;
		return -1;
	}

	expected_crc = crc16_buf(rx, data_len);
	rx_crc = rx[data_len] | ((uint16_t)rx[data_len + 1] << 8);
	if (rx_crc != expected_crc) {
		fprintf(stderr, "read CRC mismatch: got 0x%04x expected 0x%04x\n",
			rx_crc, expected_crc);
		errno = EPROTO;
		return -1;
	}

	memcpy(buf, rx, data_len);
	return 0;
}

static void hexdump(const uint8_t *buf, size_t len, unsigned int base)
{
	for (size_t off = 0; off < len; off += 16) {
		size_t line_len = len - off;

		if (line_len > 16)
			line_len = 16;

		printf("%04x  ", base + (unsigned int)off);
		for (size_t i = 0; i < 16; i++) {
			if (i < line_len)
				printf("%02x ", buf[off + i]);
			else
				printf("   ");
		}
		printf(" |");
		for (size_t i = 0; i < line_len; i++) {
			uint8_t c = buf[off + i];

			putchar((c >= 0x20 && c <= 0x7e) ? c : '.');
		}
		printf("|\n");
	}
}

static const char *device_family(const struct blheli_device *dev)
{
	if (dev->signature_lo == 0x06 && dev->signature_hi > 0x00 && dev->signature_hi < 0x90)
		return "BLHeli_32/ARM";
	if (dev->signature_hi > 0xe8 && dev->signature_hi < 0xf9)
		return "SiLabs";
	if ((dev->signature_hi == 0x93 && dev->signature_lo == 0x07) ||
	    (dev->signature_hi == 0x93 && dev->signature_lo == 0x0a) ||
	    (dev->signature_hi == 0x93 && dev->signature_lo == 0x0f) ||
	    (dev->signature_hi == 0x94 && dev->signature_lo == 0x0b))
		return "Atmel";
	return "unknown";
}

int main(int argc, char **argv)
{
	static const struct option long_opts[] = {
		{ "dshot", required_argument, NULL, 'd' },
		{ "channel", required_argument, NULL, 'c' },
		{ "esc-baud", required_argument, NULL, 'E' },
		{ "bit-us", required_argument, NULL, 'u' },
		{ "oversample", required_argument, NULL, 'o' },
		{ "rx-window-ms", required_argument, NULL, 'r' },
		{ "rx-skip-ms", required_argument, NULL, 'R' },
		{ "timeout-ms", required_argument, NULL, 't' },
		{ "flash-addr", required_argument, NULL, 'a' },
			{ "read-len", required_argument, NULL, 'l' },
		{ "boot-high-ms", required_argument, NULL, 'B' },
		{ "boot-low-ms", required_argument, NULL, 'L' },
		{ "boot-zeros", required_argument, NULL, 'Z' },
			{ "tx-invert", no_argument, NULL, 1000 },
			{ "rx-invert", no_argument, NULL, 1001 },
			{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{}
	};
	struct config cfg = {
		.dshot_dev = DEFAULT_DSHOT_DEV,
		.esc_baud = DEFAULT_ESC_BAUD,
		.oversample = DEFAULT_OVERSAMPLE,
		.rx_window_ms = DEFAULT_RX_WINDOW_MS,
		.timeout_ms = 100,
		.read_len = 256,
		.boot_zeros = 8,
	};
	struct blheli_device dev;
	uint8_t flash[MAX_READ_LEN];
	uint8_t read_cmd = CMD_READ_FLASH_SIL;
	int fd;

	for (;;) {
		int opt = getopt_long(argc, argv, "d:c:E:u:o:r:R:t:a:l:B:L:Z:vh", long_opts, NULL);

		if (opt < 0)
			break;

		switch (opt) {
		case 'd':
			cfg.dshot_dev = optarg;
			break;
		case 'c':
			cfg.channel = strtoul(optarg, NULL, 0);
			break;
		case 'E':
			cfg.esc_baud = strtoul(optarg, NULL, 0);
			break;
		case 'u':
			cfg.bit_us = strtoul(optarg, NULL, 0);
			break;
		case 'o':
			cfg.oversample = strtoul(optarg, NULL, 0);
			break;
		case 'r':
			cfg.rx_window_ms = strtoul(optarg, NULL, 0);
			break;
		case 'R':
			cfg.rx_skip_ms = strtoul(optarg, NULL, 0);
			break;
		case 't':
			cfg.timeout_ms = strtoul(optarg, NULL, 0);
			break;
		case 'a':
			cfg.flash_addr = strtoul(optarg, NULL, 0);
			break;
		case 'l':
			cfg.read_len = strtoul(optarg, NULL, 0);
			break;
		case 'B':
			cfg.boot_high_ms = strtoul(optarg, NULL, 0);
			break;
		case 'L':
			cfg.boot_low_ms = strtoul(optarg, NULL, 0);
			break;
		case 'Z':
			cfg.boot_zeros = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			cfg.verbose = 1;
			break;
		case 1000:
			cfg.flags |= RK_DSHOT_PASSTHROUGH_F_TX_INVERT;
			break;
		case 1001:
			cfg.flags |= RK_DSHOT_PASSTHROUGH_F_RX_INVERT;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (cfg.channel > 3 || cfg.oversample < 8 || !cfg.esc_baud ||
	    cfg.read_len > MAX_READ_LEN || cfg.flash_addr > 0xffff ||
	    cfg.boot_zeros > 23) {
		usage(argv[0]);
		return 1;
	}

	fd = open(cfg.dshot_dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(cfg.dshot_dev);
		return 1;
	}

	if (cfg.boot_low_ms) {
		fprintf(stderr, "drive channel %u low for %u ms\n",
			cfg.channel, cfg.boot_low_ms);
		if (drive_line_level(fd, &cfg, cfg.boot_low_ms, 0) < 0) {
			perror("drive line low");
			close(fd);
			return 1;
		}
		usleep(10000);
	}

	if (cfg.boot_high_ms) {
		fprintf(stderr, "drive channel %u high for %u ms\n",
			cfg.channel, cfg.boot_high_ms);
		if (drive_line_level(fd, &cfg, cfg.boot_high_ms, 1) < 0) {
			perror("drive line high");
			close(fd);
			return 1;
		}
		usleep(10000);
	}

	if (connect_bootloader(fd, &cfg, &dev) < 0) {
		perror("connect bootloader");
		fprintf(stderr, "Power-cycle the ESC and make sure the input line is in bootloader mode.\n");
		close(fd);
		return 1;
	}

	printf("bootloader: 471%c\n", dev.boot_tag);
	printf("signature:  0x%02x%02x\n", dev.signature_hi, dev.signature_lo);
	printf("family:     %s\n", device_family(&dev));
	printf("boot ver:   %u\n", dev.boot_version);
	printf("boot pages: %u\n", dev.boot_pages);

	if (!strcmp(device_family(&dev), "Atmel"))
		read_cmd = CMD_READ_FLASH_ATM;

	if (cfg.read_len) {
		if (read_flash(fd, &cfg, read_cmd, cfg.flash_addr, flash, cfg.read_len) < 0) {
			perror("read flash");
			close(fd);
			return 1;
		}
		printf("\nflash 0x%04x..0x%04x:\n", cfg.flash_addr,
		       cfg.flash_addr + cfg.read_len - 1);
		hexdump(flash, cfg.read_len, cfg.flash_addr);
	}

	close(fd);
	return 0;
}
