/*
 * Zephyr log backend: LOG_ERR()/LOG_WRN()/LOG_INF()/LOG_DBG() lines become Blinko messages
 * on the LEDs, no change to application code. One log line = one message (split by the
 * transmitter beyond 31 characters); the level maps to the Blinko level; prefixes
 * (timestamp, level, module) are not transmitted, the text is what matters.
 *
 * Needs a real log mode (CONFIG_LOG_MODE_DEFERRED, the default): in the deferred mode the
 * messages are formatted from the logging thread. Minimal mode has no backends.
 */
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/logging/log_backend_std.h>
#include "blinko.h"

static char line[RS_MSG_MAX_LEN * 2 + 1];
static size_t line_len;
static uint8_t cur_level = RS_LVL_INFO;
static volatile bool panic_mode;

static void flush_line(void)
{
	if (line_len == 0) {
		return;
	}
	line[line_len] = 0;
	/* log_output prints "<module>: " before the text; the 31-character messages are better
	 * spent on the text itself */
	const char *text = line;
	for (size_t i = 0; i + 1 < line_len && i < 24; i++) {
		if (line[i] == ':' && line[i + 1] == ' ') { text = line + i + 2; break; }
	}
	blinko_log(cur_level, "%s", text);
	line_len = 0;
}

static int char_out(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);
	for (size_t i = 0; i < length; i++) {
		char c = (char)data[i];
		if (c == '\n' || c == '\r') {
			flush_line();
		} else if (line_len < sizeof(line) - 1) {
			line[line_len++] = c;
		}
	}
	return (int)length;
}

static uint8_t out_buf[64];
LOG_OUTPUT_DEFINE(blinko_log_output, char_out, out_buf, sizeof(out_buf));

static void process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	ARG_UNUSED(backend);
	if (panic_mode) {
		return;                      /* the death loop owns the LEDs now */
	}
	uint8_t lvl = log_msg_get_level(&msg->log);
	if (lvl > CONFIG_BLINKO_LOG_BACKEND_LEVEL || lvl == LOG_LEVEL_NONE) {
		return;
	}
	switch (lvl) {
	case LOG_LEVEL_ERR: cur_level = RS_LVL_ERROR; break;
	case LOG_LEVEL_WRN: cur_level = RS_LVL_WARN; break;
	case LOG_LEVEL_INF: cur_level = RS_LVL_INFO; break;
	default:            cur_level = RS_LVL_DEBUG; break;
	}
	line_len = 0;
	log_output_msg_process(&blinko_log_output, &msg->log, 0);   /* no timestamp/level/module prefix */
	flush_line();
}

static void panic(struct log_backend const *const backend)
{
	ARG_UNUSED(backend);
	panic_mode = true;
}

static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
	ARG_UNUSED(backend); ARG_UNUSED(cnt);
}

static void init(struct log_backend const *const backend)
{
	ARG_UNUSED(backend);
}

static const struct log_backend_api blinko_log_backend_api = {
	.process = process,
	.panic = panic,
	.dropped = dropped,
	.init = init,
};

LOG_BACKEND_DEFINE(blinko_log_backend, blinko_log_backend_api, true);
