#include <stdint.h>
#include <sys/types.h>

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))
#define LEN(a)			(sizeof(a) / sizeof(a)[0])
#define BETWEEN(x, a, b)	((a) <= (x) && (x) <= (b))
#define DIVCEIL(n, d)		(((n) + ((d) - 1)) / (d))
#define DEFAULT(a, b)		(a) = (a) ? (a) : (b)
#define LIMIT(x, a, b)		(x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b)		(((a).mode & (~ATTR_WRAP)) != ((b).mode & (~ATTR_WRAP)) || \
				(a).fg != (b).fg || \
				(a).bg != (b).bg || (a).decor != (b).decor || \
				(a).link_id != (b).link_id)
#define TIMEDIFF(t1, t2)	((t1.tv_sec-t2.tv_sec)*1000 + \
				(t1.tv_nsec-t2.tv_nsec)/1E6)
#define MODBIT(x, set, bit)	((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

#define TRUECOLOR(r,g,b)	(1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x)		(1 << 24 & (x))
extern int histsize;

#define HEX_TO_INT(c)		((c) >= '0' && (c) <= '9' ? (c) - '0' : \
				(c) >= 'a' && (c) <= 'f' ? (c) - 'a' + 10 : \
				(c) >= 'A' && (c) <= 'F' ? (c) - 'A' + 10 : -1)

#define DECOR_DEFAULT_COLOR	0x0ffffff

enum glyph_attribute {
	ATTR_NULL       = 0,
	ATTR_BOLD       = 1 << 0,
	ATTR_FAINT      = 1 << 1,
	ATTR_ITALIC     = 1 << 2,
	ATTR_UNDERLINE  = 1 << 3,
	ATTR_BLINK      = 1 << 4,
	ATTR_REVERSE    = 1 << 5,
	ATTR_INVISIBLE  = 1 << 6,
	ATTR_STRUCK     = 1 << 7,
	ATTR_WRAP       = 1 << 8,
	ATTR_WIDE       = 1 << 9,
	ATTR_WDUMMY     = 1 << 10,
	ATTR_BOXDRAW    = 1 << 11,
	ATTR_HYPERLINK  = 1 << 12,
	ATTR_SELECTED   = 1 << 13,
	ATTR_BOLD_FAINT = ATTR_BOLD | ATTR_FAINT,
	ATTR_IMAGE      = 1 << 14,
};

/* Unicode blocks for box drawing - stable, spec-defined */
#define BOXDRAW_BLOCK         0x2500
#define BOXDRAW_BLOCK_MASK    (~0xff)
#define BRAILLE_BLOCK         0x2800
#define BRAILLE_BLOCK_MASK    (~0xff)
#define IMAGE_PLACEHOLDER     0x10EEEE
#define IMAGE_PLACEHOLDER_OLD 0xEEEE

/* Unified dummy check: WDUMMY (second cell of wide char) or IMAGE placeholder */
#define GLYPH_IS_DUMMY(g)     ((g).mode & (ATTR_WDUMMY | ATTR_IMAGE))
#define GLYPH_IS_WIDE_DUMMY(g) ((g).mode & ATTR_WDUMMY)
#define GLYPH_IS_IMAGE(g)     ((g).mode & ATTR_IMAGE)

enum drawing_mode {
    DRAW_NONE = 0,
    DRAW_BG = 1 << 0,
    DRAW_FG = 1 << 1,
};

enum selection_mode {
	SEL_IDLE = 0,
	SEL_EMPTY = 1,
	SEL_READY = 2
};

enum selection_type {
	SEL_REGULAR = 1,
	SEL_RECTANGULAR = 2
};

enum selection_snap {
	SNAP_WORD = 1,
	SNAP_LINE = 2
};

enum underline_style {
	UNDERLINE_STRAIGHT = 1,
	UNDERLINE_DOUBLE = 2,
	UNDERLINE_CURLY = 3,
	UNDERLINE_DOTTED = 4,
	UNDERLINE_DASHED = 5,
};

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

typedef uint_least32_t Rune;

#define Glyph Glyph_
typedef struct {
	Rune u;
	ushort mode;
	uint32_t fg;
	uint32_t bg;
	uint32_t decor;
	uint32_t link_id;
} Glyph;

typedef Glyph *Line;

typedef union {
	int i;
	uint ui;
	float f;
	const void *v;
	const char *s;
} Arg;

void die(const char *, ...);
void redraw(void);
void draw(void);

void externalpipe(const Arg *);
void spawnterminalcwd(void);
void openscrollbackpager(const char *);
void printscreen(const Arg *);
void printsel(const Arg *);
void sendbreak(const Arg *);
void toggleprinter(const Arg *);

int tattrset(int);
void tnew(int, int);
void tresize(int, int);
void tsetdirtattr(int);
void ttyhangup(void);
int ttynew(const char *, char *, const char *, char **);
size_t ttyread(void);
void ttyresize(int, int);
void ttywrite(const char *, size_t, int);

void resettitle(void);

void selclear(void);
void selinit(void);
void selstart(int, int, int);
void selextend(int, int, int, int);
int selected(int, int);
char *getsel(void);

Glyph getglyphat(int, int);

size_t utf8encode(Rune, char *);

void *xmalloc(size_t);
void *xrealloc(void *, size_t);
char *xstrdup(const char *);

int isboxdraw(Rune);
int tisaltscr(void);
int tisaltscreen(void);
ushort boxdrawindex(const Glyph *);
#ifdef XFT_VERSION

void boxdraw_xinit(Display *, Colormap, XftDraw *, Visual *);
void drawboxes(int, int, int, int, XftColor *, XftColor *, const XftGlyphFontSpec *, int);
#endif

void kscrollup(const Arg *);
void kscrolldown(const Arg *);
int get_sb_len(void);
int get_sb_view_offset(void);
void set_sb_view_offset(int);
void resetterm(const Arg *);
int st_mouse_active(void);

extern char *utmp;
extern char *scroll;
extern char *stty_args;
extern char *vtiden;
extern wchar_t *worddelimiters;
extern int allowaltscreen;
extern int allowwindowops;
extern char *termname;
extern unsigned int tabspaces;
extern unsigned int defaultfg;
extern unsigned int defaultbg;
extern unsigned int defaultcs;
extern unsigned int scrollback_lines;
extern int histsize;
extern const int boxdraw, boxdraw_bold, boxdraw_braille;

static inline uint32_t tgetdecorcolor(Glyph *g) { return g->decor & 0x1ffffff; }
static inline uint32_t tgetdecorstyle(Glyph *g) { return (g->decor >> 25) & 0x7; }
static inline void tsetdecorcolor(Glyph *g, uint32_t color) {
	g->decor = (g->decor & ~0x1ffffff) | (color & 0x1ffffff);
}
static inline void tsetdecorstyle(Glyph *g, uint32_t style) {
	g->decor = (g->decor & ~(0x7 << 25)) | ((style & 0x7) << 25);
}

static inline int glyph_is_dummy(const Glyph *g) { return GLYPH_IS_DUMMY(*g); }
static inline int glyph_is_wide_dummy(const Glyph *g) { return GLYPH_IS_WIDE_DUMMY(*g); }
static inline int glyph_is_image(const Glyph *g) { return GLYPH_IS_IMAGE(*g); }

static inline uint32_t tgetimgrow(Glyph *g) { return g->u & 0x1ff; }
static inline uint32_t tgetimgcol(Glyph *g) { return (g->u >> 9) & 0x1ff; }
static inline uint32_t tgetimgid4thbyteplus1(Glyph *g) { return (g->u >> 18) & 0x1ff; }
static inline uint32_t tgetimgdiacriticcount(Glyph *g) { return (g->u >> 27) & 0x3; }
static inline uint32_t tgetisclassicplaceholder(Glyph *g) { return (g->u >> 29) & 0x1; }
static inline void tsetimgrow(Glyph *g, uint32_t row) {
	g->u = (g->u & ~0x1ff) | (row & 0x1ff);
}
static inline void tsetimgcol(Glyph *g, uint32_t col) {
	g->u = (g->u & ~(0x1ff << 9)) | ((col & 0x1ff) << 9);
}
static inline void tsetimg4thbyteplus1(Glyph *g, uint32_t byteplus1) {
	g->u = (g->u & ~(0x1ff << 18)) | ((byteplus1 & 0x1ff) << 18);
}
static inline void tsetimgdiacriticcount(Glyph *g, uint32_t count) {
	g->u = (g->u & ~(0x3 << 27)) | ((count & 0x3) << 27);
}
static inline void tsetisclassicplaceholder(Glyph *g, uint32_t isclassic) {
	g->u = (g->u & ~(0x1 << 29)) | ((isclassic & 0x1) << 29);
}

static inline uint32_t tgetimgid(Glyph *g) {
	uint32_t msb = tgetimgid4thbyteplus1(g);
	if (msb != 0)
		--msb;
	return (msb << 24) | (g->fg & 0xFFFFFF);
}

static inline void tsetimgid(Glyph *g, uint32_t id) {
	g->fg = (id & 0xFFFFFF) | (1 << 24);
	tsetimg4thbyteplus1(g, ((id >> 24) & 0xFF) + 1);
}

static inline uint32_t tgetimgplacementid(Glyph *g) {
	if (tgetdecorcolor(g) == DECOR_DEFAULT_COLOR)
		return 0;
	return g->decor & 0xFFFFFF;
}

static inline void tsetimgplacementid(Glyph *g, uint32_t id) {
	g->decor = (id & 0xFFFFFF) | (1 << 24);
}

uint32_t add_hyperlink(const char *url, const char *id, int is_osc8);
const char *get_hyperlink(uint32_t lid);
int is_osc8_link(uint32_t lid);
int openlinkat(int col, int row);
int islinkat(int col, int row);
uint32_t getlinkidat(int col, int row);
void tmarkurls(int row);
