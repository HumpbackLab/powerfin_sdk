// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minimal Betaflight/Cleanflight MSP + serial 4way bridge for BLHeli_32 over
 * the Rockchip Flexbus DShot passthrough device.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define RK_DSHOT_IOCTL_BASE 'D'
#define RK_DSHOT_IOC_PASSTHROUGH_XFER \
	_IOWR(RK_DSHOT_IOCTL_BASE, 0x04, struct rk_dshot_passthrough_xfer)

#define RK_DSHOT_PASSTHROUGH_F_TX_INVERT (1U << 0)
#define RK_DSHOT_PASSTHROUGH_F_RX_INVERT (1U << 1)

#define DEFAULT_DSHOT_DEV "/dev/rk-flexbus-dshot"
#define DEFAULT_HOST_BAUD 115200
#define DEFAULT_BIT_US 52
#define DEFAULT_OVERSAMPLE 32
#define DEFAULT_RX_WINDOW_MS 80
#define DEFAULT_TIMEOUT_MS 300
#define DEFAULT_ESC_COUNT 4

#define MSP_API_VERSION 1
#define MSP_FC_VARIANT 2
#define MSP_FC_VERSION 3
#define MSP_BOARD_INFO 4
#define MSP_BUILD_INFO 5
#define MSP_NAME 10
#define MSP_FEATURE 36
#define MSP_REBOOT 68
#define MSP_ADVANCED_CONFIG 90
#define MSP_IDENT 100
#define MSP_STATUS 101
#define MSP_MOTOR 104
#define MSP_BOXNAMES 116
#define MSP_BOXIDS 119
#define MSP_3D 124
#define MSP_BATTERY_STATE 130
#define MSP_MOTOR_CONFIG 131
#define MSP_STATUS_EX 150
#define MSP_UID 160
#define MSP_SET_MOTOR 214
#define MSP_SET_PASSTHROUGH 245

#define MSP_PASSTHROUGH_ESC_4WAY 0xff

#define MOTOR_PROTOCOL_DSHOT600 7

#define BL_CMD_READ_FLASH_SIL 0x03
#define BL_CMD_PROG_FLASH 0x01
#define BL_CMD_ERASE_FLASH 0x02
#define BL_CMD_VERIFY_FLASH_ARM 0x04
#define BL_CMD_SET_ADDRESS 0xff
#define BL_CMD_SET_BUFFER 0xfe
#define BL_CMD_KEEP_ALIVE 0xfd

#define BR_SUCCESS 0x30
#define BR_ERROR_COMMAND 0xc1

#define CMD_REMOTE_ESCAPE 0x2e
#define CMD_LOCAL_ESCAPE 0x2f
#define CMD_INTERFACE_TEST_ALIVE 0x30
#define CMD_PROTOCOL_GET_VERSION 0x31
#define CMD_INTERFACE_GET_NAME 0x32
#define CMD_INTERFACE_GET_VERSION 0x33
#define CMD_INTERFACE_EXIT 0x34
#define CMD_DEVICE_RESET 0x35
#define CMD_DEVICE_INIT_FLASH 0x37
#define CMD_DEVICE_PAGE_ERASE 0x39
#define CMD_DEVICE_READ 0x3a
#define CMD_DEVICE_WRITE 0x3b
#define CMD_INTERFACE_SET_MODE 0x3f
#define CMD_DEVICE_VERIFY 0x40

#define ACK_OK 0x00
#define ACK_I_INVALID_CMD 0x02
#define ACK_I_INVALID_CRC 0x03
#define ACK_I_VERIFY_ERROR 0x04
#define ACK_I_INVALID_CHANNEL 0x08
#define ACK_I_INVALID_PARAM 0x09
#define ACK_D_GENERAL_ERROR 0x0f

#define IM_SIL_BLB 1
#define IM_ATM_BLB 2
#define IM_ARM_BLB 4

#define SERIAL_4WAY_PROTOCOL_VER 108
#define SERIAL_4WAY_VERSION_HI 200
#define SERIAL_4WAY_VERSION_LO 6
#define SERIAL_4WAY_NAME "m4wFCIntf"

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
	const char *serial_dev;
	const char *dshot_dev;
	unsigned int host_baud;
	unsigned int bit_us;
	unsigned int oversample;
	unsigned int rx_window_ms;
	unsigned int timeout_ms;
	unsigned int esc_count;
	uint32_t flags;
	int verbose;
};

struct blheli_dev {
	uint8_t mode;
	uint8_t sig_hi;
	uint8_t sig_lo;
	uint8_t tag;
	uint8_t boot_ver;
	uint8_t boot_pages;
};

static speed_t baud_to_speed(unsigned int baud)
{
	switch (baud) {
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 500000: return B500000;
	case 576000: return B576000;
	case 921600: return B921600;
	default: return 0;
	}
}

static int setup_serial(int fd, unsigned int baud)
{
	struct termios tio;
	speed_t speed = baud_to_speed(baud);

	if (!speed)
		return -1;
	if (tcgetattr(fd, &tio) < 0)
		return -1;

	cfmakeraw(&tio);
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cflag &= ~CRTSCTS;
	tio.c_cflag &= ~PARENB;
	tio.c_cflag &= ~CSTOPB;
	tio.c_cflag &= ~CSIZE;
	tio.c_cflag |= CS8;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	cfsetispeed(&tio, speed);
	cfsetospeed(&tio, speed);

	if (tcsetattr(fd, TCSANOW, &tio) < 0)
		return -1;
	return tcflush(fd, TCIOFLUSH);
}

static uint16_t crc16_update(uint16_t crc, uint8_t data)
{
	crc ^= data;
	for (unsigned int i = 0; i < 8; i++)
		crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
	return crc;
}

static uint16_t crc16_buf(const uint8_t *buf, size_t len)
{
	uint16_t crc = 0;

	for (size_t i = 0; i < len; i++)
		crc = crc16_update(crc, buf[i]);
	return crc;
}

static uint16_t crc_xmodem_update(uint16_t crc, uint8_t data)
{
	crc ^= (uint16_t)data << 8;
	for (unsigned int i = 0; i < 8; i++)
		crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
	return crc;
}

static unsigned int sample_rate(const struct config *cfg)
{
	return (1000000U * cfg->oversample + cfg->bit_us / 2) / cfg->bit_us;
}

static unsigned int rx_oversample(const struct config *cfg, unsigned int actual_rate)
{
	return (actual_rate * cfg->bit_us + 500000U) / 1000000U;
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
				  size_t sample_count, unsigned int oversample)
{
	size_t out = 0;
	size_t i = 0;

	while (i + oversample * 10 < sample_count && out < max_len) {
		size_t start, sample_at, stop_sample_at;
		uint16_t bitmask = 0;

		while (i < sample_count && samples[i])
			i++;
		if (i + oversample * 10 >= sample_count)
			break;

		start = i;
		sample_at = start + (oversample * 3 + 2) / 4;
		stop_sample_at = sample_at;

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
			bytes[out++] = (bitmask >> 1) & 0xff;
			i = stop_sample_at + 1;
		} else {
			i = start + 1;
		}
	}
	return out;
}

static void dump_sample_stats(const char *label, const uint8_t *samples, size_t sample_count,
			      size_t offset)
{
	size_t ones = 0, edges = 0;
	unsigned int printed = 0;

	if (offset > sample_count)
		offset = sample_count;
	for (size_t i = offset; i < sample_count; i++) {
		ones += samples[i] ? 1 : 0;
		if (i > offset && samples[i] != samples[i - 1])
			edges++;
	}
	fprintf(stderr, "%s offset=%zu samples=%zu high=%.2f%% edges=%zu first=%u last=%u\n",
		label, offset, sample_count - offset,
		sample_count > offset ? 100.0 * ones / (sample_count - offset) : 0.0,
		edges, sample_count > offset ? samples[offset] : 0,
		sample_count ? samples[sample_count - 1] : 0);
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


static int esc_xfer(int fd, const struct config *cfg, unsigned int channel,
		    const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len,
		    size_t *got)
{
	unsigned int req_rate = sample_rate(cfg);
	size_t tx_samples_len = tx_len * 11 * cfg->oversample + (tx_len ? 1 : 0);
	size_t rx_window_samples = (req_rate * cfg->rx_window_ms + 999) / 1000;
	size_t rx_lead_samples = tx_samples_len;
	size_t rx_samples_len = rx_lead_samples + rx_window_samples;
	unsigned int min_timeout_ms = (unsigned int)((rx_samples_len * 1000ULL + req_rate - 1) / req_rate) + 100;
	uint8_t *tx_samples = calloc(tx_samples_len ? tx_samples_len : 1, 1);
	uint8_t *rx_samples = calloc(rx_samples_len ? rx_samples_len : 1, 1);
	struct rk_dshot_passthrough_xfer xfer;
	int ret = -1;

	*got = 0;
	if (!tx_samples || !rx_samples) {
		errno = ENOMEM;
		goto out;
	}
	if (tx_len)
		encode_blheli_suart(tx_samples, tx, tx_len, cfg->oversample);

	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf = (uintptr_t)tx_samples;
	xfer.rx_buf = (uintptr_t)rx_samples;
	xfer.tx_samples = tx_samples_len;
	xfer.rx_samples = rx_samples_len;
	xfer.sample_rate = req_rate;
	xfer.channel = channel;
	xfer.timeout_ms = cfg->timeout_ms > min_timeout_ms ? cfg->timeout_ms : min_timeout_ms;
	xfer.flags = cfg->flags;

	if (ioctl(fd, RK_DSHOT_IOC_PASSTHROUGH_XFER, &xfer) < 0) {
		fprintf(stderr,
				"ioctl xfer failed ch=%u tx_len=%zu tx_samples=%zu rx_samples=%zu timeout=%u: %s\n",
				channel, tx_len, tx_samples_len, rx_samples_len,
				xfer.timeout_ms, strerror(errno));
		goto out;
	}

		unsigned int actual_rate = xfer.sample_rate ?: req_rate;
		unsigned int rx_os = rx_oversample(cfg, actual_rate);
		if (cfg->verbose) {
			fprintf(stderr, "xfer ch=%u tx_len=%zu req_rate=%u actual_rate=%u rx_os=%u tx_samples=%zu rx_samples=%zu\n",
				channel, tx_len, req_rate, actual_rate, rx_os,
				tx_samples_len, rx_samples_len);
			dump_sample_stats("rx", rx_samples, rx_samples_len, rx_lead_samples);
		}
		*got = decode_blheli_suart(rx, rx_len, rx_samples + rx_lead_samples,
					   rx_samples_len - rx_lead_samples, rx_os);
	if (cfg->verbose && *got) {
		fprintf(stderr, "rx_decoded:");
		for (size_t i = 0; i < *got; i++)
			fprintf(stderr, " %02x", rx[i]);
		fprintf(stderr, "\n");
	}
	ret = 0;

out:
	free(rx_samples);
	free(tx_samples);
	return ret;
}

static int bl_raw(int fd, const struct config *cfg, unsigned int channel,
		  const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len,
		  size_t min_rx, size_t *got)
{
	if (esc_xfer(fd, cfg, channel, tx, tx_len, rx, rx_len, got) < 0)
		return -1;
	return *got >= min_rx ? 0 : -1;
}

static int bl_connect(int fd, const struct config *cfg, unsigned int channel,
		      struct blheli_dev *dev)
{
	static const uint8_t boot_init[] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0x0d, 'B', 'L', 'H', 'e', 'l', 'i', 0xf4, 0x7d
	};
	uint8_t rx[64];
	size_t got = 0;

	memset(dev, 0, sizeof(*dev));
	for (unsigned int attempt = 0; attempt < 3; attempt++) {
		if (bl_raw(fd, cfg, channel, boot_init, sizeof(boot_init), rx, sizeof(rx), 9, &got) < 0)
			continue;
		if (rx[0] == '4' && rx[1] == '7' && rx[2] == '1' && rx[8] == BR_SUCCESS) {
			dev->tag = rx[3];
			dev->sig_hi = rx[4];
			dev->sig_lo = rx[5];
			dev->boot_ver = rx[6];
			dev->boot_pages = rx[7];
			if (dev->sig_lo == 0x06 && dev->sig_hi > 0x00 && dev->sig_hi < 0x90)
				dev->mode = IM_ARM_BLB;
			else if (dev->sig_hi > 0xe8 && dev->sig_hi < 0xf9)
				dev->mode = IM_SIL_BLB;
			else
				dev->mode = IM_ATM_BLB;
			return dev->mode == IM_ARM_BLB ? 0 : -1;
		}
	}
	return -1;
}

static int bl_send_crc_ack(int fd, const struct config *cfg, unsigned int channel,
			   const uint8_t *cmd, size_t cmd_len, unsigned int timeout_ms,
			   uint8_t *last_ack)
{
	uint8_t tx[270];
	uint8_t rx[64];
	size_t got = 0;
	uint16_t crc;
	struct config tmp = *cfg;

	if (cmd_len + 2 > sizeof(tx))
		return -1;
	tmp.rx_window_ms = timeout_ms;
	memcpy(tx, cmd, cmd_len);
	crc = crc16_buf(cmd, cmd_len);
	tx[cmd_len] = crc & 0xff;
	tx[cmd_len + 1] = crc >> 8;

	if (bl_raw(fd, &tmp, channel, tx, cmd_len + 2, rx, sizeof(rx), 1, &got) < 0) {
		fprintf(stderr, "crc_ack ch=%u cmd=0x%02x len=%zu window=%u got=%zu\n",
			channel, cmd_len ? cmd[0] : 0, cmd_len, tmp.rx_window_ms, got);
		return -1;
	}
	*last_ack = rx[got - 1];
	return 0;
}

static int bl_send_crc_noack(int fd, const struct config *cfg, unsigned int channel,
			     const uint8_t *cmd, size_t cmd_len)
{
	uint8_t tx[270];
	uint8_t rx[1];
	size_t got = 0;
	uint16_t crc;
	struct config tmp = *cfg;

	if (cmd_len + 2 > sizeof(tx))
		return -1;
	tmp.rx_window_ms = 0;
	memcpy(tx, cmd, cmd_len);
	crc = crc16_buf(cmd, cmd_len);
	tx[cmd_len] = crc & 0xff;
	tx[cmd_len + 1] = crc >> 8;

	if (esc_xfer(fd, &tmp, channel, tx, cmd_len + 2, rx, 0, &got) < 0) {
		fprintf(stderr, "crc_noack ch=%u cmd=0x%02x len=%zu failed\n",
			channel, cmd_len ? cmd[0] : 0, cmd_len);
		return -1;
	}
	return 0;
}

static void bl_restart_bootloader(int fd, const struct config *cfg, unsigned int channel)
{
	uint8_t cmd[] = { 0, 0 };
	uint8_t ack = 0;

	bl_send_crc_ack(fd, cfg, channel, cmd, sizeof(cmd), 20, &ack);
}


static int bl_set_address(int fd, const struct config *cfg, unsigned int channel, uint16_t addr)
{
	uint8_t cmd[] = { BL_CMD_SET_ADDRESS, 0, addr >> 8, addr & 0xff };
	uint8_t ack = 0;

	if (addr == 0xffff)
		return 0;
	if (bl_send_crc_ack(fd, cfg, channel, cmd, sizeof(cmd), 20, &ack) < 0) {
		fprintf(stderr, "set_address ch=%u addr=0x%04x no-ack\n", channel, addr);
		return -1;
	}
	if (ack != BR_SUCCESS) {
		fprintf(stderr, "set_address ch=%u addr=0x%04x ack=0x%02x\n",
			channel, addr, ack);
		return -1;
	}
	return 0;
}

static int bl_read_flash(int fd, const struct config *cfg, unsigned int channel,
			 uint16_t addr, uint8_t *buf, unsigned int len)
{
	uint8_t tx[4];
	uint8_t rx[300];
	size_t got = 0;
	size_t data_len = len ? len : 256;
	uint16_t crc;
	struct config tmp = *cfg;

	if (data_len > 256 || bl_set_address(fd, cfg, channel, addr) < 0)
		return -1;
	tx[0] = BL_CMD_READ_FLASH_SIL;
	tx[1] = len & 0xff;
	crc = crc16_buf(tx, 2);
	tx[2] = crc & 0xff;
	tx[3] = crc >> 8;

	tmp.rx_window_ms = data_len ? (unsigned int)(data_len + 8) : 280;
	if (tmp.rx_window_ms < 40)
		tmp.rx_window_ms = 40;
	if (bl_raw(fd, &tmp, channel, tx, sizeof(tx), rx, sizeof(rx), data_len + 3, &got) < 0) {
		fprintf(stderr, "read_flash ch=%u addr=0x%04x n=%zu got=%zu\n",
			channel, addr, data_len, got);
		return -1;
	}
	if (rx[data_len + 2] != BR_SUCCESS) {
		fprintf(stderr, "read_flash ch=%u addr=0x%04x n=%zu ack=0x%02x got=%zu\n",
			channel, addr, data_len, rx[data_len + 2], got);
		return -1;
	}
	crc = crc16_buf(rx, data_len);
	if ((uint16_t)(rx[data_len] | (rx[data_len + 1] << 8)) != crc) {
		fprintf(stderr,
			"read_flash ch=%u addr=0x%04x n=%zu crc got=0x%04x want=0x%04x bytes=%02x %02x %02x %02x\n",
			channel, addr, data_len,
			(uint16_t)(rx[data_len] | (rx[data_len + 1] << 8)), crc,
			rx[0], rx[1], rx[2], rx[3]);
		return -1;
	}
	memcpy(buf, rx, data_len);
	return 0;
}

static int bl_read_flash_continue(int fd, const struct config *cfg, unsigned int channel,
				  uint8_t *buf, unsigned int len)
{
	uint8_t tx[4];
	uint8_t rx[300];
	size_t got = 0;
	uint16_t crc;
	struct config tmp = *cfg;

	if (!len || len > 256)
		return -1;
	tx[0] = BL_CMD_READ_FLASH_SIL;
	tx[1] = len & 0xff;
	crc = crc16_buf(tx, 2);
	tx[2] = crc & 0xff;
	tx[3] = crc >> 8;
	tmp.rx_window_ms = len + 8;
	if (tmp.rx_window_ms < 40)
		tmp.rx_window_ms = 40;
	if (bl_raw(fd, &tmp, channel, tx, sizeof(tx), rx, sizeof(rx), len + 3, &got) < 0)
		return -1;
	if (rx[len + 2] != BR_SUCCESS)
		return -1;
	crc = crc16_buf(rx, len);
	if ((uint16_t)(rx[len] | (rx[len + 1] << 8)) != crc)
		return -1;
	memcpy(buf, rx, len);
	return 0;
}

static int bl_read_flash_chunked(int fd, const struct config *cfg, unsigned int channel,
				 uint16_t addr, uint8_t *buf, unsigned int len)
{
	unsigned int data_len = len ? len : 256;
	unsigned int pos = 0;

	if (!len) {
		return bl_read_flash(fd, cfg, channel, addr, buf, 0);
	}

	while (pos < data_len) {
		unsigned int chunk = data_len - pos;

		if (chunk > 16)
			chunk = 16;
		if (bl_read_flash(fd, cfg, channel, addr + pos, buf + pos, chunk) < 0)
			return -1;
		pos += chunk;
	}
	return 0;
}

static int bl_set_buffer(int fd, const struct config *cfg, unsigned int channel,
			 const uint8_t *buf, unsigned int len)
{
	uint8_t tx[4 + 2 + 256 + 2];
	uint8_t rx[16];
	uint8_t cmd[] = { BL_CMD_SET_BUFFER, 0, len ? 0 : 1, len };
	unsigned int data_len = len ? len : 256;
	size_t pos = 0;
	size_t got = 0;
	uint16_t crc;
	uint8_t ack = 0;
	struct config tmp = *cfg;

	memcpy(tx + pos, cmd, sizeof(cmd));
	pos += sizeof(cmd);
	crc = crc16_buf(cmd, sizeof(cmd));
	tx[pos++] = crc & 0xff;
	tx[pos++] = crc >> 8;

	memcpy(tx + pos, buf, data_len);
	pos += data_len;
	crc = crc16_buf(buf, data_len);
	tx[pos++] = crc & 0xff;
	tx[pos++] = crc >> 8;

	tmp.rx_window_ms = 300;
	if (bl_raw(fd, &tmp, channel, tx, pos, rx, sizeof(rx), 1, &got) < 0) {
		fprintf(stderr, "set_buffer combined ch=%u len=%u tx_len=%zu got=%zu\n",
			channel, len, pos, got);
		return -1;
	}
	ack = rx[got - 1];
	if (ack != BR_SUCCESS)
		fprintf(stderr, "set_buffer combined ch=%u len=%u ack=0x%02x got=%zu\n",
			channel, len, ack, got);
	return ack == BR_SUCCESS ? 0 : -1;
}

static int bl_write_like(int fd, const struct config *cfg, unsigned int channel,
			 uint8_t cmd_id, uint16_t addr, const uint8_t *buf,
			 unsigned int len, unsigned int timeout_ms, uint8_t *ack)
{
	uint8_t cmd[] = { cmd_id, 1 };

	if (bl_set_address(fd, cfg, channel, addr) < 0)
		return -1;
	if (bl_set_buffer(fd, cfg, channel, buf, len) < 0)
		return -1;
	if (bl_send_crc_ack(fd, cfg, channel, cmd, sizeof(cmd), timeout_ms, ack) < 0) {
		fprintf(stderr,
			"write_like cmd=0x%02x ch=%u addr=0x%04x len=%u timeout=%u no-ack\n",
			cmd_id, channel, addr, len, timeout_ms);
		return -1;
	}
	return 0;
}

static int bl_page_erase(int fd, const struct config *cfg, unsigned int channel, uint16_t addr)
{
	uint8_t cmd[] = { BL_CMD_ERASE_FLASH, 1 };
	uint8_t ack = 0;

	if (bl_set_address(fd, cfg, channel, addr) < 0)
		return -1;
	if (bl_send_crc_ack(fd, cfg, channel, cmd, sizeof(cmd), 3000, &ack) < 0)
		return -1;
	return ack == BR_SUCCESS ? 0 : -1;
}

static int read_exact_timeout(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
	size_t pos = 0;

	while (pos < len) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int r = poll(&pfd, 1, timeout_ms);

		if (r <= 0)
			return -1;
		ssize_t n = read(fd, buf + pos, len - pos);
		if (n <= 0)
			return -1;
		pos += n;
	}
	return 0;
}

static int write_all(int fd, const uint8_t *buf, size_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += n;
		len -= n;
	}
	return 0;
}

static int msp_reply(int fd, uint8_t cmd, const uint8_t *payload, uint8_t len, int err)
{
	uint8_t frame[8 + 255];
	uint8_t csum = 0;
	size_t pos = 0;

	frame[pos++] = '$';
	frame[pos++] = 'M';
	frame[pos++] = err ? '!' : '>';
	frame[pos++] = len;
	frame[pos++] = cmd;
	csum ^= len;
	csum ^= cmd;
	for (uint8_t i = 0; i < len; i++) {
		frame[pos++] = payload[i];
		csum ^= payload[i];
	}
	frame[pos++] = csum;
	return write_all(fd, frame, pos);
}

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8;
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static int fourway_loop(int serial_fd, int dshot_fd, const struct config *cfg);

static int handle_msp(int fd, int dshot_fd, const struct config *cfg)
{
	uint8_t hdr[3];
	uint8_t len, cmd, csum, payload[255], calc;

	for (;;) {
		if (read_exact_timeout(fd, hdr, 1, -1) < 0)
			return -1;
		if (hdr[0] != '$')
			continue;
		if (read_exact_timeout(fd, hdr + 1, 2, 1000) < 0)
			continue;
		if (hdr[1] != 'M' || hdr[2] != '<')
			continue;
		if (read_exact_timeout(fd, &len, 1, 1000) < 0 ||
		    read_exact_timeout(fd, &cmd, 1, 1000) < 0)
			continue;
		if (read_exact_timeout(fd, payload, len, 1000) < 0 ||
		    read_exact_timeout(fd, &csum, 1, 1000) < 0)
			continue;
		calc = len ^ cmd;
		for (uint8_t i = 0; i < len; i++)
			calc ^= payload[i];
		if (calc != csum)
			continue;

		uint8_t out[64];
		uint8_t out_len = 0;
			fprintf(stderr, "msp cmd=%u len=%u\n", cmd, len);
			switch (cmd) {
			case MSP_API_VERSION:
				out[0] = 0; out[1] = 1; out[2] = 44; out_len = 3;
				break;
		case MSP_FC_VARIANT:
			memcpy(out, "BTFL", 4); out_len = 4;
			break;
		case MSP_FC_VERSION:
			out[0] = 4; out[1] = 5; out[2] = 0; out_len = 3;
			break;
		case MSP_BOARD_INFO:
			memcpy(out, "RKFB", 4); out[4] = 0; out[5] = 0; out[6] = 0; out[7] = 0; out_len = 8;
			break;
			case MSP_BUILD_INFO:
				memcpy(out, "Jul 11 2026 12:00:00", 20);
				memcpy(out + 20, "rk3506", 6);
				out_len = 26;
				break;
			case MSP_NAME:
				memcpy(out, "rk-blheli32", 11); out_len = 11;
				break;
			case MSP_IDENT:
				out[0] = 0;        /* version */
				out[1] = 0;        /* multitype */
				out[2] = 0;        /* MSP version */
				put_u32(out + 3, 0);
				out_len = 7;
				break;
			case MSP_FEATURE:
				put_u32(out, 0);
				out_len = 4;
				break;
			case MSP_UID:
				memset(out, 0x42, 12); out_len = 12;
				break;
			case MSP_STATUS:
				memset(out, 0, 21);
				put_u16(out + 0, 1000);
				put_u16(out + 4, 0x27);
				put_u16(out + 11, 5);
				out[15] = 0;
				out[16] = 18;
				out_len = 21;
				break;
			case MSP_STATUS_EX:
				memset(out, 0, 21);
				put_u16(out + 0, 1000);
				put_u16(out + 4, 0x27);
				put_u16(out + 11, 5);
				out[15] = 0;
				out[16] = 18;
				out_len = 21;
				break;
			case MSP_ADVANCED_CONFIG:
				memset(out, 0, 10);
				out[0] = 1;
				out[1] = 4;
				out[3] = MOTOR_PROTOCOL_DSHOT600;
				put_u16(out + 4, 480);
				put_u16(out + 6, 450);
				out_len = 10;
				break;
			case MSP_3D:
				put_u16(out + 0, 1406);
				put_u16(out + 2, 1514);
				put_u16(out + 4, 1460);
				out_len = 6;
				break;
			case MSP_BATTERY_STATE:
				memset(out, 0, 11);
				out_len = 11;
				break;
			case MSP_MOTOR_CONFIG:
				put_u16(out + 0, 1000);
				put_u16(out + 2, 2000);
				put_u16(out + 4, 1000);
				out[6] = cfg->esc_count;
				out[7] = 14;
				out[8] = 0;
				out[9] = 0;
				out_len = 10;
				break;
			case MSP_MOTOR:
				memset(out, 0, 16);
				for (unsigned int i = 0; i < cfg->esc_count && i < 8; i++)
					put_u16(out + i * 2, 1000);
				out_len = 16;
				break;
			case MSP_BOXNAMES:
			case MSP_BOXIDS:
				out_len = 0;
				break;
			case MSP_SET_MOTOR:
				out_len = 0;
				break;
			case MSP_REBOOT:
				out_len = 0;
				break;
				case MSP_SET_PASSTHROUGH:
					if (len == 0 || payload[0] == MSP_PASSTHROUGH_ESC_4WAY) {
						out[0] = cfg->esc_count;
					out_len = 1;
					if (msp_reply(fd, cmd, out, out_len, 0) < 0)
						return -1;
					fprintf(stderr, "entered serial 4way\n");
					if (fourway_loop(fd, dshot_fd, cfg) < 0)
						return -1;
					fprintf(stderr, "left serial 4way\n");
					continue;
				}
				out[0] = 0; out_len = 1;
				break;
			default:
				fprintf(stderr, "ignore unknown msp cmd=%u len=%u\n", cmd, len);
				continue;
			}
		if (msp_reply(fd, cmd, out, out_len, 0) < 0)
			return -1;
	}
}

static int fourway_reply(int fd, uint8_t cmd, uint8_t ah, uint8_t al,
			 const uint8_t *payload, uint8_t len, uint8_t ack)
{
	uint8_t frame[8 + 256];
	uint16_t crc = 0;
	size_t pos = 0;
	size_t payload_len = len;

	if (cmd == CMD_DEVICE_READ && ack == ACK_OK && len == 0)
		payload_len = 256;

#define PUTCRC(v) do { uint8_t _b = (uint8_t)(v); frame[pos++] = _b; crc = crc_xmodem_update(crc, _b); } while (0)
	PUTCRC(CMD_REMOTE_ESCAPE);
	PUTCRC(cmd);
	PUTCRC(ah);
	PUTCRC(al);
	PUTCRC(len);
	for (size_t i = 0; i < payload_len; i++)
		PUTCRC(payload[i]);
	PUTCRC(ack);
#undef PUTCRC
	frame[pos++] = crc >> 8;
	frame[pos++] = crc & 0xff;
	fprintf(stderr, "4way frame cmd=0x%02x payload_len=%zu total=%zu ack=0x%02x\n",
		cmd, payload_len, pos, ack);
	return write_all(fd, frame, pos);
}

static int fourway_loop(int serial_fd, int dshot_fd, const struct config *cfg)
{
	struct blheli_dev dev[4];
	uint8_t current_mode = IM_ARM_BLB;
	unsigned int selected_ch = 0;
	uint8_t write_cache[256];
	uint16_t write_cache_addr = 0;
	unsigned int write_cache_len = 0;
	int write_cache_valid = 0;

	memset(dev, 0, sizeof(dev));
		for (;;) {
			uint8_t esc, cmd, ah, al, len, payload[256], crc_hi, crc_lo;
			uint16_t crc = 0, got_crc;
			uint8_t ack = ACK_OK;
			uint8_t out[256];
			uint8_t out_len = 1;
			unsigned int payload_len;
			unsigned int ch;

		do {
			if (read_exact_timeout(serial_fd, &esc, 1, -1) < 0)
				return -1;
		} while (esc != CMD_LOCAL_ESCAPE);

#define READCRC(v, tmo) do { if (read_exact_timeout(serial_fd, &(v), 1, (tmo)) < 0) goto timeout; crc = crc_xmodem_update(crc, (v)); } while (0)
		crc = crc_xmodem_update(0, esc);
		READCRC(cmd, 50);
			READCRC(ah, 25);
			READCRC(al, 25);
			READCRC(len, 25);
			payload_len = len;
			if ((cmd == CMD_DEVICE_WRITE || cmd == CMD_DEVICE_VERIFY) && len == 0)
				payload_len = 256;
			for (unsigned int i = 0; i < payload_len; i++)
				READCRC(payload[i], 25);
#undef READCRC
		if (read_exact_timeout(serial_fd, &crc_hi, 1, 25) < 0 ||
		    read_exact_timeout(serial_fd, &crc_lo, 1, 25) < 0)
			goto timeout;
		got_crc = ((uint16_t)crc_hi << 8) | crc_lo;
			if (got_crc != crc)
				ack = ACK_I_INVALID_CRC;

			memset(out, 0, sizeof(out));
				ch = payload_len ? payload[0] : 0;
				fprintf(stderr, "4way cmd=0x%02x addr=0x%02x%02x len=%u",
					cmd, ah, al, len);
				if (payload_len)
					fprintf(stderr, " payload0=0x%02x", payload[0]);
			fprintf(stderr, "\n");
			if (ack == ACK_OK) {
				switch (cmd) {
			case CMD_INTERFACE_TEST_ALIVE: {
					uint8_t bcmd[] = { BL_CMD_KEEP_ALIVE, 0 };
				uint8_t back = 0;
				if (dev[selected_ch].sig_lo && (bl_send_crc_ack(dshot_fd, cfg, selected_ch,
						      bcmd, sizeof(bcmd), 20, &back) < 0 ||
						      back != BR_ERROR_COMMAND))
					ack = ACK_D_GENERAL_ERROR;
				break;
			}
				case CMD_PROTOCOL_GET_VERSION:
					out[0] = SERIAL_4WAY_PROTOCOL_VER;
					break;
				case CMD_INTERFACE_GET_NAME:
					memcpy(out, SERIAL_4WAY_NAME, strlen(SERIAL_4WAY_NAME));
					out_len = strlen(SERIAL_4WAY_NAME);
				break;
			case CMD_INTERFACE_GET_VERSION:
				out[0] = SERIAL_4WAY_VERSION_HI;
				out[1] = SERIAL_4WAY_VERSION_LO;
				out_len = 2;
				break;
			case CMD_INTERFACE_SET_MODE:
				if (payload[0] >= IM_SIL_BLB && payload[0] <= IM_ARM_BLB)
					current_mode = payload[0];
				else
					ack = ACK_I_INVALID_PARAM;
				break;
			case CMD_INTERFACE_EXIT:
				fourway_reply(serial_fd, cmd, ah, al, out, out_len, ack);
				return 0;
					case CMD_DEVICE_INIT_FLASH:
						if (ch >= cfg->esc_count) {
							ack = ACK_I_INVALID_CHANNEL;
							break;
						}
					write_cache_valid = 0;
					if (bl_connect(dshot_fd, cfg, ch, &dev[ch]) < 0) {
						ack = ACK_D_GENERAL_ERROR;
						break;
					}
				selected_ch = ch;
				current_mode = dev[ch].mode;
				out[0] = dev[ch].sig_lo;
				out[1] = dev[ch].sig_hi;
					out[2] = dev[ch].tag;
					out[3] = current_mode;
					out_len = 4;
					fprintf(stderr,
						"init ch=%u sig=0x%02x%02x tag=%c mode=%u\n",
						ch, out[1], out[0], out[2], out[3]);
					break;
				case CMD_DEVICE_READ:
					if (len != 1) {
						ack = ACK_I_INVALID_PARAM;
						break;
						}
						uint16_t read_addr = ((uint16_t)ah << 8) | al;
						unsigned int read_len = payload[0] ? payload[0] : 256;
						if (write_cache_valid &&
						    read_addr >= write_cache_addr &&
						    read_addr + read_len <= write_cache_addr + write_cache_len) {
							memcpy(out, write_cache + (read_addr - write_cache_addr),
							       read_len);
							out_len = payload[0];
							fprintf(stderr,
								"read ch=%u addr=0x%04x n=%u from write cache crc=0x%04x head=%02x %02x %02x %02x tail=%02x %02x %02x %02x\n",
								selected_ch, read_addr, read_len,
								crc16_buf(out, read_len),
								out[0], out[1], out[2], out[3],
								out[read_len - 4], out[read_len - 3],
								out[read_len - 2], out[read_len - 1]);
						} else if (bl_read_flash_chunked(dshot_fd, cfg, selected_ch, read_addr,
									 out, payload[0]) < 0) {
							ack = ACK_D_GENERAL_ERROR;
						} else {
							uint16_t read_crc = crc16_buf(out, read_len);

							out_len = payload[0];
						fprintf(stderr,
							"read ch=%u addr=0x%04x host=0x%02x%02x n=%u crc=0x%04x head=%02x %02x %02x %02x %02x %02x %02x %02x tail=%02x %02x %02x %02x %02x %02x %02x %02x\n",
							selected_ch, read_addr, ah, al, read_len, read_crc,
							out[0], out[1], out[2], out[3],
							out[4], out[5], out[6], out[7],
							out[read_len - 8], out[read_len - 7],
							out[read_len - 6], out[read_len - 5],
							out[read_len - 4], out[read_len - 3],
							out[read_len - 2], out[read_len - 1]);
					}
					break;
				case CMD_DEVICE_PAGE_ERASE:
					if (len != 1 || current_mode != IM_ARM_BLB ||
						   bl_page_erase(dshot_fd, cfg, selected_ch,
								 ((uint16_t)payload[0] << 10)) < 0) {
						ack = ACK_D_GENERAL_ERROR;
					}
					break;
				case CMD_DEVICE_WRITE: {
					uint8_t back = 0;

					if (bl_write_like(dshot_fd, cfg, selected_ch, BL_CMD_PROG_FLASH,
									 ((uint16_t)ah << 8) | al,
									 payload, len, 500, &back) < 0 ||
							   back != BR_SUCCESS) {
							fprintf(stderr, "write failed ch=%u addr=0x%02x%02x len=%u back=0x%02x\n",
								selected_ch, ah, al, len, back);
							ack = ACK_D_GENERAL_ERROR;
						} else {
							unsigned int write_len = len ? len : 256;

							memcpy(write_cache, payload, write_len);
							write_cache_addr = ((uint16_t)ah << 8) | al;
							write_cache_len = write_len;
							write_cache_valid = 1;
							fprintf(stderr,
								"write cached ch=%u addr=0x%04x n=%u crc=0x%04x head=%02x %02x %02x %02x tail=%02x %02x %02x %02x\n",
								selected_ch, write_cache_addr, write_len,
								crc16_buf(write_cache, write_len),
								write_cache[0], write_cache[1],
								write_cache[2], write_cache[3],
								write_cache[write_len - 4],
								write_cache[write_len - 3],
								write_cache[write_len - 2],
								write_cache[write_len - 1]);
						}
						break;
					}
				case CMD_DEVICE_VERIFY: {
					uint8_t back = 0;

					if (bl_write_like(dshot_fd, cfg, selected_ch, BL_CMD_VERIFY_FLASH_ARM,
								 ((uint16_t)ah << 8) | al,
								 payload, len, 100, &back) < 0) {
						ack = ACK_D_GENERAL_ERROR;
					} else if (back != BR_SUCCESS) {
						fprintf(stderr, "verify failed ch=%u addr=0x%02x%02x len=%u back=0x%02x\n",
							selected_ch, ah, al, len, back);
						ack = ACK_I_VERIFY_ERROR;
					}
					break;
				}
					case CMD_DEVICE_RESET:
						if (!len || payload[0] >= cfg->esc_count) {
							ack = ACK_I_INVALID_CHANNEL;
							break;
						}
						{
							selected_ch = payload[0];
							if (dev[selected_ch].sig_lo)
								bl_restart_bootloader(dshot_fd, cfg, selected_ch);
						}
						memset(dev, 0, sizeof(dev));
						write_cache_valid = 0;
						break;
			default:
				ack = ACK_I_INVALID_CMD;
				break;
			}
			}
			fprintf(stderr, "4way reply cmd=0x%02x ack=0x%02x out_len=%u\n",
				cmd, ack, out_len);
			if (fourway_reply(serial_fd, cmd, ah, al, out, out_len, ack) < 0)
				return -1;
		continue;
timeout:
		continue;
	}
}

static void usage(const char *argv0)
{
		fprintf(stderr,
			"Usage: %s -s /dev/ttyS4 [-d /dev/rk-flexbus-dshot]\n"
			"          [--host-baud N] [--bit-us N] [--oversample N]\n"
			"          [--rx-window-ms N] [--timeout-ms N] [--esc-count N]\n"
			"          [--tx-invert] [--rx-invert] [-v]\n",
			argv0);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "serial", required_argument, NULL, 's' },
		{ "dshot", required_argument, NULL, 'd' },
		{ "host-baud", required_argument, NULL, 'H' },
		{ "bit-us", required_argument, NULL, 'u' },
		{ "oversample", required_argument, NULL, 'o' },
			{ "rx-window-ms", required_argument, NULL, 'r' },
				{ "timeout-ms", required_argument, NULL, 't' },
				{ "esc-count", required_argument, NULL, 'n' },
				{ "tx-invert", no_argument, NULL, 1000 },
				{ "rx-invert", no_argument, NULL, 1001 },
			{ "verbose", no_argument, NULL, 'v' },
			{ "help", no_argument, NULL, 'h' },
		{}
	};
	struct config cfg = {
		.dshot_dev = DEFAULT_DSHOT_DEV,
		.host_baud = DEFAULT_HOST_BAUD,
		.bit_us = DEFAULT_BIT_US,
		.oversample = DEFAULT_OVERSAMPLE,
		.rx_window_ms = DEFAULT_RX_WINDOW_MS,
		.timeout_ms = DEFAULT_TIMEOUT_MS,
		.esc_count = DEFAULT_ESC_COUNT,
	};
	int serial_fd, dshot_fd;

	for (;;) {
		int opt = getopt_long(argc, argv, "s:d:H:u:o:r:t:n:vh", opts, NULL);

		if (opt < 0)
			break;
		switch (opt) {
		case 's': cfg.serial_dev = optarg; break;
		case 'd': cfg.dshot_dev = optarg; break;
		case 'H': cfg.host_baud = strtoul(optarg, NULL, 0); break;
		case 'u': cfg.bit_us = strtoul(optarg, NULL, 0); break;
		case 'o': cfg.oversample = strtoul(optarg, NULL, 0); break;
			case 'r': cfg.rx_window_ms = strtoul(optarg, NULL, 0); break;
				case 't': cfg.timeout_ms = strtoul(optarg, NULL, 0); break;
				case 'n': cfg.esc_count = strtoul(optarg, NULL, 0); break;
				case 1000: cfg.flags |= RK_DSHOT_PASSTHROUGH_F_TX_INVERT; break;
				case 1001: cfg.flags |= RK_DSHOT_PASSTHROUGH_F_RX_INVERT; break;
			case 'v': cfg.verbose = 1; break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (!cfg.serial_dev || cfg.esc_count < 1 || cfg.esc_count > 4 ||
	    cfg.oversample < 8 || !cfg.bit_us) {
		usage(argv[0]);
		return 1;
	}

	serial_fd = open(cfg.serial_dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (serial_fd < 0) {
		perror(cfg.serial_dev);
		return 1;
	}
	if (setup_serial(serial_fd, cfg.host_baud) < 0) {
		perror("setup serial");
		return 1;
	}
	dshot_fd = open(cfg.dshot_dev, O_RDWR | O_CLOEXEC);
	if (dshot_fd < 0) {
		perror(cfg.dshot_dev);
		return 1;
	}

	fprintf(stderr, "bf4way %s host=%u esc_count=%u bit_us=%u\n",
		cfg.serial_dev, cfg.host_baud, cfg.esc_count, cfg.bit_us);
	if (handle_msp(serial_fd, dshot_fd, &cfg) < 0) {
		perror("msp");
		return 1;
	}
	return 0;
}
