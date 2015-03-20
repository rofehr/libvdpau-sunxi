/*
 * Copyright (c) 2013 Jens Kuske <jenskuske@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "vdpau_private.h"
#include <time.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fb.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>

#include "queue.h"

#include "sunxi_disp_ioctl.h"
#include "ve.h"
#include "rgba.h"

static pthread_t presentation_thread_id;

static QUEUE *queue;

static VdpTime frame_time;

static int end_presentation;

typedef struct task
{
	struct timespec		when;
	uint32_t		clip_width;
	uint32_t		clip_height;
	VdpOutputSurface	surface;
	VdpPresentationQueue	queue_id;
} task_t;

static VdpTime get_time(void)
{
	struct timespec tp;

	if (clock_gettime(CLOCK_REALTIME, &tp) == -1)
		return 0;

	return (uint64_t)tp.tv_sec * 1000000000ULL + (uint64_t)tp.tv_nsec;
}

static struct timespec
vdptime2timespec(VdpTime t)
{
	struct timespec res;
	res.tv_sec = t / (1000*1000*1000);
	res.tv_nsec = t % (1000*1000*1000);
	return res;
}

VdpStatus vdp_presentation_queue_display(VdpPresentationQueue presentation_queue,
                                         VdpOutputSurface surface,
                                         uint32_t clip_width,
                                         uint32_t clip_height,
                                         VdpTime earliest_presentation_time)
{
	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	output_surface_ctx_t *os = handle_get(surface);
	if (!os)
		return VDP_STATUS_INVALID_HANDLE;

	task_t *task = (task_t *)calloc(1, sizeof(task_t));
	task->when = vdptime2timespec(earliest_presentation_time);
	task->clip_width = clip_width;
	task->clip_height = clip_height;
	task->surface = surface;
	task->queue_id = presentation_queue;
	os->first_presentation_time = 0;
	os->status = VDP_PRESENTATION_QUEUE_STATUS_QUEUED;

	if(q_push_tail(queue, task))
	{
		VDPAU_DBG("Error inserting task");
		free(task);
	}

	return VDP_STATUS_OK;
}

static VdpStatus do_presentation_queue_display(task_t *task)
{
	queue_ctx_t *q = handle_get(task->queue_id);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	output_surface_ctx_t *os = handle_get(task->surface);
	if (!os)
		return VDP_STATUS_INVALID_HANDLE;

	uint32_t clip_width = task->clip_width;
	uint32_t clip_height = task->clip_height;

	// FIXME: not correct possition if no surface is in queue
	int i = 0;
	while (XPending(q->device->display) && i++<20)
	{
		XEvent ev;
		XNextEvent(q->device->display, &ev);

		if (ev.type == ConfigureNotify)	// window dimension or position has changed
		{
			VDPAU_DBG("%p %p %dx%d%+d%+d", (void*)ev.xconfigure.event, (void*)ev.xconfigure.window,
				ev.xconfigure.width, ev.xconfigure.height, ev.xconfigure.x, ev.xconfigure.y);

			if (ev.xconfigure.x != q->target->drawable_x
					|| ev.xconfigure.y != q->target->drawable_y
					/*|| ev.xconfigure.width != q->target->drawable_width
					|| ev.xconfigure.height != q->target->drawable_height*/)
			{
				q->target->drawable_x = ev.xconfigure.x;
				q->target->drawable_y = ev.xconfigure.y;
				//q->target->drawable_width = ev.xconfigure.width;
				//q->target->drawable_height = ev.xconfigure.height;

				q->target->drawable_changed = 1;
			}
		}
	}
	if (q->target->drawable_changed)
	{
		// get new window offset
		Window dummy;
		XTranslateCoordinates(q->device->display, q->target->drawable, RootWindow(q->device->display, q->device->screen),
		      0, 0, &q->target->x, &q->target->y, &dummy);

		// FIXME: not correct possition if no surface is in queue
		q->target->drawable_changed = 0;

		XClearWindow(q->device->display, q->target->drawable);

		uint32_t args[4] = { 0, q->target->layer, 0, 0 };
		__disp_rect_t scn_win, src_win;

		// Get scn window dimension and position
		scn_win.x = q->target->x + os->video_dst_rect.x0;
		scn_win.y = q->target->y + os->video_dst_rect.y0;
		scn_win.width = os->video_dst_rect.x1 - os->video_dst_rect.x0;
		scn_win.height = os->video_dst_rect.y1 - os->video_dst_rect.y0;

		// Get src window dimension and position
		src_win.x = os->video_src_rect.x0;
		src_win.y = os->video_src_rect.y0;
		src_win.width = os->video_src_rect.x1 - os->video_src_rect.x0;
		src_win.height = os->video_src_rect.y1 - os->video_src_rect.y0;

		// Do the y cutoff (due to a bug in sunxi disp driver)
		if (scn_win.y < 0)
		{
			int cutoff = -scn_win.y;
			src_win.y += cutoff;
			src_win.height -= cutoff;
			scn_win.y = 0;
			scn_win.height -= cutoff;
		}

		// Reset window dimension and position
		args[2] = (unsigned long)(&scn_win);
		ioctl(q->target->fd, DISP_CMD_LAYER_SET_SCN_WINDOW, args);
		args[2] = (unsigned long)(&src_win);
		ioctl(q->target->fd, DISP_CMD_LAYER_SET_SRC_WINDOW, args);
/*
		VDPAU_DBG("Offset x: %d, y: %d", q->target->x, q->target->y);
		VDPAU_DBG("LayerSCN - x0: %d, width: %d, y0: %d, height: %d",
			    scn_win.x, scn_win.width, scn_win.y, scn_win.height);
		VDPAU_DBG("OutputDST - x0: %d, width: %d, y0: %d, height: %d",
			    os->video_dst_rect.x0, (os->video_dst_rect.x1 - os->video_dst_rect.x0),
			    os->video_dst_rect.y0, (os->video_dst_rect.y1 - os->video_dst_rect.y0));

		VDPAU_DBG("LayerSRC - x0: %d, width: %d, y0: %d, height: %d",
			    src_win.x, src_win.width, src_win.y, src_win.height);
		VDPAU_DBG("VideoSRC - x0: %d, width: %d, y0: %d, height: %d",
			    os->video_src_rect.x0, (os->video_src_rect.x1 - os->video_src_rect.x0),
			    os->video_src_rect.y0, (os->video_src_rect.y1 - os->video_src_rect.y0));
*/
	}

	if (os->vs)
	{
		static int last_id;
		uint32_t args[4] = { 0, q->target->layer, 0, 0 };

		if (os->start_flag == 1 || q->target->start_flag == 1)
		{
			last_id = -1; // reset the video.id

			// VIDEO layer
			__disp_layer_info_t layer_info;
			memset(&layer_info, 0, sizeof(layer_info));

			args[2] = (unsigned long)(&layer_info);
			ioctl(q->target->fd, DISP_CMD_LAYER_GET_PARA, args);

			layer_info.pipe = q->device->osd_enabled ? 0 : 1;
			layer_info.mode = DISP_LAYER_WORK_MODE_SCALER;
			layer_info.fb.format = DISP_FORMAT_YUV420;
			layer_info.fb.seq = DISP_SEQ_UVUV;
			switch (os->vs->source_format) {
			case VDP_YCBCR_FORMAT_YUYV:
				layer_info.fb.mode = DISP_MOD_INTERLEAVED;
				layer_info.fb.format = DISP_FORMAT_YUV422;
				layer_info.fb.seq = DISP_SEQ_YUYV;
				break;
			case VDP_YCBCR_FORMAT_UYVY:
				layer_info.fb.mode = DISP_MOD_INTERLEAVED;
				layer_info.fb.format = DISP_FORMAT_YUV422;
				layer_info.fb.seq = DISP_SEQ_UYVY;
				break;
			case VDP_YCBCR_FORMAT_NV12:
				layer_info.fb.mode = DISP_MOD_NON_MB_UV_COMBINED;
				break;
			case VDP_YCBCR_FORMAT_YV12:
				layer_info.fb.mode = DISP_MOD_NON_MB_PLANAR;
				break;
			default:
			case INTERNAL_YCBCR_FORMAT:
				layer_info.fb.mode = DISP_MOD_MB_UV_COMBINED;
				break;
			}

			layer_info.fb.br_swap = 0;
			if (os->vs->height < 720)
				layer_info.fb.cs_mode = DISP_BT601;
			else
				layer_info.fb.cs_mode = DISP_BT709;
			layer_info.fb.size.width = os->vs->width;
			layer_info.fb.size.height = os->vs->height;
			layer_info.src_win.x = os->video_src_rect.x0;
			layer_info.src_win.y = os->video_src_rect.y0;
			layer_info.src_win.width = os->video_src_rect.x1 - os->video_src_rect.x0;
			layer_info.src_win.height = os->video_src_rect.y1 - os->video_src_rect.y0;
			layer_info.scn_win.x = q->target->x + os->video_dst_rect.x0;
			layer_info.scn_win.y = q->target->y + os->video_dst_rect.y0;
			layer_info.scn_win.width = os->video_dst_rect.x1 - os->video_dst_rect.x0;
			layer_info.scn_win.height = os->video_dst_rect.y1 - os->video_dst_rect.y0;
			layer_info.ck_enable = q->device->osd_enabled ? 0 : 1;

			if (layer_info.scn_win.y < 0)
			{
				int cutoff = -(layer_info.scn_win.y);
				layer_info.src_win.y += cutoff;
				layer_info.src_win.height -= cutoff;
				layer_info.scn_win.y = 0;
				layer_info.scn_win.height -= cutoff;
			}

			layer_info.fb.addr[0] = 0;
			layer_info.fb.addr[1] = 0;
			layer_info.fb.addr[2] = 0;

			args[2] = (unsigned long)(&layer_info);
			ioctl(q->target->fd, DISP_CMD_LAYER_SET_PARA, args);

			layer_info.fb.addr[0] = ve_virt2phys(os->yuv->data) + 0x40000000;
			layer_info.fb.addr[1] = ve_virt2phys(os->yuv->data + os->vs->luma_size) + 0x40000000;
			layer_info.fb.addr[2] = ve_virt2phys(os->yuv->data + os->vs->luma_size + os->vs->luma_size / 4) + 0x40000000;

			args[2] = (unsigned long)(&layer_info);
			ioctl(q->target->fd, DISP_CMD_LAYER_SET_PARA, args);

			args[2] = 0;
			ioctl(q->target->fd, DISP_CMD_LAYER_OPEN, args);
			ioctl(q->target->fd, DISP_CMD_VIDEO_START, args);

			os->start_flag = 0; // initial run is done, only set video.addr[] in the next runs
			q->target->start_flag = 0;
		}
		else
		{
			__disp_video_fb_t video;
			memset(&video, 0, sizeof(__disp_video_fb_t));
			video.id = last_id + 1;
			video.addr[0] = ve_virt2phys(os->yuv->data) + 0x40000000;
			video.addr[1] = ve_virt2phys(os->yuv->data + os->vs->luma_size) + 0x40000000;
			video.addr[2] = ve_virt2phys(os->yuv->data + os->vs->luma_size + os->vs->luma_size / 4) + 0x40000000;

			if (q->device->deint_enabled)
			{
				video.interlace = os->video_deinterlace;
				video.top_field_first = os->video_field ? 0 : 1;
			}

			args[2] = (unsigned long)(&video);
			int tmp, i = 0;
			while ((tmp = ioctl(q->target->fd, DISP_CMD_VIDEO_GET_FRAME_ID, args)) != last_id)
			{
				if (tmp == -1)
					break;
				VDPAU_DBG("Waiting for frame id ... tmp=%d, last_id=%d", tmp, last_id);

				usleep(1000);
				if (i++ > 10)
				{
					VDPAU_DBG("Waiting for frame id failed");
					break;
				}
			}

			ioctl(q->target->fd, DISP_CMD_VIDEO_SET_FB, args);
			last_id++;
		}

		// Note: might be more reliable (but slower and problematic when there
		// are driver issues and the GET functions return wrong values) to query the
		// old values instead of relying on our internal csc_change.
		// Since the driver calculates a matrix out of these values after each
		// set doing this unconditionally is costly.
		if (os->csc_change) {
			ioctl(q->target->fd, DISP_CMD_LAYER_ENHANCE_OFF, args);
			args[2] = 0xff * os->brightness + 0x20;
			ioctl(q->target->fd, DISP_CMD_LAYER_SET_BRIGHT, args);
			args[2] = 0x20 * os->contrast;
			ioctl(q->target->fd, DISP_CMD_LAYER_SET_CONTRAST, args);
			args[2] = 0x20 * os->saturation;
			ioctl(q->target->fd, DISP_CMD_LAYER_SET_SATURATION, args);
			// hue scale is randomly chosen, no idea how it maps exactly
			args[2] = (32 / 3.14) * os->hue + 0x20;
			ioctl(q->target->fd, DISP_CMD_LAYER_SET_HUE, args);
			ioctl(q->target->fd, DISP_CMD_LAYER_ENHANCE_ON, args);
			os->csc_change = 0;
		}
	}
	else
	{
		uint32_t args[4] = { 0, q->target->layer, 0, 0 };
		ioctl(q->target->fd, DISP_CMD_LAYER_CLOSE, args);
	}

	if (!q->device->osd_enabled)
		return VDP_STATUS_OK;

	if (os->rgba.flags & RGBA_FLAG_NEEDS_CLEAR)
		rgba_clear(&os->rgba);

	if (os->rgba.flags & RGBA_FLAG_DIRTY)
	{
		// TOP layer
		rgba_flush(&os->rgba);

		__disp_layer_info_t layer_info;
		memset(&layer_info, 0, sizeof(layer_info));
		layer_info.pipe = 1;
		layer_info.mode = DISP_LAYER_WORK_MODE_NORMAL;
		layer_info.fb.mode = DISP_MOD_INTERLEAVED;
		layer_info.fb.format = DISP_FORMAT_ARGB8888;
		layer_info.fb.seq = DISP_SEQ_ARGB;
		switch (os->rgba.format)
		{
		case VDP_RGBA_FORMAT_R8G8B8A8:
			layer_info.fb.br_swap = 1;
			break;
		case VDP_RGBA_FORMAT_B8G8R8A8:
		default:
			layer_info.fb.br_swap = 0;
			break;
		}
		layer_info.fb.addr[0] = ve_virt2phys(os->rgba.data) + 0x40000000;
		layer_info.fb.cs_mode = DISP_BT601;
		layer_info.fb.size.width = os->rgba.width;
		layer_info.fb.size.height = os->rgba.height;
		layer_info.src_win.x = os->rgba.dirty.x0;
		layer_info.src_win.y = os->rgba.dirty.y0;
		layer_info.src_win.width = os->rgba.dirty.x1 - os->rgba.dirty.x0;
		layer_info.src_win.height = os->rgba.dirty.y1 - os->rgba.dirty.y0;
		layer_info.scn_win.x = q->target->x + os->rgba.dirty.x0;
		layer_info.scn_win.y = q->target->y + os->rgba.dirty.y0;
		layer_info.scn_win.width = min_nz(clip_width, os->rgba.dirty.x1) - os->rgba.dirty.x0;
		layer_info.scn_win.height = min_nz(clip_height, os->rgba.dirty.y1) - os->rgba.dirty.y0;

		uint32_t args[4] = { 0, q->target->layer_top, (unsigned long)(&layer_info), 0 };
		ioctl(q->target->fd, DISP_CMD_LAYER_SET_PARA, args);

		ioctl(q->target->fd, DISP_CMD_LAYER_OPEN, args);
	}
	else
	{
		uint32_t args[4] = { 0, q->target->layer_top, 0, 0 };
		ioctl(q->target->fd, DISP_CMD_LAYER_CLOSE, args);
	}

	return VDP_STATUS_OK;
}

static void *presentation_thread(void *param)
{
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	output_surface_ctx_t *os_prev = NULL;
	output_surface_ctx_t *os_pprev = NULL;

	int fd_fb = 0;

	fd_fb = open("/dev/fb0", O_RDWR);
	if (!fd_fb)
		VDPAU_DBG("Error opening framebuffer device /dev/fb0");

	while (!end_presentation) {
		// do the VSync
		if ((!fd_fb) || (ioctl(fd_fb, FBIO_WAITFORVSYNC, 0)))
			VDPAU_DBG("VSync failed");
		frame_time = get_time();

		if(!q_isEmpty(queue))
		{
			// remove it from queue
			task_t *task;
			if (!q_pop_head(queue, (void *)&task))
			{
				output_surface_ctx_t *os_cur = handle_get(task->surface);

				if (os_prev) /* This is the actually displayed surface */
				{
					os_prev->first_presentation_time = frame_time;
					os_prev->status = VDP_PRESENTATION_QUEUE_STATUS_VISIBLE;
				}
				if (os_pprev) /* This is the previously displayed surface */
					os_pprev->status = VDP_PRESENTATION_QUEUE_STATUS_IDLE;

				os_pprev = os_prev;
				os_prev = os_cur;

				// run the task
				do_presentation_queue_display(task);
				free(task);
			}
			else
				VDPAU_DBG("Error getting task");
		}
	}

	close(fd_fb);
	return 0;
}

VdpStatus vdp_presentation_queue_target_create_x11(VdpDevice device,
                                                   Drawable drawable,
                                                   VdpPresentationQueueTarget *target)
{
	if (!target || !drawable)
		return VDP_STATUS_INVALID_POINTER;

	device_ctx_t *dev = handle_get(device);
	if (!dev)
		return VDP_STATUS_INVALID_HANDLE;

	queue_target_ctx_t *qt = handle_create(sizeof(*qt), target);
	if (!qt)
		return VDP_STATUS_RESOURCES;

	qt->fd = open("/dev/disp", O_RDWR);
	if (qt->fd == -1)
	{
		handle_destroy(*target);
		return VDP_STATUS_ERROR;
	}

	int tmp = SUNXI_DISP_VERSION;
	if (ioctl(qt->fd, DISP_CMD_VERSION, &tmp) < 0)
	{
		close(qt->fd);
		handle_destroy(*target);
		return VDP_STATUS_ERROR;
	}

	uint32_t args[4] = { 0, DISP_LAYER_WORK_MODE_SCALER, 0, 0 };
	qt->layer = ioctl(qt->fd, DISP_CMD_LAYER_REQUEST, args);
	if (qt->layer == 0)
		goto out_layer;

	args[1] = qt->layer;
	ioctl(qt->fd, dev->osd_enabled ? DISP_CMD_LAYER_TOP : DISP_CMD_LAYER_BOTTOM, args);

	if (dev->osd_enabled)
	{
		args[1] = DISP_LAYER_WORK_MODE_NORMAL;
		qt->layer_top = ioctl(qt->fd, DISP_CMD_LAYER_REQUEST, args);
		if (qt->layer_top == 0)
			goto out_layer_top;

		args[1] = qt->layer_top;
		ioctl(qt->fd, DISP_CMD_LAYER_TOP, args);
	}
	else
	{
		__disp_colorkey_t ck;
		ck.ck_max.red = ck.ck_min.red = 0;
		ck.ck_max.green = ck.ck_min.green = 1;
		ck.ck_max.blue = ck.ck_min.blue = 2;
		ck.red_match_rule = 2;
		ck.green_match_rule = 2;
		ck.blue_match_rule = 2;

		args[1] = (unsigned long)(&ck);
		ioctl(qt->fd, DISP_CMD_SET_COLORKEY, args);
	}
	qt->start_flag = 1;

	qt->drawable_changed = 0;
	qt->drawable_x = 0;
	qt->drawable_y = 0;
	//qt->drawable_width = 0;
	//qt->drawable_height = 0;

	qt->drawable = drawable;
	XSelectInput(dev->display, drawable, StructureNotifyMask);

	// get current window position
	Window dummy;
	XTranslateCoordinates(dev->display, qt->drawable, RootWindow(dev->display, dev->screen), 0, 0, &qt->x, &qt->y, &dummy);

	XSetWindowBackground(dev->display, drawable, 0x000102);
	XClearWindow(dev->display, drawable);

	return VDP_STATUS_OK;

out_layer_top:
	args[1] = qt->layer;
	ioctl(qt->fd, DISP_CMD_LAYER_RELEASE, args);
out_layer:
	close(qt->fd);
	handle_destroy(*target);
	return VDP_STATUS_RESOURCES;
}

VdpStatus vdp_presentation_queue_target_destroy(VdpPresentationQueueTarget presentation_queue_target)
{
	queue_target_ctx_t *qt = handle_get(presentation_queue_target);
	if (!qt)
		return VDP_STATUS_INVALID_HANDLE;

	uint32_t args[4] = { 0, qt->layer, 0, 0 };

	ioctl(qt->fd, DISP_CMD_VIDEO_STOP, args);
	ioctl(qt->fd, DISP_CMD_LAYER_CLOSE, args);
	ioctl(qt->fd, DISP_CMD_LAYER_RELEASE, args);

	if (qt->layer_top)
	{
		args[1] = qt->layer_top;
		ioctl(qt->fd, DISP_CMD_LAYER_CLOSE, args);
		ioctl(qt->fd, DISP_CMD_LAYER_RELEASE, args);
	}

	close(qt->fd);

	handle_destroy(presentation_queue_target);

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_create(VdpDevice device,
                                        VdpPresentationQueueTarget presentation_queue_target,
                                        VdpPresentationQueue *presentation_queue)
{
	if (!presentation_queue)
		return VDP_STATUS_INVALID_POINTER;

	device_ctx_t *dev = handle_get(device);
	if (!dev)
		return VDP_STATUS_INVALID_HANDLE;

	queue_target_ctx_t *qt = handle_get(presentation_queue_target);
	if (!qt)
		return VDP_STATUS_INVALID_HANDLE;

	queue_ctx_t *q = handle_create(sizeof(*q), presentation_queue);
	if (!q)
		return VDP_STATUS_RESOURCES;

	q->target = qt;
	q->device = dev;

	// initialize queue and launch worker thread
	if (!queue) {
		end_presentation = 0;
		queue = q_queue_init();
		pthread_create(&presentation_thread_id, NULL, presentation_thread, q);
	}

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_destroy(VdpPresentationQueue presentation_queue)
{
	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	end_presentation = 1;
	pthread_join(presentation_thread_id, NULL);

	q_queue_free(queue);
	queue = NULL;

	handle_destroy(presentation_queue);

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_set_background_color(VdpPresentationQueue presentation_queue,
                                                      VdpColor *const background_color)
{
	if (!background_color)
		return VDP_STATUS_INVALID_POINTER;

	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	q->background.red = background_color->red;
	q->background.green = background_color->green;
	q->background.blue = background_color->blue;
	q->background.alpha = background_color->alpha;

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_get_background_color(VdpPresentationQueue presentation_queue,
                                                      VdpColor *const background_color)
{
	if (!background_color)
		return VDP_STATUS_INVALID_POINTER;

	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	background_color->red = q->background.red;
	background_color->green = q->background.green;
	background_color->blue = q->background.blue;
	background_color->alpha = q->background.alpha;

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_get_time(VdpPresentationQueue presentation_queue,
                                          VdpTime *current_time)
{
	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	*current_time = get_time();
	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_block_until_surface_idle(VdpPresentationQueue presentation_queue,
                                                          VdpOutputSurface surface,
                                                          VdpTime *first_presentation_time)
{
	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	output_surface_ctx_t *os = handle_get(surface);
	if (!os)
		return VDP_STATUS_INVALID_HANDLE;

	while (os->status != VDP_PRESENTATION_QUEUE_STATUS_IDLE) {
		usleep(1000);
		output_surface_ctx_t *os = handle_get(surface);
		if (!os)
			return VDP_STATUS_INVALID_HANDLE;
	}

	*first_presentation_time = os->first_presentation_time;

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_query_surface_status(VdpPresentationQueue presentation_queue,
                                                      VdpOutputSurface surface,
                                                      VdpPresentationQueueStatus *status,
                                                      VdpTime *first_presentation_time)
{
	if (!status || !first_presentation_time)
		return VDP_STATUS_INVALID_POINTER;

	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	output_surface_ctx_t *os = handle_get(surface);
	if (!os)
		return VDP_STATUS_INVALID_HANDLE;

	*status = os->status;
	*first_presentation_time = os->first_presentation_time;

	return VDP_STATUS_OK;
}
