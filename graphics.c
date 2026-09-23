#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#define _POSIX_C_SOURCE 200809L

#include <zlib.h>
#include <Imlib2.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "khash.h"
#include "kvec.h"

#include "st.h"
#include "graphics.h"

extern char **environ;

#define MAX_FILENAME_SIZE 256
#define MAX_INFO_LEN 256
#define MAX_IMAGE_RECTS 20

typedef int64_t Milliseconds;

enum ScaleMode {
	SCALE_MODE_UNSET = 0,

	SCALE_MODE_FILL = 1,

	SCALE_MODE_CONTAIN = 2,

	SCALE_MODE_NONE = 3,

	SCALE_MODE_NONE_OR_CONTAIN = 4,
};

enum AnimationState {
	ANIMATION_STATE_UNSET = 0,

	ANIMATION_STATE_STOPPED = 1,

	ANIMATION_STATE_LOADING = 2,

	ANIMATION_STATE_LOOPING = 3,
};

enum ImageStatus {
	STATUS_UNINITIALIZED = 0,
	STATUS_UPLOADING = 1,
	STATUS_UPLOADING_ERROR = 2,
	STATUS_UPLOADING_SUCCESS = 3,
	STATUS_RAM_LOADING_ERROR = 4,
	STATUS_RAM_LOADING_SUCCESS = 6,
};

const char *image_status_strings[6] = {
	"STATUS_UNINITIALIZED",
	"STATUS_UPLOADING",
	"STATUS_UPLOADING_ERROR",
	"STATUS_UPLOADING_SUCCESS",
	"STATUS_RAM_LOADING_ERROR",
	"STATUS_RAM_LOADING_SUCCESS",
};

enum ImageUploadingFailure {
	ERROR_OVER_SIZE_LIMIT = 1,
	ERROR_CANNOT_OPEN_CACHED_FILE = 2,
	ERROR_UNEXPECTED_SIZE = 3,
	ERROR_CANNOT_COPY_FILE = 4,
	ERROR_CANNOT_OPEN_SHM = 5,
	ERROR_MTIME_MISMATCH = 3,
};

const char *image_uploading_failure_strings[7] = {
	"NO_ERROR",
	"ERROR_OVER_SIZE_LIMIT",
	"ERROR_CANNOT_OPEN_CACHED_FILE",
	"ERROR_UNEXPECTED_SIZE",
	"ERROR_CANNOT_COPY_FILE",
	"ERROR_CANNOT_OPEN_SHM",
	"ERROR_MTIME_MISMATCH",
};

struct Image;
struct ImageFrame;
struct ImagePlacement;

KHASH_MAP_INIT_INT(id2image, struct Image *)
KHASH_MAP_INIT_INT(id2placement, struct ImagePlacement *)

typedef struct PixmapTransformation {

	int pixmap_w, pixmap_h;

	int dst_w, dst_h;

	int dst_x, dst_y;
} PixmapTransformation;

typedef struct ImageFrame {

	struct Image *image;

	int index;

	Milliseconds atime;

	uint32_t background_color;

	int background_frame_index;

	int gap;

	unsigned expected_size;

	int format;

	int data_pix_width, data_pix_height;

	int x, y;

	char compression;

	char status;

	char ram_loading_in_progress;

	char uploading_failure;

	char quiet;

	char blend;

	char *original_filename;

	time_t original_file_mtime;

	FILE *open_file;

	unsigned disk_size;

	Imlib_Image imlib_object;
} ImageFrame;

typedef struct Image {

	uint32_t image_id;

	uint32_t query_id;

	uint32_t image_number;

	Milliseconds atime;

	int total_duration;

	int total_disk_size;

	uint64_t global_command_index;

	int current_frame;

	char animation_state;

	Milliseconds current_frame_time;

	Milliseconds last_redraw;

	Milliseconds next_redraw;

	int pix_width, pix_height;

	ImageFrame first_frame;

	kvec_t(ImageFrame) frames_beyond_the_first;

	khash_t(id2placement) *placements;

	uint32_t default_placement;

	uint32_t initial_placement_id;
} Image;

typedef struct ImagePlacement {

	Image *image;

	uint32_t placement_id;

	Milliseconds atime;

	int protected_frame;

	char virtual;

	char scale_mode;

	uint16_t rows, cols;

	int src_pix_x, src_pix_y;

	int src_pix_width, src_pix_height;

	Pixmap first_pixmap;

	kvec_t(Pixmap) pixmaps_beyond_the_first;

	uint16_t scaled_cw, scaled_ch;

	PixmapTransformation pixmap_transformation;

	char do_not_move_cursor;

	Glyph *text_underneath;
} ImagePlacement;

typedef struct {
	uint32_t image_id;
	uint32_t placement_id;

	int screen_x_pix, screen_y_pix;

	int screen_y_row;

	int img_start_col, img_end_col, img_start_row, img_end_row;

	int cw, ch;

	int reverse;
} ImageRect;

#define foreach_frame(image, framevar, code) { size_t __i; \
	for (__i = 0; __i <= kv_size((image).frames_beyond_the_first); ++__i) { \
		ImageFrame *framevar = \
			__i == 0 ? &(image).first_frame \
			: &kv_A((image).frames_beyond_the_first, __i - 1); \
		code; \
	} }

#define foreach_pixmap(placement, pixmapvar, code) { size_t __i; \
	for (__i = 0; __i <= kv_size((placement).pixmaps_beyond_the_first); ++__i) { \
		Pixmap pixmapvar = \
			__i == 0 ? (placement).first_pixmap \
			: kv_A((placement).pixmaps_beyond_the_first, __i - 1); \
		code; \
	} }

static Image *gr_find_image(uint32_t image_id);
static void gr_get_frame_filename(ImageFrame *frame, char *out, size_t max_len);
static void gr_delete_image(Image *img);
static void gr_erase_placement(ImagePlacement *placement);
static void gr_check_limits();
static void gr_try_restore_imagefile(ImageFrame *frame);
static char *gr_base64dec(const char *src, size_t *size);
static void sanitize_str(char *str, size_t max_len);
static const char *sanitized_filename(const char *str);

static ImageRect image_rects[MAX_IMAGE_RECTS] = {{0}};

static khash_t(id2image) *images = NULL;

static unsigned total_placement_count = 0;

static int64_t images_disk_size = 0;

static int64_t images_ram_size = 0;

static uint32_t last_image_id = 0;

static int current_cw = 0, current_ch = 0;

static uint32_t current_upload_image_id = 0;

static int current_upload_frame_index = 0;

static struct timespec initialization_time = {0};

static Milliseconds drawing_start_time;

static uint64_t global_command_counter = 0;

static kvec_t(Milliseconds) next_redraw_times = {0, 0, NULL};

static int debug_loaded_files_counter = 0;

static int debug_loaded_pixmaps_counter = 0;

static char cache_dir[MAX_FILENAME_SIZE - 16];

static unsigned char reverse_table[256];

GraphicsDebugMode graphics_debug_mode = GRAPHICS_DEBUG_NONE;
char graphics_display_images = 1;
GraphicsCommandResult graphics_command_result = {0};
int graphics_next_redraw_delay = INT_MAX;

extern const char graphics_cache_dir_template[];
extern unsigned graphics_max_single_image_file_size;
extern unsigned graphics_total_file_cache_size;
extern unsigned graphics_max_single_image_ram_size;
extern unsigned graphics_max_total_ram_size;
extern unsigned graphics_max_total_placements;
extern double graphics_excess_tolerance_ratio;
extern unsigned graphics_animation_min_delay;

static Milliseconds graphics_direct_transmission_timeout_ms = 2000;

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))

static int64_t gr_timediff_ms(const struct timespec *end,
			      const struct timespec *start) {
	return (end->tv_sec - start->tv_sec) * 1000 +
	       (end->tv_nsec - start->tv_nsec) / 1000000;
}

static Milliseconds gr_now_ms() {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return gr_timediff_ms(&now, &initialization_time);
}

#define GR_LOG(...) \
	do { if(graphics_debug_mode) fprintf(stderr, __VA_ARGS__); } while(0)

static inline int gr_last_frame_index(Image *img) {
	return kv_size(img->frames_beyond_the_first) + 1;
}

static ImageFrame *gr_get_frame(Image *img, int index) {
	if (!img)
		return NULL;
	if (index == 1)
		return &img->first_frame;
	if (2 <= index && index <= gr_last_frame_index(img))
		return &kv_A(img->frames_beyond_the_first, index - 2);
	return NULL;
}

static ImageFrame *gr_get_last_frame(Image *img) {
	if (!img)
		return NULL;
	return gr_get_frame(img, gr_last_frame_index(img));
}

static inline int gr_last_uploaded_frame_index(Image *img) {
	int last_index = gr_last_frame_index(img);
	if (last_index > 1 &&
	    gr_get_frame(img, last_index)->status < STATUS_UPLOADING_SUCCESS)
		return last_index - 1;
	return last_index;
}

static Pixmap gr_get_frame_pixmap(ImagePlacement *placement, int index) {
	if (index == 1)
		return placement->first_pixmap;
	if (2 <= index &&
	    index <= kv_size(placement->pixmaps_beyond_the_first) + 1)
		return kv_A(placement->pixmaps_beyond_the_first, index - 2);
	return 0;
}

static void gr_set_frame_pixmap(ImagePlacement *placement, int index,
				Pixmap pixmap) {
	if (index == 1) {
		placement->first_pixmap = pixmap;
		return;
	}

	size_t old_size = kv_size(placement->pixmaps_beyond_the_first);
	if (old_size < index - 1) {
		kv_a(Pixmap, placement->pixmaps_beyond_the_first, index - 2);
		for (size_t i = old_size; i < index - 1; i++)
			kv_A(placement->pixmaps_beyond_the_first, i) = 0;
	}
	kv_A(placement->pixmaps_beyond_the_first, index - 2) = pixmap;
}

static Image *gr_find_image(uint32_t image_id) {
	khiter_t k = kh_get(id2image, images, image_id);
	if (k == kh_end(images))
		return NULL;
	Image *res = kh_value(images, k);
	return res;
}

static Image *gr_find_image_by_number(uint32_t image_number) {
	if (image_number == 0)
		return NULL;
	Image *newest_img = NULL;
	Image *img = NULL;
	kh_foreach_value(images, img, {
		if (img->image_number == image_number &&
		    (!newest_img || newest_img->global_command_index <
					    img->global_command_index))
			newest_img = img;
	});
	if (!newest_img)
		GR_LOG("Image number %u not found\n", image_number);
	else
		GR_LOG("Found image number %u, its id is %u\n", image_number,
		       img->image_id);
	return newest_img;
}

static ImagePlacement *gr_find_placement(Image *img, uint32_t placement_id) {
	if (!img)
		return NULL;
	if (placement_id == 0) {

		ImagePlacement *dflt = NULL;
		if (img->default_placement != 0)
			dflt = gr_find_placement(img, img->default_placement);
		if (dflt)
			return dflt;

		kh_foreach_value(img->placements, dflt, {
			img->default_placement = dflt->placement_id;
			return dflt;
		});

		return NULL;
	}
	khiter_t k = kh_get(id2placement, img->placements, placement_id);
	if (k == kh_end(img->placements))
		return NULL;
	ImagePlacement *res = kh_value(img->placements, k);
	return res;
}

static ImagePlacement *gr_find_image_and_placement(uint32_t image_id,
						   uint32_t placement_id) {
	return gr_find_placement(gr_find_image(image_id), placement_id);
}

Glyph *gr_get_glyph_underneath_image(uint32_t image_id, uint32_t placement_id,
				     int col, int row) {
	ImagePlacement *placement =
		gr_find_image_and_placement(image_id, placement_id);
	if (!placement || !placement->text_underneath)
		return NULL;
	col--;
	row--;
	if (col < 0 || col >= placement->cols || row < 0 ||
	    row >= placement->rows)
		return NULL;
	return &placement->text_underneath[row * placement->cols + col];
}

static void gr_get_frame_filename(ImageFrame *frame, char *out,
				  size_t max_len) {
	snprintf(out, max_len, "%s/img-%.3u-%.3u", cache_dir,
		 frame->image->image_id, frame->index);
}

static unsigned gr_frame_current_ram_size(ImageFrame *frame) {
	if (!frame->imlib_object)
		return 0;
	return (unsigned)frame->image->pix_width * frame->image->pix_height * 4;
}

static unsigned gr_placement_single_frame_ram_size(ImagePlacement *placement) {
	return (unsigned)placement->pixmap_transformation.pixmap_w *
	       placement->pixmap_transformation.pixmap_h * 4;
}

static unsigned gr_placement_current_ram_size(ImagePlacement *placement) {
	unsigned single_frame_size =
		gr_placement_single_frame_ram_size(placement);
	unsigned result = 0;
	foreach_pixmap(*placement, pixmap, {
		if (pixmap)
			result += single_frame_size;
	});
	return result;
}

static void gr_unload_frame(ImageFrame *frame) {
	if (!frame->imlib_object)
		return;

	unsigned frame_ram_size = gr_frame_current_ram_size(frame);
	images_ram_size -= frame_ram_size;

	imlib_context_set_image(frame->imlib_object);
	imlib_free_image_and_decache();
	frame->imlib_object = NULL;

	GR_LOG("After unloading image %u frame %u (atime %ld ms ago) "
	       "ram: %ld KiB  (- %u KiB)\n",
	       frame->image->image_id, frame->index,
	       drawing_start_time - frame->atime, images_ram_size / 1024,
	       frame_ram_size / 1024);
}

static void gr_unload_all_frames(Image *img) {
	foreach_frame(*img, frame, {
		gr_unload_frame(frame);
	});
}

static void gr_unload_placement(ImagePlacement *placement) {
	unsigned placement_ram_size = gr_placement_current_ram_size(placement);
	images_ram_size -= placement_ram_size;

	Display *disp = imlib_context_get_display();
	foreach_pixmap(*placement, pixmap, {
		if (pixmap)
			XFreePixmap(disp, pixmap);
	});

	placement->first_pixmap = 0;
	placement->pixmaps_beyond_the_first.n = 0;
	placement->scaled_ch = placement->scaled_cw = 0;

	GR_LOG("After unloading placement %u/%u (atime %ld ms ago) "
	       "ram: %ld KiB  (- %u KiB)\n",
	       placement->image->image_id, placement->placement_id,
	       drawing_start_time - placement->atime, images_ram_size / 1024,
	       placement_ram_size / 1024);
}

static void gr_unload_pixmap(ImagePlacement *placement, int frameidx) {
	Pixmap pixmap = gr_get_frame_pixmap(placement, frameidx);
	if (!pixmap)
		return;

	Display *disp = imlib_context_get_display();
	XFreePixmap(disp, pixmap);
	gr_set_frame_pixmap(placement, frameidx, 0);
	images_ram_size -= gr_placement_single_frame_ram_size(placement);

	GR_LOG("After unloading pixmap %ld of "
	       "placement %u/%u (atime %ld ms ago) "
	       "frame %u (atime %ld ms ago) "
	       "ram: %ld KiB  (- %u KiB)\n",
	       pixmap, placement->image->image_id, placement->placement_id,
	       drawing_start_time - placement->atime, frameidx,
	       drawing_start_time -
		       gr_get_frame(placement->image, frameidx)->atime,
	       images_ram_size / 1024,
	       gr_placement_single_frame_ram_size(placement) / 1024);
}

static void gr_close_disk_cache_file(ImageFrame *frame) {
	if (frame && frame->open_file) {
		fclose(frame->open_file);
		frame->open_file = NULL;
	}
}

static void gr_delete_imagefile(ImageFrame *frame) {

	gr_close_disk_cache_file(frame);

	if (frame->disk_size == 0)
		return;

	char filename[MAX_FILENAME_SIZE];
	gr_get_frame_filename(frame, filename, MAX_FILENAME_SIZE);
	remove(filename);

	unsigned disk_size = frame->disk_size;
	images_disk_size -= disk_size;
	frame->image->total_disk_size -= disk_size;
	frame->disk_size = 0;

	GR_LOG("After deleting image file %u frame %u (atime %ld ms ago) "
	       "disk: %ld KiB  (- %u KiB)\n",
	       frame->image->image_id, frame->index,
	       drawing_start_time - frame->atime, images_disk_size / 1024,
	       disk_size / 1024);
}

static void gr_delete_imagefiles(Image *img) {
	foreach_frame(*img, frame, {
		gr_delete_imagefile(frame);
	});
}

static void gr_delete_placement_keep_id(ImagePlacement *placement) {
	if (!placement)
		return;
	GR_LOG("Deleting placement %u/%u\n", placement->image->image_id,
	       placement->placement_id);

	if (placement->text_underneath && !placement->virtual)
		gr_erase_placement(placement);
	gr_unload_placement(placement);
	kv_destroy(placement->pixmaps_beyond_the_first);
	free(placement->text_underneath);
	free(placement);
	total_placement_count--;
}

static void gr_delete_all_placements(Image *img) {
	ImagePlacement *placement = NULL;
	kh_foreach_value(img->placements, placement, {
		gr_delete_placement_keep_id(placement);
	});
	kh_clear(id2placement, img->placements);
}

static void gr_delete_image_keep_id(Image *img) {
	if (!img)
		return;
	GR_LOG("Deleting image %u\n", img->image_id);
	foreach_frame(*img, frame, {
		gr_delete_imagefile(frame);
		gr_unload_frame(frame);
		if (frame->original_filename)
			free(frame->original_filename);
	});
	kv_destroy(img->frames_beyond_the_first);
	gr_delete_all_placements(img);
	kh_destroy(id2placement, img->placements);
	free(img);
}

static void gr_delete_image(Image *img) {
	if (!img)
		return;
	uint32_t id = img->image_id;
	gr_delete_image_keep_id(img);
	khiter_t k = kh_get(id2image, images, id);
	kh_del(id2image, images, k);
}

static void gr_delete_placement(ImagePlacement *placement) {
	if (!placement)
		return;
	uint32_t id = placement->placement_id;
	Image *img = placement->image;
	gr_delete_placement_keep_id(placement);
	khiter_t k = kh_get(id2placement, img->placements, id);
	kh_del(id2placement, img->placements, k);
}

static void gr_delete_all_images() {
	Image *img = NULL;
	kh_foreach_value(images, img, {
		gr_delete_image_keep_id(img);
	});
	kh_clear(id2image, images);
}

void gr_reset(void) {
	if (images)
		gr_delete_all_images();
}

static void gr_touch_image(Image *img) {
	img->atime = gr_now_ms();
}

static void gr_touch_frame(ImageFrame *frame) {
	frame->image->atime = frame->atime = gr_now_ms();
}

static void gr_touch_placement(ImagePlacement *placement) {
	placement->image->atime = placement->atime = gr_now_ms();
}

static Image *gr_new_image(uint32_t id) {
	if (id == 0) {
		do {
			id = rand();

		} while ((id & 0xFF000000) == 0 || (id & 0x00FFFF00) == 0 ||
			 gr_find_image(id));
		GR_LOG("Generated random image id %u\n", id);
	}
	Image *img = gr_find_image(id);
	gr_delete_image_keep_id(img);
	GR_LOG("Creating image %u\n", id);
	img = malloc(sizeof(Image));
	memset(img, 0, sizeof(Image));
	img->placements = kh_init(id2placement);
	int ret;
	khiter_t k = kh_put(id2image, images, id, &ret);
	kh_value(images, k) = img;
	img->image_id = id;
	gr_touch_image(img);
	img->global_command_index = global_command_counter;
	return img;
}

static ImageFrame *gr_append_new_frame(Image *img) {
	ImageFrame *frame = NULL;
	if (img->first_frame.index == 0 &&
	    kv_size(img->frames_beyond_the_first) == 0) {
		frame = &img->first_frame;
		frame->index = 1;
	} else {
		frame = kv_pushp(ImageFrame, img->frames_beyond_the_first);
		memset(frame, 0, sizeof(ImageFrame));
		frame->index = kv_size(img->frames_beyond_the_first) + 1;
	}
	frame->image = img;
	gr_touch_frame(frame);
	GR_LOG("Appending frame %d to image %u\n", frame->index, img->image_id);
	return frame;
}

static ImagePlacement *gr_new_placement(Image *img, uint32_t id) {
	if (id == 0) {
		do {

			id = rand() & 0xFFFFFF;

		} while ((id & 0x00FFFF00) == 0 || gr_find_placement(img, id));
	}
	ImagePlacement *placement = gr_find_placement(img, id);
	gr_delete_placement_keep_id(placement);
	GR_LOG("Creating placement %u/%u\n", img->image_id, id);
	placement = malloc(sizeof(ImagePlacement));
	memset(placement, 0, sizeof(ImagePlacement));
	total_placement_count++;
	int ret;
	khiter_t k = kh_put(id2placement, img->placements, id, &ret);
	kh_value(img->placements, k) = placement;
	placement->image = img;
	placement->placement_id = id;
	gr_touch_placement(placement);
	if (img->default_placement == 0)
		img->default_placement = id;
	return placement;
}

static int64_t ceil_div(int64_t a, int64_t b) {
	return (a + b - 1) / b;
}

static void gr_infer_placement_size_maybe(ImagePlacement *placement) {

	int image_pix_width = placement->image->pix_width;
	int image_pix_height = placement->image->pix_height;

	if (placement->src_pix_x < 0)
		placement->src_pix_x = 0;
	if (placement->src_pix_y < 0)
		placement->src_pix_y = 0;
	if (placement->src_pix_width < 0)
		placement->src_pix_width = 0;
	if (placement->src_pix_height < 0)
		placement->src_pix_height = 0;

	if (placement->src_pix_x > image_pix_width)
		placement->src_pix_x = image_pix_width;
	if (placement->src_pix_y > image_pix_height)
		placement->src_pix_y = image_pix_height;

	if (placement->src_pix_width == 0 ||
	    placement->src_pix_x + placement->src_pix_width > image_pix_width)
		placement->src_pix_width =
			image_pix_width - placement->src_pix_x;
	if (placement->src_pix_height == 0 ||
	    placement->src_pix_y + placement->src_pix_height > image_pix_height)
		placement->src_pix_height =
			image_pix_height - placement->src_pix_y;

	if (placement->cols != 0 && placement->rows != 0)
		return;
	if (placement->src_pix_width == 0 || placement->src_pix_height == 0)
		return;
	if (current_cw == 0 || current_ch == 0)
		return;

	if (placement->cols == 0 && placement->rows == 0) {
		placement->cols =
			ceil_div(placement->src_pix_width, current_cw);
		placement->rows =
			ceil_div(placement->src_pix_height, current_ch);
		return;
	}

	if (placement->scale_mode == SCALE_MODE_CONTAIN) {

		if (placement->cols == 0) {
			placement->cols = ceil_div(
				placement->src_pix_width * placement->rows *
					current_ch,
				placement->src_pix_height * current_cw);
			return;
		}
		if (placement->rows == 0) {
			placement->rows =
				ceil_div(placement->src_pix_height *
						 placement->cols * current_cw,
					 placement->src_pix_width * current_ch);
			return;
		}
	} else {

		if (!placement->cols)
			placement->cols =
				ceil_div(placement->src_pix_width, current_cw);
		if (!placement->rows)
			placement->rows =
				ceil_div(placement->src_pix_height, current_ch);
	}
}

static void gr_update_frame_index(Image *img, Milliseconds now) {
	if (img->current_frame == 0) {
		img->current_frame_time = now;
		img->current_frame = 1;
		img->next_redraw = now + MAX(1, img->first_frame.gap);
		return;
	}

	if (!img->animation_state ||
	    img->animation_state == ANIMATION_STATE_STOPPED ||
	    img->animation_state == ANIMATION_STATE_UNSET) {

		img->next_redraw = 0;
		return;
	}
	int last_uploaded_frame_index = gr_last_uploaded_frame_index(img);

	if (img->animation_state == ANIMATION_STATE_LOADING &&
	    img->current_frame == last_uploaded_frame_index) {

		img->next_redraw = 0;
		return;
	}

	int passed_ms = now - img->current_frame_time;

	if (img->animation_state == ANIMATION_STATE_LOOPING &&
	    img->total_duration > 0 && passed_ms >= img->total_duration) {
		passed_ms %= img->total_duration;
		img->current_frame_time = now - passed_ms;
	}

	int original_frame_index = img->current_frame;
	while (1) {
		ImageFrame *frame = gr_get_frame(img, img->current_frame);
		if (!frame) {

			img->current_frame = 1;
			img->current_frame_time = now;
			img->next_redraw = now + MAX(1, img->first_frame.gap);
			return;
		}
		if (frame->gap >= 0 && passed_ms < frame->gap) {

			img->next_redraw =
				img->current_frame_time + MAX(1, frame->gap);
			return;
		}

		passed_ms -= MAX(0, frame->gap);
		if (img->current_frame >= last_uploaded_frame_index) {

			if (img->animation_state == ANIMATION_STATE_LOADING) {
				img->next_redraw = 0;
				return;
			}

			img->current_frame = 1;

		} else {
			img->current_frame++;
		}

		if (img->current_frame == original_frame_index) {

			img->current_frame++;
			if (img->current_frame >
			    last_uploaded_frame_index)
				img->current_frame = 1;
			img->current_frame_time = now;
			img->next_redraw = now + MAX(
				1, gr_get_frame(img, img->current_frame)->gap);
			return;
		}

		img->current_frame_time += MAX(0, frame->gap);
	}
}

static int gr_is_original_file_still_available(ImageFrame *frame) {
	if (!frame->original_filename)
		return 0;
	struct stat st;
	if (stat(frame->original_filename, &st) != 0)
		return 0;
	if (!S_ISREG(st.st_mode))
		return 0;
	if (st.st_size == 0 || st.st_size > graphics_max_single_image_file_size)
		return 0;
	if (frame->expected_size && st.st_size != frame->expected_size)
		return 0;
	if (st.st_mtime != frame->original_file_mtime)
		return 0;
	return 1;
}

static int gr_cmp_frames_by_atime(const void *a, const void *b) {
	ImageFrame *frame_a = *(ImageFrame *const *)a;
	ImageFrame *frame_b = *(ImageFrame *const *)b;
	if (frame_a->atime == frame_b->atime)
		return frame_a->image->global_command_index -
		       frame_b->image->global_command_index;
	return frame_a->atime - frame_b->atime;
}

static int gr_cmp_images_by_atime(const void *a, const void *b) {
	Image *img_a = *(Image *const *)a;
	Image *img_b = *(Image *const *)b;
	if (img_a->atime == img_b->atime)
		return img_a->global_command_index -
		       img_b->global_command_index;
	return img_a->atime - img_b->atime;
}

static int gr_cmp_placements_by_atime(const void *a, const void *b) {
	ImagePlacement *p_a = *(ImagePlacement **)a;
	ImagePlacement *p_b = *(ImagePlacement **)b;
	if (p_a->atime == p_b->atime)
		return p_a->image->global_command_index -
		       p_b->image->global_command_index;
	return p_a->atime - p_b->atime;
}

typedef kvec_t(Image *) ImageVec;
typedef kvec_t(ImagePlacement *) ImagePlacementVec;
typedef kvec_t(ImageFrame *) ImageFrameVec;

static ImageVec gr_get_images_sorted_by_atime() {
	ImageVec vec;
	kv_init(vec);
	if (kh_size(images) == 0)
		return vec;
	kv_resize(Image *, vec, kh_size(images));
	Image *img = NULL;
	kh_foreach_value(images, img, { kv_push(Image *, vec, img); });
	qsort(vec.a, kv_size(vec), sizeof(Image *), gr_cmp_images_by_atime);
	return vec;
}

static ImagePlacementVec gr_get_placements_sorted_by_atime() {
	ImagePlacementVec vec;
	kv_init(vec);
	if (total_placement_count == 0)
		return vec;
	kv_resize(ImagePlacement *, vec, total_placement_count);
	Image *img = NULL;
	ImagePlacement *placement = NULL;
	kh_foreach_value(images, img, {
		kh_foreach_value(img->placements, placement, {
			kv_push(ImagePlacement *, vec, placement);
		});
	});
	qsort(vec.a, kv_size(vec), sizeof(ImagePlacement *),
	      gr_cmp_placements_by_atime);
	return vec;
}

static ImageFrameVec gr_get_frames_sorted_by_atime() {
	ImageFrameVec frames;
	kv_init(frames);
	Image *img = NULL;
	kh_foreach_value(images, img, {
		foreach_frame(*img, frame, {
			kv_push(ImageFrame *, frames, frame);
		});
	});
	qsort(frames.a, kv_size(frames), sizeof(ImageFrame *),
	      gr_cmp_frames_by_atime);
	return frames;
}

typedef struct {

	int64_t score;
	union {
		ImagePlacement *placement;
		ImageFrame *frame;
	};

	int frameidx;
} UnloadableObject;

typedef kvec_t(UnloadableObject) UnloadableObjectVec;

static int gr_cmp_unloadable_objects(const void *a, const void *b) {
	UnloadableObject *obj_a = (UnloadableObject *)a;
	UnloadableObject *obj_b = (UnloadableObject *)b;
	return obj_a->score - obj_b->score;
}

static void gr_unload_object(UnloadableObject *obj) {
	if (obj->frameidx) {
		if (obj->placement->protected_frame == obj->frameidx)
			return;
		gr_unload_pixmap(obj->placement, obj->frameidx);
	} else {
		gr_unload_frame(obj->frame);
	}
}

static Milliseconds gr_recency_threshold(Image *img) {
	return img->total_duration * 2 + 1000;
}

static UnloadableObject gr_unloadable_object_for_frame(Milliseconds now,
						       ImageFrame *frame) {
	UnloadableObject obj = {0};
	obj.frameidx = 0;
	obj.frame = frame;
	Milliseconds atime = frame->atime;
	obj.score = atime;
	if (atime >= now - gr_recency_threshold(frame->image)) {

		obj.score = now + 1000 + rand() % 1000;
	}
	return obj;
}

static UnloadableObject
gr_unloadable_object_for_pixmap(Milliseconds now, ImageFrame *frame,
				ImagePlacement *placement) {
	UnloadableObject obj = {0};
	obj.frameidx = frame->index;
	obj.placement = placement;
	obj.score = placement->atime;

	Milliseconds atime = MIN(placement->atime, frame->atime);
	obj.score = atime;
	if (atime >= now - gr_recency_threshold(frame->image)) {

		int num_frames = gr_last_frame_index(frame->image);
		int dist = frame->index - frame->image->current_frame;
		if (dist < 0)
			dist += num_frames;
		obj.score =
			now + 1000 + (num_frames - dist) * 1000 / num_frames;

		float imlib_size = gr_frame_current_ram_size(frame);
		float pixmap_size =
			gr_placement_single_frame_ram_size(placement);
		obj.score +=
			2000 * (imlib_size / (imlib_size + pixmap_size) - 0.5);
	}
	return obj;
}

static UnloadableObjectVec
gr_get_unloadable_objects_sorted_by_score(Milliseconds now) {
	UnloadableObjectVec objects;
	kv_init(objects);
	Image *img = NULL;
	ImagePlacement *placement = NULL;
	kh_foreach_value(images, img, {
		foreach_frame(*img, frame, {
			if (frame->imlib_object) {
				kv_push(UnloadableObject, objects,
					gr_unloadable_object_for_frame(now,
								       frame));
			}
			int frameidx = frame->index;
			kh_foreach_value(img->placements, placement, {
				if (!gr_get_frame_pixmap(placement, frameidx))
					continue;
				kv_push(UnloadableObject, objects,
					gr_unloadable_object_for_pixmap(
						now, frame, placement));
			});
		});
	});
	qsort(objects.a, kv_size(objects), sizeof(UnloadableObject),
	      gr_cmp_unloadable_objects);
	return objects;
}

static inline unsigned apply_tolerance(unsigned limit) {
	return limit + (unsigned)(limit * graphics_excess_tolerance_ratio);
}

static void gr_check_limits() {
	Milliseconds now = gr_now_ms();
	ImageVec images_sorted = {0};
	ImagePlacementVec placements_sorted = {0};
	ImageFrameVec frames_sorted = {0};
	UnloadableObjectVec objects_sorted = {0};
	int images_begin = 0;
	int placements_begin = 0;
	char changed = 0;

	if (kh_size(images) > apply_tolerance(graphics_max_total_placements)) {
		GR_LOG("Too many images: %d\n", kh_size(images));
		changed = 1;
		images_sorted = gr_get_images_sorted_by_atime();
		int to_delete = kv_size(images_sorted) -
				graphics_max_total_placements;
		for (; images_begin < to_delete; images_begin++)
			gr_delete_image(images_sorted.a[images_begin]);
	}

	if (total_placement_count >
	    apply_tolerance(graphics_max_total_placements)) {
		GR_LOG("Too many placements: %d\n", total_placement_count);
		changed = 1;
		placements_sorted = gr_get_placements_sorted_by_atime();
		int to_delete = kv_size(placements_sorted) -
				graphics_max_total_placements;
		for (; placements_begin < to_delete; placements_begin++) {
			ImagePlacement *placement =
				placements_sorted.a[placements_begin];
			if (placement->protected_frame)
				break;
			gr_delete_placement(placement);
		}
	}

	if (images_disk_size >
	    apply_tolerance(graphics_total_file_cache_size)) {
		GR_LOG("Too big disk cache: %ld KiB\n",
		       images_disk_size / 1024);
		changed = 1;
		frames_sorted = gr_get_frames_sorted_by_atime();
		for (int i = 0; i < kv_size(frames_sorted); i++) {
			if (images_disk_size <= graphics_total_file_cache_size)
				break;
			gr_delete_imagefile(kv_A(frames_sorted, i));
		}
	}

	if (images_ram_size > apply_tolerance(graphics_max_total_ram_size)) {
		changed = 1;
		int frames_begin = 0;
		GR_LOG("Too much ram: %ld KiB\n", images_ram_size / 1024);
		objects_sorted = gr_get_unloadable_objects_sorted_by_score(now);
		for (int i = 0; i < kv_size(objects_sorted); i++) {
			if (images_ram_size <= graphics_max_total_ram_size)
				break;
			gr_unload_object(&kv_A(objects_sorted, i));
		}
	}
	if (changed) {
		Milliseconds end = gr_now_ms();
		GR_LOG("After cleaning:  ram: %ld KiB  disk: %ld KiB  "
		       "img count: %d  placement count: %d  Took %ld ms\n",
		       images_ram_size / 1024, images_disk_size / 1024,
		       kh_size(images), total_placement_count, end - now);
	}
	kv_destroy(images_sorted);
	kv_destroy(placements_sorted);
	kv_destroy(frames_sorted);
	kv_destroy(objects_sorted);
}

void gr_unload_images_to_reduce_ram() {
	Image *img = NULL;
	ImagePlacement *placement = NULL;
	kh_foreach_value(images, img, {
		kh_foreach_value(img->placements, placement, {
			if (placement->protected_frame)
				continue;
			gr_unload_placement(placement);
		});
		gr_unload_all_frames(img);
	});
}

static inline void gr_copy_pixels(DATA32 *to, unsigned char *from, int format,
				  size_t num_pixels) {
	size_t pixel_size = format == 24 ? 3 : 4;
	if (format == 32) {
		for (unsigned i = 0; i < num_pixels; ++i) {
			unsigned byte_i = i * pixel_size;
			to[i] = ((DATA32)from[byte_i + 2]) |
				((DATA32)from[byte_i + 1]) << 8 |
				((DATA32)from[byte_i]) << 16 |
				((DATA32)from[byte_i + 3]) << 24;
		}
	} else {
		for (unsigned i = 0; i < num_pixels; ++i) {
			unsigned byte_i = i * pixel_size;
			to[i] = ((DATA32)from[byte_i + 2]) |
				((DATA32)from[byte_i + 1]) << 8 |
				((DATA32)from[byte_i]) << 16 | 0xFF000000;
		}
	}
}

static void gr_load_raw_pixel_data_uncompressed(DATA32 *data, FILE *file,
						int format,
						size_t total_pixels) {
	unsigned char chunk[BUFSIZ];
	size_t pixel_size = format == 24 ? 3 : 4;
	size_t chunk_size_pix = BUFSIZ / 4;
	size_t chunk_size_bytes = chunk_size_pix * pixel_size;
	size_t bytes = total_pixels * pixel_size;
	for (size_t chunk_start_pix = 0; chunk_start_pix < total_pixels;
	     chunk_start_pix += chunk_size_pix) {
		size_t read_size = fread(chunk, 1, chunk_size_bytes, file);
		size_t read_pixels = read_size / pixel_size;
		if (chunk_start_pix + read_pixels > total_pixels)
			read_pixels = total_pixels - chunk_start_pix;
		gr_copy_pixels(data + chunk_start_pix, chunk, format,
			       read_pixels);
	}
}

#define COMPRESSED_CHUNK_SIZE BUFSIZ
#define DECOMPRESSED_CHUNK_SIZE (BUFSIZ * 4)

static int gr_load_raw_pixel_data_compressed(DATA32 *data, FILE *file,
					     int format, size_t total_pixels) {
	size_t pixel_size = format == 24 ? 3 : 4;
	unsigned char compressed_chunk[COMPRESSED_CHUNK_SIZE];
	unsigned char decompressed_chunk[DECOMPRESSED_CHUNK_SIZE];

	z_stream strm;
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.next_out = decompressed_chunk;
	strm.avail_out = DECOMPRESSED_CHUNK_SIZE;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	int ret = inflateInit(&strm);
	if (ret != Z_OK)
	    return 1;

	int error = 0;
	int progress = 0;
	size_t total_copied_pixels = 0;
	while (1) {

		if (strm.avail_in <= COMPRESSED_CHUNK_SIZE / 4) {

			memmove(compressed_chunk, strm.next_in, strm.avail_in);
			strm.next_in = compressed_chunk;

			size_t bytes_read = fread(
				compressed_chunk + strm.avail_in, 1,
				COMPRESSED_CHUNK_SIZE - strm.avail_in, file);
			strm.avail_in += bytes_read;
			if (bytes_read != 0)
				progress = 1;
		}

		int ret = inflate(&strm, Z_SYNC_FLUSH);
		if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR) {
			error = 1;
			fprintf(stderr,
				"error: could not decompress the image, error "
				"%s\n",
				ret == Z_MEM_ERROR ? "Z_MEM_ERROR"
						   : "Z_DATA_ERROR");
			break;
		}

		size_t full_pixels =
			(DECOMPRESSED_CHUNK_SIZE - strm.avail_out) / pixel_size;

		if (full_pixels > total_pixels - total_copied_pixels)
			full_pixels = total_pixels - total_copied_pixels;
		if (full_pixels > 0) {

			gr_copy_pixels(data, decompressed_chunk, format,
				       full_pixels);
			data += full_pixels;
			total_copied_pixels += full_pixels;
			if (total_copied_pixels >= total_pixels) {

				break;
			}

			size_t copied_bytes = full_pixels * pixel_size;
			size_t leftover =
				(DECOMPRESSED_CHUNK_SIZE - strm.avail_out) -
				copied_bytes;
			memmove(decompressed_chunk,
				decompressed_chunk + copied_bytes, leftover);
			strm.next_out -= copied_bytes;
			strm.avail_out += copied_bytes;
			progress = 1;
		}

		if (!progress)
			break;
		progress = 0;
	}

	inflateEnd(&strm);
	return error;
}

#undef COMPRESSED_CHUNK_SIZE
#undef DECOMPRESSED_CHUNK_SIZE

static Imlib_Image gr_load_raw_pixel_data(ImageFrame *frame,
					  const char *filename) {
	size_t total_pixels = frame->data_pix_width * frame->data_pix_height;
	if (total_pixels * 4 > graphics_max_single_image_ram_size) {
		fprintf(stderr,
			"error: image %u frame %u is too big too load: %zu > %u\n",
			frame->image->image_id, frame->index, total_pixels * 4,
			graphics_max_single_image_ram_size);
		return NULL;
	}

	FILE* file = fopen(filename, "rb");
	if (!file) {
		fprintf(stderr,
			"error: could not open image file: %s\n",
			sanitized_filename(filename));
		return NULL;
	}

	Imlib_Image image = imlib_create_image(frame->data_pix_width,
					       frame->data_pix_height);
	if (!image) {
		fprintf(stderr,
			"error: could not create an image of size %d x %d\n",
			frame->data_pix_width, frame->data_pix_height);
		fclose(file);
		return NULL;
	}

	imlib_context_set_image(image);
	imlib_image_set_has_alpha(1);
	DATA32* data = imlib_image_get_data();

	if (frame->compression == 0) {
		gr_load_raw_pixel_data_uncompressed(data, file, frame->format,
						    total_pixels);
	} else {
		int ret = gr_load_raw_pixel_data_compressed(
			data, file, frame->format, total_pixels);
		if (ret != 0) {
			imlib_image_put_back_data(data);
			imlib_free_image();
			fclose(file);
			return NULL;
		}
	}

	fclose(file);
	imlib_image_put_back_data(data);
	return image;
}

static void gr_load_imlib_object(ImageFrame *frame) {
	if (frame->imlib_object)
		return;

	if (frame->status < STATUS_UPLOADING_SUCCESS)
		return;
	if (frame->disk_size == 0) {

		gr_try_restore_imagefile(frame);
	}
	if (frame->disk_size == 0) {
		if (frame->status != STATUS_RAM_LOADING_ERROR &&
		    frame->status >= STATUS_UPLOADING_SUCCESS) {
			fprintf(stderr,
				"error: cached image was deleted: %u frame %u\n",
				frame->image->image_id, frame->index);
		}
		frame->status = STATUS_RAM_LOADING_ERROR;
		return;
	}

	if (frame->ram_loading_in_progress) {
		if (frame->status != STATUS_RAM_LOADING_ERROR) {
			fprintf(stderr,
				"error: recursive loading of image %u frame "
				"%u\n",
				frame->image->image_id, frame->index);
		}
		frame->status = STATUS_RAM_LOADING_ERROR;
		return;
	}
	frame->ram_loading_in_progress = 1;

	ImageFrame *bg_frame = NULL;
	if (frame->background_frame_index) {
		bg_frame = gr_get_frame(frame->image,
					frame->background_frame_index);
		if (!bg_frame) {
			if (frame->status != STATUS_RAM_LOADING_ERROR) {
				fprintf(stderr,
					"error: could not find background "
					"frame %d for image %u frame %d\n",
					frame->background_frame_index,
					frame->image->image_id, frame->index);
				frame->status = STATUS_RAM_LOADING_ERROR;
			}
			return;
		}
		gr_load_imlib_object(bg_frame);
		if (!bg_frame->imlib_object) {
			if (frame->status != STATUS_RAM_LOADING_ERROR) {
				fprintf(stderr,
					"error: could not load background "
					"frame %d for image %u frame %d\n",
					frame->background_frame_index,
					frame->image->image_id, frame->index);
			}
			frame->status = STATUS_RAM_LOADING_ERROR;
			return;
		}
	}

	frame->ram_loading_in_progress = 0;

	Milliseconds loading_start = gr_now_ms();

	Imlib_Image frame_data_image = NULL;
	char filename[MAX_FILENAME_SIZE];
	gr_get_frame_filename(frame, filename, MAX_FILENAME_SIZE);
	GR_LOG("Loading image: %s\n", sanitized_filename(filename));
	if (frame->format == 100)
		frame_data_image = imlib_load_image(filename);
	if (frame->format == 32 || frame->format == 24)
		frame_data_image = gr_load_raw_pixel_data(frame, filename);
	debug_loaded_files_counter++;

	if (!frame_data_image) {
		if (frame->status != STATUS_RAM_LOADING_ERROR) {
			fprintf(stderr, "error: could not load image: %s\n",
				sanitized_filename(filename));
		}
		frame->status = STATUS_RAM_LOADING_ERROR;
		return;
	}

	imlib_context_set_image(frame_data_image);
	int frame_data_width = imlib_image_get_width();
	int frame_data_height = imlib_image_get_height();

	if (frame_data_width * frame_data_height * 4 >
	    graphics_max_single_image_ram_size) {
		if (frame->status != STATUS_RAM_LOADING_ERROR) {
			fprintf(stderr,
				"error: image %u frame %u is too big too load: "
				"%d x %d * 4 = %d > %u\n",
				frame->image->image_id, frame->index,
				frame_data_width, frame_data_height,
				frame_data_width * frame_data_height * 4,
				graphics_max_single_image_ram_size);
		}
		imlib_free_image();
		frame->status = STATUS_RAM_LOADING_ERROR;
		return;
	}

	GR_LOG("Successfully loaded, size %d x %d\n", frame_data_width,
	       frame_data_height);

	if (frame->index == 1 && frame->image->pix_width == 0 &&
	    frame->image->pix_height == 0) {
		frame->image->pix_width = frame_data_width;
		frame->image->pix_height = frame_data_height;
	}

	int image_width = frame->image->pix_width;
	int image_height = frame->image->pix_height;

	if (frame->background_color != 0 || bg_frame ||
	    image_width != frame_data_width ||
	    image_height != frame_data_height) {
		GR_LOG("Composing the frame bg = 0x%08X, bgframe = %d\n",
		       frame->background_color, frame->background_frame_index);
		Imlib_Image composed_image = imlib_create_image(
			image_width, image_height);
		imlib_context_set_image(composed_image);
		imlib_image_set_has_alpha(1);
		imlib_context_set_anti_alias(0);

		imlib_context_set_blend(0);
		if (bg_frame && bg_frame->imlib_object) {
			imlib_blend_image_onto_image(
				bg_frame->imlib_object, 1, 0, 0,
				image_width, image_height, 0, 0,
				image_width, image_height);
		} else {
			int r = (frame->background_color >> 24) & 0xFF;
			int g = (frame->background_color >> 16) & 0xFF;
			int b = (frame->background_color >> 8) & 0xFF;
			int a = frame->background_color & 0xFF;
			imlib_context_set_color(r, g, b, a);
			imlib_image_fill_rectangle(0, 0, image_width,
						   image_height);
		}

		imlib_context_set_blend(1);
		imlib_blend_image_onto_image(
			frame_data_image, 1, 0, 0, frame->data_pix_width,
			frame->data_pix_height, frame->x, frame->y,
			frame->data_pix_width, frame->data_pix_height);

		imlib_context_set_image(frame_data_image);
		imlib_free_image();

		frame_data_image = composed_image;
	}

	frame->imlib_object = frame_data_image;

	images_ram_size += gr_frame_current_ram_size(frame);
	frame->status = STATUS_RAM_LOADING_SUCCESS;

	Milliseconds loading_end = gr_now_ms();
	GR_LOG("After loading image %u frame %d ram: %ld KiB  (+ %u KiB)  Took "
	       "%ld ms\n",
	       frame->image->image_id, frame->index, images_ram_size / 1024,
	       gr_frame_current_ram_size(frame) / 1024,
	       loading_end - loading_start);
}

static void gr_premultiply_alpha(DATA32 *data, size_t num_pixels) {
	for (size_t i = 0; i < num_pixels; ++i) {
		DATA32 pixel = data[i];
		unsigned char a = pixel >> 24;
		if (a == 0) {
			data[i] = 0;
		} else if (a != 255) {
			unsigned char b = (pixel & 0xFF) * a / 255;
			unsigned char g = ((pixel >> 8) & 0xFF) * a / 255;
			unsigned char r = ((pixel >> 16) & 0xFF) * a / 255;
			data[i] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
}

void gr_compute_pixmap_transformation(ImagePlacement *placement) {

	gr_infer_placement_size_maybe(placement);

	int box_w = (int)placement->cols * placement->scaled_cw;
	int box_h = (int)placement->rows * placement->scaled_ch;

	int src_w = placement->src_pix_width;
	int src_h = placement->src_pix_height;

	char box_too_small = box_w < src_w || box_h < src_h;
	char mode = placement->scale_mode;

	PixmapTransformation *tr = &placement->pixmap_transformation;

	if (src_w <= 0 || src_h <= 0) {
		tr->dst_x = tr->dst_y = tr->dst_w = tr->dst_h = 0;
	} else if (mode == SCALE_MODE_FILL) {
		tr->dst_x = tr->dst_y = 0;
		tr->dst_w = box_w;
		tr->dst_h = box_h;
	} else if (mode == SCALE_MODE_NONE ||
		   (mode == SCALE_MODE_NONE_OR_CONTAIN && !box_too_small)) {
		tr->dst_x = tr->dst_y = 0;
		tr->dst_w = src_w;
		tr->dst_h = src_h;
	} else {
		if (mode != SCALE_MODE_CONTAIN &&
		    mode != SCALE_MODE_NONE_OR_CONTAIN) {
			fprintf(stderr,
				"warning: unknown scale mode %u, using "
				"'contain' instead\n",
				mode);
		}
		if (box_w * src_h > src_w * box_h) {

			tr->dst_h = box_h;
			tr->dst_y = 0;
			tr->dst_w = src_w * box_h / src_h;
			tr->dst_x = (box_w - tr->dst_w) / 2;
		} else {

			tr->dst_w = box_w;
			tr->dst_x = 0;
			tr->dst_h = src_h * box_w / src_w;
			tr->dst_y = (box_h - tr->dst_h) / 2;
		}
	}

	tr->dst_w = MAX(1, tr->dst_w);
	tr->dst_h = MAX(1, tr->dst_h);

	tr->pixmap_w = tr->dst_w;
	tr->pixmap_h = tr->dst_h;

	if (tr->pixmap_w * tr->pixmap_h > src_w * src_h) {
		tr->pixmap_w = MAX(1, src_w);
		tr->pixmap_h = MAX(1, src_h);
	}

	if (tr->pixmap_w * tr->pixmap_h * 4 >
	    graphics_max_single_image_ram_size) {
		double scale = sqrt((double)graphics_max_single_image_ram_size /
				    (tr->pixmap_w * tr->pixmap_h * 4));
		tr->pixmap_w = MAX(1, (int)(tr->pixmap_w * scale));
		tr->pixmap_h = MAX(1, (int)(tr->pixmap_h * scale));
	}
}

Imlib_Image gr_create_scaled_image_object(ImagePlacement *placement,
					  ImageFrame *frame) {

	int src_x = placement->src_pix_x;
	int src_y = placement->src_pix_y;
	int src_w = placement->src_pix_width;
	int src_h = placement->src_pix_height;

	int pixmap_w = placement->pixmap_transformation.pixmap_w;
	int pixmap_h = placement->pixmap_transformation.pixmap_h;

	if (pixmap_w * pixmap_h * 4 > graphics_max_single_image_ram_size) {
		fprintf(stderr,
			"error: placement %u/%u would be too big to load: %d x "
			"%d x 4 > %u\n",
			placement->image->image_id, placement->placement_id,
			pixmap_w, pixmap_h, graphics_max_single_image_ram_size);
		return 0;
	}

	if (pixmap_w == 0 || pixmap_h == 0)
		fprintf(stderr, "warning: image of zero size\n");

	imlib_context_set_image(frame->imlib_object);
	imlib_context_set_anti_alias(1);
	imlib_context_set_blend(1);

	return imlib_create_cropped_scaled_image(src_x, src_y, src_w, src_h,
						 pixmap_w, pixmap_h);
}

Pixmap gr_load_pixmap(ImagePlacement *placement, int frameidx, int cw, int ch) {
	Milliseconds loading_start = gr_now_ms();
	Image *img = placement->image;
	ImageFrame *frame = gr_get_frame(img, frameidx);

	gr_touch_placement(placement);
	if (frame)
		gr_touch_frame(frame);

	if (placement->scaled_cw != cw || placement->scaled_ch != ch) {
		gr_unload_placement(placement);
		placement->scaled_cw = cw;
		placement->scaled_ch = ch;
		gr_compute_pixmap_transformation(placement);
	}

	Pixmap pixmap = gr_get_frame_pixmap(placement, frameidx);
	if (pixmap)
		return pixmap;

	GR_LOG("Loading placement: %u/%u frame %u\n", img->image_id,
	       placement->placement_id, frameidx);

	if (placement->pixmap_transformation.pixmap_w == 0 ||
	    placement->pixmap_transformation.pixmap_h == 0) {
		GR_LOG("Not loading because the pixmap size is zero\n");
		return 0;
	}

	if (!frame) {
		fprintf(stderr,
			"error: could not find frame %u for image %u\n",
			frameidx, img->image_id);
		return 0;
	}
	gr_load_imlib_object(frame);
	if (!frame->imlib_object)
		return 0;

	Imlib_Image scaled_image =
		gr_create_scaled_image_object(placement, frame);
	if (!scaled_image)
		return 0;
	imlib_context_set_image(scaled_image);
	int pixmap_w = imlib_image_get_width();
	int pixmap_h = imlib_image_get_height();

	DATA32 *data = imlib_image_get_data();
	gr_premultiply_alpha(data, pixmap_w * pixmap_h);

	Display *disp = imlib_context_get_display();
	Visual *vis = imlib_context_get_visual();
	Colormap cmap = imlib_context_get_colormap();
	Drawable drawable = imlib_context_get_drawable();
	if (!drawable)
		drawable = DefaultRootWindow(disp);
	pixmap = XCreatePixmap(disp, drawable, pixmap_w, pixmap_h, 32);
	XVisualInfo visinfo = {0};
	Status visual_found = XMatchVisualInfo(disp, DefaultScreen(disp), 32,
					       TrueColor, &visinfo) ||
			      XMatchVisualInfo(disp, DefaultScreen(disp), 24,
					       TrueColor, &visinfo);
	if (!visual_found) {
		fprintf(stderr,
			"error: could not find 32-bit TrueColor visual\n");

		visinfo.visual = NULL;
	}
	XImage *ximage = XCreateImage(disp, visinfo.visual, 32, ZPixmap, 0,
				      (char *)data, pixmap_w, pixmap_h, 32, 0);
	if (!ximage) {
		fprintf(stderr, "error: could not create XImage\n");
		imlib_image_put_back_data(data);
		imlib_free_image();
		return 0;
	}
	GC gc = XCreateGC(disp, pixmap, 0, NULL);
	XPutImage(disp, pixmap, gc, ximage, 0, 0, 0, 0, pixmap_w, pixmap_h);
	XFreeGC(disp, gc);

	ximage->data = NULL;
	XDestroyImage(ximage);
	imlib_image_put_back_data(data);
	imlib_free_image();

	gr_set_frame_pixmap(placement, frameidx, pixmap);
	images_ram_size += gr_placement_single_frame_ram_size(placement);
	debug_loaded_pixmaps_counter++;

	Milliseconds loading_end = gr_now_ms();
	GR_LOG("After loading placement %u/%u frame %d ram: %ld KiB  (+ %u "
	       "KiB)  Took %ld ms\n",
	       frame->image->image_id, placement->placement_id, frame->index,
	       images_ram_size / 1024,
	       gr_placement_single_frame_ram_size(placement) / 1024,
	       loading_end - loading_start);

	placement->protected_frame = frameidx;
	gr_check_limits();
	placement->protected_frame = 0;

	return pixmap;
}

static int gr_create_cache_dir() {
	const char *tmpdir = getenv("TMPDIR");
	if (!tmpdir || !*tmpdir)
		tmpdir = "/tmp";
	snprintf(cache_dir, sizeof(cache_dir), "%s/st-images-XXXXXX", tmpdir);
	if (!mkdtemp(cache_dir)) {
		fprintf(stderr,
			"error: could not create temporary dir from template "
			"%s\n",
			sanitized_filename(cache_dir));
		return 0;
	}
	GR_LOG("Graphics cache directory: %s\n", cache_dir);
	return 1;
}

static void gr_make_sure_tmpdir_exists() {
	struct stat st;
	if (stat(cache_dir, &st) == 0 && S_ISDIR(st.st_mode))
		return;
	fprintf(stderr,
		"error: %s is not a directory, will need to create a new "
		"graphics cache directory\n",
		sanitized_filename(cache_dir));
	gr_create_cache_dir();
}

void gr_init(Display *disp, Visual *vis, Colormap cm) {

	clock_gettime(CLOCK_MONOTONIC, &initialization_time);

	if (!gr_create_cache_dir())
		abort();

	imlib_context_set_display(disp);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cm);
	imlib_context_set_anti_alias(1);
	imlib_context_set_blend(1);

	imlib_set_cache_size(0);

	for (size_t i = 0; i < 256; ++i)
		reverse_table[i] = 255 - i;

	images = kh_init(id2image);
	kv_init(next_redraw_times);

	atexit(gr_deinit);
}

void gr_deinit() {
	static int deinitialized = 0;
	if (deinitialized)
		return;
	deinitialized = 1;

	remove(cache_dir);
	kv_destroy(next_redraw_times);
	next_redraw_times.a = NULL;
	next_redraw_times.n = next_redraw_times.m = 0;
	if (images) {

		gr_delete_all_images();

		kh_destroy(id2image, images);
		images = NULL;
	}
}

static const char *gr_ago(Milliseconds diff) {
	static char result[32];
	double seconds = (double)diff / 1000.0;
	if (seconds < 1)
		snprintf(result, sizeof(result), "%.2f sec ago", seconds);
	else if (seconds < 60)
		snprintf(result, sizeof(result), "%d sec ago", (int)seconds);
	else if (seconds < 3600)
		snprintf(result, sizeof(result), "%d min %d sec ago",
			 (int)(seconds / 60), (int)(seconds) % 60);
	else {
		snprintf(result, sizeof(result), "%d hr %d min %d sec ago",
			 (int)(seconds / 3600), (int)(seconds) % 3600 / 60,
			 (int)(seconds) % 60);
	}
	return result;
}

static void fprintf_ind(FILE *file, int ind, const char *format, ...) {
	fprintf(file, "%*s", ind, "");
	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);
}

static void gr_dump_image_info(FILE *file, Image *img, int ind) {
	if (!img) {
		fprintf_ind(file, ind, "Image is NULL\n");
		return;
	}
	Milliseconds now = gr_now_ms();
	fprintf_ind(file, ind, "Image %u\n", img->image_id);
	ind += 4;
	fprintf_ind(file, ind, "number: %u\n", img->image_number);
	fprintf_ind(file, ind, "global command index: %lu\n",
		img->global_command_index);
	fprintf_ind(file, ind, "accessed: %ld  %s\n", img->atime,
		    gr_ago(now - img->atime));
	fprintf_ind(file, ind, "pix size: %ux%u\n", img->pix_width,
		    img->pix_height);
	fprintf_ind(file, ind, "cur frame start time: %ld  %s\n",
		    img->current_frame_time,
		    gr_ago(now - img->current_frame_time));
	if (img->next_redraw)
		fprintf_ind(file, ind, "next redraw: %ld  in %ld ms\n",
			    img->next_redraw, img->next_redraw - now);
	fprintf_ind(file, ind, "total disk size: %u KiB\n",
		img->total_disk_size / 1024);
	fprintf_ind(file, ind, "total duration: %d\n", img->total_duration);
	fprintf_ind(file, ind, "frames: %d\n", gr_last_frame_index(img));
	fprintf_ind(file, ind, "cur frame: %d\n", img->current_frame);
	fprintf_ind(file, ind, "animation state: %d\n", img->animation_state);
	fprintf_ind(file, ind, "default_placement: %u\n",
		    img->default_placement);
}

static void gr_dump_frame_info(FILE *file, ImageFrame *frame, int ind) {
	if (!frame) {
		fprintf_ind(file, ind, "Frame is NULL\n");
		return;
	}
	Milliseconds now = gr_now_ms();
	fprintf_ind(file, ind, "Frame %d\n", frame->index);
	ind += 4;
	if (frame->index == 0) {
		fprintf_ind(file, ind, "NOT INITIALIZED\n");
		return;
	}
	if (frame->original_filename) {
		fprintf_ind(file, ind, "original filename (sanitized): %s\n",
			    sanitized_filename(frame->original_filename));
		fprintf_ind(file, ind, "original file %s\n",
			    gr_is_original_file_still_available(frame)
				    ? "is still available"
				    : "is NOT available anymore");
	}
	if (frame->uploading_failure)
		fprintf_ind(file, ind, "uploading failure: %s\n",
			    image_uploading_failure_strings
				    [frame->uploading_failure]);
	fprintf_ind(file, ind, "gap: %d\n", frame->gap);
	fprintf_ind(file, ind, "accessed: %ld  %s\n", frame->atime,
		    gr_ago(now - frame->atime));
	fprintf_ind(file, ind, "data pix size: %ux%u\n", frame->data_pix_width,
		    frame->data_pix_height);
	char filename[MAX_FILENAME_SIZE];
	gr_get_frame_filename(frame, filename, MAX_FILENAME_SIZE);
	if (access(filename, F_OK) != -1)
		fprintf_ind(file, ind, "file: %s\n",
			    sanitized_filename(filename));
	else
		fprintf_ind(file, ind, "not on disk\n");
	fprintf_ind(file, ind, "disk size: %u KiB\n", frame->disk_size / 1024);
	if (frame->imlib_object) {
		unsigned ram_size = gr_frame_current_ram_size(frame);
		fprintf_ind(file, ind,
			    "loaded into ram, size: %d "
			    "KiB\n",
			    ram_size / 1024);
	} else {
		fprintf_ind(file, ind, "not loaded into ram\n");
	}
}

static void gr_dump_placement_info(FILE *file, ImagePlacement *placement,
				   int ind) {
	if (!placement) {
		fprintf_ind(file, ind, "Placement is NULL\n");
		return;
	}
	Milliseconds now = gr_now_ms();
	fprintf_ind(file, ind, "Placement %u\n", placement->placement_id);
	ind += 4;
	fprintf_ind(file, ind, "accessed: %ld  %s\n", placement->atime,
		    gr_ago(now - placement->atime));
	fprintf_ind(file, ind, "scale_mode: %u\n", placement->scale_mode);
	fprintf_ind(file, ind, "size: %u cols x %u rows\n", placement->cols,
		    placement->rows);
	fprintf_ind(file, ind, "cell size: %ux%u\n", placement->scaled_cw,
		    placement->scaled_ch);
	PixmapTransformation *tr = &placement->pixmap_transformation;
	fprintf_ind(file, ind, "pixmap size: %ux%u\n", tr->pixmap_w,
		    tr->pixmap_h);
	fprintf_ind(file, ind, "dst size: %ux%u  offset: (%d, %d)\n", tr->dst_w,
		    tr->dst_h, tr->dst_x, tr->dst_y);
	fprintf_ind(file, ind, "ram per frame: %u KiB\n",
		    gr_placement_single_frame_ram_size(placement) / 1024);
	unsigned ram_size = gr_placement_current_ram_size(placement);
	fprintf_ind(file, ind, "ram size: %d KiB\n", ram_size / 1024);
}

static void gr_dump_placement_pixmaps(FILE *file, ImagePlacement *placement,
				      int ind) {
	if (!placement)
		return;
	int frameidx = 1;
	foreach_pixmap(*placement, pixmap, {
		fprintf_ind(file, ind, "Frame %d pixmap %lu\n", frameidx,
			    pixmap);
		++frameidx;
	});
}

void gr_dump_state() {
	FILE *file = stderr;
	int ind = 0;
	fprintf_ind(file, ind, "======= Graphics module state dump =======\n");
	fprintf_ind(file, ind,
		"sizeof(Image) = %lu  sizeof(ImageFrame) = %lu  "
		"sizeof(ImagePlacement) = %lu\n",
		sizeof(Image), sizeof(ImageFrame), sizeof(ImagePlacement));
	fprintf_ind(file, ind, "Image count: %u\n", kh_size(images));
	fprintf_ind(file, ind, "Placement count: %u\n", total_placement_count);
	fprintf_ind(file, ind, "Estimated RAM usage: %ld KiB\n",
		images_ram_size / 1024);
	fprintf_ind(file, ind, "Estimated Disk usage: %ld KiB\n",
		images_disk_size / 1024);

	Milliseconds now = gr_now_ms();

	int64_t images_ram_size_computed = 0;
	int64_t images_disk_size_computed = 0;

	Image *img = NULL;
	ImagePlacement *placement = NULL;
	kh_foreach_value(images, img, {
		fprintf_ind(file, ind, "----------------\n");
		gr_dump_image_info(file, img, 0);
		int64_t total_disk_size_computed = 0;
		int total_duration_computed = 0;
		foreach_frame(*img, frame, {
			gr_dump_frame_info(file, frame, 4);
			if (frame->image != img)
				fprintf_ind(file, 8,
					    "ERROR: WRONG IMAGE POINTER\n");
			total_duration_computed += frame->gap;
			images_disk_size_computed += frame->disk_size;
			total_disk_size_computed += frame->disk_size;
			if (frame->imlib_object)
				images_ram_size_computed +=
					gr_frame_current_ram_size(frame);
		});
		if (img->total_disk_size != total_disk_size_computed) {
			fprintf_ind(file, ind,
				"    ERROR: total_disk_size is %u, but "
				"computed value is %ld\n",
				img->total_disk_size, total_disk_size_computed);
		}
		if (img->total_duration != total_duration_computed) {
			fprintf_ind(file, ind,
				"    ERROR: total_duration is %d, but computed "
				"value is %d\n",
				img->total_duration, total_duration_computed);
		}
		kh_foreach_value(img->placements, placement, {
			gr_dump_placement_info(file, placement, 4);
			if (placement->image != img)
				fprintf_ind(file, 8,
					    "ERROR: WRONG IMAGE POINTER\n");
			fprintf_ind(file, 8,
				    "Pixmaps:\n");
			gr_dump_placement_pixmaps(file, placement, 12);
			unsigned ram_size =
				gr_placement_current_ram_size(placement);
			images_ram_size_computed += ram_size;
		});
	});
	if (images_ram_size != images_ram_size_computed) {
		fprintf_ind(file, ind,
			"ERROR: images_ram_size is %ld, but computed value "
			"is %ld\n",
			images_ram_size, images_ram_size_computed);
	}
	if (images_disk_size != images_disk_size_computed) {
		fprintf_ind(file, ind,
			"ERROR: images_disk_size is %ld, but computed value "
			"is %ld\n",
			images_disk_size, images_disk_size_computed);
	}
	fprintf_ind(file, ind, "===========================================\n");
}

void gr_preview_image(uint32_t image_id, const char *exec) {
	char command[256];
	size_t len;
	Image *img = gr_find_image(image_id);
	if (img) {
		ImageFrame *frame = &img->first_frame;
		char filename[MAX_FILENAME_SIZE];
		gr_get_frame_filename(frame, filename, MAX_FILENAME_SIZE);
		if (frame->disk_size == 0) {
			len = snprintf(command, 255,
				       "xmessage 'Image with id=%u is not "
				       "fully copied to %s'",
				       image_id, sanitized_filename(filename));
		} else {
			len = snprintf(command, 255, "%s %s &", exec,
				       sanitized_filename(filename));
		}
	} else {
		len = snprintf(command, 255,
			       "xmessage 'Cannot find image with id=%u'",
			       image_id);
	}
	if (len > 255) {
		fprintf(stderr, "error: command too long: %s\n", command);
		snprintf(command, 255, "xmessage 'error: command too long'");
	}
	if (system(command) != 0) {
		fprintf(stderr, "error: could not execute command %s\n",
			command);
	}
}

void gr_show_image_info(uint32_t image_id, uint32_t placement_id,
			uint32_t imgcol, uint32_t imgrow,
			char is_classic_placeholder, int32_t diacritic_count,
			char *st_executable) {
	char filename[MAX_FILENAME_SIZE];
	snprintf(filename, sizeof(filename), "%s/info-%u", cache_dir, image_id);
	FILE *file = fopen(filename, "w");
	if (!file) {
		perror("fopen");
		return;
	}

	fprintf(file, "image_id = %u = 0x%08X\n", image_id, image_id);
	fprintf(file, "placement_id = %u = 0x%08X\n", placement_id, placement_id);
	fprintf(file, "column = %d, row = %d\n", imgcol, imgrow);
	fprintf(file, "classic/unicode placeholder = %s\n",
		is_classic_placeholder ? "classic" : "unicode");
	fprintf(file, "original diacritic count = %d\n", diacritic_count);

	Image *img = gr_find_image(image_id);
	ImagePlacement *placement = gr_find_placement(img, placement_id);
	gr_dump_image_info(file, img, 0);
	gr_dump_placement_info(file, placement, 0);

	if (placement && placement->text_underneath && imgcol >= 1 &&
	    imgrow >= 1 && imgcol <= placement->cols &&
	    imgrow <= placement->rows) {
		fprintf(file, "Glyph underneath:\n");
		Glyph *glyph =
			&placement->text_underneath[(imgrow - 1) *
							    placement->cols +
						    imgcol - 1];
		fprintf(file, "    rune = 0x%08X\n", glyph->u);
		fprintf(file, "    bg = 0x%08X\n", glyph->bg);
		fprintf(file, "    fg = 0x%08X\n", glyph->fg);
		fprintf(file, "    decor = 0x%08X\n", glyph->decor);
		fprintf(file, "    mode = 0x%08X\n", glyph->mode);
	}
	if (img) {
		fprintf(file, "Frames:\n");
		foreach_frame(*img, frame, {
			gr_dump_frame_info(file, frame, 4);
		});
	}
	if (placement) {
		fprintf(file, "Placement pixmaps:\n");
		gr_dump_placement_pixmaps(file, placement, 4);
	}
	fclose(file);
	char *argv[] = {st_executable, "-e", "less", filename, NULL};
	if (posix_spawnp(NULL, st_executable, NULL, NULL, argv, environ) != 0) {
		perror("posix_spawnp");
		return;
	}
}

static void gr_displayinfo(Drawable buf, ImageRect *rect, int col1, int col2,
			   const char *message) {
	int w_pix = (rect->img_end_col - rect->img_start_col) * rect->cw;
	int h_pix = (rect->img_end_row - rect->img_start_row) * rect->ch;
	Display *disp = imlib_context_get_display();
	GC gc = XCreateGC(disp, buf, 0, NULL);
	char info[MAX_INFO_LEN];
	if (rect->placement_id)
		snprintf(info, MAX_INFO_LEN, "%s%u/%u [%d:%d)x[%d:%d)", message,
			 rect->image_id, rect->placement_id,
			 rect->img_start_col, rect->img_end_col,
			 rect->img_start_row, rect->img_end_row);
	else
		snprintf(info, MAX_INFO_LEN, "%s%u [%d:%d)x[%d:%d)", message,
			 rect->image_id, rect->img_start_col, rect->img_end_col,
			 rect->img_start_row, rect->img_end_row);
	XSetForeground(disp, gc, col1);
	XDrawString(disp, buf, gc, rect->screen_x_pix + 4,
		    rect->screen_y_pix + h_pix - 3, info, strlen(info));
	XSetForeground(disp, gc, col2);
	XDrawString(disp, buf, gc, rect->screen_x_pix + 2,
		    rect->screen_y_pix + h_pix - 5, info, strlen(info));
	XFreeGC(disp, gc);
}

static void gr_showrect(Drawable buf, ImageRect *rect) {
	int w_pix = (rect->img_end_col - rect->img_start_col) * rect->cw;
	int h_pix = (rect->img_end_row - rect->img_start_row) * rect->ch;
	Display *disp = imlib_context_get_display();
	GC gc = XCreateGC(disp, buf, 0, NULL);
	XSetForeground(disp, gc, 0xFF00FF00);
	XDrawRectangle(disp, buf, gc, rect->screen_x_pix, rect->screen_y_pix,
		       w_pix - 1, h_pix - 1);
	XSetForeground(disp, gc, 0xFFFF0000);
	XDrawRectangle(disp, buf, gc, rect->screen_x_pix + 1,
		       rect->screen_y_pix + 1, w_pix - 3, h_pix - 3);
	XFreeGC(disp, gc);
}

static void gr_update_next_redraw_time(int row, Milliseconds next_redraw) {
	if (next_redraw == 0)
		return;
	if (row >= kv_size(next_redraw_times)) {
		size_t old_size = kv_size(next_redraw_times);
		kv_a(Milliseconds, next_redraw_times, row);
		for (size_t i = old_size; i <= row; ++i)
			kv_A(next_redraw_times, i) = 0;
	}
	Milliseconds old_value = kv_A(next_redraw_times, row);
	if (old_value == 0 || old_value > next_redraw)
		kv_A(next_redraw_times, row) = next_redraw;
}

static void gr_drawimagerect(Drawable buf, ImageRect *rect) {
	ImagePlacement *placement =
		gr_find_image_and_placement(rect->image_id, rect->placement_id);

	if (!placement || !graphics_display_images) {
		gr_showrect(buf, rect);
		if (graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES)
			gr_displayinfo(buf, rect, 0xFF000000, 0xFFFFFFFF, "");
		return;
	}

	Image *img = placement->image;

	if (img->last_redraw < drawing_start_time) {

		int old_frame = img->current_frame;
		gr_update_frame_index(img, drawing_start_time);
		img->last_redraw = drawing_start_time;
	}

	if (img->next_redraw) {
		for (int row = rect->screen_y_row;
		     row <= rect->screen_y_row + rect->img_end_row -
					  rect->img_start_row - 1; ++row) {
			gr_update_next_redraw_time(
				row, img->next_redraw);
		}
	}

	Pixmap pixmap = gr_load_pixmap(placement, img->current_frame, rect->cw,
				       rect->ch);

	if (!pixmap) {
		gr_showrect(buf, rect);
		if (graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES)
			gr_displayinfo(buf, rect, 0xFF000000, 0xFFFFFFFF, "");
		return;
	}

	int src_x = rect->img_start_col * rect->cw;
	int src_y = rect->img_start_row * rect->ch;
	int src_w = (rect->img_end_col - rect->img_start_col) * rect->cw;
	int src_h = (rect->img_end_row - rect->img_start_row) * rect->ch;

	int window_x = rect->screen_x_pix;
	int window_y = rect->screen_y_pix;

	Display *disp = imlib_context_get_display();
	Visual *vis = imlib_context_get_visual();

	XRenderPictFormat *win_format =
		XRenderFindVisualFormat(disp, vis);
	Picture window_pic =
		XRenderCreatePicture(disp, buf, win_format, 0, NULL);

	if (rect->reverse) {
		unsigned pixmap_w = placement->pixmap_transformation.pixmap_w;
		unsigned pixmap_h = placement->pixmap_transformation.pixmap_h;
		Pixmap invpixmap =
			XCreatePixmap(disp, buf, pixmap_w, pixmap_h, 32);
		XGCValues gcv = {.function = GXcopyInverted};
		GC gc = XCreateGC(disp, invpixmap, GCFunction, &gcv);
		XCopyArea(disp, pixmap, invpixmap, gc, 0, 0, pixmap_w,
			  pixmap_h, 0, 0);
		XFreeGC(disp, gc);
		pixmap = invpixmap;
	}

	XRenderPictFormat *pic_format =
		XRenderFindStandardFormat(disp, PictStandardARGB32);

	XRenderPictureAttributes attrs = {0};
	attrs.repeat = RepeatPad;
	Picture pixmap_pic = XRenderCreatePicture(disp, pixmap, pic_format,
						  CPRepeat, &attrs);

	PixmapTransformation *tr = &placement->pixmap_transformation;
	double xs = (double)tr->pixmap_w / MAX(tr->dst_w, 1);
	double ys = (double)tr->pixmap_h / MAX(tr->dst_h, 1);

	XTransform xform = {{
	    { XDoubleToFixed(xs), XDoubleToFixed( 0), XDoubleToFixed( 0) },
	    { XDoubleToFixed( 0), XDoubleToFixed(ys), XDoubleToFixed( 0) },
	    { XDoubleToFixed( 0), XDoubleToFixed( 0), XDoubleToFixed( 1) }
	}};

	XRenderSetPictureTransform(disp, pixmap_pic, &xform);
	XRenderSetPictureFilter(disp, pixmap_pic, FilterBilinear, NULL, 0);

	src_x -= tr->dst_x;
	src_y -= tr->dst_y;

	if (src_x < 0) {
		window_x += -src_x;
		src_w -= -src_x;
		src_x = 0;
	}
	if (src_y < 0) {
		window_y += -src_y;
		src_h -= -src_y;
		src_y = 0;
	}

	src_w = MIN(src_w, tr->dst_w - src_x);
	src_h = MIN(src_h, tr->dst_h - src_y);

	int pictop = rect->reverse ? PictOpSrc : PictOpOver;
	if (src_w > 0 && src_h > 0)
		XRenderComposite(disp, pictop, pixmap_pic, 0, window_pic, src_x,
				 src_y, src_x, src_y, window_x, window_y, src_w,
				 src_h);

	XRenderFreePicture(disp, pixmap_pic);
	XRenderFreePicture(disp, window_pic);
	if (rect->reverse)
		XFreePixmap(disp, pixmap);

	if (graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES) {
		gr_showrect(buf, rect);
		gr_displayinfo(buf, rect, 0xFF000000, 0xFFFFFFFF, "");
	}
}

static void gr_freerect(ImageRect *rect) { memset(rect, 0, sizeof(ImageRect)); }

static int gr_getrectbottom(ImageRect *rect) {
	return rect->screen_y_pix +
	       (rect->img_end_row - rect->img_start_row) * rect->ch;
}

void gr_start_drawing(Drawable buf, int cw, int ch) {
	current_cw = cw;
	current_ch = ch;
	debug_loaded_files_counter = 0;
	debug_loaded_pixmaps_counter = 0;
	drawing_start_time = gr_now_ms();
	imlib_context_set_drawable(buf);
}

void gr_finish_drawing(Drawable buf) {

	for (size_t i = 0; i < MAX_IMAGE_RECTS; ++i) {
		ImageRect *rect = &image_rects[i];
		if (!rect->image_id)
			continue;
		gr_drawimagerect(buf, rect);
		gr_freerect(rect);
	}

	Milliseconds drawing_end_time = gr_now_ms();
	graphics_next_redraw_delay = INT_MAX;
	for (int row = 0; row < kv_size(next_redraw_times); ++row) {
		Milliseconds row_next_redraw = kv_A(next_redraw_times, row);
		if (row_next_redraw > 0) {
			int delay = MAX(graphics_animation_min_delay,
					row_next_redraw - drawing_end_time);
			graphics_next_redraw_delay =
				MIN(graphics_next_redraw_delay, delay);
		}
	}

	if (graphics_debug_mode) {
		int milliseconds = drawing_end_time - drawing_start_time;

		Display *disp = imlib_context_get_display();
		GC gc = XCreateGC(disp, buf, 0, NULL);
		const char *debug_mode_str =
			graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES
				? "(boxes shown) "
				: "";
		int redraw_delay = graphics_next_redraw_delay == INT_MAX
					   ? -1
					   : graphics_next_redraw_delay;
		char info[MAX_INFO_LEN];
		snprintf(info, MAX_INFO_LEN,
			 "%sRender time: %d ms  ram %ld K  disk %ld K  count "
			 "%d  cell %dx%d  delay %d",
			 debug_mode_str, milliseconds, images_ram_size / 1024,
			 images_disk_size / 1024, kh_size(images), current_cw,
			 current_ch, redraw_delay);
		XSetForeground(disp, gc, 0xFF000000);
		XFillRectangle(disp, buf, gc, 0, 0, 600, 16);
		XSetForeground(disp, gc, 0xFFFFFFFF);
		XDrawString(disp, buf, gc, 0, 14, info, strlen(info));
		XFreeGC(disp, gc);

		if (milliseconds > 0) {
			fprintf(stderr, "%s  (loaded %d files, %d pixmaps)\n",
				info, debug_loaded_files_counter,
				debug_loaded_pixmaps_counter);
		}
	}

	gr_check_limits();
}

void gr_append_imagerect(Drawable buf, uint32_t image_id, uint32_t placement_id,
			 int img_start_col, int img_end_col, int img_start_row,
			 int img_end_row, int x_col, int y_row, int x_pix,
			 int y_pix, int cw, int ch, int reverse) {
	current_cw = cw;
	current_ch = ch;

	ImageRect new_rect;
	new_rect.image_id = image_id;
	new_rect.placement_id = placement_id;
	new_rect.img_start_col = img_start_col;
	new_rect.img_end_col = img_end_col;
	new_rect.img_start_row = img_start_row;
	new_rect.img_end_row = img_end_row;
	new_rect.screen_y_row = y_row;
	new_rect.screen_x_pix = x_pix;
	new_rect.screen_y_pix = y_pix;
	new_rect.ch = ch;
	new_rect.cw = cw;
	new_rect.reverse = reverse;

	if (graphics_debug_mode == GRAPHICS_DEBUG_LOG_AND_BOXES)
		gr_displayinfo(buf, &new_rect, 0xFF000000, 0xFFFF0000, "? ");

	if (image_id == 0 || img_end_col - img_start_col <= 0 ||
	    img_end_row - img_start_row <= 0)
		return;

	ImageRect *free_rect = NULL;
	for (size_t i = 0; i < MAX_IMAGE_RECTS; ++i) {
		ImageRect *rect = &image_rects[i];
		if (rect->image_id == 0) {
			if (!free_rect)
				free_rect = rect;
			continue;
		}
		if (rect->image_id != image_id ||
		    rect->placement_id != placement_id || rect->cw != cw ||
		    rect->ch != ch || rect->reverse != reverse)
			continue;

		if (rect->img_end_row == img_start_row &&
		    gr_getrectbottom(rect) == y_pix) {
			if (rect->img_start_col == img_start_col &&
			    rect->img_end_col == img_end_col &&
			    rect->screen_x_pix == x_pix) {
				rect->img_end_row = img_end_row;
				return;
			}
		}
	}

	if (!free_rect) {
		for (size_t i = 0; i < MAX_IMAGE_RECTS; ++i) {
			ImageRect *rect = &image_rects[i];
			if (!free_rect || gr_getrectbottom(free_rect) >
						  gr_getrectbottom(rect))
				free_rect = rect;
		}
		gr_drawimagerect(buf, free_rect);
		gr_freerect(free_rect);
	}

	*free_rect = new_rect;
}

void gr_mark_dirty_animations(int *dirty, int rows) {
	if (rows < kv_size(next_redraw_times))
		kv_size(next_redraw_times) = rows;
	if (rows * 2 < kv_max(next_redraw_times))
		kv_resize(Milliseconds, next_redraw_times, rows);
	for (int i = 0; i < MIN(rows, kv_size(next_redraw_times)); ++i) {
		if (dirty[i]) {
			kv_A(next_redraw_times, i) = 0;
			continue;
		}
		Milliseconds next_update = kv_A(next_redraw_times, i);
		if (next_update > 0 && next_update <= drawing_start_time) {
			dirty[i] = 1;
			kv_A(next_redraw_times, i) = 0;
		}
	}
}

typedef struct {

	char *command;

	char *payload;

	char action;

	int quiet;

	int format;

	int compression;

	char transmission_medium;

	char delete_specifier;

	int frame_pix_width, frame_pix_height;

	int src_pix_x, src_pix_y;

	int src_pix_width, src_pix_height;

	int rows, columns;

	uint32_t image_id;

	uint32_t image_number;

	uint32_t placement_id;

	int more;

	char is_direct_transmission_continuation;

	int size;

	unsigned offset;

	int virtual;

	char do_not_move_cursor;

	int frame_dst_pix_x, frame_dst_pix_y;

	char replace_instead_of_blending;

	uint32_t background_color;

	int background_frame;

	int current_frame;

	int edit_frame;

	int gap;

	int animation_state;

	int loops;
} GraphicsCommand;

static void sanitize_str(char *str, size_t max_size) {
	assert(max_size >= 4);
	for (size_t i = 0; i < max_size; ++i) {
		unsigned c = str[i];
		if (c == '\0')
			return;
		if (c >= 128 || !isprint(c))
			str[i] = '?';
	}
	str[max_size - 1] = '\0';
	str[max_size - 2] = '.';
	str[max_size - 3] = '.';
	str[max_size - 4] = '.';
}

static const char *sanitized_filename(const char *str) {
	static char buf[MAX_FILENAME_SIZE];
	strncpy(buf, str, sizeof(buf));
	sanitize_str(buf, sizeof(buf));
	return buf;
}

static void gr_createresponse(uint32_t image_id, uint32_t image_number,
			      uint32_t placement_id, const char *msg) {
	if (!image_id && !image_number && !placement_id) {

		fprintf(stderr,
			"error: No image id or image number or placement_id, "
			"but still there is a response: %s\n",
			msg);
		return;
	}
	char *buf = graphics_command_result.response;
	size_t maxlen = MAX_GRAPHICS_RESPONSE_LEN;
	size_t written;
	written = snprintf(buf, maxlen, "\033_G");
	buf += written;
	maxlen -= written;
	if (image_id) {
		written = snprintf(buf, maxlen, "i=%u,", image_id);
		buf += written;
		maxlen -= written;
	}
	if (image_number) {
		written = snprintf(buf, maxlen, "I=%u,", image_number);
		buf += written;
		maxlen -= written;
	}
	if (placement_id) {
		written = snprintf(buf, maxlen, "p=%u,", placement_id);
		buf += written;
		maxlen -= written;
	}
	buf[-1] = ';';
	written = snprintf(buf, maxlen, "%s\033\\", msg);
	buf += written;
	maxlen -= written;
	buf[-2] = '\033';
	buf[-1] = '\\';
}

static void gr_reportsuccess_cmd(GraphicsCommand *cmd) {
	if (cmd->quiet < 1 && !cmd->more)
		gr_createresponse(cmd->image_id, cmd->image_number,
				  cmd->placement_id, "OK");
}

static void gr_reportsuccess_frame(ImageFrame *frame) {
	uint32_t id = frame->image->query_id ? frame->image->query_id
					     : frame->image->image_id;
	if (frame->quiet < 1)
		gr_createresponse(id, frame->image->image_number,
				  frame->image->initial_placement_id, "OK");
}

static void gr_reporterror_cmd(GraphicsCommand *cmd, const char *format, ...) {
	char errmsg[MAX_GRAPHICS_RESPONSE_LEN];
	graphics_command_result.error = 1;
	va_list args;
	va_start(args, format);
	vsnprintf(errmsg, MAX_GRAPHICS_RESPONSE_LEN, format, args);
	va_end(args);

	fprintf(stderr, "%s  in command: %s\n", errmsg, cmd->command);
	if (cmd->quiet < 2)
		gr_createresponse(cmd->image_id, cmd->image_number,
				  cmd->placement_id, errmsg);
}

static void gr_reporterror_frame(ImageFrame *frame, const char *format, ...) {
	char errmsg[MAX_GRAPHICS_RESPONSE_LEN];
	graphics_command_result.error = 1;
	va_list args;
	va_start(args, format);
	vsnprintf(errmsg, MAX_GRAPHICS_RESPONSE_LEN, format, args);
	va_end(args);

	if (!frame) {
		fprintf(stderr, "%s\n", errmsg);
		gr_createresponse(0, 0, 0, errmsg);
	} else {
		uint32_t id = frame->image->query_id ? frame->image->query_id
						     : frame->image->image_id;
		fprintf(stderr, "%s  id=%u\n", errmsg, id);
		if (frame->quiet < 2)
			gr_createresponse(id, frame->image->image_number,
					  frame->image->initial_placement_id,
					  errmsg);
	}
}

static ImageFrame *gr_loadimage_and_report(ImageFrame *frame) {
	gr_load_imlib_object(frame);
	if (!frame->imlib_object) {
		gr_reporterror_frame(frame, "EBADF: could not load image");
	} else {
		gr_reportsuccess_frame(frame);
	}

	if (frame->image->query_id) {
		gr_delete_image(frame->image);
		return NULL;
	}
	return frame;
}

static void gr_reportuploaderror(ImageFrame *frame) {
	switch (frame->uploading_failure) {
	case 0:
		return;
	case ERROR_CANNOT_OPEN_CACHED_FILE:
		gr_reporterror_frame(frame,
				   "EIO: could not create a file for image");
		break;
	case ERROR_OVER_SIZE_LIMIT:
		gr_reporterror_frame(
			frame,
			"EFBIG: the size of the uploaded image exceeded "
			"the image size limit %u",
			graphics_max_single_image_file_size);
		break;
	case ERROR_UNEXPECTED_SIZE:
		gr_reporterror_frame(frame,
				   "EINVAL: the size of the uploaded image %u "
				   "doesn't match the expected size %u",
				   frame->disk_size, frame->expected_size);
		break;
	};
}

static void gr_display_nonvirtual_placement(ImagePlacement *placement) {
	if (placement->virtual)
		return;
	if (placement->image->first_frame.status < STATUS_RAM_LOADING_SUCCESS)
		return;

	gr_infer_placement_size_maybe(placement);

	graphics_command_result.create_placeholder = 1;
	graphics_command_result.placeholder.image_id = placement->image->image_id;
	graphics_command_result.placeholder.placement_id = placement->placement_id;
	graphics_command_result.placeholder.columns = placement->cols;
	graphics_command_result.placeholder.rows = placement->rows;
	graphics_command_result.placeholder.do_not_move_cursor =
		placement->do_not_move_cursor;
	placement->text_underneath =
		calloc(placement->rows * placement->cols, sizeof(Glyph));
	graphics_command_result.placeholder.text_underneath =
		placement->text_underneath;
	GR_LOG("Creating a placeholder for %u/%u  %d x %d\n",
	       placement->image->image_id, placement->placement_id,
	       placement->cols, placement->rows);
}

static void gr_schedule_image_redraw(Image *img) {
	if (!img)
		return;
	gr_schedule_image_redraw_by_id(img->image_id);
}

static void gr_close_current_upload_file() {
	Image *img = gr_find_image(current_upload_image_id);
	ImageFrame *frame = gr_get_frame(img, current_upload_frame_index);
	gr_close_disk_cache_file(frame);
}

static void gr_set_current_upload_frame(ImageFrame *frame) {
	if (frame) {
		if (current_upload_image_id != frame->image->image_id ||
		    current_upload_frame_index != frame->index) {
			gr_close_current_upload_file();
		}
		current_upload_image_id = frame->image->image_id;
		current_upload_frame_index = frame->index;
		GR_LOG("Set current_upload_image_id = %u, "
		       "current_upload_frame_index = %u\n",
		       current_upload_image_id, current_upload_frame_index);
	} else {
		gr_close_current_upload_file();
		current_upload_image_id = 0;
		current_upload_frame_index = 0;
		GR_LOG("Set current_upload_image_id = 0\n");
	}
}

static int gr_transmission_continuation_is_allowed(GraphicsCommand *cmd,
						   ImageFrame *frame) {
	if (!frame || frame->status != STATUS_UPLOADING)
		return 0;

	if (current_upload_image_id == frame->image->image_id &&
	    current_upload_frame_index == frame->index)
		return 1;

	if (cmd->size && cmd->size != frame->expected_size) {
		fprintf(stderr, "warning: Not resuming interrupted upload "
				"because of expected size mismatch\n");
		return 0;
	}
	if (cmd->format && cmd->format != frame->format) {
		fprintf(stderr, "warning: Not resuming interrupted upload "
				"because of format mismatch\n");
		return 0;
	}
	if (cmd->compression && cmd->compression != frame->compression) {
		fprintf(stderr, "warning: Not resuming interrupted upload "
				"because of compression mismatch\n");
		return 0;
	}
	if ((cmd->frame_pix_width &&
	     cmd->frame_pix_width != frame->data_pix_width) ||
	    (cmd->frame_pix_height &&
	     cmd->frame_pix_height != frame->data_pix_height) ||
	    (cmd->background_color &&
	     cmd->background_color != frame->background_color) ||
	    (cmd->background_frame &&
	     cmd->background_frame != frame->background_frame_index) ||
	    (cmd->gap && cmd->gap != frame->gap) ||
	    (cmd->replace_instead_of_blending &&
	     cmd->replace_instead_of_blending != !frame->blend)) {
		fprintf(stderr, "warning: Not resuming interrupted upload "
				"because of frame parameters mismatch\n");
		return 0;
	}

	Milliseconds now = gr_now_ms();
	if (now - frame->atime > graphics_direct_transmission_timeout_ms) {
		fprintf(stderr, "warning: Not resuming interrupted upload "
				"because of time out\n");
		return 0;
	}

	return 1;
}

static int gr_append_raw_data_to_file(ImageFrame *frame, const char *data,
				      size_t data_size) {

	if (!frame->open_file) {
		gr_make_sure_tmpdir_exists();
		char filename[MAX_FILENAME_SIZE];
		gr_get_frame_filename(frame, filename, MAX_FILENAME_SIZE);
		FILE *file = fopen(filename, frame->disk_size ? "a" : "w");
		if (!file)
			return 0;
		frame->open_file = file;
	}

	fwrite(data, 1, data_size, frame->open_file);
	frame->disk_size += data_size;
	frame->image->total_disk_size += data_size;
	images_disk_size += data_size;
	gr_touch_frame(frame);
	return 1;
}

static void gr_append_data(ImageFrame *frame, const char *payload, int more) {
	gr_set_current_upload_frame(frame);

	if (frame->status != STATUS_UPLOADING) {
		if (!more)
			gr_reportuploaderror(frame);
		gr_set_current_upload_frame(NULL);
		return;
	}

	size_t data_size = 0;
	char *data = gr_base64dec(payload, &data_size);

	GR_LOG("appending %u + %zu = %zu bytes\n", frame->disk_size, data_size,
	       frame->disk_size + data_size);

	if (frame->disk_size + data_size >
		    graphics_max_single_image_file_size ||
	    frame->expected_size > graphics_max_single_image_file_size) {
		free(data);
		gr_delete_imagefile(frame);
		frame->uploading_failure = ERROR_OVER_SIZE_LIMIT;
		if (!more)
			gr_reportuploaderror(frame);
		gr_set_current_upload_frame(NULL);
		return;
	}

	if (!gr_append_raw_data_to_file(frame, data, data_size)) {
		frame->status = STATUS_UPLOADING_ERROR;
		frame->uploading_failure = ERROR_CANNOT_OPEN_CACHED_FILE;
		if (!more)
			gr_reportuploaderror(frame);
		gr_set_current_upload_frame(NULL);
		return;
	}
	free(data);

	if (!more) {
		gr_set_current_upload_frame(NULL);
		frame->status = STATUS_UPLOADING_SUCCESS;
		uint32_t placement_id = frame->image->default_placement;
		if (frame->expected_size &&
		    frame->expected_size != frame->disk_size) {

			frame->status = STATUS_UPLOADING_ERROR;
			frame->uploading_failure = ERROR_UNEXPECTED_SIZE;
			gr_reportuploaderror(frame);
		} else {

			gr_schedule_image_redraw(frame->image);

			frame = gr_loadimage_and_report(frame);

			if (frame && frame->index == 1) {
				Image *img = frame->image;
				ImagePlacement *placement = NULL;
				kh_foreach_value(img->placements, placement, {
					gr_display_nonvirtual_placement(placement);
				});
			}
		}
	}

	gr_check_limits();
}

static Image *gr_find_image_for_command(GraphicsCommand *cmd) {
	if (cmd->image_id)
		return gr_find_image(cmd->image_id);
	Image *img = NULL;

	if (cmd->image_number == 0 && cmd->action == 'p')
		img = gr_find_image(last_image_id);
	else
		img = gr_find_image_by_number(cmd->image_number);
	return img;
}

static ImageFrame *gr_new_image_or_frame_from_command(GraphicsCommand *cmd) {
	if (cmd->format != 0 && cmd->format != 32 && cmd->format != 24 &&
	    cmd->compression != 0) {
		gr_reporterror_cmd(cmd, "EINVAL: compression is supported only "
					"for raw pixel data (f=32 or f=24)");

	}

	Image *img = NULL;
	if (cmd->action == 'f') {

		img = gr_find_image_for_command(cmd);
		if (img) {
			cmd->image_id = img->image_id;
		} else {
			gr_reporterror_cmd(cmd, "ENOENT: image not found");
			return NULL;
		}
	} else {

		uint32_t image_id = cmd->action == 'q' ? 0 : cmd->image_id;
		img = gr_new_image(image_id);
		if (!img)
			return NULL;
		if (cmd->action == 'q')
			img->query_id = cmd->image_id;
		else if (!cmd->image_id)
			cmd->image_id = img->image_id;

		img->image_number = cmd->image_number;
	}

	ImageFrame *frame = gr_append_new_frame(img);

	frame->expected_size = cmd->size;

	frame->format = cmd->format ? cmd->format : 32;
	frame->compression = cmd->compression;
	frame->background_color = cmd->background_color;
	frame->background_frame_index = cmd->background_frame;
	frame->gap = cmd->gap;
	img->total_duration += frame->gap;
	frame->blend = !cmd->replace_instead_of_blending;
	frame->data_pix_width = cmd->frame_pix_width;
	frame->data_pix_height = cmd->frame_pix_height;
	if (cmd->action == 'f') {
		frame->x = cmd->frame_dst_pix_x;
		frame->y = cmd->frame_dst_pix_y;
	}

	if (!frame->expected_size && !frame->compression &&
	    (frame->format == 24 || frame->format == 32)) {
		frame->expected_size = frame->data_pix_width *
				       frame->data_pix_height *
				       (frame->format / 8);
	}

	frame->quiet = cmd->quiet;
	return frame;
}

static void gr_delete_tmp_file(const char *filename) {
	if (strstr(filename, "tty-graphics-protocol") == NULL)
		return;
	if (strstr(filename, "/tmp/") != filename) {
		const char *tmpdir = getenv("TMPDIR");
		if (!tmpdir || !tmpdir[0] ||
		    strstr(filename, tmpdir) != filename)
			return;
	}
	unlink(filename);
}

static void gr_copy_imagefile(ImageFrame *frame, GraphicsCommand *cmd) {
	GR_LOG("Copying image %s\n",
	       sanitized_filename(frame->original_filename));

	struct stat st;
	int stat_res = stat(frame->original_filename, &st);

	const char *stat_error = NULL;
	if (stat_res)
		stat_error = strerror(errno);
	else if (!S_ISREG(st.st_mode))
		stat_error = "Not a regular file";
	else if (st.st_size == 0)
		stat_error = "The size of the file is zero";
	else if (st.st_size > graphics_max_single_image_file_size)
		stat_error = "The file is too large";
	if (stat_error) {
		fprintf(stderr, "Could not load the file %s: %s\n",
			sanitized_filename(frame->original_filename),
			stat_error);
		if (cmd)
			gr_reporterror_cmd(cmd, "EBADF: %s", stat_error);
		frame->status = STATUS_UPLOADING_ERROR;
		frame->uploading_failure = ERROR_CANNOT_COPY_FILE;
		return;
	}

	if (frame->expected_size && frame->expected_size != st.st_size) {
		fprintf(stderr,
			"Could not load, the size doesn't match: %s expected "
			"%u vs actual %ld\n",
			sanitized_filename(frame->original_filename),
			frame->expected_size, st.st_size);

		frame->status = STATUS_UPLOADING_ERROR;
		frame->uploading_failure = ERROR_UNEXPECTED_SIZE;
		if (cmd)
			gr_reportuploaderror(frame);
		return;
	}

	if (frame->original_file_mtime &&
	    frame->original_file_mtime != st.st_mtime) {
		fprintf(stderr, "Could not load, the mtime doesn't match: %s\n",
			sanitized_filename(frame->original_filename));
		frame->status = STATUS_UPLOADING_ERROR;
		frame->uploading_failure = ERROR_MTIME_MISMATCH;
		if (cmd)
			gr_reportuploaderror(frame);
		return;
	}

	frame->original_file_mtime = st.st_mtime;

	gr_make_sure_tmpdir_exists();

	char cache_filename[MAX_FILENAME_SIZE];
	gr_get_frame_filename(frame, cache_filename, MAX_FILENAME_SIZE);

	char tmp_filename_symlink[MAX_FILENAME_SIZE + 4] = {0};
	strcat(tmp_filename_symlink, cache_filename);
	strcat(tmp_filename_symlink, ".sym");
	char command[MAX_FILENAME_SIZE + 256];
	size_t len = snprintf(command, MAX_FILENAME_SIZE + 255, "cp '%s' '%s'",
			      tmp_filename_symlink, cache_filename);

	if (len > MAX_FILENAME_SIZE + 255 ||
	    symlink(frame->original_filename, tmp_filename_symlink) ||
	    system(command) != 0) {
		fprintf(stderr,
			"Could not copy the image "
			"%s (symlink %s) to %s",
			sanitized_filename(frame->original_filename),
			tmp_filename_symlink, cache_filename);
		if (cmd)
			gr_reporterror_cmd(cmd, "EBADF: could not copy the "
						"image to the cache dir");
		frame->status = STATUS_UPLOADING_ERROR;
		frame->uploading_failure = ERROR_CANNOT_COPY_FILE;

		unlink(tmp_filename_symlink);
		return;
	}

	unlink(tmp_filename_symlink);

	frame->status = STATUS_UPLOADING_SUCCESS;
	frame->disk_size = st.st_size;
	frame->image->total_disk_size += st.st_size;
	images_disk_size += frame->disk_size;
}

static void gr_try_restore_imagefile(ImageFrame *frame) {
	if (frame->disk_size != 0)
		return;
	if (gr_is_original_file_still_available(frame))
		gr_copy_imagefile(frame, NULL);
}

static ImageFrame *gr_handle_transmit_command(GraphicsCommand *cmd) {

	if (!cmd->transmission_medium)
		cmd->transmission_medium = 'd';

	if (current_upload_image_id != 0 && cmd->image_id == 0 &&
	    cmd->image_number == 0 && cmd->transmission_medium == 'd') {
		cmd->image_id = current_upload_image_id;
		GR_LOG("No images id is specified, continuing uploading %u\n",
		       cmd->image_id);
	}

	ImageFrame *frame = NULL;
	if (cmd->transmission_medium == 'f' ||
	    cmd->transmission_medium == 't') {

		frame = gr_new_image_or_frame_from_command(cmd);
		if (!frame)
			return NULL;
		last_image_id = frame->image->image_id;

		frame->original_filename = gr_base64dec(cmd->payload, NULL);

		gr_copy_imagefile(frame, cmd);
		if (frame->status == STATUS_UPLOADING_SUCCESS) {

			gr_schedule_image_redraw(frame->image);
			frame = gr_loadimage_and_report(frame);
		}

		if (cmd->transmission_medium == 't')
			gr_delete_tmp_file(frame->original_filename);
		gr_check_limits();
	} else if (cmd->transmission_medium == 'd') {

		frame = gr_get_last_frame(gr_find_image_for_command(cmd));
		if (gr_transmission_continuation_is_allowed(cmd, frame)) {

			cmd->is_direct_transmission_continuation = 1;
			cmd->image_id = frame->image->image_id;
			gr_append_data(frame, cmd->payload, cmd->more);
			return frame;
		}

		frame = gr_new_image_or_frame_from_command(cmd);
		if (!frame)
			return NULL;
		last_image_id = frame->image->image_id;
		frame->status = STATUS_UPLOADING;

		gr_append_data(frame, cmd->payload, cmd->more);
	} else if (cmd->transmission_medium == 's') {

		frame = gr_new_image_or_frame_from_command(cmd);
		if (!frame)
			return NULL;
		last_image_id = frame->image->image_id;

		if (!frame->expected_size) {
			frame->status = STATUS_UPLOADING_ERROR;
			frame->uploading_failure = ERROR_UNEXPECTED_SIZE;
			gr_reporterror_cmd(
				cmd, "EINVAL: the size of the image is not "
				     "specified and cannot be inferred");
			return frame;
		}

		if (frame->expected_size > graphics_max_single_image_file_size) {
			frame->uploading_failure = ERROR_OVER_SIZE_LIMIT;
			gr_reportuploaderror(frame);
			return frame;
		}

		char *original_filename = gr_base64dec(cmd->payload, NULL);
		GR_LOG("Loading image from shared memory %s\n",
		       sanitized_filename(original_filename));

		int fd = shm_open(original_filename, O_RDONLY, 0);
		if (fd == -1) {
			gr_reporterror_cmd(cmd, "EBADF: shm_open: %s",
					   strerror(errno));
			frame->status = STATUS_UPLOADING_ERROR;
			frame->uploading_failure = ERROR_CANNOT_OPEN_SHM;
			fprintf(stderr, "shm_open failed for %s\n",
				sanitized_filename(original_filename));
			shm_unlink(original_filename);
			free(original_filename);
			return frame;
		}
		shm_unlink(original_filename);
		free(original_filename);

		size_t page_size = sysconf(_SC_PAGESIZE);
		if (page_size == -1)
			page_size = 1;
		size_t offset = cmd->offset - (cmd->offset % page_size);
		size_t size = frame->expected_size + (cmd->offset - offset);

		void *data =
			mmap(NULL, size, PROT_READ, MAP_SHARED, fd, offset);
		if (data == MAP_FAILED) {
			gr_reporterror_cmd(cmd, "EBADF: mmap: %s",
					   strerror(errno));
			frame->status = STATUS_UPLOADING_ERROR;
			frame->uploading_failure = ERROR_CANNOT_OPEN_SHM;
			fprintf(stderr,
				"mmap failed for size = %ld, offset = %ld\n",
				size, offset);
			close(fd);
			return frame;
		}
		close(fd);

		if (gr_append_raw_data_to_file(frame,
					       data + (cmd->offset - offset),
					       frame->expected_size)) {
			frame->status = STATUS_UPLOADING_SUCCESS;
		} else {
			frame->status = STATUS_UPLOADING_ERROR;
			frame->uploading_failure =
				ERROR_CANNOT_OPEN_CACHED_FILE;
			gr_reportuploaderror(frame);
		}

		gr_close_disk_cache_file(frame);

		if (munmap(data, size) != 0)
			fprintf(stderr, "munmap failed: %s\n", strerror(errno));

		gr_schedule_image_redraw(frame->image);
		frame = gr_loadimage_and_report(frame);
		gr_check_limits();
	} else {
		gr_reporterror_cmd(
			cmd,
			"EINVAL: transmission medium '%c' is not supported",
			cmd->transmission_medium);
		return NULL;
	}

	return frame;
}

static void gr_handle_put_command(GraphicsCommand *cmd) {
	if (cmd->image_id == 0 && cmd->image_number == 0) {
		gr_reporterror_cmd(cmd,
				   "EINVAL: neither image id nor image number "
				   "are specified or both are zero");
		return;
	}

	Image *img = gr_find_image_for_command(cmd);
	if (img) {
		cmd->image_id = img->image_id;
	} else {
		gr_reporterror_cmd(cmd, "ENOENT: image not found");
		return;
	}

	ImagePlacement *placement = gr_new_placement(img, cmd->placement_id);
	placement->virtual = cmd->virtual;
	placement->src_pix_x = cmd->src_pix_x;
	placement->src_pix_y = cmd->src_pix_y;
	placement->src_pix_width = cmd->src_pix_width;
	placement->src_pix_height = cmd->src_pix_height;
	placement->cols = cmd->columns;
	placement->rows = cmd->rows;
	placement->do_not_move_cursor = cmd->do_not_move_cursor;

	if (placement->virtual) {
		placement->scale_mode = SCALE_MODE_CONTAIN;
	} else if (placement->cols && placement->rows) {

		placement->scale_mode = SCALE_MODE_FILL;
	} else if (placement->cols || placement->rows) {

		placement->scale_mode = SCALE_MODE_CONTAIN;
	} else {

		placement->scale_mode = SCALE_MODE_NONE;
	}

	gr_display_nonvirtual_placement(placement);

	gr_reportsuccess_cmd(cmd);
}

typedef struct DeletionData {
	uint32_t image_id;
	uint32_t placement_id;

	ImagePlacementVec placements_to_delete;
} DeletionData;

static int gr_deletion_callback(void *data, Glyph *gp) {
	DeletionData *del_data = data;

	if (!tgetisclassicplaceholder(gp))
		return 0;
	uint32_t image_id = tgetimgid(gp);
	uint32_t placement_id = tgetimgplacementid(gp);
	if (del_data->image_id && del_data->image_id != image_id)
		return 0;
	if (del_data->placement_id && del_data->placement_id != placement_id)
		return 0;

	ImagePlacement *placement = NULL;

	for (int i = 0; i < kv_size(del_data->placements_to_delete); ++i) {
		ImagePlacement *cand = kv_A(del_data->placements_to_delete, i);
		if (cand->image->image_id == image_id &&
		    cand->placement_id == placement_id) {
			placement = cand;
			break;
		}
	}
	if (!placement) {
		placement = gr_find_image_and_placement(image_id, placement_id);
		if (placement) {
			kv_push(ImagePlacement *,
				del_data->placements_to_delete, placement);
		}
	}

	if (placement && placement->text_underneath) {
		int row = tgetimgrow(gp) - 1;
		int col = tgetimgcol(gp) - 1;
		if (col >= 0 && row >= 0 && row < placement->rows &&
		    col < placement->cols) {
			*gp = placement->text_underneath[row * placement->cols +
							 col];
			return 1;
		}
	}

	gp->mode = 0;
	gp->u = ' ';
	return 1;
}

static void gr_handle_delete_command(GraphicsCommand *cmd) {
	DeletionData del_data = {0};
	char delete_image_if_no_ref = isupper(cmd->delete_specifier) != 0;
	char d = tolower(cmd->delete_specifier);

	if (d == 'n') {
		d = 'i';
		Image *img = gr_find_image_by_number(cmd->image_number);
		if (!img)
			return;
		del_data.image_id = img->image_id;
	}

	kv_init(del_data.placements_to_delete);

	if (!d || d == 'a') {

		gr_for_each_image_cell(gr_deletion_callback, &del_data);
	} else if (d == 'i') {

		if (!del_data.image_id)
			del_data.image_id = cmd->image_id;
		if (!del_data.image_id) {
			fprintf(stderr,
				"ERROR: image id is not specified in the "
				"delete command\n");
			kv_destroy(del_data.placements_to_delete);
			return;
		}
		del_data.placement_id = cmd->placement_id;
		gr_for_each_image_cell(gr_deletion_callback, &del_data);
	} else {
		fprintf(stderr,
			"WARNING: unsupported value of the d key: '%c'. The "
			"command is ignored.\n",
			cmd->delete_specifier);
	}

	for (int i = 0; i < kv_size(del_data.placements_to_delete); ++i) {
		ImagePlacement *placement =
			kv_A(del_data.placements_to_delete, i);

		free(placement->text_underneath);
		placement->text_underneath = NULL;
		Image *img = placement->image;
		gr_delete_placement(placement);

		if (delete_image_if_no_ref && kh_size(img->placements) == 0)
			gr_delete_image(img);
	}

	if (d == 'i' && !del_data.placement_id && delete_image_if_no_ref)
		gr_delete_image(gr_find_image(cmd->image_id));

	kv_destroy(del_data.placements_to_delete);
}

static void gr_erase_placement(ImagePlacement *placement) {
	DeletionData del_data = {0};
	del_data.image_id = placement->image->image_id;
	del_data.placement_id = placement->placement_id;
	kv_init(del_data.placements_to_delete);
	gr_for_each_image_cell(gr_deletion_callback, &del_data);

	free(placement->text_underneath);
	placement->text_underneath = NULL;
	kv_destroy(del_data.placements_to_delete);
}

static void gr_handle_animation_control_command(GraphicsCommand *cmd) {
	if (cmd->image_id == 0 && cmd->image_number == 0) {
		gr_reporterror_cmd(cmd,
				   "EINVAL: neither image id nor image number "
				   "are specified or both are zero");
		return;
	}

	Image *img = gr_find_image_for_command(cmd);
	if (img) {
		cmd->image_id = img->image_id;
	} else {
		gr_reporterror_cmd(cmd, "ENOENT: image not found");
		return;
	}

	ImageFrame *frame = NULL;
	if (cmd->edit_frame)
		frame = gr_get_frame(img, cmd->edit_frame);
	if (cmd->edit_frame || cmd->gap) {
		if (!frame) {
			gr_reporterror_cmd(cmd, "ENOENT: frame %d not found",
					   cmd->edit_frame);
			return;
		}
		if (cmd->gap) {
			img->total_duration -= frame->gap;
			frame->gap = cmd->gap;
			img->total_duration += frame->gap;
		}
	}

	if (cmd->current_frame)
		img->current_frame = cmd->current_frame;
	if (cmd->animation_state) {
		if (cmd->animation_state == 1) {
			img->animation_state = ANIMATION_STATE_STOPPED;
		} else if (cmd->animation_state == 2) {
			img->animation_state = ANIMATION_STATE_LOADING;
		} else if (cmd->animation_state == 3) {
			img->animation_state = ANIMATION_STATE_LOOPING;
		} else {
			gr_reporterror_cmd(
				cmd, "EINVAL: invalid animation state: %d",
				cmd->animation_state);
		}
	}

	gr_schedule_image_redraw(img);
}

static void gr_handle_command(GraphicsCommand *cmd) {
	if (!cmd->image_id && !cmd->image_number) {

		cmd->quiet = 2;
	}

	int was_transmission = 0;
	ImageFrame *frame = NULL;

	switch (cmd->action) {
	case 0:

	case 't':
	case 'q':
	case 'f':
		was_transmission = 1;

		gr_handle_transmit_command(cmd);
		break;
	case 'p':

		gr_handle_put_command(cmd);
		break;
	case 'T':
		was_transmission = 1;

		frame = gr_handle_transmit_command(cmd);
		if (frame && !cmd->is_direct_transmission_continuation) {
			gr_handle_put_command(cmd);
			if (cmd->placement_id)
				frame->image->initial_placement_id =
					cmd->placement_id;
		}
		break;
	case 'd':
		gr_handle_delete_command(cmd);
		break;
	case 'a':
		gr_handle_animation_control_command(cmd);
		break;
	default:
		gr_reporterror_cmd(cmd, "EINVAL: unsupported action: %c",
				   cmd->action);
		break;
	}

	if (!was_transmission ||
	    (cmd->transmission_medium && cmd->transmission_medium != 'd')) {

		gr_set_current_upload_frame(NULL);
	}
}

typedef struct KeyAndValue {
	char *key_start;
	char *val_start;
	unsigned key_len, val_len;
} KeyAndValue;

static void gr_set_keyvalue(GraphicsCommand *cmd, KeyAndValue *kv) {
	char *key_start = kv->key_start;
	char *key_end = key_start + kv->key_len;
	char *value_start = kv->val_start;
	char *value_end = value_start + kv->val_len;

	if (key_end - key_start != 1) {
		gr_reporterror_cmd(cmd, "EINVAL: unknown key of length %ld: %s",
				   key_end - key_start, key_start);
		return;
	}
	long num = 0;
	if (*key_start == 'a' || *key_start == 't' || *key_start == 'd' ||
	    *key_start == 'o') {

		if (value_end - value_start != 1) {
			gr_reporterror_cmd(
				cmd,
				"EINVAL: value of 'a', 't' or 'd' must be a "
				"single char: %s",
				key_start);
			return;
		}
	} else {

		char *num_end = NULL;
		num = strtol(value_start, &num_end, 10);
		if (num_end != value_end) {
			gr_reporterror_cmd(
				cmd, "EINVAL: could not parse number value: %s",
				key_start);
			return;
		}
	}
	switch (*key_start) {
	case 'a':
		cmd->action = *value_start;
		break;
	case 't':
		cmd->transmission_medium = *value_start;
		break;
	case 'd':
		cmd->delete_specifier = *value_start;
		break;
	case 'q':
		cmd->quiet = num;
		break;
	case 'f':
		cmd->format = num;
		if (num != 0 && num != 24 && num != 32 && num != 100) {
			gr_reporterror_cmd(
				cmd,
				"EINVAL: unsupported format specification: %s",
				key_start);
		}
		break;
	case 'o':
		cmd->compression = *value_start;
		if (cmd->compression != 'z') {
			gr_reporterror_cmd(cmd,
					   "EINVAL: unsupported compression "
					   "specification: %s",
					   key_start);
		}
		break;
	case 's':
		if (cmd->action == 'a')
			cmd->animation_state = num;
		else
			cmd->frame_pix_width = num;
		break;
	case 'v':
		if (cmd->action == 'a')
			cmd->loops = num;
		else
			cmd->frame_pix_height = num;
		break;
	case 'i':
		cmd->image_id = num;
		break;
	case 'I':
		cmd->image_number = num;
		break;
	case 'p':
		cmd->placement_id = num;
		break;
	case 'x':
		cmd->src_pix_x = num;
		cmd->frame_dst_pix_x = num;
		break;
	case 'y':
		if (cmd->action == 'f')
			cmd->frame_dst_pix_y = num;
		else
			cmd->src_pix_y = num;
		break;
	case 'w':
		cmd->src_pix_width = num;
		break;
	case 'h':
		cmd->src_pix_height = num;
		break;
	case 'c':
		if (cmd->action == 'f')
			cmd->background_frame = num;
		else if (cmd->action == 'a')
			cmd->current_frame = num;
		else
			cmd->columns = num;
		break;
	case 'r':
		if (cmd->action == 'f' || cmd->action == 'a')
			cmd->edit_frame = num;
		else
			cmd->rows = num;
		break;
	case 'm':
		cmd->more = num;
		break;
	case 'S':
		cmd->size = num;
		break;
	case 'O':
		cmd->offset = num;
		break;
	case 'U':
		cmd->virtual = num;
		break;
	case 'X':
		if (cmd->action == 'f')
			cmd->replace_instead_of_blending = num;
		else
			break;
		break;
	case 'Y':
		if (cmd->action == 'f')
			cmd->background_color = num;
		else
			break;
		break;
	case 'z':
		if (cmd->action == 'f' || cmd->action == 'a')
			cmd->gap = num;
		else
			break;
		break;
	case 'C':
		cmd->do_not_move_cursor = num;
		break;
	default:
		gr_reporterror_cmd(cmd, "EINVAL: unsupported key: %s",
				   key_start);
		return;
	}
}

int gr_parse_command(char *buf, size_t len) {
	if (buf[0] != 'G')
		return 0;

	Milliseconds command_start_time = gr_now_ms();
	debug_loaded_files_counter = 0;
	debug_loaded_pixmaps_counter = 0;
	global_command_counter++;
	GR_LOG("### Command %lu: %.80s\n", global_command_counter, buf);

	memset(&graphics_command_result, 0, sizeof(GraphicsCommandResult));

	++buf;
	--len;

	GraphicsCommand cmd = {.command = buf};

	char state = 'k';

	KeyAndValue key_vals[32];
	unsigned key_vals_count = 0;
	char *key_start = buf;
	char *key_end = NULL;
	char *val_start = NULL;
	char *val_end = NULL;
	char *c = buf;
	while (c - buf < len + 1) {
		if (state == 'k') {
			switch (*c) {
			case ',':
			case ';':
			case '\0':
				state = *c == ',' ? 'k' : 'p';
				key_end = c;
				gr_reporterror_cmd(
					&cmd, "EINVAL: key without value: %s ",
					key_start);
				break;
			case '=':
				key_end = c;
				state = 'v';
				val_start = c + 1;
				break;
			default:
				break;
			}
		} else if (state == 'v') {
			switch (*c) {
			case ',':
			case ';':
			case '\0':
				state = *c == ',' ? 'k' : 'p';
				val_end = c;
				if (key_vals_count >=
				    sizeof(key_vals) / sizeof(*key_vals)) {
					gr_reporterror_cmd(&cmd,
							   "EINVAL: too many "
							   "key-value pairs");
					break;
				}
				key_vals[key_vals_count].key_start = key_start;
				key_vals[key_vals_count].val_start = val_start;
				key_vals[key_vals_count].key_len =
					key_end - key_start;
				key_vals[key_vals_count].val_len =
					val_end - val_start;
				++key_vals_count;
				key_start = c + 1;
				break;
			default:
				break;
			}
		} else if (state == 'p') {
			cmd.payload = c;

			break;
		}
		++c;
	}

	for (unsigned i = 0; i < key_vals_count; ++i) {
		if (key_vals[i].key_len == 1) {
			char *start = key_vals[i].key_start;
			if (*start == 'a' || *start == 'i' || *start == 'I') {
				gr_set_keyvalue(&cmd, &key_vals[i]);
				break;
			}
		}
	}

	for (unsigned i = 0; i < key_vals_count; ++i)
		gr_set_keyvalue(&cmd, &key_vals[i]);

	if (!cmd.payload)
		cmd.payload = buf + len;

	if (cmd.payload && cmd.payload[0])
		GR_LOG("    payload size: %ld\n", strlen(cmd.payload));

	if (!graphics_command_result.error)
		gr_handle_command(&cmd);

	if (graphics_debug_mode) {
		fprintf(stderr, "Response: ");
		for (const char *resp = graphics_command_result.response;
		     *resp != '\0'; ++resp) {
			if (isprint(*resp))
				fprintf(stderr, "%c", *resp);
			else
				fprintf(stderr, "(0x%x)", *resp);
		}
		fprintf(stderr, "\n");
	}

	if (cmd.quiet) {
		if (!graphics_command_result.error || cmd.quiet >= 2)
			graphics_command_result.response[0] = '\0';
	}

	Milliseconds command_end_time = gr_now_ms();
	GR_LOG("Command %lu took %ld ms  (loaded %d files, %d pixmaps)\n\n",
	       global_command_counter, command_end_time - command_start_time,
	       debug_loaded_files_counter, debug_loaded_pixmaps_counter);

	return 1;
}

static const char gr_base64_digits[] = {
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  62, 0,  0,  0,  63, 52, 53, 54,
	55, 56, 57, 58, 59, 60, 61, 0,  0,  0,  -1, 0,  0,  0,  0,  1,  2,
	3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
	20, 21, 22, 23, 24, 25, 0,  0,  0,  0,  0,  0,  26, 27, 28, 29, 30,
	31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
	48, 49, 50, 51, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0};

static char gr_base64_getc(const char **src) {
	while (**src && !isprint(**src))
		(*src)++;
	return **src ? *((*src)++) : '=';
}

char *gr_base64dec(const char *src, size_t *size) {
	size_t in_len = strlen(src);
	char *result, *dst;

	result = dst = malloc((in_len + 3) / 4 * 3 + 1);
	while (*src) {
		int a = gr_base64_digits[(unsigned char)gr_base64_getc(&src)];
		int b = gr_base64_digits[(unsigned char)gr_base64_getc(&src)];
		int c = gr_base64_digits[(unsigned char)gr_base64_getc(&src)];
		int d = gr_base64_digits[(unsigned char)gr_base64_getc(&src)];

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
	if (size) {
		*size = dst - result;
	}
	return result;
}
