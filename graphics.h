#include <stdint.h>
#include <sys/types.h>
#include <X11/Xlib.h>

void gr_init(Display *disp, Visual *vis, Colormap cm);

void gr_deinit();

void gr_append_imagerect(Drawable buf, uint32_t image_id, uint32_t placement_id,
			 int img_start_col, int img_end_col, int img_start_row,
			 int img_end_row, int x_col, int y_row, int x_pix,
			 int y_pix, int cw, int ch, int reverse);

void gr_start_drawing(Drawable buf, int cw, int ch);

void gr_finish_drawing(Drawable buf);

void gr_mark_dirty_animations(int *dirty, int rows);

int gr_parse_command(char *buf, size_t len);

void gr_preview_image(uint32_t image_id, const char *command);

void gr_show_image_info(uint32_t image_id, uint32_t placement_id,
			uint32_t imgcol, uint32_t imgrow,
			char is_classic_placeholder, int32_t diacritic_count,
			char *st_executable);

void gr_dump_state();

void gr_unload_images_to_reduce_ram();

void gr_for_each_image_cell(int (*callback)(void *data, Glyph *gp),
			    void *data);

void gr_schedule_image_redraw_by_id(uint32_t image_id);

Glyph *gr_get_glyph_underneath_image(uint32_t image_id, uint32_t placement_id,
				     int col, int row);

typedef enum {
	GRAPHICS_DEBUG_NONE = 0,
	GRAPHICS_DEBUG_LOG = 1,
	GRAPHICS_DEBUG_LOG_AND_BOXES = 2,
} GraphicsDebugMode;

extern GraphicsDebugMode graphics_debug_mode;

extern char graphics_display_images;

extern int graphics_next_redraw_delay;

#define MAX_GRAPHICS_RESPONSE_LEN 256

typedef struct {

	char redraw;

	char response[MAX_GRAPHICS_RESPONSE_LEN];

	char error;

	char create_placeholder;

	struct {
		uint32_t rows, columns;
		uint32_t image_id, placement_id;
		char do_not_move_cursor;
		Glyph *text_underneath;
	} placeholder;
} GraphicsCommandResult;

extern GraphicsCommandResult graphics_command_result;

void gr_reset(void);
