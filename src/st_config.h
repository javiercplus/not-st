#ifndef ST_CONFIG_H
#define ST_CONFIG_H

#include <stddef.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#define ST_CONFIG_MAX_PATH 1024
#define ST_CONFIG_COLOR_COUNT 260
#define ST_CONFIG_MAX_SHORTCUTS 64
#define ST_CONFIG_MAX_INCLUDES 32

typedef struct {
	char path[ST_CONFIG_MAX_PATH];
	long mtime;
} ConfigIncludedFile;

typedef enum {
	ACT_NONE = 0,
	ACT_COPY,
	ACT_PASTE,
	ACT_SELPASTE,
	ACT_ZOOM_IN,
	ACT_ZOOM_OUT,
	ACT_ZOOM_RESET,
	ACT_SCROLL_UP,
	ACT_SCROLL_DOWN,
	ACT_OPEN_URL,
	ACT_COPY_URL,
	ACT_RELOAD_CONFIG,
	ACT_NEW_TERMINAL,
	ACT_SCROLLBACK_PAGER,
	ACT_CHANGE_ALPHA_UP,
	ACT_CHANGE_ALPHA_DOWN,
	ACT_CHANGE_ALPHA_RESET,
	ACT_PIPE,
	ACT_SPAWN,
	ACT_RESET_TERMINAL,
	ACT_FULLSCREEN
} ShortcutAction;

enum {
	SCROLLBAR_HIDE = 0,
	SCROLLBAR_OVERLAY = 1,
	SCROLLBAR_ALWAYS = 2,
};

enum {
	URL_UNDERLINE_NEVER = 0,
	URL_UNDERLINE_HOVER = 1,
	URL_UNDERLINE_ALWAYS = 2,
};

enum {
	UNDERCURL_SPARSE = 0,
	UNDERCURL_DENSE = 1,
};

enum {
	URL_CLICK_NONE = 0,
	URL_CLICK_CTRL = 1,
	URL_CLICK_SHIFT = 2,
	URL_CLICK_ALT = 3,
};

typedef struct {
	unsigned int mod;
	KeySym keysym;
	ShortcutAction act;
	char cmd[256];
} ConfigShortcut;

typedef struct {

	char font[256];
	char sparefont[256];
	float cwscale;
	float chscale;

	float alpha;
	float alpha_unfocused;
	int borderpx;
	int anysize_halign;
	int anysize_valign;
	unsigned int cols;
	unsigned int rows;

	int cursor_style;
	int cursor_blink_timeout;
	int cursor_dynamic_color;
	int cursor_thickness;

	int bold_is_not_bright;
	int boxdraw;
	int boxdraw_bold;
	int boxdraw_braille;

	char termname[64];
	char shell[256];
	int tabspaces;
	int bellvolume;
	double minlatency;
	double maxlatency;
	unsigned int histsize;
	int mouse_scroll_multiplier;
	int scrollbar;
	int scrollbar_width;
	char scrollbar_color[32];
	char worddelimiters[128];
	unsigned int doubleclicktimeout;
	unsigned int tripleclicktimeout;
	int allowaltscreen;
	int allowwindowops;
	char image_preview_cmd[128];
	char url_launcher[128];
	char url_prefixes[256];
	int url_style;
	char url_color[32];
	int underline_hyperlinks;
	int undercurl_style;
	int url_click_modifiers;
	char selection_bg[32];
	char selection_fg[32];
	unsigned int graphics_max_file_size;
	unsigned int graphics_max_ram_size;
	unsigned int graphics_max_single_file_size; /* per-file cache limit */
	unsigned int graphics_total_file_size;   /* total file cache budget */
	unsigned int graphics_max_single_ram_size;  /* per-image ram limit */
	unsigned int graphics_max_total_ram_size;   /* total ram budget */
	unsigned int graphics_max_placements;
	double graphics_tolerance;
	unsigned int graphics_anim_delay;
	int kitty_graphics;
	int sixel_graphics;

	char icon[ST_CONFIG_MAX_PATH];

	char xdndescchar[128];

	int has_color[ST_CONFIG_COLOR_COUNT];
	char colorname[ST_CONFIG_COLOR_COUNT][32];

	ConfigShortcut shortcuts[ST_CONFIG_MAX_SHORTCUTS];
	size_t shortcut_count;
	char pager_cmd[256];

	char loaded_path[ST_CONFIG_MAX_PATH];
	int is_loaded;
	long last_mtime;
	ConfigIncludedFile includes[ST_CONFIG_MAX_INCLUDES];
	size_t include_count;
} StConfig;

extern StConfig g_st_config;

void st_config_init_defaults(const char *def_font, float def_alpha, int def_borderpx,
                             int def_halign, int def_valign, unsigned int def_cols,
                             unsigned int def_rows, unsigned int def_cursorstyle,
                             unsigned int def_blinktimeout, unsigned int def_cursorthickness,
                             const char *def_termname, const char *def_shell,
                             unsigned int def_tabspaces, int def_bellvolume,
                             double def_minlatency, double def_maxlatency,
                             float def_cwscale, float def_chscale,
                             int def_boxdraw, int def_boxdraw_bold, int def_boxdraw_braille,
                             const char *def_colorname[], size_t def_color_count);

int st_config_parse_key_combo(const char *str, unsigned int *out_mod, KeySym *out_keysym);
ShortcutAction st_config_parse_action(const char *str, char *out_cmd, size_t cmd_size);
void st_config_add_shortcut(unsigned int mod, KeySym keysym, ShortcutAction act, const char *cmd);

const char *st_config_find_file(const char *explicit_path);

int st_config_load(const char *path);

int st_config_reload(void);

int st_config_check_modified(void);

#endif
