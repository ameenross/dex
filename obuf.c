#include "obuf.h"
#include "term.h"
#include "common.h"
#include "uchar.h"

struct output_buffer obuf;
int screen_w = 80;
int screen_h = 24;

static int obuf_avail(void)
{
	return sizeof(obuf.buf) - obuf.count;
}

void buf_reset(unsigned int start_x, unsigned int width, unsigned int scroll_x)
{
	obuf.x = 0;
	obuf.width = width;
	obuf.scroll_x = scroll_x;
	obuf.tab_width = 8;
	obuf.tab = TAB_CONTROL;
	obuf.can_clear = start_x + width == screen_w;
}

// does not update obuf.x
void buf_add_bytes(const char *str, int count)
{
	if (count > obuf_avail()) {
		buf_flush();
		if (count >= sizeof(obuf.buf)) {
			xwrite(1, str, count);
			return;
		}
	}
	memcpy(obuf.buf + obuf.count, str, count);
	obuf.count += count;
}

// does not update obuf.x
void buf_set_bytes(char ch, int count)
{
	while (count) {
		int avail = obuf_avail();
		int n = count;

		if (avail == 0) {
			buf_flush();
			avail = obuf_avail();
		}
		if (n > avail)
			n = avail;

		memset(obuf.buf + obuf.count, ch, n);
		obuf.count += n;
		count -= n;
	}
}

// does not update obuf.x
void buf_add_ch(char ch)
{
	if (obuf_avail() == 0)
		buf_flush();
	obuf.buf[obuf.count++] = ch;
}

void buf_escape(const char *str)
{
	buf_add_bytes(str, strlen(str));
}

void buf_add_str(const char *str)
{
	int len = strlen(str);
	buf_add_bytes(str, len);
	obuf.x += len;
}

// width of ch must be 1
void buf_ch(char ch)
{
	if (obuf.x >= obuf.scroll_x && obuf.x < obuf.width + obuf.scroll_x)
		buf_add_ch(ch);
	obuf.x++;
}

void buf_hide_cursor(void)
{
	if (term_cap.strings[STR_CAP_CMD_vi])
		buf_escape(term_cap.strings[STR_CAP_CMD_vi]);
}

void buf_show_cursor(void)
{
	if (term_cap.strings[STR_CAP_CMD_ve])
		buf_escape(term_cap.strings[STR_CAP_CMD_ve]);
}

void buf_move_cursor(int x, int y)
{
	buf_escape(term_move_cursor(x, y));
}

void buf_set_color(const struct term_color *color)
{
	if (!memcmp(color, &obuf.color, sizeof(*color)))
		return;

	buf_escape(term_set_color(color));
	obuf.color = *color;
}

void buf_clear_eol(void)
{
	if (obuf.x < obuf.scroll_x + obuf.width) {
		if (obuf.can_clear && term_cap.strings[STR_CAP_CMD_ce] && (obuf.color.bg < 0 || term_cap.ut)) {
			buf_escape(term_cap.strings[STR_CAP_CMD_ce]);
		} else {
			buf_set_bytes(' ', obuf.scroll_x + obuf.width - obuf.x);
		}
		obuf.x = obuf.scroll_x + obuf.width;
	}
}

void buf_flush(void)
{
	if (obuf.count) {
		xwrite(1, obuf.buf, obuf.count);
		obuf.count = 0;
	}
}

static void skipped_too_much(unsigned int u)
{
	int n = obuf.x - obuf.scroll_x;

	if (obuf_avail() < 8)
		buf_flush();
	if (u == '\t' && obuf.tab != TAB_CONTROL) {
		char ch = ' ';
		if (obuf.tab == TAB_SPECIAL)
			ch = '-';
		memset(obuf.buf + obuf.count, ch, n);
		obuf.count += n;
	} else if (u < 0x20) {
		obuf.buf[obuf.count++] = u | 0x40;
	} else if (u == 0x7f) {
		obuf.buf[obuf.count++] = '?';
	} else if (u_is_unprintable(u)) {
		char tmp[4];
		unsigned int idx = 0;
		u_set_hex(tmp, &idx, u);
		memcpy(obuf.buf + obuf.count, tmp + 4 - n, n);
		obuf.count += n;
	} else {
		obuf.buf[obuf.count++] = '>';
	}
}

void buf_skip(unsigned int u)
{
	if (likely(u < 0x80)) {
		if (likely(!u_is_ctrl(u))) {
			obuf.x++;
		} else if (u == '\t' && obuf.tab != TAB_CONTROL) {
			obuf.x += (obuf.x + obuf.tab_width) / obuf.tab_width * obuf.tab_width - obuf.x;
		} else {
			// control
			obuf.x += 2;
		}
	} else {
		// u_char_width() needed to handle 0x80-0x9f even if term_utf8 is 0
		obuf.x += u_char_width(u);
	}

	if (obuf.x > obuf.scroll_x)
		skipped_too_much(u);
}

static void print_tab(unsigned int width)
{
	char ch = ' ';

	if (obuf.tab == TAB_SPECIAL) {
		obuf.buf[obuf.count++] = '>';
		obuf.x++;
		width--;
		ch = '-';
	}
	if (width > 0) {
		memset(obuf.buf + obuf.count, ch, width);
		obuf.count += width;
		obuf.x += width;
	}
}

int buf_put_char(unsigned int u)
{
	unsigned int space = obuf.scroll_x + obuf.width - obuf.x;
	unsigned int width;

	if (!space)
		return 0;
	if (obuf_avail() < 8)
		buf_flush();

	if (likely(u < 0x80)) {
		if (likely(!u_is_ctrl(u))) {
			obuf.buf[obuf.count++] = u;
			obuf.x++;
		} else if (u == '\t' && obuf.tab != TAB_CONTROL) {
			width = (obuf.x + obuf.tab_width) / obuf.tab_width * obuf.tab_width - obuf.x;
			if (width > space)
				width = space;
			print_tab(width);
		} else {
			u_set_ctrl(obuf.buf, &obuf.count, u);
			obuf.x += 2;
			if (unlikely(space == 1)) {
				// wrote too much
				obuf.count--;
				obuf.x--;
			}
		}
	} else if (term_utf8) {
		width = u_char_width(u);
		if (width <= space) {
			u_set_char(obuf.buf, &obuf.count, u);
			obuf.x += width;
		} else if (u_is_unprintable(u)) {
			// <xx> would not fit
			// there's enough space in the buffer so render all 4 characters
			// but increment position less
			unsigned int idx = obuf.count;
			u_set_hex(obuf.buf, &idx, u);
			obuf.count += space;
			obuf.x += space;
		} else {
			obuf.buf[obuf.count++] = '>';
			obuf.x++;
		}
	} else {
		// terminal character set is assumed to be latin1
		if (u > 0x9f) {
			// 0xa0 - 0xff
			obuf.buf[obuf.count++] = u;
			obuf.x++;
		} else {
			// 0x80 - 0x9f
			if (space >= 4) {
				u_set_hex(obuf.buf, &obuf.count, u);
				obuf.x += 4;
			} else {
				unsigned int idx = obuf.count;
				u_set_hex(obuf.buf, &idx, u);
				obuf.count += space;
				obuf.x += space;
			}
		}
	}
	return 1;
}
