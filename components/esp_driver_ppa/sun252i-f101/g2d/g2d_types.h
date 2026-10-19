/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * The operations of the 2D accelerator: surfaces, rectangles and the three operation types.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "g2d_compat.h"

enum g2d_format {
	G2D_PIXFMT_ARGB8888,
	G2D_PIXFMT_ABGR8888,
	G2D_PIXFMT_RGBA8888,
	G2D_PIXFMT_BGRA8888,
	G2D_PIXFMT_XRGB8888,
	G2D_PIXFMT_XBGR8888,
	G2D_PIXFMT_RGB888,
	G2D_PIXFMT_BGR888,
	G2D_PIXFMT_RGB565,
	G2D_PIXFMT_BGR565,
	G2D_PIXFMT_ARGB4444,
	G2D_PIXFMT_ARGB1555,
	/** YUV 4:2:0, plane 0 Y, plane 1 interleaved UV. Source only. */
	G2D_PIXFMT_NV12,
	/** YUV 4:2:0, plane 0 Y, plane 1 interleaved VU. Source only. */
	G2D_PIXFMT_NV21,
	/** YUV 4:2:0, three planes Y, U, V. Source only. */
	G2D_PIXFMT_I420,
	/** YUV 4:2:2 packed Y0 U Y1 V. Source only. */
	G2D_PIXFMT_YUYV,
	G2D_PIXFMT_MAX,
};

/** Rotation, applied before the flips of @ref G2D_FLIP_H / @ref G2D_FLIP_V. */
enum g2d_rotation {
	G2D_ROTATE_0,
	G2D_ROTATE_90,
	G2D_ROTATE_180,
	G2D_ROTATE_270,
};

/** Porter-Duff composition. "Source" is the foreground, "destination" the background. */
enum g2d_blend_mode {
	G2D_BLEND_CLEAR,
	G2D_BLEND_SRC,
	G2D_BLEND_DST,
	G2D_BLEND_SRC_OVER,
	G2D_BLEND_DST_OVER,
	G2D_BLEND_SRC_IN,
	G2D_BLEND_DST_IN,
	G2D_BLEND_SRC_OUT,
	G2D_BLEND_DST_OUT,
	G2D_BLEND_SRC_ATOP,
	G2D_BLEND_DST_ATOP,
	G2D_BLEND_XOR,
};

/** How the alpha of a layer is taken. */
enum g2d_alpha_mode {
	/** use the alpha channel of the pixels */
	G2D_ALPHA_PIXEL,
	/** use the layer alpha only */
	G2D_ALPHA_GLOBAL,
	/** pixel alpha multiplied by the layer alpha */
	G2D_ALPHA_MIXED,
};

/** @name Operation flags
 * @{
 */
/** Flip horizontally (after the rotation) */
#define G2D_FLIP_H		BIT(0)
/** Flip vertically (after the rotation) */
#define G2D_FLIP_V		BIT(1)
/** The pixels of the source are premultiplied by their alpha */
#define G2D_FLAG_SRC_PREMULTIPLIED	BIT(2)
/** The pixels of the destination are premultiplied by their alpha */
#define G2D_FLAG_DST_PREMULTIPLIED	BIT(3)
/** Do not clean/invalidate the data cache around the operation */
#define G2D_FLAG_NO_CACHE_OPS	BIT(4)
/** YCbCr sources use the full 0..255 range (default: limited, 16..235) */
#define G2D_FLAG_YUV_FULL_RANGE	BIT(5)
/** YCbCr sources use the BT.709 matrix (default: BT.601) */
#define G2D_FLAG_YUV_BT709	BIT(6)
/** @} */

/** Rectangle in pixels */
struct g2d_rect {
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
};

/** A memory buffer holding pixels */
struct g2d_surface {
	enum g2d_format format;
	/** size of the buffer in pixels */
	uint16_t width;
	uint16_t height;
	/** plane base addresses; planar and semi-planar YUV use more than one */
	void *plane[3];
	/** line pitch of each plane in bytes (0: tightly packed) */
	uint32_t pitch[3];
};

struct g2d_op;

enum g2d_op_type {
	/** fill @ref g2d_op::dst_rect with @ref g2d_op::color */
	G2D_OP_FILL,
	/**
	 * copy @ref g2d_op::src_rect of src to dst_rect of dst; converts the pixel format,
	 * scales when the rectangle sizes differ, and rotates/flips
	 */
	G2D_OP_BLIT,
	/**
	 * compose src_rect of src (foreground, scaled to the size of dst_rect when
	 * they differ) with bg_rect of bg (background, same size as dst_rect) and
	 * write the result to dst_rect of dst; dst may be bg
	 */
	G2D_OP_BLEND,
};

/** Blend parameters of @ref G2D_OP_BLEND */
struct g2d_blend {
	enum g2d_blend_mode mode;
	enum g2d_alpha_mode fg_alpha_mode;
	/** foreground layer alpha (0..255), used by the global and mixed modes */
	uint8_t fg_alpha;
	enum g2d_alpha_mode bg_alpha_mode;
	uint8_t bg_alpha;
	/** compare the destination (bg) pixels with the colour key range, in ARGB8888 */
	bool color_key;
	uint32_t color_key_min;
	uint32_t color_key_max;
};

/** One queued operation */
struct g2d_op {
	enum g2d_op_type type;
	uint32_t flags;
	/** @ref G2D_OP_FILL colour in ARGB8888 */
	uint32_t color;
	struct g2d_surface dst;
	struct g2d_rect dst_rect;
	/** @ref G2D_OP_BLIT and @ref G2D_OP_BLEND source (foreground) */
	struct g2d_surface src;
	struct g2d_rect src_rect;
	/** @ref G2D_OP_BLIT: rotation and flips */
	enum g2d_rotation rotation;
	/** @ref G2D_OP_BLEND background, same size as dst_rect */
	struct g2d_surface bg;
	struct g2d_rect bg_rect;
	struct g2d_blend blend;
};

/** Capabilities of a G2D device */
static inline unsigned int g2d_format_bytes_per_pixel(enum g2d_format format)
{
	switch (format) {
	case G2D_PIXFMT_ARGB8888:
	case G2D_PIXFMT_ABGR8888:
	case G2D_PIXFMT_RGBA8888:
	case G2D_PIXFMT_BGRA8888:
	case G2D_PIXFMT_XRGB8888:
	case G2D_PIXFMT_XBGR8888:
		return 4;
	case G2D_PIXFMT_RGB888:
	case G2D_PIXFMT_BGR888:
		return 3;
	case G2D_PIXFMT_RGB565:
	case G2D_PIXFMT_BGR565:
	case G2D_PIXFMT_ARGB4444:
	case G2D_PIXFMT_ARGB1555:
	case G2D_PIXFMT_YUYV:
		return 2;
	default:
		return 0;
	}
}

/** @} */

