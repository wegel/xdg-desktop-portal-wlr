#include "wlr_screencast.h"

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "vwl-vout-image-capture-source-unstable-v1-client-protocol.h"
#include <drm_fourcc.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <assert.h>
#include <wayland-client-protocol.h>
#include <xf86drm.h>

#include "screencast.h"
#include "wlr_screencopy.h"
#include "ext_image_copy.h"
#include "xdpw.h"
#include "logger.h"
#include "fps_limit.h"

static bool use_ext_image_copy_capture(struct xdpw_screencast_instance *cast) {
	if (cast->ctx->ext_image_copy_capture_manager == NULL) {
		return false;
	}

	switch (cast->target->type) {
	case MONITOR:
		return cast->ctx->ext_output_image_capture_source_manager != NULL;
	case WINDOW:
		return cast->ctx->ext_foreign_toplevel_image_capture_source_manager != NULL;
	case VOUT:
		return cast->ctx->vwl_vout_image_capture_source_manager != NULL;
	}

	return false;
}

static void wlr_frame_capture_start(struct xdpw_screencast_instance *cast) {
	fps_limit_measure_start(&cast->fps_limit, cast->framerate);
	if (use_ext_image_copy_capture(cast)) {
		xdpw_ext_ic_frame_capture(cast);
	} else if (cast->target->type == MONITOR && cast->ctx->screencopy_manager) {
		xdpw_wlr_sc_frame_capture(cast);
	}
}

static void wlr_frame_capture_timer(void *data) {
	wlr_frame_capture_start((struct xdpw_screencast_instance *)data);
}

void xdpw_wlr_frame_capture(struct xdpw_screencast_instance *cast) {
	uint64_t delay_ns = fps_limit_measure_end(&cast->fps_limit, cast->framerate);
	if (delay_ns > 0) {
		xdpw_add_timer(cast->ctx->state, delay_ns, wlr_frame_capture_timer, cast);
	} else {
		wlr_frame_capture_start(cast);
	}
}

void xdpw_wlr_session_close(struct xdpw_screencast_instance *cast) {
	if (use_ext_image_copy_capture(cast)) {
		xdpw_ext_ic_session_close(cast);
	} else if (cast->target->type == MONITOR && cast->ctx->screencopy_manager) {
		xdpw_wlr_sc_session_close(cast);
	}
}

int xdpw_wlr_session_init(struct xdpw_screencast_instance *cast) {
	if (use_ext_image_copy_capture(cast)) {
		return xdpw_ext_ic_session_init(cast);
	} else if (cast->target->type == MONITOR && cast->ctx->screencopy_manager) {
		return xdpw_wlr_sc_session_init(cast);
	}
	return -1;
}

static void wlr_output_handle_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t phys_width, int32_t phys_height,
		int32_t subpixel, const char *make, const char *model, int32_t transform) {
	struct xdpw_wlr_output *output = data;
	output->transformation = transform;
}

static void wlr_output_handle_mode(void *data, struct wl_output *wl_output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		struct xdpw_wlr_output *output = data;
		output->framerate = (float)refresh/1000;
	}
}

static void wlr_output_handle_done(void *data, struct wl_output *wl_output) {
	/* Nothing to do */
}

static void wlr_output_handle_scale(void *data, struct wl_output *wl_output,
		int32_t factor) {
	/* Nothing to do */
}

static void wlr_output_handle_name(void *data, struct wl_output *wl_output,
		const char *name) {
	struct xdpw_wlr_output *output = data;
	output->name = strdup(name);
}

static void wlr_output_handle_description(void *data, struct wl_output *wl_output,
		const char *description) {
	struct xdpw_wlr_output *output = data;
	free(output->description);
	output->description = strdup(description);
}

static const struct wl_output_listener wlr_output_listener = {
	.geometry = wlr_output_handle_geometry,
	.mode = wlr_output_handle_mode,
	.done = wlr_output_handle_done,
	.scale = wlr_output_handle_scale,
	.name = wlr_output_handle_name,
	.description = wlr_output_handle_description,
};

static void xdg_output_handle_logical_position(void *data, struct zxdg_output_v1 *xdg_output_v1,
		int32_t x, int32_t y) {
	struct xdpw_wlr_output *output = data;
	output->x = x;
	output->y = y;
}

static void xdg_output_handle_logical_size(void *data, struct zxdg_output_v1 *xdg_output_v1,
		int32_t width, int32_t height) {
	struct xdpw_wlr_output *output = data;
	output->width = width;
	output->height = height;
}

static void xdg_output_handle_done(void *data, struct zxdg_output_v1 *xdg_output_v1) {
	/* Nothing to do */
}

static void xdg_output_handle_name(void *data, struct zxdg_output_v1 *xdg_output_v1,
		const char *name) {
	/* Nothing to do */
}

static void xdg_output_handle_description(void *data, struct zxdg_output_v1 *xdg_output_v1,
		const char *description) {
	/* Nothing to do */
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_handle_logical_position,
	.logical_size = xdg_output_handle_logical_size,
	.done = xdg_output_handle_done,
	.name = xdg_output_handle_name,
	.description = xdg_output_handle_description,
};

struct xdpw_wlr_output *xdpw_wlr_output_find_by_name(struct wl_list *output_list, const char *name) {
	struct xdpw_wlr_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, output_list, link) {
		if (strcmp(output->name, name) == 0) {
			return output;
		}
	}
	return NULL;
}

static struct xdpw_wlr_output *xdpw_wlr_output_find(struct xdpw_screencast_context *ctx,
		struct wl_output *out, uint32_t id) {
	struct xdpw_wlr_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &ctx->output_list, link) {
		if ((output->output == out) || (output->id == id)) {
			return output;
		}
	}
	return NULL;
}

bool xdpw_wlr_target_from_data(struct xdpw_screencast_context *ctx, struct xdpw_screencast_target *target,
		struct xdpw_screencast_restore_data *data) {
	struct xdpw_wlr_output *out = NULL;
	out = xdpw_wlr_output_find_by_name(&ctx->output_list, data->output_name);

	if (!out) {
		return false;
	}
	target->type = MONITOR;
	target->output = out;
	return true;
}

static void wlr_remove_output(struct xdpw_wlr_output *out) {
	free(out->name);
	free(out->description);
	if (out->xdg_output) {
		zxdg_output_v1_destroy(out->xdg_output);
	}
	wl_output_destroy(out->output);
	wl_list_remove(&out->link);
	free(out);
}

static void wlr_format_modifier_pair_add(struct xdpw_screencast_context *ctx,
		uint32_t format, uint64_t modifier) {
	struct xdpw_format_modifier_pair *fm_pair;
	wl_array_for_each(fm_pair, &ctx->format_modifier_pairs) {
		if (fm_pair->fourcc == format && fm_pair->modifier == modifier) {
			logprint(TRACE, "wlroots: skipping duplicated format %u (%lu)", fm_pair->fourcc, fm_pair->modifier);
			return;
		}
	}

	fm_pair = wl_array_add(&ctx->format_modifier_pairs, sizeof(struct xdpw_format_modifier_pair));
	fm_pair->fourcc = format;
	fm_pair->modifier = modifier;
	logprint(TRACE, "wlroots: format %u (%lu)", fm_pair->fourcc, fm_pair->modifier);
}

static void linux_dmabuf_handle_modifier(void *data,
		struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
		uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo) {
	struct xdpw_screencast_context *ctx = data;

	logprint(TRACE, "wlroots: linux_dmabuf_handle_modifier called");

	uint64_t modifier = (((uint64_t)modifier_hi) << 32) | modifier_lo;
	wlr_format_modifier_pair_add(ctx, format, modifier);
}

static void noop() {
       // This space intentionally left blank
}

static const struct zwp_linux_dmabuf_v1_listener linux_dmabuf_listener = {
	.format = noop,
	.modifier = linux_dmabuf_handle_modifier,
};

static void linux_dmabuf_feedback_handle_main_device(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1, struct wl_array *device_arr) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_handle_main_device called");

	assert(ctx->gbm == NULL);

	dev_t device;
	assert(device_arr->size == sizeof(device));
	memcpy(&device, device_arr->data, sizeof(device));

	drmDevice *drmDev;
	if (drmGetDeviceFromDevId(device, /* flags */ 0, &drmDev) != 0) {
		logprint(WARN, "wlroots: unable to open main device");
		ctx->state->config->screencast_conf.force_mod_linear = true;
		return;
	}
	ctx->gbm = xdpw_gbm_device_create(drmDev);
}

static void linux_dmabuf_feedback_format_table(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1, int fd, uint32_t size) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_format_table called");

	wl_array_release(&ctx->format_modifier_pairs);
	wl_array_init(&ctx->format_modifier_pairs);

	ctx->feedback_data.format_table_data = mmap(NULL , size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (ctx->feedback_data.format_table_data == MAP_FAILED) {
		ctx->feedback_data.format_table_data = NULL;
		ctx->feedback_data.format_table_size = 0;
		return;
	}
	ctx->feedback_data.format_table_size = size;
}

static void linux_dmabuf_feedback_handle_done(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_handle_done called");

	if (ctx->feedback_data.format_table_data) {
		munmap(ctx->feedback_data.format_table_data, ctx->feedback_data.format_table_size);
	}
	ctx->feedback_data.format_table_data = NULL;
	ctx->feedback_data.format_table_size = 0;
}

static void linux_dmabuf_feedback_tranche_target_devices(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1, struct wl_array *device_arr) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_tranche_target_devices called");

	dev_t device;
	assert(device_arr->size == sizeof(device));
	memcpy(&device, device_arr->data, sizeof(device));

	drmDevice *drmDev;
	if (drmGetDeviceFromDevId(device, /* flags */ 0, &drmDev) != 0) {
		return;
	}

	if (ctx->gbm) {
		drmDevice *drmDevRenderer = NULL;
		drmGetDevice2(gbm_device_get_fd(ctx->gbm), /* flags */ 0, &drmDevRenderer);
		ctx->feedback_data.device_used = drmDevicesEqual(drmDevRenderer, drmDev);
	} else {
		ctx->gbm = xdpw_gbm_device_create(drmDev);
		ctx->feedback_data.device_used = ctx->gbm != NULL;
	}
}

static void linux_dmabuf_feedback_tranche_flags(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1, uint32_t flags) {
	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_tranche_flags called");
}

static void linux_dmabuf_feedback_tranche_formats(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1, struct wl_array *indices) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_tranche_formats called");

	if (!ctx->feedback_data.device_used || !ctx->feedback_data.format_table_data) {
		return;
	}
	struct fm_entry {
		uint32_t format;
		uint32_t padding;
		uint64_t modifier;
	};
	// An entry in the table has to be 16 bytes long
	assert(sizeof(struct fm_entry) == 16);

	uint32_t n_modifiers = ctx->feedback_data.format_table_size/sizeof(struct fm_entry);
	struct fm_entry *fm_entry = ctx->feedback_data.format_table_data;
	uint16_t *idx;
	wl_array_for_each(idx, indices) {
		if (*idx >= n_modifiers) {
			continue;
		}
		wlr_format_modifier_pair_add(ctx, (fm_entry + *idx)->format, (fm_entry + *idx)->modifier);
	}
}

static void linux_dmabuf_feedback_tranche_done(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_tranche_done called");

	ctx->feedback_data.device_used = false;
}

static const struct zwp_linux_dmabuf_feedback_v1_listener linux_dmabuf_listener_feedback = {
	.main_device = linux_dmabuf_feedback_handle_main_device,
	.format_table = linux_dmabuf_feedback_format_table,
	.done = linux_dmabuf_feedback_handle_done,
	.tranche_target_device = linux_dmabuf_feedback_tranche_target_devices,
	.tranche_flags = linux_dmabuf_feedback_tranche_flags,
	.tranche_formats = linux_dmabuf_feedback_tranche_formats,
	.tranche_done = linux_dmabuf_feedback_tranche_done,
};

static void foreign_toplevel_destroy(struct xdpw_toplevel *toplevel) {
	wl_list_remove(&toplevel->link);
	ext_foreign_toplevel_handle_v1_destroy(toplevel->handle);
	free(toplevel->title);
	free(toplevel->app_id);
	free(toplevel->identifier);
	free(toplevel);
}

static void foreign_toplevel_handle_closed(void *data,
		struct ext_foreign_toplevel_handle_v1 *handle) {
	struct xdpw_toplevel *toplevel = data;
	foreign_toplevel_destroy(toplevel);
}

static void foreign_toplevel_handle_done(void *data,
		struct ext_foreign_toplevel_handle_v1 *handle) {
}

static void foreign_toplevel_handle_title(void *data,
		struct ext_foreign_toplevel_handle_v1 *handle, const char *title) {
	struct xdpw_toplevel *toplevel = data;
	free(toplevel->title);
	toplevel->title = strdup(title);
}

static void foreign_toplevel_handle_app_id(void *data,
		struct ext_foreign_toplevel_handle_v1 *handle, const char *app_id) {
	struct xdpw_toplevel *toplevel = data;
	free(toplevel->app_id);
	toplevel->app_id = strdup(app_id);
}

static void foreign_toplevel_handle_identifier(void *data,
		struct ext_foreign_toplevel_handle_v1 *handle, const char *identifier) {
	struct xdpw_toplevel *toplevel = data;
	free(toplevel->identifier);
	toplevel->identifier = strdup(identifier);
}

static const struct ext_foreign_toplevel_handle_v1_listener foreign_toplevel_handle_listener = {
	.closed = foreign_toplevel_handle_closed,
	.done = foreign_toplevel_handle_done,
	.title = foreign_toplevel_handle_title,
	.app_id = foreign_toplevel_handle_app_id,
	.identifier = foreign_toplevel_handle_identifier,
};

static void foreign_toplevel_list_handle_toplevel(void *data,
		struct ext_foreign_toplevel_list_v1 *list,
		struct ext_foreign_toplevel_handle_v1 *handle) {
	struct xdpw_screencast_context *ctx = data;

	struct xdpw_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (toplevel == NULL) {
		return;
	}

	toplevel->handle = handle;
	wl_list_insert(&ctx->toplevels, &toplevel->link);
	ext_foreign_toplevel_handle_v1_add_listener(handle, &foreign_toplevel_handle_listener, toplevel);
}

static void foreign_toplevel_list_handle_finished(void *data,
		struct ext_foreign_toplevel_list_v1 *list) {
	struct xdpw_screencast_context *ctx = data;

	struct xdpw_toplevel *toplevel, *toplevel_tmp;
	wl_list_for_each_safe(toplevel, toplevel_tmp, &ctx->toplevels, link) {
		foreign_toplevel_destroy(toplevel);
	}

	ext_foreign_toplevel_list_v1_destroy(ctx->ext_foreign_toplevel_list);
	ctx->ext_foreign_toplevel_list = NULL;
}

static const struct ext_foreign_toplevel_list_v1_listener foreign_toplevel_list_listener = {
	.toplevel = foreign_toplevel_list_handle_toplevel,
	.finished = foreign_toplevel_list_handle_finished,
};

static void vwl_destroy_vout(struct xdpw_vout *vout) {
	wl_list_remove(&vout->link);
	free(vout->name);
	free(vout->output_name);
	free(vout);
}

static void vwl_clear_vouts(struct xdpw_screencast_context *ctx) {
	struct xdpw_vout *vout, *tmp;

	wl_list_for_each_safe(vout, tmp, &ctx->vouts, link) {
		vwl_destroy_vout(vout);
	}
}

static const char *skip_json_ws(const char *s) {
	while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') {
		++s;
	}
	return s;
}

static const char *skip_json_string(const char *s) {
	if (*s != '"') {
		return NULL;
	}
	++s;
	while (*s) {
		if (*s == '\\') {
			if (!s[1]) {
				return NULL;
			}
			s += 2;
			continue;
		}
		if (*s == '"') {
			return s + 1;
		}
		++s;
	}
	return NULL;
}

static const char *find_matching_json(const char *s, char open_ch, char close_ch) {
	int depth = 0;

	for (; *s; ++s) {
		if (*s == '"') {
			s = skip_json_string(s);
			if (s == NULL) {
				return NULL;
			}
			--s;
			continue;
		}
		if (*s == open_ch) {
			++depth;
		} else if (*s == close_ch) {
			--depth;
			if (depth == 0) {
				return s;
			}
		}
	}

	return NULL;
}

static const char *find_json_field(const char *obj, const char *key) {
	char pattern[128];
	const char *p;
	size_t key_len;

	key_len = strlen(key);
	if (key_len + 3 >= sizeof(pattern)) {
		return NULL;
	}

	pattern[0] = '"';
	memcpy(pattern + 1, key, key_len);
	pattern[key_len + 1] = '"';
	pattern[key_len + 2] = '\0';

	p = obj;
	while ((p = strstr(p, pattern)) != NULL) {
		const char *after = p + key_len + 2;
		after = skip_json_ws(after);
		if (*after != ':') {
			p = after;
			continue;
		}
		return skip_json_ws(after + 1);
	}

	return NULL;
}

static char *dup_json_string_value(const char *value) {
	const char *end;
	const char *p;
	char *out;
	size_t len = 0;

	if (value == NULL || *value != '"') {
		return NULL;
	}

	end = skip_json_string(value);
	if (end == NULL) {
		return NULL;
	}

	for (p = value + 1; p < end - 1; ++p) {
		if (*p == '\\' && p + 1 < end - 1) {
			++p;
		}
		++len;
	}

	out = calloc(len + 1, 1);
	if (out == NULL) {
		return NULL;
	}

	len = 0;
	for (p = value + 1; p < end - 1; ++p) {
		if (*p == '\\' && p + 1 < end - 1) {
			++p;
		}
		out[len++] = *p;
	}

	return out;
}

static bool parse_json_uint_field(const char *obj, const char *key, uint32_t *value) {
	const char *field = find_json_field(obj, key);
	char *end = NULL;
	unsigned long parsed;

	if (field == NULL) {
		return false;
	}

	parsed = strtoul(field, &end, 10);
	if (end == field) {
		return false;
	}

	*value = (uint32_t)parsed;
	return true;
}

static bool parse_json_int_field(const char *obj, const char *key, int *value) {
	const char *field = find_json_field(obj, key);
	char *end = NULL;
	long parsed;

	if (field == NULL) {
		return false;
	}

	parsed = strtol(field, &end, 10);
	if (end == field) {
		return false;
	}

	*value = (int)parsed;
	return true;
}

static char *parse_json_string_field(const char *obj, const char *key) {
	return dup_json_string_value(find_json_field(obj, key));
}

static char *parse_vout_output_name(const char *obj) {
	const char *outputs = find_json_field(obj, "outputs");
	const char *first;

	if (outputs == NULL || *outputs != '[') {
		return NULL;
	}

	first = skip_json_ws(outputs + 1);
	return dup_json_string_value(first);
}

static bool parse_vout_geometry(const char *obj, int *x, int *y, int *width, int *height) {
	const char *regions = find_json_field(obj, "regions");
	const char *region_obj;
	const char *geometry;
	uint32_t uw, uh;

	if (regions == NULL || *regions != '[') {
		return false;
	}

	region_obj = skip_json_ws(regions + 1);
	if (*region_obj != '{') {
		return false;
	}

	geometry = find_json_field(region_obj, "geometry");
	if (geometry == NULL || *geometry != '{') {
		return false;
	}

	if (!parse_json_int_field(geometry, "x", x) ||
			!parse_json_int_field(geometry, "y", y) ||
			!parse_json_uint_field(geometry, "width", &uw) ||
			!parse_json_uint_field(geometry, "height", &uh)) {
		return false;
	}

	*width = (int)uw;
	*height = (int)uh;
	return true;
}

static char *vwl_query_state(void) {
	static const char request[] = "{\"id\":1,\"type\":\"get_state\"}\n";
	char default_socket[PATH_MAX];
	const char *runtime_dir;
	const char *socket_path;
	struct sockaddr_un addr = {0};
	char *buffer = NULL;
	size_t size = 0;
	size_t used = 0;
	ssize_t nread;
	int fd = -1;

	socket_path = getenv("VWL_SOCKET");
	if (socket_path == NULL || socket_path[0] == '\0') {
		runtime_dir = getenv("XDG_RUNTIME_DIR");
		if (runtime_dir == NULL || runtime_dir[0] == '\0') {
			return NULL;
		}
		if (snprintf(default_socket, sizeof(default_socket), "%s/vwl.sock", runtime_dir) >= (int)sizeof(default_socket)) {
			return NULL;
		}
		socket_path = default_socket;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return NULL;
	}

	addr.sun_family = AF_UNIX;
	if (strlen(socket_path) >= sizeof(addr.sun_path)) {
		close(fd);
		return NULL;
	}
	memcpy(addr.sun_path, socket_path, strlen(socket_path) + 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return NULL;
	}

	if (write(fd, request, sizeof(request) - 1) < 0) {
		close(fd);
		return NULL;
	}

	size = 4096;
	buffer = calloc(size, 1);
	if (buffer == NULL) {
		close(fd);
		return NULL;
	}

	while ((nread = read(fd, buffer + used, size - used - 1)) > 0) {
		char *newline;

		used += (size_t)nread;
		buffer[used] = '\0';
		newline = strchr(buffer, '\n');
		if (newline != NULL) {
			*newline = '\0';
			close(fd);
			return buffer;
		}

		if (used + 1 < size) {
			continue;
		}

		size *= 2;
		buffer = realloc(buffer, size);
		if (buffer == NULL) {
			close(fd);
			return NULL;
		}
	}

	close(fd);
	if (nread < 0) {
		free(buffer);
		return NULL;
	}

	return buffer;
}

void xdpw_vwl_refresh_vouts(struct xdpw_screencast_context *ctx) {
	char *json;
	const char *list;
	const char *list_end;
	const char *entry;

	vwl_clear_vouts(ctx);

	json = vwl_query_state();
	if (json == NULL) {
		return;
	}

	list = find_json_field(json, "virtual_outputs");
	if (list == NULL || *list != '[') {
		free(json);
		return;
	}
	list_end = find_matching_json(list, '[', ']');
	if (list_end == NULL) {
		free(json);
		return;
	}

	entry = skip_json_ws(list + 1);
	while (entry && entry < list_end && *entry != ']') {
		const char *entry_end;
		struct xdpw_vout *vout;

		if (*entry != '{') {
			break;
		}

		entry_end = find_matching_json(entry, '{', '}');
		if (entry_end == NULL || entry_end > list_end) {
			break;
		}

		vout = calloc(1, sizeof(*vout));
		if (vout == NULL) {
			break;
		}
		wl_list_init(&vout->link);

		if (!parse_json_uint_field(entry, "id", &vout->id)) {
			free(vout);
			break;
		}
		vout->name = parse_json_string_field(entry, "name");
		vout->output_name = parse_vout_output_name(entry);
		if (vout->name == NULL) {
			free(vout->output_name);
			free(vout);
			break;
		}
		if (!parse_vout_geometry(entry, &vout->x, &vout->y, &vout->width, &vout->height)) {
			vout->x = vout->y = 0;
			vout->width = vout->height = 0;
		}
		if (vout->output_name != NULL) {
			struct xdpw_wlr_output *output = xdpw_wlr_output_find_by_name(&ctx->output_list, vout->output_name);
			if (output != NULL) {
				vout->framerate = output->framerate;
			}
		}

		wl_list_insert(&ctx->vouts, &vout->link);
		entry = skip_json_ws(entry_end + 1);
		if (*entry == ',') {
			entry = skip_json_ws(entry + 1);
		}
	}

	free(json);
}

static void wlr_registry_handle_add(void *data, struct wl_registry *reg,
		uint32_t id, const char *interface, uint32_t ver) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: interface to register %s  (Version: %u)",interface, ver);
	if (!strcmp(interface, wl_output_interface.name)) {
		struct xdpw_wlr_output *output = calloc(1, sizeof(*output));

		output->id = id;
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, WL_OUTPUT_VERSION);
		output->output = wl_registry_bind(reg, id, &wl_output_interface, WL_OUTPUT_VERSION);

		if (ctx->xdg_output_manager) {
			output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
				ctx->xdg_output_manager, output->output);
			zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener, output);
		}

		wl_output_add_listener(output->output, &wlr_output_listener, output);
		wl_list_insert(&ctx->output_list, &output->link);
	}

	if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
		uint32_t version = ver;
		if (SC_MANAGER_VERSION < ver) {
			version = SC_MANAGER_VERSION;
		} else if (ver < SC_MANAGER_VERSION_MIN) {
			version = SC_MANAGER_VERSION_MIN;
		}
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, version);
		ctx->screencopy_manager = wl_registry_bind(
			reg, id, &zwlr_screencopy_manager_v1_interface, version);
	}

	if (!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, ver);
		ctx->ext_output_image_capture_source_manager = wl_registry_bind(
				reg, id, &ext_output_image_capture_source_manager_v1_interface, 1);
	}

	if (!strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name)) {
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, ver);
		ctx->ext_foreign_toplevel_image_capture_source_manager = wl_registry_bind(
				reg, id, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	}

	if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, ver);
		ctx->ext_image_copy_capture_manager = wl_registry_bind(
				reg, id, &ext_image_copy_capture_manager_v1_interface, 1);
	}

	if (!strcmp(interface, ext_foreign_toplevel_list_v1_interface.name)) {
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, ver);
		ctx->ext_foreign_toplevel_list = wl_registry_bind(
				reg, id, &ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(ctx->ext_foreign_toplevel_list,
				&foreign_toplevel_list_listener, ctx);
	}

	if (!strcmp(interface, vwl_vout_image_capture_source_manager_v1_interface.name)) {
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, ver);
		ctx->vwl_vout_image_capture_source_manager = wl_registry_bind(
				reg, id, &vwl_vout_image_capture_source_manager_v1_interface, 1);
	}

	if (strcmp(interface, wl_shm_interface.name) == 0) {
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, WL_SHM_VERSION);
		ctx->shm = wl_registry_bind(reg, id, &wl_shm_interface, WL_SHM_VERSION);
	}

	if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
		uint32_t version = ver;
		if (LINUX_DMABUF_VERSION < ver) {
			version = LINUX_DMABUF_VERSION;
		} else if (LINUX_DMABUF_VERSION_MIN > ver) {
			logprint(INFO, "wlroots: interface %s (Version %u) is required for DMA-BUF screencast", interface, LINUX_DMABUF_VERSION_MIN);
			return;
		}
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, version);
		ctx->linux_dmabuf = wl_registry_bind(reg, id, &zwp_linux_dmabuf_v1_interface, version);
	}

	if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0
			&& ver >= XDG_OUTPUT_VERSION_MIN) {
		uint32_t version = ver;
		if (XDG_OUTPUT_VERSION < ver) {
			version = XDG_OUTPUT_VERSION;
		}
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, version);
		ctx->xdg_output_manager = wl_registry_bind(
			reg, id, &zxdg_output_manager_v1_interface, version);
	}
}

static void wlr_registry_handle_remove(void *data, struct wl_registry *reg,
		uint32_t id) {
	struct xdpw_screencast_context *ctx = data;
	struct xdpw_wlr_output *output = xdpw_wlr_output_find(ctx, NULL, id);
	if (output) {
		logprint(DEBUG, "wlroots: output removed (%s)", output->name);
		struct xdpw_screencast_instance *cast, *tmp;
		wl_list_for_each_safe(cast, tmp, &ctx->screencast_instances, link) {
			if (cast->target->output == output) {
				// screencopy might be in process for this instance
				xdpw_screencast_instance_destroy(cast);
			}
		}
		wlr_remove_output(output);
	}
}

static const struct wl_registry_listener wlr_registry_listener = {
	.global = wlr_registry_handle_add,
	.global_remove = wlr_registry_handle_remove,
};

int xdpw_wlr_screencopy_init(struct xdpw_state *state) {
	struct xdpw_screencast_context *ctx = &state->screencast;

	wl_list_init(&ctx->output_list);
	wl_list_init(&ctx->screencast_instances);
	wl_list_init(&ctx->toplevels);
	wl_list_init(&ctx->vouts);
	wl_array_init(&ctx->format_modifier_pairs);

	// retrieve registry
	ctx->registry = wl_display_get_registry(state->wl_display);
	wl_registry_add_listener(ctx->registry, &wlr_registry_listener, ctx);

	wl_display_roundtrip(state->wl_display);

	logprint(DEBUG, "wayland: registry listeners run");

	if (ctx->ext_image_copy_capture_manager && ctx->ext_output_image_capture_source_manager) {
		logprint(DEBUG, "wayland: using ext_image_copy_capture");
	} else if (ctx->screencopy_manager && ctx->linux_dmabuf) {
		if (zwp_linux_dmabuf_v1_get_version(ctx->linux_dmabuf) >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) {
			ctx->linux_dmabuf_feedback = zwp_linux_dmabuf_v1_get_default_feedback(ctx->linux_dmabuf);
			zwp_linux_dmabuf_feedback_v1_add_listener(ctx->linux_dmabuf_feedback, &linux_dmabuf_listener_feedback, ctx);
		} else {
			zwp_linux_dmabuf_v1_add_listener(ctx->linux_dmabuf, &linux_dmabuf_listener, ctx);
		}

		wl_display_roundtrip(state->wl_display);

		logprint(DEBUG, "wayland: dmabuf_feedback listeners run");

		// make sure we have a gbm device
		if (!ctx->gbm) {
			ctx->gbm = xdpw_gbm_device_create(NULL);
			if (!ctx->gbm) {
				logprint(ERROR, "System doesn't support gbm!");
			}
		}
	} else {
		logprint(ERROR, "Compositor supports neither ext_image_copy_capture or wlr_screencopy!");
		return -1;
	}

	if (ctx->ext_image_copy_capture_manager &&
			(ctx->ext_foreign_toplevel_image_capture_source_manager || ctx->vwl_vout_image_capture_source_manager)) {
		state->screencast_source_types |= WINDOW;
	}

	// make sure our wlroots supports shm protocol
	if (!ctx->shm) {
		logprint(ERROR, "Compositor doesn't support %s!", "wl_shm");
		return -1;
	}

	if (ctx->xdg_output_manager) {
		struct xdpw_wlr_output *output;
		wl_list_for_each(output, &ctx->output_list, link) {
			if (!output->xdg_output) {
				output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
					ctx->xdg_output_manager, output->output);
				zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener, output);
			}
		}
		wl_display_roundtrip(state->wl_display);
		logprint(DEBUG, "wayland: xdg_output listeners run");
	}

	xdpw_vwl_refresh_vouts(ctx);

	return 0;
}

void xdpw_wlr_screencopy_finish(struct xdpw_screencast_context *ctx) {
	wl_array_release(&ctx->format_modifier_pairs);

	struct xdpw_wlr_output *output, *tmp_o;
	wl_list_for_each_safe(output, tmp_o, &ctx->output_list, link) {
		wl_list_remove(&output->link);
		if (output->xdg_output) {
			zxdg_output_v1_destroy(output->xdg_output);
		}
		wl_output_destroy(output->output);
	}

	struct xdpw_toplevel *toplevel, *toplevel_tmp;
	wl_list_for_each_safe(toplevel, toplevel_tmp, &ctx->toplevels, link) {
		foreign_toplevel_destroy(toplevel);
	}

	vwl_clear_vouts(ctx);

	struct xdpw_screencast_instance *cast, *tmp_c;
	wl_list_for_each_safe(cast, tmp_c, &ctx->screencast_instances, link) {
		xdpw_screencast_instance_destroy(cast);
	}

	if (ctx->screencopy_manager) {
		zwlr_screencopy_manager_v1_destroy(ctx->screencopy_manager);
	}
	if (ctx->ext_image_copy_capture_manager) {
		ext_image_copy_capture_manager_v1_destroy(ctx->ext_image_copy_capture_manager);
	}
	if (ctx->ext_output_image_capture_source_manager) {
		ext_output_image_capture_source_manager_v1_destroy(ctx->ext_output_image_capture_source_manager);
	}
	if (ctx->ext_foreign_toplevel_image_capture_source_manager) {
		ext_foreign_toplevel_image_capture_source_manager_v1_destroy(ctx->ext_foreign_toplevel_image_capture_source_manager);
	}
	if (ctx->vwl_vout_image_capture_source_manager) {
		vwl_vout_image_capture_source_manager_v1_destroy(ctx->vwl_vout_image_capture_source_manager);
	}
	if (ctx->shm) {
		wl_shm_destroy(ctx->shm);
	}
	if (ctx->gbm) {
		int fd = gbm_device_get_fd(ctx->gbm);
		gbm_device_destroy(ctx->gbm);
		close(fd);
	}
	if (ctx->linux_dmabuf_feedback) {
		zwp_linux_dmabuf_feedback_v1_destroy(ctx->linux_dmabuf_feedback);
	}
	if (ctx->linux_dmabuf) {
		zwp_linux_dmabuf_v1_destroy(ctx->linux_dmabuf);
	}
	if (ctx->ext_foreign_toplevel_list) {
		ext_foreign_toplevel_list_v1_destroy(ctx->ext_foreign_toplevel_list);
	}
	if (ctx->xdg_output_manager) {
		zxdg_output_manager_v1_destroy(ctx->xdg_output_manager);
	}
	if (ctx->registry) {
		wl_registry_destroy(ctx->registry);
	}
}
