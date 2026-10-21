/* Exercise the real PipeWire callbacks and capture scheduler with fake transports.
 * No compositor, PipeWire daemon or wall-clock sleeps are needed. */
#define xdpw_buffer_destroy test_buffer_destroy
#include "../src/screencast/pipewire_screencast.c"
#include "../src/screencast/wlr_screencast.c"

static struct xdpw_state state;
static struct xdpw_screencast_context ctx;
static struct xdpw_screencast_instance cast;
static struct xdpw_buffer buffer;
static struct xdpw_screencast_target target;
static struct spa_meta_header header;
static struct spa_meta meta = { .type = SPA_META_Header, .size = sizeof(header), .data = &header };
static struct spa_chunk chunk;
static struct spa_data data = { .chunk = &chunk };
static struct spa_buffer spa_buffer = { .n_metas = 1, .metas = &meta, .n_datas = 1, .datas = &data };
static struct pw_buffer pw_buffer;
static bool available = true;
static bool active;
static bool delayed = true;
static unsigned captures, queued, destroyed, dequeued;
static char fake_frame;

void logprint(enum LOGLEVEL level, char *msg, ...) {}
void fps_limit_measure_start(struct fps_limit_state *fps, double rate) {}
uint64_t fps_limit_measure_end(struct fps_limit_state *fps, double rate) {
	return delayed ? 1000000 : 0;
}
uint32_t pw_stream_get_node_id(struct pw_stream *stream) { return 1; }
const struct pw_properties *pw_stream_get_properties(struct pw_stream *stream) { return NULL; }
struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *stream) {
	if (!available) {
		return NULL;
	}
	available = false;
	dequeued++;
	return &pw_buffer;
}
int pw_stream_queue_buffer(struct pw_stream *stream, struct pw_buffer *buf) {
	assert(buf == &pw_buffer);
	assert(!active);
	assert(wl_list_empty(&state.timers));
	available = true;
	queued++;
	return 0;
}
int pw_stream_trigger_process(struct pw_stream *stream) { return 0; }
void xdpw_buffer_destroy(struct xdpw_buffer *buf) {
	assert(!active);
	assert(wl_list_empty(&state.timers));
	destroyed++;
}
void xdpw_screencast_instance_destroy(struct xdpw_screencast_instance *c) {
	abort(); // Not expected for these recoverable state changes.
}

static void capture(struct xdpw_screencast_instance *c) {
	assert(c->pwr_stream_state);
	assert(c->current_frame.xdpw_buffer == &buffer);
	assert(!active); // A second frame is a fatal ext-image-copy protocol error.
	active = true;
	captures++;
}
void xdpw_ext_ic_frame_capture(struct xdpw_screencast_instance *c) {
	capture(c);
	c->ext_session.frame = (void *)&fake_frame;
}
void xdpw_wlr_sc_frame_capture(struct xdpw_screencast_instance *c) {
	capture(c);
	c->wlr_session.wlr_frame = (void *)&fake_frame;
}
void xdpw_ext_ic_session_close(struct xdpw_screencast_instance *c) {
	if (c->ext_session.frame) {
		ext_image_copy_capture_frame_v1_destroy(c->ext_session.frame);
		c->ext_session.frame = NULL;
	}
}
void xdpw_wlr_sc_session_close(struct xdpw_screencast_instance *c) {
	active = false;
	c->wlr_session.wlr_frame = NULL;
}
uint32_t wl_proxy_get_version(struct wl_proxy *proxy) { return 1; }
struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
		const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
	assert(proxy == (void *)&fake_frame);
	assert(flags & WL_MARSHAL_FLAG_DESTROY);
	active = false;
	return NULL;
}

static void change_state(enum pw_stream_state old, enum pw_stream_state next) {
	pwr_handle_stream_state_changed(&cast, old, next, NULL);
	// PipeWire drives the graph in this older branch.
	if (next == PW_STREAM_STATE_STREAMING) {
		pwr_handle_stream_on_process(&cast);
	}
}
static void dispatch_timers(void) {
	while (!wl_list_empty(&state.timers)) {
		struct xdpw_timer *timer = wl_container_of(state.timers.next, timer, link);
		xdpw_event_loop_timer_func_t func = timer->func;
		void *data = timer->user_data;
		xdpw_destroy_timer(timer); // Same ownership as the real event loop.
		func(data);
	}
}
int main(int argc, char **argv) {
	assert(argc == 3);
	state.timer_poll_fd = -1;
	wl_list_init(&state.timers);
	ctx.state = &state;
	cast.target = &target;
	target.type = MONITOR;
	if (strcmp(argv[2], "wlr") != 0) {
		ctx.ext_image_copy_capture_manager = (void *)&ctx;
		ctx.ext_output_image_capture_source_manager = (void *)&ctx;
		if (strcmp(argv[2], "ext-window") == 0) {
			target.type = WINDOW;
			ctx.ext_foreign_toplevel_image_capture_source_manager = (void *)&ctx;
		} else if (strcmp(argv[2], "ext-vout") == 0) {
			target.type = VOUT;
			ctx.vwl_vout_image_capture_source_manager = (void *)&ctx;
		}
	} else {
		ctx.screencopy_manager = (void *)&ctx;
	}
	cast.ctx = &ctx;
	cast.initialized = true;
	wl_list_init(&buffer.link);
	pw_buffer.buffer = &spa_buffer;
	pw_buffer.user_data = &buffer;

	{
		change_state(PW_STREAM_STATE_PAUSED, PW_STREAM_STATE_STREAMING);
		assert(wl_list_length(&state.timers) == 1);
		if (strstr(argv[1], "active")) {
			dispatch_timers();
			assert(active);
		}
		if (strncmp(argv[1], "pause", 5) == 0) {
			// Pause before the first completed frame, including repeated pauses.
			for (unsigned i = 0; i < 3; i++) {
				change_state(PW_STREAM_STATE_STREAMING, PW_STREAM_STATE_PAUSED);
				assert(!active);
				assert(wl_list_empty(&state.timers));
				assert(queued == 0);
				assert(cast.current_frame.pw_buffer == &pw_buffer);
				assert(cast.current_frame.xdpw_buffer == &buffer);
				assert(dequeued == 1);
				if (strstr(argv[1], "remove")) {
					// PipeWire can replace the held buffer during a resize.
					pwr_handle_stream_remove_buffer(&cast, &pw_buffer);
					assert(cast.current_frame.pw_buffer == NULL);
					assert(cast.current_frame.xdpw_buffer == NULL);
					assert(destroyed == 1 && queued == 0);
					wl_list_init(&buffer.link);
					pw_buffer.user_data = &buffer;
					available = true;
				} else if (strstr(argv[1], "close")) {
					xdpw_pwr_stream_destroy(&cast);
					assert(queued == 0 && wl_list_empty(&state.timers));
					return 0;
				}
				change_state(PW_STREAM_STATE_PAUSED, PW_STREAM_STATE_STREAMING);
				unsigned before = captures;
				// A process callback must not start a second capture into the held buffer.
				pwr_handle_stream_on_process(&cast);
				dispatch_timers();
				assert(captures == before + 1);
				if (strstr(argv[1], "remove")) {
					assert(dequeued == 2);
					break;
				}
			}
			// Simulate a successful frame after resume. Only it may reach the consumer.
			xdpw_wlr_frame_capture_cancel(&cast);
			cast.current_frame.tv_sec = 42;
			cast.current_frame.tv_nsec = 123000000;
			cast.current_frame.completed = true;
			xdpw_pwr_enqueue_buffer(&cast);
			assert(queued == 1 && header.pts == 42123000000LL);
			assert(!(header.flags & SPA_META_HEADER_FLAG_CORRUPTED));
			assert(!(chunk.flags & SPA_CHUNK_FLAG_CORRUPTED));
			assert(cast.current_frame.pw_buffer == NULL);
		} else if (strncmp(argv[1], "remove", 6) == 0) {
			pwr_handle_stream_remove_buffer(&cast, &pw_buffer);
			assert(!active);
			assert(wl_list_empty(&state.timers));
			assert(destroyed == 1 && queued == 0);
			assert(cast.current_frame.pw_buffer == NULL);
		} else if (strcmp(argv[1], "error") == 0) {
			change_state(PW_STREAM_STATE_STREAMING, PW_STREAM_STATE_ERROR);
			assert(wl_list_empty(&state.timers));
			assert(!active);
		} else if (strcmp(argv[1], "close") != 0) {
			abort();
		}
	}
	xdpw_wlr_session_close(&cast);
	assert(!active);
	assert(wl_list_empty(&state.timers));
	return 0;
}
