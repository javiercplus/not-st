
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include "st.h"
#include "win.h"
#include "graphics.h"

#if   defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif

#define UTF_INVALID   0xFFFD
#define UTF_SIZ       4
#define ESC_BUF_SIZ   (128*UTF_SIZ)
#define ESC_ARG_SIZ   16
#define CAR_PER_ARG   6
/* String buffer: larger than ESC_BUF_SIZ for long DCS sequences (images, etc.) */
#define STR_BUF_SIZ   8192
#define STR_ARG_SIZ   ESC_ARG_SIZ

#define IMAGE_PLACEHOLDER_CHAR IMAGE_PLACEHOLDER
#define IMAGE_PLACEHOLDER_CHAR_OLD IMAGE_PLACEHOLDER_OLD

#define IS_SET(flag)		((term.mode & (flag)) != 0)
#define ISCONTROLC0(c)		(BETWEEN(c, 0, 0x1f) || (c) == 0x7f)
#define ISCONTROLC1(c)		(BETWEEN(c, 0x80, 0x9f))
#include "st_config.h"

extern char *argv0;
int histsize = 2000;
void tmarkurls(int row);

#define MAX_HYPERLINKS 4096

typedef struct {
	char *url;
	char *id;
	int is_osc8;
} Hyperlink;

static Hyperlink hyperlinks[MAX_HYPERLINKS];
static uint32_t current_link_id = 0;
static uint32_t next_link_id = 1;

uint32_t
add_hyperlink(const char *url, const char *id, int is_osc8)
{
	if (!url || !*url)
		return 0;

	if (current_link_id > 0) {
		uint32_t cur_idx = current_link_id % MAX_HYPERLINKS;
		if (hyperlinks[cur_idx].url && strcmp(hyperlinks[cur_idx].url, url) == 0) {
			if (!id || (hyperlinks[cur_idx].id && strcmp(hyperlinks[cur_idx].id, id) == 0)) {
				hyperlinks[cur_idx].is_osc8 = is_osc8;
				return current_link_id;
			}
		}
	}

	for (uint32_t k = 1; k < MAX_HYPERLINKS && k < next_link_id; k++) {
		uint32_t cand_lid = next_link_id - k;
		uint32_t idx = cand_lid % MAX_HYPERLINKS;
		if (hyperlinks[idx].url && strcmp(hyperlinks[idx].url, url) == 0) {
			if ((!id && !hyperlinks[idx].id) ||
			    (id && hyperlinks[idx].id && strcmp(hyperlinks[idx].id, id) == 0)) {
				return cand_lid;
			}
		}
	}

	uint32_t lid = next_link_id++;
	if (next_link_id == 0) next_link_id = 1;
	uint32_t idx = lid % MAX_HYPERLINKS;

	free(hyperlinks[idx].url);
	free(hyperlinks[idx].id);
	hyperlinks[idx].url = xstrdup(url);
	hyperlinks[idx].id = (id && *id) ? xstrdup(id) : NULL;
	hyperlinks[idx].is_osc8 = is_osc8;

	return lid;
}

const char *
get_hyperlink(uint32_t lid)
{
	if (lid == 0)
		return NULL;
	uint32_t idx = lid % MAX_HYPERLINKS;
	return hyperlinks[idx].url;
}

int
is_osc8_link(uint32_t lid)
{
	if (lid == 0)
		return 0;
	return hyperlinks[lid % MAX_HYPERLINKS].is_osc8;
}

#define ISCONTROL(c)		(ISCONTROLC0(c) || ISCONTROLC1(c))
#define ISDELIM(u)		(u && wcschr(worddelimiters, u))

enum term_mode {
	MODE_WRAP        = 1 << 0,
	MODE_INSERT      = 1 << 1,
	MODE_ALTSCREEN   = 1 << 2,
	MODE_CRLF        = 1 << 3,
	MODE_ECHO        = 1 << 4,
	MODE_PRINT       = 1 << 5,
	MODE_UTF8        = 1 << 6,
};

enum cursor_movement {
	CURSOR_SAVE,
	CURSOR_LOAD
};

enum cursor_state {
	CURSOR_DEFAULT  = 0,
	CURSOR_WRAPNEXT = 1,
	CURSOR_ORIGIN   = 2
};

enum charset {
	CS_GRAPHIC0,
	CS_GRAPHIC1,
	CS_UK,
	CS_USA,
	CS_MULTI,
	CS_GER,
	CS_FIN
};

enum escape_state {
	ESC_START      = 1,
	ESC_CSI        = 2,
	ESC_STR        = 4,
	ESC_ALTCHARSET = 8,
	ESC_STR_END    = 16,
	ESC_TEST       = 32,
	ESC_UTF8       = 64,
};

typedef struct {
	Glyph attr;
	int x;
	int y;
	char state;
} TCursor;

typedef struct {
	int mode;
	int type;
	int snap;

	struct {
		int x, y;
	} nb, ne, ob, oe;

	int alt;
} Selection;

typedef struct {
	int row;
	int col;
	int pixw;
	int pixh;
	Line *line;
	Line *alt;
	int *dirty;
	TCursor c;
	int ocx;
	int ocy;
	int top;
	int bot;
	int mode;
	int esc;
	char trantbl[4];
	int charset;
	int icharset;
	int *tabs;
	Rune lastc;
	int prompt_y;
	int mode_2031;
	int last_ws_row;
	int last_ws_col;
} Term;

typedef struct {
	char buf[ESC_BUF_SIZ];
	size_t len;
	char priv;
	int arg[ESC_ARG_SIZ];
	int narg;
	char mode[2];
	int carg[ESC_ARG_SIZ][CAR_PER_ARG];
} CSIEscape;

typedef struct {
	char type;
	char *buf;
	size_t siz;
	size_t len;
	char *args[STR_ARG_SIZ];
	int narg;
} STREscape;

static void execsh(char *, char **);
static void stty(char **);
static void sigchld(int);
static void ttywriteraw(const char *, size_t);

static void csidump(void);
static void csihandle(void);
static void csiparse(void);
static void csireset(void);
static void readcolonargs(char **, int, int[][CAR_PER_ARG]);
static void osc_color_response(int, int, int);
static void report_color_scheme(void);
static int eschandle(uchar);
static void strdump(void);
static void strhandle(void);
static void strparse(void);
static void strreset(void);

static void tprinter(char *, size_t);
static void tdumpsel(void);
static void tdumpline(int);
static void tdump(void);
static void tclearregion(int, int, int, int);
static void tcursor(int);
static void tdeletechar(int);
static void tdeleteline(int);
static void tinsertblank(int);
static void tinsertblankline(int);
static int tlinelen(Line);
static void tmoveto(int, int);
static void tmoveato(int, int);
static void tnewline(int);
static void tputtab(int);
static void tputc(Rune);
static void treset(void);
static void tscrollup(int, int);
static void tscrolldown(int, int);
static void tsetattr(const int *, int);
static void tsetchar(Rune, const Glyph *, int, int);
static void tsetdirt(int, int);
static void tsetscroll(int, int);
static void tswapscreen(void);
static void tsetmode(int, int, const int *, int);
static int twrite(const char *, int, int);
static void tfulldirt(void);
static void tcontrolcode(uchar );
static void tdectest(char );
static void tdefutf8(char);
static int32_t tdefcolor(const int *, int *, int);
static void tdeftran(char);
static void tstrsequence(uchar);

static void drawregion(int, int, int, int);

static void selnormalize(void);
static void selscroll(int, int);
static void selsnap(int *, int *, int);

static size_t utf8decode(const char *, Rune *, size_t);
static Rune utf8decodebyte(char, size_t *);
static char utf8encodebyte(Rune, size_t);
static size_t utf8validate(Rune *, size_t);

static char base64dec_getc(const char **);

static ssize_t xwrite(int, const char *, size_t);

static Term term;
static Selection sel;
static CSIEscape csiescseq;
static STREscape strescseq;
static TCursor c[2];
static int iofd = 1;
static int cmdfd;
static pid_t pid;
static pid_t shell_pgrp = 0;
static pid_t mouse_pgrp = 0;

static const uchar utfbyte[UTF_SIZ + 1] = {0x80,    0, 0xC0, 0xE0, 0xF0};
static const uchar utfmask[UTF_SIZ + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static const Rune utfmin[UTF_SIZ + 1] = {       0,    0,  0x80,  0x800,  0x10000};
static const Rune utfmax[UTF_SIZ + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

uint16_t diacritic_to_num(uint32_t code);

typedef struct
{
	Line *buf;
	int cap;
	int len;
	int head;
	uint64_t base;

	int max_width;
	int view_offset;
} Scrollback;

static Scrollback sb;

static int
sb_phys_index(int logical_idx)
{

	return (sb.head + logical_idx) % sb.cap;
}

static Line
lineclone(Line src)
{
	Line dst;

	if (!src)
		return NULL;

	dst = xmalloc(term.col * sizeof(Glyph));
	memcpy(dst, src, term.col * sizeof(Glyph));
	return dst;
}

static void
sb_init(int lines)
{
	int i;

	sb.buf  = xmalloc(sizeof(Line) * lines);
	sb.cap  = lines;
	sb.len  = 0;
	sb.head = 0;
	sb.base = 0;
	for (i = 0; i < sb.cap; i++)
		sb.buf[i] = NULL;

	sb.view_offset = 0;
	sb.max_width = 0;
}

static void
sb_push(Line line)
{
	Line copy;
	int tail;
	int width;

	if (sb.cap <= 0)
		return;

	copy = lineclone(line);

	if (sb.len < sb.cap) {
		tail = sb_phys_index(sb.len);
		sb.buf[tail] = copy;
		sb.len++;
	} else {

		free(sb.buf[sb.head]);
		sb.buf[sb.head] = copy;
		sb.head = (sb.head + 1) % sb.cap;
		sb.base++;
	}
	width = tlinelen(copy);

	if (width > sb.max_width)
		sb.max_width = width;
}

static Line
sb_get(int idx)
{

	if (idx < 0 || idx >= sb.len)
		return NULL;
	return sb.buf[sb_phys_index(idx)];
}

static void
sb_clear(void)
{
	int i;
	int p;

	if (!sb.buf)
		return;

	for (i = 0; i < sb.len; i++) {
		p = sb_phys_index(i);
		if (sb.buf[p]) {
			free(sb.buf[p]);
			sb.buf[p] = NULL;
		}
	}

	sb.len = 0;
	sb.head = 0;
	sb.base = 0;
	sb.view_offset = 0;
	sb.max_width = 0;
}



static uint64_t
sb_view_start(void)
{
	return sb.base + sb.len - sb.view_offset;
}

static void
sb_view_changed(void)
{
	if (!term.dirty || term.row <= 0)
		return;
	tfulldirt();
}

static void
selscrollback(int delta)
{
	if (delta == 0)
		return;

	if (sel.ob.x == -1 || sel.mode == SEL_EMPTY)
		return;

	if (sel.alt != IS_SET(MODE_ALTSCREEN))
		return;

	sel.nb.y += delta;
	sel.ne.y += delta;
	sel.ob.y += delta;
	sel.oe.y += delta;

	sb_view_changed();
}

static Line
emptyline(void)
{
	static Line empty;
	static int empty_cols;
	int i = 0;

	if (empty_cols != term.col) {
		free(empty);
		empty = xmalloc(term.col * sizeof(Glyph));
		empty_cols = term.col;
	}

	for (i = 0; i < term.col; i++) {
		empty[i] = term.c.attr;
		empty[i].u = ' ';
		empty[i].mode = 0;
		empty[i].link_id = 0;
	}
	return empty;
}

static Line
renderline(int y)
{
	int v = sb.len - sb.view_offset + y;

	if (v < 0)
		return emptyline();

	if (v < sb.len) {
		Line l = sb_get(v);
		return l ? l : emptyline();
	}

	v -= sb.len;
	if (v >= 0 && v < term.row)
		return term.line[v];

	return emptyline();
}

static void
sb_reset_on_clear(void)
{
	sb_clear();
	sb_view_changed();
	if (sel.ob.x != -1 && term.row > 0)
		selclear();
}

int
tisaltscreen(void)
{
	return IS_SET(MODE_ALTSCREEN);
}

ssize_t
xwrite(int fd, const char *s, size_t len)
{
	size_t aux = len;
	ssize_t r;

	while (len > 0) {
		r = write(fd, s, len);
		if (r < 0)
			return r;
		len -= r;
		s += r;
	}

	return aux;
}

void *
xmalloc(size_t len)
{
	void *p;

	if (!(p = malloc(len)))
		die("malloc: %s\n", strerror(errno));

	return p;
}

void *
xrealloc(void *p, size_t len)
{
	if ((p = realloc(p, len)) == NULL)
		die("realloc: %s\n", strerror(errno));

	return p;
}

char *
xstrdup(const char *s)
{
	char *p;

	if ((p = strdup(s)) == NULL)
		die("strdup: %s\n", strerror(errno));

	return p;
}

size_t
utf8decode(const char *c, Rune *u, size_t clen)
{
	size_t i, j, len, type;
	Rune udecoded;

	*u = UTF_INVALID;
	if (!clen)
		return 0;
	udecoded = utf8decodebyte(c[0], &len);
	if (!BETWEEN(len, 1, UTF_SIZ))
		return 1;
	for (i = 1, j = 1; i < clen && j < len; ++i, ++j) {
		udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
		if (type != 0)
			return j;
	}
	if (j < len)
		return 0;
	*u = udecoded;
	utf8validate(u, len);

	return len;
}

Rune
utf8decodebyte(char c, size_t *i)
{
	for (*i = 0; *i < LEN(utfmask); ++(*i))
		if (((uchar)c & utfmask[*i]) == utfbyte[*i])
			return (uchar)c & ~utfmask[*i];

	return 0;
}

size_t
utf8encode(Rune u, char *c)
{
	size_t len, i;

	len = utf8validate(&u, 0);
	if (len > UTF_SIZ)
		return 0;

	for (i = len - 1; i != 0; --i) {
		c[i] = utf8encodebyte(u, 0);
		u >>= 6;
	}
	c[0] = utf8encodebyte(u, len);

	return len;
}

char
utf8encodebyte(Rune u, size_t i)
{
	return utfbyte[i] | (u & ~utfmask[i]);
}

size_t
utf8validate(Rune *u, size_t i)
{
	if (!BETWEEN(*u, utfmin[i], utfmax[i]) || BETWEEN(*u, 0xD800, 0xDFFF))
		*u = UTF_INVALID;
	for (i = 1; *u > utfmax[i]; ++i)
		;

	return i;
}

char
base64dec_getc(const char **src)
{
	while (**src && !isprint((unsigned char)**src))
		(*src)++;
	return **src ? *((*src)++) : '=';
}

char *
base64dec(const char *src)
{
	size_t in_len = strlen(src);
	char *result, *dst;
	static const char base64_digits[256] = {
		[43] = 62, 0, 0, 0, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
		0, 0, 0, -1, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
		13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 0, 0, 0, 0,
		0, 0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
		40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51
	};

	if (in_len % 4)
		in_len += 4 - (in_len % 4);
	result = dst = xmalloc(in_len / 4 * 3 + 1);
	while (*src) {
		int a = base64_digits[(unsigned char) base64dec_getc(&src)];
		int b = base64_digits[(unsigned char) base64dec_getc(&src)];
		int c = base64_digits[(unsigned char) base64dec_getc(&src)];
		int d = base64_digits[(unsigned char) base64dec_getc(&src)];

		if (a == -1 || b == -1)
			break;

		*dst++ = (a << 2) | ((b & 0x30) >> 4);
		if (c == -1)
			break;
		*dst++ = ((b & 0x0f) << 4) | ((c & 0x3c) >> 2);
		if (d == -1)
			break;
		*dst++ = ((c & 0x03) << 6) | d;
	}
	*dst = '\0';
	return result;
}

void
selinit(void)
{
	sel.mode = SEL_IDLE;
	sel.snap = 0;
	sel.ob.x = -1;
}

static int
tlinelen(Line line)
{
	int i = term.col;
	if (line[i - 1].mode & ATTR_WRAP)
		return i;
	while (i > 0 && line[i - 1].u == ' ')
		--i;
	return i;
}

static int
tlinelen_render(int y)
{
	return tlinelen(renderline(y));
}

void
selstart(int col, int row, int snap)
{
	selclear();
	sel.mode = SEL_EMPTY;
	sel.type = SEL_REGULAR;
	sel.alt = IS_SET(MODE_ALTSCREEN);
	sel.snap = snap;
	sel.oe.x = sel.ob.x = col;
	sel.oe.y = sel.ob.y = row;
	selnormalize();

	if (sel.snap != 0)
		sel.mode = SEL_READY;
	tsetdirt(sel.nb.y, sel.ne.y);
}

void
selextend(int col, int row, int type, int done)
{
	int oldey, oldex, oldsby, oldsey, oldtype;

	if (sel.mode == SEL_IDLE)
		return;
	if (done && sel.mode == SEL_EMPTY) {
		selclear();
		return;
	}

	oldey = sel.oe.y;
	oldex = sel.oe.x;
	oldsby = sel.nb.y;
	oldsey = sel.ne.y;
	oldtype = sel.type;

	sel.oe.x = col;
	sel.oe.y = row;
	selnormalize();
	sel.type = type;

	if (oldey != sel.oe.y || oldex != sel.oe.x || oldtype != sel.type || sel.mode == SEL_EMPTY)
		tsetdirt(MIN(sel.nb.y, oldsby), MAX(sel.ne.y, oldsey));

	sel.mode = done ? SEL_IDLE : SEL_READY;
}

void
selnormalize(void)
{
	int i;

	if (sel.type == SEL_REGULAR && sel.ob.y != sel.oe.y) {
		sel.nb.x = sel.ob.y < sel.oe.y ? sel.ob.x : sel.oe.x;
		sel.ne.x = sel.ob.y < sel.oe.y ? sel.oe.x : sel.ob.x;
	} else {
		sel.nb.x = MIN(sel.ob.x, sel.oe.x);
		sel.ne.x = MAX(sel.ob.x, sel.oe.x);
	}
	sel.nb.y = MIN(sel.ob.y, sel.oe.y);
	sel.ne.y = MAX(sel.ob.y, sel.oe.y);

	selsnap(&sel.nb.x, &sel.nb.y, -1);
	selsnap(&sel.ne.x, &sel.ne.y, +1);

	if (sel.type == SEL_RECTANGULAR)
		return;
	i = tlinelen_render(sel.nb.y);
	if (i < sel.nb.x)
		sel.nb.x = i;
	if (tlinelen_render(sel.ne.y) <= sel.ne.x)
		sel.ne.x = term.col - 1;
}

int
selected(int x, int y)
{
	if (sel.mode == SEL_EMPTY || sel.ob.x == -1 ||
			sel.alt != IS_SET(MODE_ALTSCREEN))
		return 0;

	if (sel.type == SEL_RECTANGULAR)
		return BETWEEN(y, sel.nb.y, sel.ne.y)
		    && BETWEEN(x, sel.nb.x, sel.ne.x);

	return BETWEEN(y, sel.nb.y, sel.ne.y)
	    && (y != sel.nb.y || x >= sel.nb.x)
	    && (y != sel.ne.y || x <= sel.ne.x);
}

void
selsnap(int *x, int *y, int direction)
{
	int newx, newy, xt, yt;
	int delim, prevdelim;
	const Glyph *gp, *prevgp;
	Line line;

	switch (sel.snap) {
	case SNAP_WORD:

		prevgp = &renderline(*y)[*x];
		prevdelim = ISDELIM(prevgp->u);
		for (;;) {
			newx = *x + direction;
			newy = *y;
			if (!BETWEEN(newx, 0, term.col - 1)) {
				newy += direction;
				newx = (newx + term.col) % term.col;
				if (!BETWEEN(newy, 0, term.row - 1))
					break;

				if (direction > 0)
					yt = *y, xt = *x;
				else
					yt = newy, xt = newx;
				line = renderline(yt);
				if (!(line[xt].mode & ATTR_WRAP))
					break;
			}

			if (newx >= tlinelen_render(newy))
				break;

			gp = &renderline(newy)[newx];
			delim = ISDELIM(gp->u);
			if (!glyph_is_wide_dummy(gp) && (delim != prevdelim
					|| (delim && gp->u != prevgp->u)))
				break;

			*x = newx;
			*y = newy;
			prevgp = gp;
			prevdelim = delim;
		}
		break;
	case SNAP_LINE:

		*x = (direction < 0) ? 0 : term.col - 1;
		if (direction < 0) {
			for (; *y > 0; *y += direction) {
				if (!(renderline(*y-1)[term.col-1].mode
						& ATTR_WRAP)) {
					break;
				}
			}
		} else if (direction > 0) {
			for (; *y < term.row-1; *y += direction) {
				if (!(renderline(*y)[term.col-1].mode
						& ATTR_WRAP)) {
					break;
				}
			}
		}
		break;
	}
}

char *
getsel(void)
{
	char *str, *ptr;
	int y, bufsize, lastx, linelen, end_idx, insert_newline, is_wrapped;
	const Glyph *gp, *last;
	Line line;

	if (sel.ob.x == -1)
		return NULL;

	bufsize = (term.col+1) * (sel.ne.y-sel.nb.y+1) * UTF_SIZ;
	ptr = str = xmalloc(bufsize);

	for (y = sel.nb.y; y <= sel.ne.y; y++) {
		line = renderline(y);
		linelen = tlinelen_render(y);

		if (linelen == 0) {
			*ptr++ = '\n';
			continue;
		}

		if (sel.type == SEL_RECTANGULAR) {
			gp = &line[sel.nb.x];
			lastx = sel.ne.x;
		} else {
			gp = &line[sel.nb.y == y ? sel.nb.x : 0];
			lastx = (sel.ne.y == y) ? sel.ne.x : term.col-1;
		}
		end_idx = MIN(lastx, linelen-1);
		is_wrapped = (line[end_idx].mode & ATTR_WRAP) != 0;
		last = &line[end_idx];
		while (last >= gp && last->u == ' ') {
			--last;
		}

		for ( ; gp <= last; ++gp) {
			if (glyph_is_wide_dummy(gp))
				continue;

			if (glyph_is_image(gp)) {

				ptr += utf8encode(IMAGE_PLACEHOLDER, ptr);
				continue;
			}

			ptr += utf8encode(gp->u, ptr);
		}

		insert_newline = 0;
		if ((y < sel.ne.y || lastx >= linelen) &&
			(!is_wrapped || sel.type == SEL_RECTANGULAR)) {
			insert_newline = 1;
		}

		if (insert_newline)
			*ptr++ = '\n';
	}
	*ptr = 0;
	return str;
}

void
selclear(void)
{
	if (sel.ob.x == -1)
		return;
	sel.mode = SEL_IDLE;
	sel.ob.x = -1;
	tsetdirt(sel.nb.y, sel.ne.y);
}

void
die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

void
execsh(char *cmd, char **args)
{
	char *sh, *prog, *arg;
	const struct passwd *pw;

	errno = 0;
	if ((pw = getpwuid(getuid())) == NULL) {
		if (errno)
			die("getpwuid: %s\n", strerror(errno));
		else
			die("who are you?\n");
	}

	if ((sh = getenv("SHELL")) == NULL)
		sh = (pw->pw_shell[0]) ? pw->pw_shell : cmd;

	if (args) {
		prog = args[0];
		arg = NULL;
	} else if (scroll) {
		prog = scroll;
		arg = utmp ? utmp : sh;
	} else if (utmp) {
		prog = utmp;
		arg = NULL;
	} else {
		prog = sh;
		arg = NULL;
	}
	DEFAULT(args, ((char *[]) {prog, arg, NULL}));

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("SHELL", sh, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("TERM", termname, 1);
	setenv("COLORTERM", "truecolor", 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	execvp(prog, args);
	_exit(1);
}

void
sigchld(int a)
{
	int stat;
	pid_t p;

	if ((p = waitpid(-1, &stat, WNOHANG)) < 0)
		die("waiting for pid %hd failed: %s\n", pid, strerror(errno));

	if (pid != p) {

		signal(SIGCHLD, sigchld);
		return;
	}

	if (WIFEXITED(stat) && WEXITSTATUS(stat))
		die("child exited with status %d\n", WEXITSTATUS(stat));
	else if (WIFSIGNALED(stat))
		die("child terminated due to signal %d\n", WTERMSIG(stat));
	_exit(0);
}

void
stty(char **args)
{
	char cmd[_POSIX_ARG_MAX], **p, *q, *s;
	size_t n, siz;

	if ((n = strlen(stty_args)) > sizeof(cmd)-1)
		die("incorrect stty parameters\n");
	memcpy(cmd, stty_args, n);
	q = cmd + n;
	siz = sizeof(cmd) - n;
	for (p = args; p && (s = *p); ++p) {
		if ((n = strlen(s)) > siz-1)
			die("stty parameter length too long\n");
		*q++ = ' ';
		memcpy(q, s, n);
		q += n;
		siz -= n + 1;
	}
	*q = '\0';
	if (system(cmd) != 0)
		perror("Couldn't call stty");
}

int
ttynew(const char *line, char *cmd, const char *out, char **args)
{
	int m, s;

	if (out) {
		term.mode |= MODE_PRINT;
		iofd = (!strcmp(out, "-")) ?
			  1 : open(out, O_WRONLY | O_CREAT, 0666);
		if (iofd < 0) {
			fprintf(stderr, "Error opening %s:%s\n",
				out, strerror(errno));
		}
	}

	if (line) {
		if ((cmdfd = open(line, O_RDWR)) < 0)
			die("open line '%s' failed: %s\n",
			    line, strerror(errno));
		dup2(cmdfd, 0);
		stty(args);
		return cmdfd;
	}

	struct winsize ws = {
		.ws_row = term.row > 0 ? term.row : 24,
		.ws_col = term.col > 0 ? term.col : 80,
		.ws_xpixel = term.pixw,
		.ws_ypixel = term.pixh,
	};

	if (openpty(&m, &s, NULL, NULL, &ws) < 0)
		die("openpty failed: %s\n", strerror(errno));

	switch (pid = fork()) {
	case -1:
		die("fork failed: %s\n", strerror(errno));
		break;
	case 0:
		close(iofd);
		close(m);
		setsid();
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		if (ioctl(s, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
		if (s > 2)
			close(s);
#ifdef __OpenBSD__
		if (pledge("stdio getpw proc exec", NULL) == -1)
			die("pledge\n");
#endif
		execsh(cmd, args);
		break;
	default:
#ifdef __OpenBSD__
		if (pledge("stdio rpath tty proc", NULL) == -1)
			die("pledge\n");
#endif
		close(s);
		cmdfd = m;
		signal(SIGCHLD, sigchld);
		shell_pgrp = pid;
		break;
	}
	return cmdfd;
}

static void
mouse_reset(void)
{
	xsetmode(0, MODE_MOUSE | MODE_MOUSESGR | MODE_MOUSEBTN | MODE_MOUSEMOTION | MODE_MOUSEX10 | MODE_MOUSEMANY | MODE_FOCUS);
	xsetpointermotion(0);
	xsetmode(0, MODE_HIDE);
	mouse_pgrp = 0;
}

int
st_mouse_active(void)
{
	if (!xismode(MODE_MOUSE))
		return 0;

	pid_t fg = tcgetpgrp(cmdfd);
	if ((fg > 0 && mouse_pgrp > 0 && fg != mouse_pgrp) ||
	    (fg > 0 && shell_pgrp > 0 && fg == shell_pgrp && mouse_pgrp != shell_pgrp) ||
	    (mouse_pgrp > 0 && kill(mouse_pgrp, 0) < 0 && errno == ESRCH)) {
		mouse_reset();
		return 0;
	}
	return 1;
}

size_t
ttyread(void)
{
	static char buf[BUFSIZ];
	static int buflen = 0;
	static int already_processing = 0;
	int ret, written = 0;

	if (mouse_pgrp > 0) {
		pid_t fg = tcgetpgrp(cmdfd);
		if ((fg > 0 && fg != mouse_pgrp) ||
		    (fg > 0 && shell_pgrp > 0 && fg == shell_pgrp && mouse_pgrp != shell_pgrp) ||
		    (kill(mouse_pgrp, 0) < 0 && errno == ESRCH)) {
			mouse_reset();
		}
	}

	if (buflen >= LEN(buf))
		return 0;

	ret = read(cmdfd, buf+buflen, LEN(buf)-buflen);

	switch (ret) {
	case 0:
		exit(0);
	case -1:
		die("couldn't read from shell: %s\n", strerror(errno));
	default:
		buflen += ret;
		if (already_processing) {

			return ret;
		}
		already_processing = 1;
		while (1) {
			int buflen_before_processing = buflen;
			written += twrite(buf + written, buflen - written, 0);

			if (buflen_before_processing == buflen)
				break;
		}
		already_processing = 0;
		buflen -= written;

		if (buflen > 0)
			memmove(buf, buf + written, buflen);
		return ret;
	}
}

void
ttywrite(const char *s, size_t n, int may_echo)
{
	const char *next;

	if (sb.view_offset > 0) {
		selclear();
		sb.view_offset = 0;
		sb_view_changed();
	}

	if (may_echo && IS_SET(MODE_ECHO))
		twrite(s, n, 1);

	if (!IS_SET(MODE_CRLF)) {
		ttywriteraw(s, n);
		return;
	}

	while (n > 0) {
		if (*s == '\r') {
			next = s + 1;
			ttywriteraw("\r\n", 2);
		} else {
			next = memchr(s, '\r', n);
			DEFAULT(next, s + n);
			ttywriteraw(s, next - s);
		}
		n -= next - s;
		s = next;
	}
}

void
ttywriteraw(const char *s, size_t n)
{
	fd_set wfd, rfd;
	ssize_t r;
	size_t lim = 256;
	int retries_left = 100;

	while (n > 0) {
		if (retries_left-- <= 0)
			goto too_many_retries;

		FD_ZERO(&wfd);
		FD_ZERO(&rfd);
		FD_SET(cmdfd, &wfd);
		FD_SET(cmdfd, &rfd);

		if (pselect(cmdfd+1, &rfd, &wfd, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			die("select failed: %s\n", strerror(errno));
		}
		if (FD_ISSET(cmdfd, &wfd)) {

			if ((r = write(cmdfd, s, (n < lim)? n : lim)) < 0)
				goto write_error;
			if (r < n) {

				if (n < lim)
					lim = ttyread();
				n -= r;
				s += r;
			} else {

				break;
			}
		}
		if (FD_ISSET(cmdfd, &rfd))
			lim = ttyread();
	}
	return;

write_error:
	die("write error on tty: %s\n", strerror(errno));
too_many_retries:
	fprintf(stderr, "Could not write %zu bytes to tty\n", n);
}

void
ttyresize(int tw, int th)
{
	struct winsize w;

	if (term.row == term.last_ws_row && term.col == term.last_ws_col &&
	    tw == term.pixw && th == term.pixh)
		return;

	term.last_ws_row = term.row;
	term.last_ws_col = term.col;
	term.pixw = tw;
	term.pixh = th;

	w.ws_row = term.row;
	w.ws_col = term.col;
	w.ws_xpixel = tw;
	w.ws_ypixel = th;
	if (cmdfd > 0 && ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

void
ttyhangup(void)
{

	kill(pid, SIGHUP);
}

int
tattrset(int attr)
{
	int i, j;

	for (i = 0; i < term.row-1; i++) {
		for (j = 0; j < term.col-1; j++) {
			if (term.line[i][j].mode & attr)
				return 1;
		}
	}

	return 0;
}

void
tsetdirt(int top, int bot)
{
	int i;

	if (term.row < 1)
		return;
	LIMIT(top, 0, term.row-1);
	LIMIT(bot, 0, term.row-1);

	for (i = top; i <= bot; i++)
		term.dirty[i] = 1;
}

void
tsetdirtattr(int attr)
{
	int i, j;

	for (i = 0; i < term.row-1; i++) {
		for (j = 0; j < term.col-1; j++) {
			if (term.line[i][j].mode & attr) {
				tsetdirt(i, i);
				break;
			}
		}
	}
}

void
tfulldirt(void)
{
	tsetdirt(0, term.row-1);
}

void
tcursor(int mode)
{
	int alt = IS_SET(MODE_ALTSCREEN);

	if (mode == CURSOR_SAVE) {
		c[alt] = term.c;
	} else if (mode == CURSOR_LOAD) {
		term.c = c[alt];
		current_link_id = 0;
		tmoveto(c[alt].x, c[alt].y);
	}
}

void
treset(void)
{
	uint i;

	current_link_id = 0;
	term.c = (TCursor){{
		.mode = ATTR_NULL,
		.fg = defaultfg,
		.bg = defaultbg,
		.decor = DECOR_DEFAULT_COLOR
	}, .x = 0, .y = 0, .state = CURSOR_DEFAULT};

	memset(term.tabs, 0, term.col * sizeof(*term.tabs));
	for (i = tabspaces; i < term.col; i += tabspaces)
		term.tabs[i] = 1;
	term.top = 0;
	term.bot = term.row - 1;
	term.mode = MODE_WRAP|MODE_UTF8;
	memset(term.trantbl, CS_USA, sizeof(term.trantbl));
	term.charset = 0;
	term.prompt_y = -1;
	term.mode_2031 = 0;
	term.last_ws_row = -1;
	term.last_ws_col = -1;

	for (i = 0; i < 2; i++) {
		tmoveto(0, 0);
		tcursor(CURSOR_SAVE);
		if (term.col > 0 && term.row > 0 && term.line > 0)
			tclearregion(0, 0, term.col-1, term.row-1);
		tswapscreen();
	}
	sb_clear();
	if (sel.ob.x != -1 && term.row > 0)
		selclear();
	if (IS_SET(MODE_ALTSCREEN))
		tswapscreen();
	xsetmode(0, MODE_MOUSE | MODE_MOUSESGR | MODE_BRCKTPASTE | MODE_FOCUS | MODE_APPCURSOR | MODE_APPKEYPAD);
	xsetpointermotion(0);
	mouse_pgrp = 0;
	gr_reset();
}

static unsigned int primary_win_mode = 0;

void
tnew(int col, int row)
{
	term = (Term){.c = {.attr = {.fg = defaultfg,
				     .bg = defaultbg,
				     .decor = DECOR_DEFAULT_COLOR}}};
	sb_init(g_st_config.histsize > 0 ? g_st_config.histsize : scrollback_lines);
	tresize(col, row);
	treset();
}

void
tswapscreen(void)
{
	Line *tmp = term.line;

	term.line = term.alt;
	term.alt = tmp;
	term.mode ^= MODE_ALTSCREEN;
	tfulldirt();
}

void
tscrolldown(int orig, int n)
{
	int i;
	Line temp;

	LIMIT(n, 0, term.bot-orig+1);

	tsetdirt(orig, term.bot-n);
	tclearregion(0, term.bot-n+1, term.col-1, term.bot);

	for (i = term.bot; i >= orig+n; i--) {
		temp = term.line[i];
		term.line[i] = term.line[i-n];
		term.line[i-n] = temp;
	}

	selscroll(orig, n);
}

void
tscrollup(int orig, int n)
{
	int i;
	uint64_t newstart;
	uint64_t oldstart;

	int attop;
	Line temp;

	oldstart = sb_view_start();
	LIMIT(n, 0, term.bot-orig+1);

	if (!IS_SET(MODE_ALTSCREEN) && orig == term.top) {

		attop = (sb.len != 0 && sb.view_offset == sb.len);

		if (sb.view_offset > 0 && !attop)
			sb.view_offset += n;

		for (i = 0; i < n; i++)
			sb_push(term.line[orig + i]);

		if (attop)
			sb.view_offset = sb.len;

		else if (sb.view_offset > sb.len)
			sb.view_offset = sb.len;

		if (term.prompt_y >= 0)
			term.prompt_y -= n;
	}

	newstart = sb_view_start();
	if (sb.view_offset > 0)
		selscrollback(oldstart - newstart);

	tclearregion(0, orig, term.col-1, orig+n-1);
	tsetdirt(orig+n, term.bot);

	for (i = orig; i <= term.bot-n; i++) {
		temp = term.line[i];
		term.line[i] = term.line[i+n];
		term.line[i+n] = temp;
	}

	selscroll(orig, -n);
}

void
selscroll(int orig, int n)
{
	if (sb.view_offset != 0)
		return;
	if (sel.ob.x == -1 || sel.alt != IS_SET(MODE_ALTSCREEN))
		return;

	if (BETWEEN(sel.nb.y, orig, term.bot) != BETWEEN(sel.ne.y, orig, term.bot)) {
		selclear();
	} else if (BETWEEN(sel.nb.y, orig, term.bot)) {
		sel.ob.y += n;
		sel.oe.y += n;
		selnormalize();
	}
}

void
tnewline(int first_col)
{
	int y = term.c.y;

	if (y == term.bot) {
		tscrollup(term.top, 1);
	} else {
		y++;
	}
	tmoveto(first_col ? 0 : term.c.x, y);
}

void
readcolonargs(char **p, int cursor, int params[][CAR_PER_ARG])
{
	int i;
	for (i = 0; i < CAR_PER_ARG; i++)
		params[cursor][i] = -1;

	if (**p != ':')
		return;

	char *np = NULL;
	i = 0;

	while (**p == ':' && i < CAR_PER_ARG) {
		(*p)++;
		if (**p == ':' || **p == ';' || **p == '\0' || (**p >= 'A' && **p <= 'Z') || (**p >= 'a' && **p <= 'z')) {
			params[cursor][i++] = -1;
			continue;
		}
		params[cursor][i++] = strtol(*p, &np, 10);
		*p = np;
	}
}

void
csiparse(void)
{
	char *p = csiescseq.buf, *np;
	long int v;

	csiescseq.narg = 0;
	memset(csiescseq.carg, -1, sizeof(csiescseq.carg));
	if (*p == '?' || *p == '>' || *p == '<' || *p == '=') {
		csiescseq.priv = *p++;
	}

	csiescseq.buf[csiescseq.len] = '\0';
	while (p < csiescseq.buf+csiescseq.len) {
		np = NULL;
		v = strtol(p, &np, 10);
		if (np == p)
			v = 0;
		if (v == LONG_MAX || v == LONG_MIN)
			v = -1;
		csiescseq.arg[csiescseq.narg++] = v;
		p = np;
		readcolonargs(&p, csiescseq.narg-1, csiescseq.carg);
		if (*p != ';' || csiescseq.narg == ESC_ARG_SIZ)
			break;
		p++;
	}
	csiescseq.mode[0] = *p++;
	csiescseq.mode[1] = (p < csiescseq.buf+csiescseq.len) ? *p : '\0';
}

void
tmoveato(int x, int y)
{
	tmoveto(x, y + ((term.c.state & CURSOR_ORIGIN) ? term.top: 0));
}

void
tmoveto(int x, int y)
{
	int miny, maxy;

	if (term.c.state & CURSOR_ORIGIN) {
		miny = term.top;
		maxy = term.bot;
	} else {
		miny = 0;
		maxy = term.row - 1;
	}
	term.c.state &= ~CURSOR_WRAPNEXT;
	term.c.x = LIMIT(x, 0, term.col-1);
	term.c.y = LIMIT(y, miny, maxy);
}

void
tsetchar(Rune u, const Glyph *attr, int x, int y)
{
	static const char *vt100_0[62] = {
		"↑", "↓", "→", "←", "█", "▚", "☃",
		0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, " ",
		"◆", "▒", "␉", "␌", "␍", "␊", "°", "±",
		"␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺",
		"⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬",
		"│", "≤", "≥", "π", "≠", "£", "·",
	};

	if (term.trantbl[term.charset] == CS_GRAPHIC0 &&
	   BETWEEN(u, 0x41, 0x7e) && vt100_0[u - 0x41])
		utf8decode(vt100_0[u - 0x41], &u, UTF_SIZ);

	if (glyph_is_image(&term.line[y][x]) && u == ' ' && tgetisclassicplaceholder(&term.line[y][x])) {
		term.line[y][x].bg = attr->bg;
		term.dirty[y] = 1;
		return;
	}

	if (term.line[y][x].mode & ATTR_WIDE) {
		if (x+1 < term.col) {
			term.line[y][x+1].u = ' ';
			term.line[y][x+1].mode &= ~ATTR_WDUMMY;
		}
	} else if (glyph_is_wide_dummy(&term.line[y][x])) {
		term.line[y][x-1].u = ' ';
		term.line[y][x-1].mode &= ~ATTR_WIDE;
	}

	term.dirty[y] = 1;
	term.line[y][x] = *attr;
	term.line[y][x].u = u;

	if (u == IMAGE_PLACEHOLDER_CHAR || u == IMAGE_PLACEHOLDER_CHAR_OLD) {
		term.line[y][x].u = 0;
		term.line[y][x].mode |= ATTR_IMAGE;
	}

	if (isboxdraw(u))
		term.line[y][x].mode |= ATTR_BOXDRAW;

	term.line[y][x].link_id = current_link_id;
	if (current_link_id > 0)
		term.line[y][x].mode |= ATTR_HYPERLINK;
}

void
tclearregion(int x1, int y1, int x2, int y2)
{
	int x, y, temp;
	Glyph *gp;

	if (x1 > x2)
		temp = x1, x1 = x2, x2 = temp;
	if (y1 > y2)
		temp = y1, y1 = y2, y2 = temp;

	LIMIT(x1, 0, term.col-1);
	LIMIT(x2, 0, term.col-1);
	LIMIT(y1, 0, term.row-1);
	LIMIT(y2, 0, term.row-1);

	for (y = y1; y <= y2; y++) {
		term.dirty[y] = 1;
		for (x = x1; x <= x2; x++) {
			gp = &term.line[y][x];
			if (selected(x, y))
				selclear();
			gp->fg = term.c.attr.fg;
			gp->bg = term.c.attr.bg;
			gp->decor = term.c.attr.decor;
			gp->mode = 0;
			gp->u = ' ';
			gp->link_id = 0;
		}
	}
}

void tcreateimgplaceholder(uint32_t image_id, uint32_t placement_id, int cols,
			   int rows, char do_not_move_cursor,
			   Glyph *text_underneath) {
	for (int row = 0; row < rows; ++row) {
		int y = term.c.y;
		term.dirty[y] = 1;
		for (int col = 0; col < cols; ++col) {
			int x = term.c.x + col;
			if (x >= term.col)
				break;
			Glyph *gp = &term.line[y][x];
			if (selected(x, y))
				selclear();
			if (text_underneath) {
				Glyph *to_save = gp;

				if (gp->mode & ATTR_IMAGE &&
				    tgetisclassicplaceholder(gp)) {
					Glyph *under =
						gr_get_glyph_underneath_image(
							tgetimgid(gp),
							tgetimgplacementid(gp),
							tgetimgcol(gp),
							tgetimgrow(gp));
					if (under)
						to_save = under;
				}
				text_underneath[cols * row + col] = *to_save;
			}
			gp->mode = ATTR_IMAGE;
			gp->u = 0;
			tsetimgrow(gp, row + 1);
			tsetimgcol(gp, col + 1);
			tsetimgid(gp, image_id);
			tsetimgplacementid(gp, placement_id);
			tsetimgdiacriticcount(gp, 3);
			tsetisclassicplaceholder(gp, 1);
		}

		if (do_not_move_cursor && y == term.row - 1)
			break;

		if (row != rows - 1)
			tnewline(0);
	}
	if (do_not_move_cursor) {

		tmoveto(term.c.x, term.c.y - rows + 1);
	} else {

		if (term.c.x + cols >= term.col)
			tnewline(1);
		else
			tmoveto(term.c.x + cols, term.c.y);
	}
}

void gr_for_each_image_cell(int (*callback)(void *data, Glyph *gp),
			    void *data) {
	for (int row = 0; row < term.row; ++row) {
		for (int col = 0; col < term.col; ++col) {
			Glyph *gp = &term.line[row][col];
			if (glyph_is_image(gp)) {
				if (callback(data, gp))
					term.dirty[row] = 1;
			}
		}
	}
}

void gr_schedule_image_redraw_by_id(uint32_t image_id) {
	for (int row = 0; row < term.row; ++row) {
		if (term.dirty[row])
			continue;
		for (int col = 0; col < term.col; ++col) {
			Glyph *gp = &term.line[row][col];
			if (glyph_is_image(gp)) {
				uint32_t cell_image_id = tgetimgid(gp);
				if (cell_image_id == image_id) {
					term.dirty[row] = 1;
					break;
				}
			}
		}
	}
}

void
tdeletechar(int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);

	dst = term.c.x;
	src = term.c.x + n;
	size = term.col - src;
	line = term.line[term.c.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(term.col-n, term.c.y, term.col-1, term.c.y);
}

void
tinsertblank(int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);

	dst = term.c.x + n;
	src = term.c.x;
	size = term.col - dst;
	line = term.line[term.c.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(src, term.c.y, dst - 1, term.c.y);
}

void
tinsertblankline(int n)
{
	if (BETWEEN(term.c.y, term.top, term.bot))
		tscrolldown(term.c.y, n);
}

void
tdeleteline(int n)
{
	if (BETWEEN(term.c.y, term.top, term.bot))
		tscrollup(term.c.y, n);
}

int32_t
tdefcolor(const int *attr, int *npar, int l)
{
	int32_t idx = -1;
	uint r, g, b;
	int cur = *npar;

	if (cur >= 0 && cur < ESC_ARG_SIZ && csiescseq.carg[cur][0] != -1) {
		switch (csiescseq.carg[cur][0]) {
		case 2:
			if (csiescseq.carg[cur][1] == -1) {
				r = csiescseq.carg[cur][2];
				g = csiescseq.carg[cur][3];
				b = csiescseq.carg[cur][4];
			} else {
				r = csiescseq.carg[cur][1];
				g = csiescseq.carg[cur][2];
				b = csiescseq.carg[cur][3];
			}
			if (BETWEEN(r, 0, 255) && BETWEEN(g, 0, 255) && BETWEEN(b, 0, 255))
				idx = TRUECOLOR(r, g, b);
			else
				fprintf(stderr, "erresc: bad rgb color (%u,%u,%u)\n", r, g, b);
			return idx;
		case 5:
			if (BETWEEN(csiescseq.carg[cur][1], 0, 255))
				idx = csiescseq.carg[cur][1];
			else
				fprintf(stderr, "erresc: bad fgcolor %d\n", csiescseq.carg[cur][1]);
			return idx;
		default:
			fprintf(stderr, "erresc(38): gfx attr %d unknown\n", csiescseq.carg[cur][0]);
			return -1;
		}
	}

	if (*npar + 1 >= l) {
		fprintf(stderr, "erresc(38): Incorrect number of parameters (%d)\n", *npar);
		return -1;
	}

	switch (attr[*npar + 1]) {
	case 2:
		if (*npar + 4 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		r = attr[*npar + 2];
		g = attr[*npar + 3];
		b = attr[*npar + 4];
		*npar += 4;
		if (!BETWEEN(r, 0, 255) || !BETWEEN(g, 0, 255) || !BETWEEN(b, 0, 255))
			fprintf(stderr, "erresc: bad rgb color (%u,%u,%u)\n",
				r, g, b);
		else
			idx = TRUECOLOR(r, g, b);
		break;
	case 5:
		if (*npar + 2 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		*npar += 2;
		if (!BETWEEN(attr[*npar], 0, 255))
			fprintf(stderr, "erresc: bad fgcolor %d\n", attr[*npar]);
		else
			idx = attr[*npar];
		break;
	case 0:
	case 1:
	case 3:
	case 4:
	default:
		fprintf(stderr,
		        "erresc(38): gfx attr %d unknown\n", attr[*npar + 1]);
		break;
	}

	return idx;
}

void
tsetattr(const int *attr, int l)
{
	int i;
	int32_t idx;

	for (i = 0; i < l; i++) {
		switch (attr[i]) {
		case 0:
			term.c.attr.mode &= ~(
				ATTR_BOLD       |
				ATTR_FAINT      |
				ATTR_ITALIC     |
				ATTR_UNDERLINE  |
				ATTR_BLINK      |
				ATTR_REVERSE    |
				ATTR_INVISIBLE  |
				ATTR_STRUCK     );
			term.c.attr.fg = defaultfg;
			term.c.attr.bg = defaultbg;
			term.c.attr.decor = DECOR_DEFAULT_COLOR;
			break;
		case 1:
			term.c.attr.mode |= ATTR_BOLD;
			break;
		case 2:
			term.c.attr.mode |= ATTR_FAINT;
			break;
		case 3:
			term.c.attr.mode |= ATTR_ITALIC;
			break;
		case 4:
			if (i < ESC_ARG_SIZ && csiescseq.carg[i][0] != -1) {
				idx = csiescseq.carg[i][0];
				if (BETWEEN(idx, 1, 5)) {
					term.c.attr.mode |= ATTR_UNDERLINE;
					tsetdecorstyle(&term.c.attr, idx);
				} else if (idx == 0) {
					term.c.attr.mode &= ~ATTR_UNDERLINE;
					tsetdecorstyle(&term.c.attr, 0);
				}
			} else {
				term.c.attr.mode |= ATTR_UNDERLINE;
				tsetdecorstyle(&term.c.attr, UNDERLINE_STRAIGHT);
			}
			break;
		case 5:

		case 6:
			term.c.attr.mode |= ATTR_BLINK;
			break;
		case 7:
			term.c.attr.mode |= ATTR_REVERSE;
			break;
		case 8:
			term.c.attr.mode |= ATTR_INVISIBLE;
			break;
		case 9:
			term.c.attr.mode |= ATTR_STRUCK;
			break;
		case 22:
			term.c.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT);
			break;
		case 23:
			term.c.attr.mode &= ~ATTR_ITALIC;
			break;
		case 24:
			term.c.attr.mode &= ~ATTR_UNDERLINE;
			tsetdecorstyle(&term.c.attr, 0);
			break;
		case 25:
			term.c.attr.mode &= ~ATTR_BLINK;
			break;
		case 27:
			term.c.attr.mode &= ~ATTR_REVERSE;
			break;
		case 28:
			term.c.attr.mode &= ~ATTR_INVISIBLE;
			break;
		case 29:
			term.c.attr.mode &= ~ATTR_STRUCK;
			break;
		case 38:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.fg = idx;
			break;
		case 39:
			term.c.attr.fg = defaultfg;
			break;
		case 48:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.bg = idx;
			break;
		case 49:
			term.c.attr.bg = defaultbg;
			break;
		case 58:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				tsetdecorcolor(&term.c.attr, idx);
			break;
		case 59:
			tsetdecorcolor(&term.c.attr, DECOR_DEFAULT_COLOR);
			break;
		default:
			if (BETWEEN(attr[i], 30, 37)) {
				term.c.attr.fg = attr[i] - 30;
			} else if (BETWEEN(attr[i], 40, 47)) {
				term.c.attr.bg = attr[i] - 40;
			} else if (BETWEEN(attr[i], 90, 97)) {
				term.c.attr.fg = attr[i] - 90 + 8;
			} else if (BETWEEN(attr[i], 100, 107)) {
				term.c.attr.bg = attr[i] - 100 + 8;
			} else {
				fprintf(stderr,
					"erresc(default): gfx attr %d unknown\n",
					attr[i]);
				csidump();
			}
			break;
		}
	}
}

void
tsetscroll(int t, int b)
{
	int temp;

	LIMIT(t, 0, term.row-1);
	LIMIT(b, 0, term.row-1);
	if (t > b) {
		temp = t;
		t = b;
		b = temp;
	}
	term.top = t;
	term.bot = b;
}

void
tsetmode(int priv, int set, const int *args, int narg)
{
	int alt; const int *lim;

	for (lim = args + narg; args < lim; ++args) {
		if (priv) {
			switch (*args) {
			case 1:
				xsetmode(set, MODE_APPCURSOR);
				break;
			case 5:
				xsetmode(set, MODE_REVERSE);
				break;
			case 6:
				MODBIT(term.c.state, set, CURSOR_ORIGIN);
				tmoveato(0, 0);
				break;
			case 7:
				MODBIT(term.mode, set, MODE_WRAP);
				break;
			case 0:
			case 2:
			case 3:
			case 4:
			case 8:
			case 18:
			case 19:
			case 42:
			case 12:
				break;
			case 25:
				xsetmode(!set, MODE_HIDE);
				break;
			case 9:
				xsetpointermotion(0);
				xsetmode(0, MODE_MOUSE);
				xsetmode(set, MODE_MOUSEX10);
				mouse_pgrp = set ? tcgetpgrp(cmdfd) : 0;
				break;
			case 1000:
				xsetpointermotion(0);
				xsetmode(0, MODE_MOUSE);
				xsetmode(set, MODE_MOUSEBTN);
				mouse_pgrp = set ? tcgetpgrp(cmdfd) : 0;
				break;
			case 1002:
				xsetpointermotion(0);
				xsetmode(0, MODE_MOUSE);
				xsetmode(set, MODE_MOUSEMOTION);
				mouse_pgrp = set ? tcgetpgrp(cmdfd) : 0;
				break;
			case 1003:
				xsetpointermotion(set);
				xsetmode(0, MODE_MOUSE);
				xsetmode(set, MODE_MOUSEMANY);
				mouse_pgrp = set ? tcgetpgrp(cmdfd) : 0;
				break;
			case 1004:
				xsetmode(set, MODE_FOCUS);
				break;
			case 1006:
				xsetmode(set, MODE_MOUSESGR);
				if (set && (xgetmode() & MODE_MOUSE) && !mouse_pgrp)
					mouse_pgrp = tcgetpgrp(cmdfd);
				break;
			case 1034:
				xsetmode(set, MODE_8BIT);
				break;
			case 1049:
				if (!allowaltscreen)
					break;
				tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);

			case 47:
			case 1047:
				if (!allowaltscreen)
					break;
				alt = IS_SET(MODE_ALTSCREEN);
				if (alt) {
					tclearregion(0, 0, term.col-1,
							term.row-1);
				}
				if (set ^ alt) {
					if (set) {
						primary_win_mode = xgetmode() & (MODE_MOUSE | MODE_MOUSESGR | MODE_FOCUS | MODE_APPCURSOR | MODE_APPKEYPAD);
					}
					tswapscreen();
					if (!set) {
						xsetmode(0, MODE_MOUSE | MODE_MOUSESGR | MODE_FOCUS | MODE_APPCURSOR | MODE_APPKEYPAD);
						xsetpointermotion(0);
						mouse_pgrp = 0;
						xsetmode(1, primary_win_mode);
						primary_win_mode = 0;
					}
				}
				if (*args != 1049)
					break;

			case 1048:
				tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
				break;
			case 2004:
				xsetmode(set, MODE_BRCKTPASTE);
				break;
			case 2026:
				xsetmode(set, MODE_SYNC);
				break;
			case 2027:
				break;
			case 2031:
				term.mode_2031 = set;
				if (set)
					report_color_scheme();
				break;
			case 5522:
				break;

			case 7727:
				/* TODO: implement (not widely used yet) */
				break;

			case 2017:
				/* kitty keyboard protocol - stub, report as supported but no enhanced encoding yet */
				/* FIXME: implement kitty_keyboard */
				break;

			case 1001:

			case 1005:

			case 1015:

				break;
			default:
				fprintf(stderr,
					"erresc: unknown private set/reset mode %d\n",
					*args);
				break;
			}
		} else {
			switch (*args) {
			case 0:
				break;
			case 2:
				xsetmode(set, MODE_KBDLOCK);
				break;
			case 4:
				MODBIT(term.mode, set, MODE_INSERT);
				break;
			case 12:
				MODBIT(term.mode, !set, MODE_ECHO);
				break;
			case 20:
				MODBIT(term.mode, set, MODE_CRLF);
				break;
			default:
				fprintf(stderr,
					"erresc: unknown set/reset mode %d\n",
					*args);
				break;
			}
		}
	}
}

static void
report_color_scheme(void)
{
	unsigned char r = 0, g = 0, b = 0;
	int scheme = 1;
	char buf[32];
	int len;

	if (!xgetcolor(defaultbg, &r, &g, &b)) {
		int lum = (r * 299 + g * 587 + b * 114) / 1000;
		scheme = (lum > 128) ? 2 : 1;
	}
	len = snprintf(buf, sizeof(buf), "\033[?997;%dn", scheme);
	ttywrite(buf, len, 0);
}

void
csihandle(void)
{
	char buf[40];
	int len, i;

	switch (csiescseq.mode[0]) {
	default:
	unknown:
		fprintf(stderr, "erresc: unknown csi ");
		csidump();

		break;
	case '!':
		switch (csiescseq.mode[1]) {
		case 'p':
			xsetmode(0, MODE_MOUSE | MODE_MOUSESGR | MODE_FOCUS | MODE_APPCURSOR | MODE_APPKEYPAD);
			xsetpointermotion(0);
			mouse_pgrp = 0;
			primary_win_mode = 0;
			term.c.attr.mode = ATTR_NULL;
			term.c.attr.fg = defaultfg;
			term.c.attr.bg = defaultbg;
			term.c.attr.decor = DECOR_DEFAULT_COLOR;
			term.c.state &= ~CURSOR_ORIGIN;
			term.mode = MODE_WRAP | MODE_UTF8;
			term.top = 0;
			term.bot = term.row - 1;
			tfulldirt();
			break;
		default:
			goto unknown;
		}
		break;
	case '@':
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblank(csiescseq.arg[0]);
		break;
	case 'A':
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y-csiescseq.arg[0]);
		break;
	case 'B':
	case 'e':
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y+csiescseq.arg[0]);
		break;
	case 'i':
		switch (csiescseq.arg[0]) {
		case 0:
			tdump();
			break;
		case 1:
			tdumpline(term.c.y);
			break;
		case 2:
			tdumpsel();
			break;
		case 4:
			term.mode &= ~MODE_PRINT;
			break;
		case 5:
			term.mode |= MODE_PRINT;
			break;
		}
		break;
	case 'c':
		if (csiescseq.priv == '>' || csiescseq.priv == '=')
			break;
		if (csiescseq.arg[0] == 0)
			ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 'b':
		LIMIT(csiescseq.arg[0], 1, 65535);
		if (term.lastc)
			while (csiescseq.arg[0]-- > 0)
				tputc(term.lastc);
		break;
	case 'C':
	case 'a':
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x+csiescseq.arg[0], term.c.y);
		break;
	case 'D':
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x-csiescseq.arg[0], term.c.y);
		break;
	case 'E':
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(0, term.c.y+csiescseq.arg[0]);
		break;
	case 'F':
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(0, term.c.y-csiescseq.arg[0]);
		break;
	case 'g':
		switch (csiescseq.arg[0]) {
		case 0:
			term.tabs[term.c.x] = 0;
			break;
		case 3:
			memset(term.tabs, 0, term.col * sizeof(*term.tabs));
			break;
		default:
			goto unknown;
		}
		break;
	case 'G':
	case '`':
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(csiescseq.arg[0]-1, term.c.y);
		break;
	case 'H':
	case 'f':
		DEFAULT(csiescseq.arg[0], 1);
		DEFAULT(csiescseq.arg[1], 1);
		tmoveato(csiescseq.arg[1]-1, csiescseq.arg[0]-1);
		break;
	case 'I':
		DEFAULT(csiescseq.arg[0], 1);
		tputtab(csiescseq.arg[0]);
		break;
	case 'J':
		switch (csiescseq.arg[0]) {
		case 0:
			if (term.c.x == 0)
				term.prompt_y = term.c.y;
			tclearregion(term.c.x, term.c.y, term.col-1, term.c.y);
			if (term.c.y < term.row-1) {
				tclearregion(0, term.c.y+1, term.col-1,
						term.row-1);
			}
			break;
		case 1:
			if (term.c.y > 0)
				tclearregion(0, 0, term.col-1, term.c.y-1);
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2:
			term.prompt_y = 0;
			tclearregion(0, 0, term.col-1, term.row-1);
			if (!IS_SET(MODE_ALTSCREEN))
				sb_reset_on_clear();
			break;
		case 3:
			term.prompt_y = 0;
			if (!IS_SET(MODE_ALTSCREEN))
				sb_reset_on_clear();
			break;
		default:
			goto unknown;
		}
		break;
	case 'K':
		switch (csiescseq.arg[0]) {
		case 0:
			tclearregion(term.c.x, term.c.y, term.col-1,
					term.c.y);
			break;
		case 1:
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2:
			tclearregion(0, term.c.y, term.col-1, term.c.y);
			break;
		}
		break;
	case 'S':
		if (csiescseq.priv) break;
		DEFAULT(csiescseq.arg[0], 1);
		tscrollup(term.top, csiescseq.arg[0]);
		break;
	case 'T':
		DEFAULT(csiescseq.arg[0], 1);
		tscrolldown(term.top, csiescseq.arg[0]);
		break;
	case 'L':
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblankline(csiescseq.arg[0]);
		break;
	case 'l':
		tsetmode(csiescseq.priv, 0, csiescseq.arg, csiescseq.narg);
		break;
	case 'M':
		DEFAULT(csiescseq.arg[0], 1);
		tdeleteline(csiescseq.arg[0]);
		break;
	case 'X':
		DEFAULT(csiescseq.arg[0], 1);
		tclearregion(term.c.x, term.c.y,
				term.c.x + csiescseq.arg[0] - 1, term.c.y);
		break;
	case 'P':
		DEFAULT(csiescseq.arg[0], 1);
		tdeletechar(csiescseq.arg[0]);
		break;
	case 'Z':
		DEFAULT(csiescseq.arg[0], 1);
		tputtab(-csiescseq.arg[0]);
		break;
	case 'd':
		DEFAULT(csiescseq.arg[0], 1);
		tmoveato(term.c.x, csiescseq.arg[0]-1);
		break;
	case 'h':
		tsetmode(csiescseq.priv, 1, csiescseq.arg, csiescseq.narg);
		break;
	case 'm':
		if (csiescseq.priv == '>') {
			if (csiescseq.arg[0] == 4) {
				int m = csiescseq.narg > 1 ? csiescseq.arg[1] : 0;
				xsetmodifyotherkeys(m);
			}
			break;
		}
		tsetattr(csiescseq.arg, csiescseq.narg);
		break;
	case 'n':
		if (csiescseq.priv) {
			switch (csiescseq.arg[0]) {
			case 996:
			case 997:
				report_color_scheme();
				break;
			default:
				goto unknown;
			}
			break;
		}
		switch (csiescseq.arg[0]) {
		case 5:
			ttywrite("\033[0n", sizeof("\033[0n") - 1, 0);
			break;
		case 6:
			len = snprintf(buf, sizeof(buf), "\033[%i;%iR",
			               term.c.y+1, term.c.x+1);
			ttywrite(buf, len, 0);
			break;
		case 996:
		case 997:
			report_color_scheme();
			break;
		default:
			goto unknown;
		}
		break;
	case 'r':
		if (csiescseq.priv) {
			goto unknown;
		} else {
			DEFAULT(csiescseq.arg[0], 1);
			DEFAULT(csiescseq.arg[1], term.row);
			tsetscroll(csiescseq.arg[0]-1, csiescseq.arg[1]-1);
			tmoveato(0, 0);
		}
		break;
	case 's':
		tcursor(CURSOR_SAVE);
		break;
	case 'u':
		if (csiescseq.priv == '?') {
			char qbuf[32];
			int qlen = snprintf(qbuf, sizeof(qbuf), "\033[?%du", xgetkittyflags());
			ttywrite(qbuf, qlen, 0);
			break;
		} else if (csiescseq.priv == '=') {
			int flags = csiescseq.narg > 0 ? csiescseq.arg[0] : 0;
			int mode = csiescseq.narg > 1 ? csiescseq.arg[1] : 1;
			xsetkittyflags(flags, mode);
			break;
		} else if (csiescseq.priv == '>') {
			int flags = csiescseq.narg > 0 ? csiescseq.arg[0] : -1;
			xpushkittyflags(flags);
			break;
		} else if (csiescseq.priv == '<') {
			int count = csiescseq.narg > 0 ? csiescseq.arg[0] : 1;
			xpopkittyflags(count);
			break;
		} else if (csiescseq.priv) {
			goto unknown;
		} else {
			tcursor(CURSOR_LOAD);
		}
		break;
	case 'W':
		if (csiescseq.priv) {
			if (csiescseq.arg[0] == 5) {
				int x;
				memset(term.tabs, 0, term.col * sizeof(*term.tabs));
				for (x = 8; x < term.col; x += 8)
					term.tabs[x] = 1;
			}
		} else {
			switch (csiescseq.arg[0]) {
			case 0:
				term.tabs[term.c.x] = 1;
				break;
			case 2:
				term.tabs[term.c.x] = 0;
				break;
			case 5:
				memset(term.tabs, 0, term.col * sizeof(*term.tabs));
				break;
			default:
				goto unknown;
			}
		}
		break;
	case '$':
		switch (csiescseq.mode[1]) {
		case 'p':
			if (csiescseq.narg == 0)
				csiescseq.narg = 1;
			for (i = 0; i < csiescseq.narg; i++) {
				int status = 0;
				if (csiescseq.priv) {
					switch (csiescseq.arg[i]) {
					case 1:
						status = xismode(MODE_APPCURSOR) ? 1 : 2;
						break;
					case 5:
						status = xismode(MODE_REVERSE) ? 1 : 2;
						break;
					case 6:
						status = (term.c.state & CURSOR_ORIGIN) ? 1 : 2;
						break;
					case 7:
						status = IS_SET(MODE_WRAP) ? 1 : 2;
						break;
					case 25:
						status = xismode(MODE_HIDE) ? 2 : 1;
						break;
					case 1000:
						status = xismode(MODE_MOUSEBTN) ? 1 : 2;
						break;
					case 1002:
						status = xismode(MODE_MOUSEMOTION) ? 1 : 2;
						break;
					case 1003:
						status = xismode(MODE_MOUSEMANY) ? 1 : 2;
						break;
					case 1004:
						status = xismode(MODE_FOCUS) ? 1 : 2;
						break;
					case 1006:
						status = xismode(MODE_MOUSESGR) ? 1 : 2;
						break;
					case 1049:
						status = IS_SET(MODE_ALTSCREEN) ? 1 : 2;
						break;
					case 2004:
						status = xismode(MODE_BRCKTPASTE) ? 1 : 2;
						break;
					case 2026:
						status = xismode(MODE_SYNC) ? 1 : 2;
						break;
					case 2027:
						status = 2;
						break;
					case 2031:
						status = term.mode_2031 ? 1 : 2;
						break;
					case 5522:
						status = 2;
						break;
					case 2017:
						status = 2;
						break;
					case 7727:
						status = 2;
						break;
					default:
						status = 0;
						break;
					}
					len = snprintf(buf, sizeof(buf), "\033[?%d;%d$y", csiescseq.arg[i], status);
					ttywrite(buf, len, 0);
				} else {
					switch (csiescseq.arg[i]) {
					case 4:
						status = IS_SET(MODE_INSERT) ? 1 : 2;
						break;
					case 20:
						status = IS_SET(MODE_CRLF) ? 1 : 2;
						break;
					default:
						status = 0;
						break;
					}
					len = snprintf(buf, sizeof(buf), "\033[%d;%d$y", csiescseq.arg[i], status);
					ttywrite(buf, len, 0);
				}
			}
			break;
		default:
			goto unknown;
		}
		break;
	case ' ':
		switch (csiescseq.mode[1]) {
		case 'q':
			if (xsetcursor(csiescseq.arg[0]))
				goto unknown;
			break;
		default:
			goto unknown;
		}
		break;
	case 'q':
		if (csiescseq.priv == '>') {
			len = snprintf(buf, sizeof(buf),
				       "\033P>|st-graphics(%s)\033\\", VERSION);
			ttywrite(buf, len, 0);
			break;
		}
		goto unknown;
	case '>':
		switch (csiescseq.mode[1]) {
		case 'q':
			len = snprintf(buf, sizeof(buf),
				       "\033P>|st-graphics(%s)\033\\", VERSION);
			ttywrite(buf, len, 0);
			break;
		default:
			goto unknown;
		}
		break;
	case 't':
		switch (csiescseq.arg[0]) {
		case 14:
			len = snprintf(buf, sizeof(buf), "\033[4;%i;%it",
					term.pixh, term.pixw);
			ttywrite(buf, len, 0);
			break;
		case 16:
			len = snprintf(buf, sizeof(buf), "\033[6;%i;%it",
					term.pixh / term.row,
					term.pixw / term.col);
			ttywrite(buf, len, 0);
			break;
		case 18:
			len = snprintf(buf, sizeof(buf), "\033[8;%i;%it",
					term.row, term.col);
			ttywrite(buf, len, 0);
			break;
		default:
			goto unknown;
		}
		break;
	}
}

void
csidump(void)
{
	size_t i;
	uint c;

	fprintf(stderr, "ESC[");
	for (i = 0; i < csiescseq.len; i++) {
		c = csiescseq.buf[i] & 0xff;
		if (isprint(c)) {
			putc(c, stderr);
		} else if (c == '\n') {
			fprintf(stderr, "(\\n)");
		} else if (c == '\r') {
			fprintf(stderr, "(\\r)");
		} else if (c == 0x1b) {
			fprintf(stderr, "(\\e)");
		} else {
			fprintf(stderr, "(%02x)", c);
		}
	}
	putc('\n', stderr);
}

void
csireset(void)
{
	memset(&csiescseq, 0, sizeof(csiescseq));
	memset(csiescseq.carg, -1, sizeof(csiescseq.carg));
}

void
osc_color_response(int num, int index, int is_osc4)
{
	int n;
	char buf[32];
	unsigned char r, g, b;

	if (xgetcolor(is_osc4 ? num : index, &r, &g, &b)) {
		fprintf(stderr, "erresc: failed to fetch %s color %d\n",
		        is_osc4 ? "osc4" : "osc",
		        is_osc4 ? num : index);
		return;
	}

	n = snprintf(buf, sizeof buf, "\033]%s%d;rgb:%02x%02x/%02x%02x/%02x%02x\007",
	             is_osc4 ? "4;" : "", num, r, r, g, g, b, b);
	if (n < 0 || n >= sizeof(buf)) {
		fprintf(stderr, "error: %s while printing %s response\n",
		        n < 0 ? "snprintf failed" : "truncation occurred",
		        is_osc4 ? "osc4" : "osc");
	} else {
		ttywrite(buf, n, 1);
	}
}

void
strhandle(void)
{
	char *p = NULL, *dec;
	int j, narg, par;
	const struct { int idx; char *str; } osc_table[] = {
		{ defaultfg, "foreground" },
		{ defaultbg, "background" },
		{ defaultcs, "cursor" },
		{ defaultfg, "mouse foreground" },
		{ defaultbg, "mouse background" },
		{ defaultfg, "tektronix foreground" },
		{ defaultbg, "tektronix background" },
		{ defaultcs, "highlight background" },
		{ defaultcs, "tektronix cursor" },
		{ defaultfg, "highlight foreground" }
	};

	term.esc &= ~(ESC_STR_END|ESC_STR);
	strparse();
	par = (narg = strescseq.narg) ? atoi(strescseq.args[0]) : 0;

	switch (strescseq.type) {
	case ']':
		switch (par) {
		case 0:
			if (narg > 1) {
				xsettitle(strescseq.args[1]);
				xseticontitle(strescseq.args[1]);
			}
			return;
		case 1:
			if (narg > 1)
				xseticontitle(strescseq.args[1]);
			return;
		case 2:
			if (narg > 1)
				xsettitle(strescseq.args[1]);
			return;
		case 52:
			if (narg > 2 && allowwindowops) {
				dec = base64dec(strescseq.args[2]);
				if (dec) {
					xsetsel(dec);
					xclipcopy();
				} else {
					fprintf(stderr, "erresc: invalid base64\n");
				}
			}
			return;
		case 10:
		case 11:
		case 12:
		case 13:
		case 14:
		case 15:
		case 16:
		case 17:
		case 18:
		case 19:
			if (narg < 2)
				break;
			p = strescseq.args[1];
			if ((j = par - 10) < 0 || j >= LEN(osc_table))
				break;

			if (!strcmp(p, "?")) {
				osc_color_response(par, osc_table[j].idx, 0);
			} else if (par <= 12) {
				if (!strcasecmp(p, "default") || !strcasecmp(p, "reset"))
					p = NULL;
				if (xsetcolorname(osc_table[j].idx, p)) {
					fprintf(stderr, "erresc: invalid %s color: %s\n",
					        osc_table[j].str, p ? p : "(null)");
				} else {
					tfulldirt();
					if (par == 11 && term.mode_2031)
						report_color_scheme();
				}
			}
			return;
		case 4:
			if (narg < 3)
				break;
			p = strescseq.args[2];

		case 104:
			j = (narg > 1) ? atoi(strescseq.args[1]) : -1;

			if (p && !strcmp(p, "?")) {
				osc_color_response(j, 0, 1);
			} else if (p && (!strcasecmp(p, "default") || !strcasecmp(p, "reset"))) {
				if (xsetcolorname(j, NULL)) {
					fprintf(stderr, "erresc: invalid color j=%d, p=%s\n",
					        j, p);
				} else {
					tfulldirt();
				}
			} else if (xsetcolorname(j, p)) {
				if (par == 104 && narg <= 1) {
					xloadcols();
					tfulldirt();
					return;
				}
				fprintf(stderr, "erresc: invalid color j=%d, p=%s\n",
				        j, p ? p : "(null)");
			} else {

				tfulldirt();
			}
			return;
		case 110:
		case 111:
		case 112:
		case 113:
		case 114:
		case 115:
		case 116:
		case 117:
		case 118:
		case 119:
			if (narg != 1)
				break;
			if ((j = par - 110) < 0 || j >= LEN(osc_table))
				break;
			if (par <= 112) {
				if (xsetcolorname(osc_table[j].idx, NULL)) {
					fprintf(stderr, "erresc: %s color not found\n", osc_table[j].str);
				} else {
					tfulldirt();
					if (par == 111 && term.mode_2031)
						report_color_scheme();
				}
			}
			return;
		case 22:
			if (narg > 1)
				xsetpointershape(strescseq.args[1]);
			else
				xsetpointershape(NULL);
			return;
		case 6:
		case 1042:
		case 7:
			return;
		case 8:
			if (narg >= 3 && strescseq.args[2][0] != '\0') {
				for (int k = 2; k < narg - 1; k++) {
					strescseq.args[k][strlen(strescseq.args[k])] = ';';
				}
				const char *params = strescseq.args[1];
				const char *url = strescseq.args[2];
				const char *id = NULL;
				if (strncmp(params, "id=", 3) == 0)
					id = params + 3;
				current_link_id = add_hyperlink(url, id, 1);
				term.c.attr.link_id = current_link_id;
				term.c.attr.mode |= ATTR_HYPERLINK;
			} else {
				current_link_id = 0;
				term.c.attr.link_id = 0;
				term.c.attr.mode &= ~ATTR_HYPERLINK;
			}
			return;
		case 9:
		case 66:
		case 72:
		case 99:
		case 1337:
		case 133:
			return;
		}
		break;
	case 'k':
		xsettitle(strescseq.args[0]);
		return;
	case '_':
		if (!g_st_config.kitty_graphics)
			return;
		if (gr_parse_command(strescseq.buf, strescseq.len)) {
			GraphicsCommandResult *res = &graphics_command_result;
			if (res->create_placeholder) {
				tcreateimgplaceholder(
					res->placeholder.image_id,
					res->placeholder.placement_id,
					res->placeholder.columns,
					res->placeholder.rows,
					res->placeholder.do_not_move_cursor,
					res->placeholder.text_underneath);
			}
			if (res->response[0])
				ttywrite(res->response, strlen(res->response),
					 0);
			if (res->redraw)
				tfulldirt();
			return;
		}
		return;
	case 'P':
		if (!g_st_config.sixel_graphics)
			return;
		return;
	case '^':
		return;
	}

	fprintf(stderr, "erresc: unknown str ");
	strdump();
}

void
strparse(void)
{
	int c;
	char *p = strescseq.buf;

	strescseq.narg = 0;
	strescseq.buf[strescseq.len] = '\0';

	if (*p == '\0')
		return;

	while (strescseq.narg < STR_ARG_SIZ) {
		strescseq.args[strescseq.narg++] = p;
		while ((c = *p) != ';' && c != '\0')
			++p;
		if (c == '\0')
			return;
		*p++ = '\0';
	}
}

void
strdump(void)
{
	size_t i;
	uint c;

	fprintf(stderr, "ESC%c", strescseq.type);
	for (i = 0; i < strescseq.len; i++) {
		c = strescseq.buf[i] & 0xff;
		if (c == '\0') {
			putc('\n', stderr);
			return;
		} else if (isprint(c)) {
			putc(c, stderr);
		} else if (c == '\n') {
			fprintf(stderr, "(\\n)");
		} else if (c == '\r') {
			fprintf(stderr, "(\\r)");
		} else if (c == 0x1b) {
			fprintf(stderr, "(\\e)");
		} else {
			fprintf(stderr, "(%02x)", c);
		}
	}
	fprintf(stderr, "ESC\\\n");
}

void
strreset(void)
{
	strescseq = (STREscape){
		.buf = xrealloc(strescseq.buf, STR_BUF_SIZ),
		.siz = STR_BUF_SIZ,
	};
}

void
sendbreak(const Arg *arg)
{
	if (tcsendbreak(cmdfd, 0))
		perror("Error sending break");
}

void
tprinter(char *s, size_t len)
{
	if (iofd != -1 && xwrite(iofd, s, len) < 0) {
		perror("Error writing to output file");
		close(iofd);
		iofd = -1;
	}
}

void
toggleprinter(const Arg *arg)
{
	term.mode ^= MODE_PRINT;
}

void
printscreen(const Arg *arg)
{
	tdump();
}

void
printsel(const Arg *arg)
{
	tdumpsel();
}

void
tdumpsel(void)
{
	char *ptr;

	if ((ptr = getsel())) {
		tprinter(ptr, strlen(ptr));
		free(ptr);
	}
}

void
tdumpline(int n)
{
	char buf[UTF_SIZ];
	const Glyph *bp, *end;

	bp = &term.line[n][0];
	end = &bp[MIN(tlinelen_render(n), term.col) - 1];
	if (bp != end || bp->u != ' ') {
		for ( ; bp <= end; ++bp)
			tprinter(buf, utf8encode(bp->u, buf));
	}
	tprinter("\n", 1);
}

void
tdump(void)
{
	int i;

	for (i = 0; i < term.row; ++i)
		tdumpline(i);
}

void
tputtab(int n)
{
	uint x = term.c.x;

	if (n > 0) {
		while (x < term.col && n--)
			for (++x; x < term.col && !term.tabs[x]; ++x)
				 ;
	} else if (n < 0) {
		while (x > 0 && n++)
			for (--x; x > 0 && !term.tabs[x]; --x)
				 ;
	}
	term.c.x = LIMIT(x, 0, term.col-1);
}

void
tdefutf8(char ascii)
{
	if (ascii == 'G')
		term.mode |= MODE_UTF8;
	else if (ascii == '@')
		term.mode &= ~MODE_UTF8;
}

void
tdeftran(char ascii)
{
	static char cs[] = "0B";
	static int vcs[] = {CS_GRAPHIC0, CS_USA};
	char *p;

	if ((p = strchr(cs, ascii)) == NULL) {
		fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
	} else {
		term.trantbl[term.icharset] = vcs[p - cs];
	}
}

static void
kscroll(const Arg *arg)
{
	uint64_t oldstart;
	uint64_t newstart;

	oldstart = sb_view_start();
	sb.view_offset += arg->i;
	LIMIT(sb.view_offset, 0, sb.len);
	newstart = sb_view_start();
	selscrollback(oldstart - newstart);
	redraw();
}

void
kscrolldown(const Arg *arg)
{
	Arg a;

	if (arg->i < 0)
		a.i = -term.row;
	else
		a.i = -arg->i;

	kscroll(&a);
}

void
kscrollup(const Arg *arg)
{
	Arg a;

	if (arg->i < 0)
		a.i = term.row;
	else
		a.i = arg->i;

	kscroll(&a);
}

void
tdectest(char c)
{
	int x, y;

	if (c == '8') {
		for (x = 0; x < term.col; ++x) {
			for (y = 0; y < term.row; ++y)
				tsetchar('E', &term.c.attr, x, y);
		}
	}
}

void
tstrsequence(uchar c)
{
	switch (c) {
	case 0x90:
		c = 'P';
		break;
	case 0x9f:
		c = '_';
		break;
	case 0x9e:
		c = '^';
		break;
	case 0x9d:
		c = ']';
		break;
	}
	strreset();
	strescseq.type = c;
	term.esc |= ESC_STR;
}

void
tcontrolcode(uchar ascii)
{
	switch (ascii) {
	case '\t':
		tputtab(1);
		return;
	case '\b':
		tmoveto(term.c.x-1, term.c.y);
		return;
	case '\r':
		tmoveto(0, term.c.y);
		return;
	case '\f':
	case '\v':
	case '\n':

		tnewline(IS_SET(MODE_CRLF));
		return;
	case '\a':
		if (term.esc & ESC_STR_END) {

			strhandle();
		} else {
			xbell();
		}
		break;
	case '\033':
		csireset();
		term.esc &= ~(ESC_CSI|ESC_ALTCHARSET|ESC_TEST);
		term.esc |= ESC_START;
		return;
	case '\016':
	case '\017':
		term.charset = 1 - (ascii - '\016');
		return;
	case '\032':
		tsetchar('?', &term.c.attr, term.c.x, term.c.y);

	case '\030':
		csireset();
		break;
	case '\005':
	case '\000':
	case '\021':
	case '\023':
	case 0177:
		return;
	case 0x80:
	case 0x81:
	case 0x82:
	case 0x83:
	case 0x84:
		break;
	case 0x85:
		tnewline(1);
		break;
	case 0x86:
	case 0x87:
		break;
	case 0x88:
		term.tabs[term.c.x] = 1;
		break;
	case 0x89:
	case 0x8a:
	case 0x8b:
	case 0x8c:
	case 0x8d:
	case 0x8e:
	case 0x8f:
	case 0x91:
	case 0x92:
	case 0x93:
	case 0x94:
	case 0x95:
	case 0x96:
	case 0x97:
	case 0x98:
	case 0x99:
		break;
	case 0x9a:
		ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 0x9b:
	case 0x9c:
		break;
	case 0x90:
	case 0x9d:
	case 0x9e:
	case 0x9f:
		tstrsequence(ascii);
		return;
	}

	term.esc &= ~(ESC_STR_END|ESC_STR);
}

int
eschandle(uchar ascii)
{
	switch (ascii) {
	case '[':
		term.esc |= ESC_CSI;
		return 0;
	case '#':
		term.esc |= ESC_TEST;
		return 0;
	case '%':
		term.esc |= ESC_UTF8;
		return 0;
	case 'P':
	case '_':
	case '^':
	case ']':
	case 'k':
		tstrsequence(ascii);
		return 0;
	case 'n':
	case 'o':
		term.charset = 2 + (ascii - 'n');
		break;
	case '(':
	case ')':
	case '*':
	case '+':
		term.icharset = ascii - '(';
		term.esc |= ESC_ALTCHARSET;
		return 0;
	case 'D':
		if (term.c.y == term.bot) {
			tscrollup(term.top, 1);
			term.c.state &= ~CURSOR_WRAPNEXT;
		} else {
			tmoveto(term.c.x, term.c.y+1);
		}
		break;
	case 'E':
		tnewline(1);
		break;
	case 'H':
		term.tabs[term.c.x] = 1;
		break;
	case 'M':
		if (term.c.y == term.top) {
			tscrolldown(term.top, 1);
		} else {
			tmoveto(term.c.x, term.c.y-1);
		}
		break;
	case 'Z':
		ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 'c':
		treset();
		resettitle();
		xloadcols();
		xsetmode(0, MODE_HIDE);
		break;
	case '=':
		xsetmode(1, MODE_APPKEYPAD);
		break;
	case '>':
		xsetmode(0, MODE_APPKEYPAD);
		break;
	case '7':
		tcursor(CURSOR_SAVE);
		break;
	case '8':
		tcursor(CURSOR_LOAD);
		break;
	case '\\':
		if (term.esc & ESC_STR_END)
			strhandle();
		break;
	default:
		fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
			(uchar) ascii, isprint(ascii)? ascii:'.');
		break;
	}
	return 1;
}

void
tputc(Rune u)
{
	char c[UTF_SIZ];
	int control;
	int width, len;
	Glyph *gp;

	control = ISCONTROL(u);
	if (u < 127 || !IS_SET(MODE_UTF8)) {
		c[0] = u;
		width = len = 1;
	} else {
		len = utf8encode(u, c);
		if (!control && (width = wcwidth(u)) == -1)
			width = 1;
	}

	if (IS_SET(MODE_PRINT))
		tprinter(c, len);

	if (term.esc & ESC_STR) {
		if (u == '\a' || u == 030 || u == 032 || u == 033 ||
		   ISCONTROLC1(u)) {
			term.esc &= ~(ESC_START|ESC_STR);
			term.esc |= ESC_STR_END;
			goto check_control_code;
		}

		if (strescseq.len+len >= strescseq.siz) {

			if (strescseq.siz > (SIZE_MAX - UTF_SIZ) / 2)
				return;
			strescseq.siz *= 2;
			strescseq.buf = xrealloc(strescseq.buf, strescseq.siz);
		}

		memmove(&strescseq.buf[strescseq.len], c, len);
		strescseq.len += len;
		return;
	}

check_control_code:

	if (control) {

		if (IS_SET(MODE_UTF8) && ISCONTROLC1(u))
			return;
		tcontrolcode(u);

		if (!term.esc)
			term.lastc = 0;
		return;
	} else if (term.esc & ESC_START) {
		if (term.esc & ESC_CSI) {
			csiescseq.buf[csiescseq.len++] = u;
			if (BETWEEN(u, 0x40, 0x7E)
					|| csiescseq.len >= \
					sizeof(csiescseq.buf)-1) {
				term.esc = 0;
				csiparse();
				csihandle();
			}
			return;
		} else if (term.esc & ESC_UTF8) {
			tdefutf8(u);
		} else if (term.esc & ESC_ALTCHARSET) {
			tdeftran(u);
		} else if (term.esc & ESC_TEST) {
			tdectest(u);
		} else {
			if (!eschandle(u))
				return;

		}
		term.esc = 0;

		return;
	}
	if (selected(term.c.x, term.c.y))
		selclear();

	uint16_t num = diacritic_to_num(u);
	if (num != 0)
		width = 0;

	if (u == IMAGE_PLACEHOLDER_CHAR || u == IMAGE_PLACEHOLDER_CHAR_OLD)
		width = 1;

	if (width == 0) {

		if (term.c.y <= 0 && term.c.x <= 0)
			return;
		else if (term.c.x == 0)
			gp = &term.line[term.c.y-1][term.col-1];
		else if (term.c.state & CURSOR_WRAPNEXT)
			gp = &term.line[term.c.y][term.c.x];
		else
			gp = &term.line[term.c.y][term.c.x-1];
		if (num && (gp->mode & ATTR_IMAGE)) {
			unsigned diaccount = tgetimgdiacriticcount(gp);
			if (diaccount == 0)
				tsetimgrow(gp, num);
			else if (diaccount == 1)
				tsetimgcol(gp, num);
			else if (diaccount == 2)
				tsetimg4thbyteplus1(gp, num);
			tsetimgdiacriticcount(gp, diaccount + 1);
		}
		term.lastc = u;
		return;
	}

	gp = &term.line[term.c.y][term.c.x];
	if (IS_SET(MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT)) {
		gp->mode |= ATTR_WRAP;
		tnewline(1);
		gp = &term.line[term.c.y][term.c.x];
	}

	if (IS_SET(MODE_INSERT) && term.c.x+width < term.col) {
		memmove(gp+width, gp, (term.col - term.c.x - width) * sizeof(Glyph));
		gp->mode &= ~ATTR_WIDE;
	}

	if (term.c.x+width > term.col) {
		if (IS_SET(MODE_WRAP))
			tnewline(1);
		else
			tmoveto(term.col - width, term.c.y);
		gp = &term.line[term.c.y][term.c.x];
	}

	tsetchar(u, &term.c.attr, term.c.x, term.c.y);
	term.lastc = u;

	if (width == 2) {
		gp->mode |= ATTR_WIDE;
		if (term.c.x+1 < term.col) {
			if (gp[1].mode == ATTR_WIDE && term.c.x+2 < term.col) {
				gp[2].u = ' ';
				gp[2].mode &= ~ATTR_WDUMMY;
			}
			gp[1].u = '\0';
			gp[1].mode = ATTR_WDUMMY;
		}
	}
	if (term.c.x+width < term.col) {
		tmoveto(term.c.x+width, term.c.y);
	} else {
		term.c.state |= CURSOR_WRAPNEXT;
	}
}

int
twrite(const char *buf, int buflen, int show_ctrl)
{
	int charsize;
	Rune u;
	int n;

	for (n = 0; n < buflen; n += charsize) {
		if (IS_SET(MODE_UTF8)) {

			charsize = utf8decode(buf + n, &u, buflen - n);
			if (charsize == 0)
				break;
		} else {
			u = buf[n] & 0xFF;
			charsize = 1;
		}
		if (show_ctrl && ISCONTROL(u)) {
			if (u & 0x80) {
				u &= 0x7f;
				tputc('^');
				tputc('[');
			} else if (u != '\n' && u != '\r' && u != '\t') {
				u ^= 0x40;
				tputc('^');
			}
		}
		tputc(u);
	}
	return n;
}

static void treflow(int col, int row);

void
tresize(int col, int row)
{
	int i, j;
	int is_alt = IS_SET(MODE_ALTSCREEN);
	TCursor alt_cursor;

	if (col < 1 || row < 1) {
		fprintf(stderr,
			"tresize: error resizing to %dx%d\n", col, row);
		return;
	}

	if (sel.ob.x != -1)
		selclear();

	if (term.row == 0 || term.col == 0) {
		term.col = col;
		term.row = row;
		term.line  = xmalloc(term.row * sizeof(Line));
		term.alt   = xmalloc(term.row * sizeof(Line));
		term.dirty = xmalloc(term.row * sizeof(int));
		term.tabs  = xmalloc(term.col * sizeof(*term.tabs));

		for (i = 0; i < term.row; i++) {
			term.line[i] = xmalloc(term.col * sizeof(Glyph));
			term.alt[i]  = xmalloc(term.col * sizeof(Glyph));
			term.dirty[i] = 1;
			for (j = 0; j < term.col; j++) {
				term.line[i][j] = (Glyph){
					.u = ' ',
					.mode = 0,
					.fg = defaultfg,
					.bg = defaultbg,
					.decor = DECOR_DEFAULT_COLOR,
				};
				term.alt[i][j] = (Glyph){
					.u = ' ',
					.mode = 0,
					.fg = defaultfg,
					.bg = defaultbg,
					.decor = DECOR_DEFAULT_COLOR,
				};
			}
		}

		memset(term.tabs, 0, term.col * sizeof(*term.tabs));
		for (i = 8; i < term.col; i += 8)
			term.tabs[i] = 1;

		tsetscroll(0, term.row - 1);
		tfulldirt();
		return;
	}

	if (col == term.col && row == term.row)
		return;

	if (is_alt) {
		Line *tmp = term.line;

		term.line = term.alt;
		term.alt = tmp;
		term.mode &= ~MODE_ALTSCREEN;

		alt_cursor = term.c;
		term.c = c[0];
	}

	if (!is_alt && col == term.col) {
		if (row > term.row) {
			int diff = row - term.row;
			int pull = MIN(sb.len, diff);

			term.line  = xrealloc(term.line, row * sizeof(Line));
			term.alt   = xrealloc(term.alt, row * sizeof(Line));
			term.dirty = xrealloc(term.dirty, row * sizeof(int));

			if (pull > 0) {
				memmove(term.line + pull, term.line, term.row * sizeof(Line));
				for (i = 0; i < pull; i++) {
					int idx = sb.len - pull + i;
					term.line[i] = sb.buf[sb_phys_index(idx)];
					sb.buf[sb_phys_index(idx)] = NULL;
				}
				sb.len -= pull;
				term.c.y += pull;
			}

			for (i = term.row + pull; i < row; i++) {
				term.line[i] = xmalloc(col * sizeof(Glyph));
				for (j = 0; j < col; j++)
					term.line[i][j] = (Glyph){ .u = ' ', .mode = 0, .fg = defaultfg, .bg = defaultbg, .decor = DECOR_DEFAULT_COLOR };
			}

			for (i = term.row; i < row; i++) {
				term.alt[i] = xmalloc(col * sizeof(Glyph));
				for (j = 0; j < col; j++)
					term.alt[i][j] = (Glyph){ .u = ' ', .mode = 0, .fg = defaultfg, .bg = defaultbg, .decor = DECOR_DEFAULT_COLOR };
			}

			for (i = 0; i < row; i++)
				term.dirty[i] = 1;

			term.row = row;
			tsetscroll(0, term.row - 1);
			tfulldirt();
			sb_view_changed();
			return;
		} else if (row < term.row) {
			int slide = 0;
			if (term.c.y >= row)
				slide = term.c.y - row + 1;

			for (i = 0; i < slide; i++)
				sb_push(term.line[i]);

			for (i = 0; i < slide; i++) {
				free(term.line[i]);
				free(term.alt[i]);
			}
			if (slide > 0) {
				memmove(term.line, term.line + slide, (term.row - slide) * sizeof(Line));
				memmove(term.alt, term.alt + slide, (term.row - slide) * sizeof(Line));
				term.c.y -= slide;
			}
			for (i = row; i < term.row - slide; i++) {
				free(term.line[i]);
				free(term.alt[i]);
			}

			term.line  = xrealloc(term.line, row * sizeof(Line));
			term.alt   = xrealloc(term.alt, row * sizeof(Line));
			term.dirty = xrealloc(term.dirty, row * sizeof(int));
			for (i = 0; i < row; i++)
				term.dirty[i] = 1;

			term.row = row;
			tsetscroll(0, term.row - 1);
			tfulldirt();
			sb_view_changed();
			return;
		} else {
			return;
		}
	}

	treflow(col, row);

	if (is_alt) {
		c[0] = term.c;
		term.c = alt_cursor;
		term.mode |= MODE_ALTSCREEN;
		LIMIT(term.c.x, 0, term.col - 1);
		LIMIT(term.c.y, 0, term.row - 1);

		Line *tmp = term.line;
		term.line = term.alt;
		term.alt = tmp;
	}
}

static void
treflow(int col, int row)
{
	int i, j;
	int active_screen_rows = term.row;
	while (active_screen_rows > term.c.y + 1 && tlinelen(term.line[active_screen_rows - 1]) == 0)
		active_screen_rows--;

	int total_old = sb.len + active_screen_rows;
	int cursor_old_line = sb.len + term.c.y;
	int cursor_old_col  = term.c.x;

	int new_cap = total_old * 2 + row + 64;
	Line *new_lines = xmalloc(new_cap * sizeof(Line));
	int new_count = 0;
	int new_cursor_line = -1;
	int new_cursor_col = -1;

	int log_cap = term.col * 2 + 128;
	Glyph *logical = xmalloc(log_cap * sizeof(Glyph));
	int log_len = 0;
	int cursor_log_offset = -1;

	int prompt_line_doc = -1;
	if (xismode(MODE_BRCKTPASTE)) {
		int py = term.prompt_y;
		if (py < 0 || py > term.c.y) {
			py = term.c.y;
			if (py > 0 && (term.line[py - 1][0].u == 0x256d || term.line[py - 1][0].u == 0x250c))
				py--;
		}
		prompt_line_doc = sb.len + py;
	}

	for (i = 0; i < total_old; i++) {
		Line cur_line = (i < sb.len) ? sb_get(i) : term.line[i - sb.len];

		if (prompt_line_doc >= 0 && i == prompt_line_doc && log_len > 0) {
			while (log_len > 0) {
				Glyph *g = &logical[log_len - 1];
				if (g->u == ' ' && g->bg == defaultbg && (g->mode & ATTR_BOLD) == 0)
					log_len--;
				else
					break;
			}
			if (log_len == 0)
				log_len = 1;

			if (cursor_log_offset > log_len)
				cursor_log_offset = log_len;

			int offset = 0;
			while (offset < log_len) {
				int copy_w = MIN(col, log_len - offset);
				Line nl = xmalloc(col * sizeof(Glyph));
				for (j = 0; j < col; j++)
					nl[j] = (Glyph){ .u = ' ', .mode = 0, .fg = defaultfg, .bg = defaultbg, .decor = DECOR_DEFAULT_COLOR };

				memcpy(nl, logical + offset, copy_w * sizeof(Glyph));

				if (offset + copy_w < log_len)
					nl[col - 1].mode |= ATTR_WRAP;
				else
					nl[col - 1].mode &= ~ATTR_WRAP;

				if (cursor_log_offset >= offset && (cursor_log_offset < offset + col || offset + copy_w >= log_len)) {
					new_cursor_line = new_count;
					new_cursor_col = cursor_log_offset - offset;
					cursor_log_offset = -1;
				}

				if (new_count >= new_cap) {
					new_cap *= 2;
					new_lines = xrealloc(new_lines, new_cap * sizeof(Line));
				}
				new_lines[new_count++] = nl;
				offset += copy_w;
			}
			log_len = 0;
			cursor_log_offset = -1;
		}

		int is_wrap = (cur_line[term.col - 1].mode & ATTR_WRAP);

		if (i == cursor_old_line)
			cursor_log_offset = log_len + cursor_old_col;

		if (log_len + term.col > log_cap) {
			log_cap = (log_len + term.col) * 2;
			logical = xrealloc(logical, log_cap * sizeof(Glyph));
		}
		memcpy(logical + log_len, cur_line, term.col * sizeof(Glyph));
		for (j = 0; j < term.col; j++)
			logical[log_len + j].mode &= ~ATTR_WRAP;
		log_len += term.col;

		if (is_wrap && i + 1 < total_old)
			continue;

		while (log_len > 0) {
			Glyph *g = &logical[log_len - 1];
			if (g->u == ' ' && g->bg == defaultbg && (g->mode & ATTR_BOLD) == 0)
				log_len--;
			else
				break;
		}
		if (log_len == 0)
			log_len = 1;

		if (cursor_log_offset > log_len)
			cursor_log_offset = log_len;

		int offset = 0;
		while (offset < log_len) {
			int copy_w = MIN(col, log_len - offset);
			Line nl = xmalloc(col * sizeof(Glyph));
			for (j = 0; j < col; j++)
				nl[j] = (Glyph){ .u = ' ', .mode = 0, .fg = defaultfg, .bg = defaultbg, .decor = DECOR_DEFAULT_COLOR };

			memcpy(nl, logical + offset, copy_w * sizeof(Glyph));

			if (offset + copy_w < log_len)
				nl[col - 1].mode |= ATTR_WRAP;
			else
				nl[col - 1].mode &= ~ATTR_WRAP;

			if (cursor_log_offset >= offset && (cursor_log_offset < offset + col || offset + copy_w >= log_len)) {
				new_cursor_line = new_count;
				new_cursor_col = cursor_log_offset - offset;
				cursor_log_offset = -1;
			}

			if (new_count >= new_cap) {
				new_cap *= 2;
				new_lines = xrealloc(new_lines, new_cap * sizeof(Line));
			}
			new_lines[new_count++] = nl;
			offset += copy_w;
		}

		log_len = 0;
		cursor_log_offset = -1;
	}
	free(logical);

	if (new_cursor_line < 0) {
		new_cursor_line = new_count > 0 ? new_count - 1 : 0;
		new_cursor_col = 0;
	}

	int screen_start = new_count - row;
	if (screen_start < 0)
		screen_start = 0;
	if (new_cursor_line < screen_start)
		screen_start = new_cursor_line;
	if (new_cursor_line >= screen_start + row)
		screen_start = new_cursor_line - row + 1;

	for (i = 0; i < sb.len; i++) {
		int p = sb_phys_index(i);
		if (sb.buf[p]) {
			free(sb.buf[p]);
			sb.buf[p] = NULL;
		}
	}

	int new_sb_len = screen_start;
	int sb_start = 0;
	if (new_sb_len > sb.cap) {
		int drop = new_sb_len - sb.cap;
		for (i = 0; i < drop; i++)
			free(new_lines[i]);
		sb_start = drop;
		new_sb_len = sb.cap;
	}

	for (i = 0; i < new_sb_len; i++)
		sb.buf[i] = new_lines[sb_start + i];
	sb.len = new_sb_len;
	sb.head = 0;
	sb.base = 0;

	for (i = 0; i < term.row; i++) {
		free(term.line[i]);
		free(term.alt[i]);
	}
	term.line  = xrealloc(term.line, row * sizeof(Line));
	term.alt   = xrealloc(term.alt, row * sizeof(Line));
	term.dirty = xrealloc(term.dirty, row * sizeof(int));
	term.tabs  = xrealloc(term.tabs, col * sizeof(*term.tabs));

	int screen_lines_copied = new_count - screen_start;
	if (screen_lines_copied > row)
		screen_lines_copied = row;

	for (i = 0; i < screen_lines_copied; i++) {
		term.line[i] = new_lines[screen_start + i];
		term.alt[i]  = xmalloc(col * sizeof(Glyph));
		term.dirty[i] = 1;
		for (j = 0; j < col; j++)
			term.alt[i][j] = (Glyph){ .u = ' ', .mode = 0, .fg = defaultfg, .bg = defaultbg, .decor = DECOR_DEFAULT_COLOR };
	}
	for (i = screen_lines_copied; i < row; i++) {
		term.line[i] = xmalloc(col * sizeof(Glyph));
		term.alt[i]  = xmalloc(col * sizeof(Glyph));
		term.dirty[i] = 1;
		for (j = 0; j < col; j++) {
			term.line[i][j] = (Glyph){ .u = ' ', .mode = 0, .fg = defaultfg, .bg = defaultbg, .decor = DECOR_DEFAULT_COLOR };
			term.alt[i][j]  = (Glyph){ .u = ' ', .mode = 0, .fg = defaultfg, .bg = defaultbg, .decor = DECOR_DEFAULT_COLOR };
		}
	}
	free(new_lines);

	term.c.y = new_cursor_line - screen_start;
	term.c.x = LIMIT(new_cursor_col, 0, col - 1);
	LIMIT(term.c.y, 0, row - 1);

	term.col = col;
	term.row = row;

	memset(term.tabs, 0, term.col * sizeof(*term.tabs));
	for (i = 8; i < term.col; i += 8)
		term.tabs[i] = 1;

	tsetscroll(0, term.row - 1);
	tfulldirt();
	sb.view_offset = 0;
	sb_view_changed();
}

void
resettitle(void)
{
	xsettitle(NULL);
}

void
drawregion(int x1, int y1, int x2, int y2)
{
	int y;

	xstartimagedraw(term.dirty, term.row);

	Line line;
	for (y = y1; y < y2; y++) {
		if (!term.dirty[y])
			continue;
		term.dirty[y] = 0;
		tmarkurls(y);
		line = renderline(y);
		xdrawline(line, x1, y, x2);
	}

	xfinishimagedraw();
}

void
draw(void)
{
	int cx = term.c.x, ocx = term.ocx, ocy = term.ocy;

	if (!xstartdraw())
		return;

	LIMIT(term.ocx, 0, term.col-1);
	LIMIT(term.ocy, 0, term.row-1);
	if (glyph_is_wide_dummy(&term.line[term.ocy][term.ocx]))
		term.ocx--;
	if (glyph_is_wide_dummy(&term.line[term.c.y][cx]))
		cx--;

	drawregion(0, 0, term.col, term.row);
	if (sb.view_offset == 0) {
		xdrawcursor(cx, term.c.y, term.line[term.c.y][cx],
		            term.ocx, term.ocy, term.line[term.ocy][term.ocx],
		            term.line[term.ocy], term.col);
		term.ocx = cx;
		term.ocy = term.c.y;
	}
	xfinishdraw();
	if (ocx != term.ocx || ocy != term.ocy)
		xximspot(term.ocx, term.ocy);
}

void
redraw(void)
{
	tfulldirt();
	draw();
}

Glyph
getglyphat(int col, int row)
{
	return term.line[row][col];
}

static int
is_url_char(Rune r)
{
	return r > 32 && r < 127 && r != '"' && r != '\'' && r != '<' && r != '>' && r != '`';
}

static int
is_url_trailing_punct(Rune r)
{
	return r == '.' || r == ',' || r == ';' || r == ':' ||
	       r == ')' || r == ']' || r == '}' || r == '>' ||
	       r == '!' || r == '?' || r == '"' || r == '\'';
}

static int
match_url_scheme(const char *s)
{
	if (isalpha((unsigned char)s[0])) {
		const char *p = s + 1;
		while (isalnum((unsigned char)*p) || *p == '+' || *p == '.' || *p == '-')
			p++;
		if (p[0] == ':' && p[1] == '/' && p[2] == '/')
			return (int)(p - s + 3);
	}

	for (const char *tok = g_st_config.url_prefixes; tok && *tok; ) {
		size_t n = strcspn(tok, " \t");
		if (n > 0 && strncmp(s, tok, n) == 0 && s[n] == ':')
			return (int)(n + 1);
		tok += n + strspn(tok + n, " \t");
	}

	return 0;
}

static void
openurl(const char *url)
{
	if (!url || !*url)
		return;

	const char *launcher = g_st_config.url_launcher[0] ? g_st_config.url_launcher : "xdg-open";

	switch (fork()) {
	case -1:
		return;
	case 0:
		setsid();
		int nullfd = open("/dev/null", O_RDWR);
		if (nullfd != -1) {
			dup2(nullfd, STDIN_FILENO);
			dup2(nullfd, STDOUT_FILENO);
			dup2(nullfd, STDERR_FILENO);
			if (nullfd > 2)
				close(nullfd);
		}
		execlp(launcher, launcher, url, (char *)NULL);
		_exit(1);
	}
}

static int
detect_url_at(int col, int row, char *out, size_t maxlen, int *out_srow, int *out_scol, int *out_erow, int *out_ecol)
{
	if (row < 0 || row >= term.row || col < 0 || col >= term.col)
		return 0;

	Line line = renderline(row);
	if (!line || line[col].u <= ' ' || !is_url_char(line[col].u))
		return 0;

	int start_col = col;
	int start_row = row;
	while (1) {
		if (start_col > 0) {
			Line cur = renderline(start_row);
			if (cur && is_url_char(cur[start_col - 1].u)) {
				start_col--;
			} else {
				break;
			}
		} else if (start_row > 0) {
			int prev_row = start_row - 1;
			int prev_col = term.col - 1;
			Line prev = renderline(prev_row);
			if (prev && ((prev[prev_col].mode & ATTR_WRAP) || is_url_char(prev[prev_col].u))) {
				start_row = prev_row;
				start_col = prev_col;
				Line cur = renderline(start_row);
				if (!cur || !is_url_char(cur[start_col].u)) {
					start_row++;
					start_col = 0;
					break;
				}
			} else {
				break;
			}
		} else {
			break;
		}
	}

	int end_col = col;
	int end_row = row;
	while (1) {
		Line cur = renderline(end_row);
		if (!cur)
			break;
		if (end_col < term.col - 1) {
			if (is_url_char(cur[end_col + 1].u)) {
				end_col++;
			} else {
				break;
			}
		} else if (end_row < term.row - 1) {
			int next_row = end_row + 1;
			Line next = renderline(next_row);
			if (next && ((cur[end_col].mode & ATTR_WRAP) ||
			    (end_col == term.col - 1 && is_url_char(next[0].u)))) {
				end_row = next_row;
				end_col = 0;
				if (!is_url_char(next[end_col].u)) {
					end_row--;
					end_col = term.col - 1;
					break;
				}
			} else {
				break;
			}
		} else {
			break;
		}
	}

	size_t len = 0;
	int r = start_row;
	int c = start_col;
	while (len < maxlen - 1) {
		Line cur = renderline(r);
		if (!cur)
			break;
		Rune u = cur[c].u;
		if (u > 0 && u < 128)
			out[len++] = (char)u;
		if (r == end_row && c == end_col)
			break;
		c++;
		if (c >= term.col) {
			c = 0;
			r++;
			if (r > end_row)
				break;
		}
	}
	out[len] = '\0';

	while (len > 0 && is_url_trailing_punct((unsigned char)out[len - 1])) {
		out[--len] = '\0';
		if (end_col > 0) {
			end_col--;
		} else if (end_row > start_row) {
			end_row--;
			end_col = term.col - 1;
		}
	}

	int valid = 0;
	if (match_url_scheme(out)) {
		valid = 1;
	} else if (strncmp(out, "www.", 4) == 0) {
		char tmp[2048];
		snprintf(tmp, sizeof(tmp), "https://%s", out);
		strncpy(out, tmp, maxlen - 1);
		out[maxlen - 1] = '\0';
		valid = 1;
	}

	if (!valid)
		return 0;

	if (out_srow) *out_srow = start_row;
	if (out_scol) *out_scol = start_col;
	if (out_erow) *out_erow = end_row;
	if (out_ecol) *out_ecol = end_col;
	return 1;
}

int
openlinkat(int col, int row)
{
	if (row < 0 || row >= term.row || col < 0 || col >= term.col)
		return 0;

	Line line = renderline(row);
	if (!line)
		return 0;

	Glyph g = line[col];

	if (g.link_id > 0 && is_osc8_link(g.link_id)) {
		const char *url = get_hyperlink(g.link_id);
		if (url && *url) {
			openurl(url);
			return 1;
		}
	}

	char url[2048];
	if (detect_url_at(col, row, url, sizeof(url), NULL, NULL, NULL, NULL)) {
		openurl(url);
		return 1;
	}

	if (g.link_id > 0) {
		const char *url = get_hyperlink(g.link_id);
		if (url && *url) {
			openurl(url);
			return 1;
		}
	}

	return 0;
}

uint32_t
getlinkidat(int col, int row)
{
	if (row < 0 || row >= term.row || col < 0 || col >= term.col)
		return 0;

	Line line = renderline(row);
	if (!line)
		return 0;

	if (line[col].link_id > 0)
		return line[col].link_id;

	char url[2048];
	int srow, scol, erow, ecol;
	if (detect_url_at(col, row, url, sizeof(url), &srow, &scol, &erow, &ecol)) {
		uint32_t lid = add_hyperlink(url, NULL, 0);
		int r = srow, c = scol;
		while (1) {
			Line cur = renderline(r);
			if (cur) {
				cur[c].link_id = lid;
				cur[c].mode |= ATTR_HYPERLINK;
			}
			if (r == erow && c == ecol)
				break;
			c++;
			if (c >= term.col) {
				c = 0;
				r++;
				if (r > erow)
					break;
			}
		}
		return lid;
	}

	return 0;
}

int
islinkat(int col, int row)
{
	return getlinkidat(col, row) > 0;
}

void
tmarkurls(int row)
{
	if (row < 0 || row >= term.row)
		return;

	Line line = renderline(row);
	if (!line)
		return;

	int start_idx = 0;

	if (row > 0 && line[0].link_id > 0 && !is_osc8_link(line[0].link_id)) {
		Line prev = renderline(row - 1);
		if (prev) {
			int pcol = term.col - 1;
			while (pcol > 0 && prev[pcol].u <= ' ')
				pcol--;
			if (prev[pcol].link_id == line[0].link_id) {
				uint32_t cont_id = line[0].link_id;
				while (start_idx < term.col && line[start_idx].link_id == cont_id)
					start_idx++;
			}
		}
	}

	for (int i = start_idx; i < term.col; i++) {
		if (line[i].link_id > 0 && !is_osc8_link(line[i].link_id)) {
			line[i].mode &= ~ATTR_HYPERLINK;
			line[i].link_id = 0;
		}
	}

	for (int i = start_idx; i < term.col; ) {
		if (line[i].link_id > 0) {
			i++;
			continue;
		}

		char ubuf[2048];
		int srow, scol, erow, ecol;
		if (detect_url_at(i, row, ubuf, sizeof(ubuf), &srow, &scol, &erow, &ecol)) {
			if (srow == row && scol == i) {
				uint32_t lid = add_hyperlink(ubuf, NULL, 0);
				int r = srow, c = scol;
				while (1) {
					Line cur = renderline(r);
					if (cur) {
						cur[c].link_id = lid;
						cur[c].mode |= ATTR_HYPERLINK;
					}
					if (r == erow && c == ecol)
						break;
					c++;
					if (c >= term.col) {
						c = 0;
						r++;
						if (r > erow)
							break;
					}
				}
				i = (erow == row) ? (ecol + 1) : term.col;
				continue;
			}
		}
		i++;
	}
}

int
tisaltscr(void)
{
	return IS_SET(MODE_ALTSCREEN);
}

void
externalpipe(const Arg *arg)
{
	int to[2];
	char buf[UTF_SIZ];
	void (*oldsigpipe)(int);
	Glyph *bp, *end;
	int lastpos, n, newline;

	if (pipe(to) == -1)
		return;

	switch (fork()) {
	case -1:
		close(to[0]);
		close(to[1]);
		return;
	case 0:
		dup2(to[0], STDIN_FILENO);
		close(to[0]);
		close(to[1]);
		execvp(((char **)arg->v)[0], (char **)arg->v);
		fprintf(stderr, "st: execvp %s\n", ((char **)arg->v)[0]);
		perror("failed");
		exit(0);
	}

	close(to[0]);

	oldsigpipe = signal(SIGPIPE, SIG_IGN);
	newline = 0;
	for (n = 0; n < term.row; n++) {
		bp = term.line[n];
		lastpos = MIN(tlinelen(bp) + 1, term.col) - 1;
		if (lastpos < 0)
			break;
		end = &bp[lastpos + 1];
		for (; bp < end; ++bp)
			if (xwrite(to[1], buf, utf8encode(bp->u, buf)) < 0)
				break;
		if ((newline = term.line[n][lastpos].mode & ATTR_WRAP))
			continue;
		if (xwrite(to[1], "\n", 1) < 0)
			break;
		newline = 0;
	}
	if (newline)
		(void)xwrite(to[1], "\n", 1);
	close(to[1]);

	signal(SIGPIPE, oldsigpipe);
}

void
spawnterminalcwd(void)
{
	char procpath[64], cwd[1024];
	pid_t pgrp, target;
	ssize_t len = -1;

	pgrp = tcgetpgrp(cmdfd);
	target = (pgrp > 0) ? pgrp : pid;

	snprintf(procpath, sizeof(procpath), "/proc/%d/cwd", target);
	len = readlink(procpath, cwd, sizeof(cwd) - 1);
	if (len < 0 && target != pid && pid > 0) {
		snprintf(procpath, sizeof(procpath), "/proc/%d/cwd", pid);
		len = readlink(procpath, cwd, sizeof(cwd) - 1);
	}
	if (len > 0)
		cwd[len] = '\0';
	else
		cwd[0] = '\0';

	switch (fork()) {
	case -1:
		return;
	case 0:
		if (cwd[0]) {
			if (chdir(cwd) < 0) {
			}
		}
		if (argv0 && *argv0)
			execlp(argv0, argv0, (char *)NULL);
		execlp("st", "st", (char *)NULL);
		exit(1);
	}
}

void
openscrollbackpager(const char *cmd)
{
	const char *tmpdir = getenv("TMPDIR");
	if (!tmpdir || !*tmpdir)
		tmpdir = "/tmp";
	char tmppath[PATH_MAX];
	snprintf(tmppath, sizeof(tmppath), "%s/st-scroll-XXXXXX", tmpdir);
	int fd;
	int newline;
	char buf[UTF_SIZ];
	Glyph *bp, *end;

	fd = mkstemp(tmppath);
	if (fd < 0)
		return;

	newline = 0;
	for (int i = 0; i < sb.len; i++) {
		Line line = sb_get(i);
		if (!line) continue;
		int lastpos = MIN(tlinelen(line) + 1, term.col) - 1;
		if (lastpos < 0) {
			if (write(fd, "\n", 1) < 0) break;
			continue;
		}
		bp = line;
		end = &bp[lastpos + 1];
		for (; bp < end; ++bp) {
			int ulen = utf8encode(bp->u, buf);
			if (write(fd, buf, ulen) < 0) break;
		}
		if ((newline = line[lastpos].mode & ATTR_WRAP))
			continue;
		if (write(fd, "\n", 1) < 0) break;
		newline = 0;
	}

	for (int i = 0; i < term.row; i++) {
		Line line = term.line[i];
		if (!line) continue;
		int lastpos = MIN(tlinelen(line) + 1, term.col) - 1;
		if (lastpos < 0) {
			if (write(fd, "\n", 1) < 0) break;
			continue;
		}
		bp = line;
		end = &bp[lastpos + 1];
		for (; bp < end; ++bp) {
			int ulen = utf8encode(bp->u, buf);
			if (write(fd, buf, ulen) < 0) break;
		}
		if ((newline = line[lastpos].mode & ATTR_WRAP))
			continue;
		if (write(fd, "\n", 1) < 0) break;
		newline = 0;
	}
	if (newline)
		(void)write(fd, "\n", 1);
	close(fd);

	switch (fork()) {
	case -1:
		unlink(tmppath);
		return;
	case 0: {
		char shcmd[1024];
		const char *pager = (cmd && cmd[0]) ? cmd : getenv("PAGER");
		if (!pager || !*pager)
			pager = "nvim";
		if (strstr(pager, "nvim")) {
			snprintf(shcmd, sizeof(shcmd),
				"nvim -c 'set buftype=nofile bufhidden=wipe ft=terminal nonu nornu' '%s' ; rm -f '%s'",
				tmppath, tmppath);
		} else {
			snprintf(shcmd, sizeof(shcmd), "%s '%s' ; rm -f '%s'", pager, tmppath, tmppath);
		}
		if (argv0 && *argv0)
			execlp(argv0, argv0, "-e", "/bin/sh", "-c", shcmd, (char *)NULL);
		execlp("st", "st", "-e", "/bin/sh", "-c", shcmd, (char *)NULL);
		exit(1);
	}
	}
}

int
get_sb_len(void)
{
	return sb.len;
}

int
get_sb_view_offset(void)
{
	return sb.view_offset;
}

void
set_sb_view_offset(int offset)
{
	Arg a;

	LIMIT(offset, 0, sb.len);
	if (offset == sb.view_offset)
		return;
	a.i = offset - sb.view_offset;
	kscroll(&a);
}

void
resetterm(const Arg *arg)
{
	if (IS_SET(MODE_ALTSCREEN))
		tswapscreen();
	treset();
	resettitle();
	xloadcols();
	xsetmode(0, MODE_HIDE | MODE_MOUSE | MODE_MOUSESGR | MODE_BRCKTPASTE | MODE_FOCUS | MODE_APPCURSOR | MODE_APPKEYPAD);
	xsetpointermotion(0);
	xsetkittyflags(0, 1);
	xsetmodifyotherkeys(0);
	primary_win_mode = 0;
	mouse_pgrp = 0;
	gr_reset();
	tfulldirt();
	redraw();
}
