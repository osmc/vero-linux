/*
 * Copyright 2009-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

/*!
 * @defgroup Framebuffer Framebuffer Driver for SDC and ADC.
 */

/*!
 * @file mxc_edid.h
 *
 * @brief MXC EDID tools
 *
 * @ingroup Framebuffer
 */

#ifndef MXC_EDID_H
#define MXC_EDID_H

#include <linux/fb.h>
#include <drm/drm_crtc.h>

enum cea_audio_coding_types {
	AUDIO_CODING_TYPE_REF_STREAM_HEADER	=  0,
	AUDIO_CODING_TYPE_LPCM			=  1,
	AUDIO_CODING_TYPE_AC3			=  2,
	AUDIO_CODING_TYPE_MPEG1			=  3,
	AUDIO_CODING_TYPE_MP3			=  4,
	AUDIO_CODING_TYPE_MPEG2			=  5,
	AUDIO_CODING_TYPE_AACLC			=  6,
	AUDIO_CODING_TYPE_DTS			=  7,
	AUDIO_CODING_TYPE_ATRAC			=  8,
	AUDIO_CODING_TYPE_SACD			=  9,
	AUDIO_CODING_TYPE_EAC3			= 10,
	AUDIO_CODING_TYPE_DTS_HD		= 11,
	AUDIO_CODING_TYPE_MLP			= 12,
	AUDIO_CODING_TYPE_DST			= 13,
	AUDIO_CODING_TYPE_WMAPRO		= 14,
	AUDIO_CODING_TYPE_RESERVED		= 15,
};

struct mxc_hdmi_3d_format {
	unsigned char vic_order_2d;
	unsigned char struct_3d;
	unsigned char detail_3d;
	unsigned char reserved;
};

struct mxc_edid_cfg {
	u8 vsd_video_latency;
	u8 vsd_audio_latency;
	u8 vsd_I_video_latency;
	u8 vsd_I_audio_latency;

	u8 sample_sizes[4];
	u8 sample_rates[4];
	u8 speaker_alloc;
	u8 physical_address[4];

	bool cea_basicaudio;

	bool cea_underscan;
	bool cea_ycbcr444;
	bool cea_ycbcr422;
	bool hdmi_cap;
	bool cea_rgb_range_selectable;
	u8 cea_scan_mode_ce;
	u8 cea_scan_mode_it;
	u8 cea_scan_mode_pt;

	/*VSD*/
	bool vsd_support_ai;
	bool vsd_dc_48bit;
	bool vsd_dc_36bit;
	bool vsd_dc_30bit;
	bool vsd_dc_y444;
	bool vsd_dvi_dual;

	u8 CONFIGKEEP;

	bool vsd_cnc0;
	bool vsd_cnc1;
	bool vsd_cnc2;
	bool vsd_cnc3;

	u8 hdmi_vic[64];
	struct mxc_hdmi_3d_format hdmi_3d_format[64];
	u16 hdmi_3d_mask_all;
	u16 hdmi_3d_struct_all;
	unsigned char hdmi_3d_present;
	unsigned char hdmi_3d_len;
	unsigned char hdmi_3d_multi_present;
	u32 vsd_max_tmdsclk_rate;

	u8 hdmi_eld[MAX_ELD_BYTES];
};

static inline bool try_ntsc(const struct fb_videomode *mode)
{
	if (!mode)
		return false;

	switch (mode->refresh) {
	case 24:
	case 30:
	case 60:
		return true;
	default:
		return false;
	}
}

static inline unsigned long mxcPICOS2KHZ(u32 pixclock, struct fb_info *fbi) {
	bool ntsc = fbi && (fbi->flags & FBINFO_TIMING_NTSC) && try_ntsc(fbi->mode);
	return (((ntsc ? (999000999UL*4) : (1000000000UL*4)) / pixclock) + 1) >> 2;
}

int mxc_edid_var_to_vic(struct fb_var_screeninfo *var);
int mxc_edid_mode_to_vic(const struct fb_videomode *mode, u32 mode_mask);
int mxc_edid_parse_raw(unsigned char *edid, struct mxc_edid_cfg *cfg, struct fb_info *fbi);
int mxc_edid_read(struct i2c_adapter *adp, unsigned short addr,
	unsigned char *edid, struct mxc_edid_cfg *cfg, struct fb_info *fbi);
int mxc_edid_parse_ext_blk(unsigned char *edid, struct mxc_edid_cfg *cfg,
	struct fb_monspecs *specs);
const struct fb_videomode *mxc_fb_find_nearest_mode(const struct fb_videomode *mode,
	struct list_head *head, bool relax);
int mxc_edid_fb_mode_is_equal(bool use_aspect, const struct fb_videomode *mode1,
	const struct fb_videomode *mode2, u32 mode_mask);
int mxc_fb_mode_is_equal_res(const struct fb_videomode *mode1,
	 const struct fb_videomode *mode2);
#endif
