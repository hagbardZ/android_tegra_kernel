/*
 *  hdac_hdmi.c - ASoc HDA-HDMI codec driver for Intel platforms
 *
 *  Copyright (C) 2014-2015 Intel Corp
 *  Author: Samreen Nilofer <samreen.nilofer@intel.com>
 *	    Subhransu S. Prusty <subhransu.s.prusty@intel.com>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/hdmi.h>
#include <drm/drm_edid.h>
#include <sound/pcm_params.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/hdaudio_ext.h>
#include <sound/hda_i915.h>
#include <sound/pcm_drm_eld.h>
#include "../../hda/local.h"

#define NAME_SIZE	32

#define AMP_OUT_MUTE		0xb080
#define AMP_OUT_UNMUTE		0xb000
#define PIN_OUT			(AC_PINCTL_OUT_EN)

#define HDA_MAX_CONNECTIONS     32

#define HDA_MAX_CVTS		3

#define ELD_MAX_SIZE    256
#define ELD_FIXED_BYTES	20

struct hdac_hdmi_cvt_params {
	unsigned int channels_min;
	unsigned int channels_max;
	u32 rates;
	u64 formats;
	unsigned int maxbps;
};

struct hdac_hdmi_cvt {
	struct list_head head;
	hda_nid_t nid;
	const char *name;
	struct hdac_hdmi_cvt_params params;
};

struct hdac_hdmi_eld {
	bool	monitor_present;
	bool	eld_valid;
	int	eld_size;
	char    eld_buffer[ELD_MAX_SIZE];
};

struct hdac_hdmi_pin {
	struct list_head head;
	hda_nid_t nid;
	int num_mux_nids;
	hda_nid_t mux_nids[HDA_MAX_CONNECTIONS];
	struct hdac_hdmi_eld eld;
	struct hdac_ext_device *edev;
	int repoll_count;
	struct delayed_work work;
};

struct hdac_hdmi_pcm {
	struct list_head head;
	int pcm_id;
	struct hdac_hdmi_pin *pin;
	struct hdac_hdmi_cvt *cvt;
};

struct hdac_hdmi_dai_pin_map {
	int dai_id;
	struct hdac_hdmi_pin *pin;
	struct hdac_hdmi_cvt *cvt;
};

struct hdac_hdmi_jack {
	struct list_head head;
	int pcm;
	struct snd_jack *jack;
};

struct hdac_hdmi_priv {
	struct hdac_hdmi_dai_pin_map dai_map[HDA_MAX_CVTS];
	struct list_head pin_list;
	struct list_head cvt_list;
	struct list_head jack_list;
	struct list_head pcm_list;
	int num_pin;
	int num_cvt;
	struct mutex pin_mutex;
};

static inline struct hdac_ext_device *to_hda_ext_device(struct device *dev)
{
	struct hdac_device *hdac = container_of(dev, struct hdac_device, dev);

	return container_of(hdac, struct hdac_ext_device, hdac);
}

static unsigned int sad_format(const u8 *sad)
{
	return ((sad[0] >> 0x3) & 0x1f);
}

static unsigned int sad_sample_bits_lpcm(const u8 *sad)
{
	return (sad[2] & 7);
}

static int hdac_hdmi_eld_limit_formats(struct snd_pcm_runtime *runtime,
						void *eld)
{
	u64 formats = SNDRV_PCM_FMTBIT_S16;
	int i;
	const u8 *sad, *eld_buf = eld;

	sad = drm_eld_sad(eld_buf);
	if (!sad)
		goto format_constraint;

	for (i = drm_eld_sad_count(eld_buf); i > 0; i--, sad += 3) {
		if (sad_format(sad) == 1) { /* AUDIO_CODING_TYPE_LPCM */

			/* 20 bit and 24 bit */
			if (sad_sample_bits_lpcm(sad) & 0x6)
				formats |= SNDRV_PCM_FMTBIT_S32;
		}
	}

format_constraint:
	return snd_pcm_hw_constraint_mask64(runtime, SNDRV_PCM_HW_PARAM_FORMAT,
				formats);

}

 /* HDMI Eld routines */
static unsigned int hdac_hdmi_get_eld_data(struct hdac_device *codec,
				hda_nid_t nid, int byte_index)
{
	unsigned int val;

	val = snd_hdac_codec_read(codec, nid, 0, AC_VERB_GET_HDMI_ELDD,
							byte_index);

	dev_dbg(&codec->dev, "HDMI: ELD data byte %d: 0x%x\n",
			byte_index, val);

	return val;
}

static int hdac_hdmi_get_eld_size(struct hdac_device *codec, hda_nid_t nid)
{
	return snd_hdac_codec_read(codec, nid, 0, AC_VERB_GET_HDMI_DIP_SIZE,
						 AC_DIPSIZE_ELD_BUF);
}

/*
 * This function queries the eld size and eld data and fills in the buffer
 * passed by user
 */
static int hdac_hdmi_get_eld(struct hdac_device *codec, hda_nid_t nid,
		     unsigned char *buf, int *eld_size)
{
	int i;
	int ret = 0;
	int size;

	/*
	 * ELD size is initialized to zero in caller function. If no errors and
	 * ELD is valid, actual eld_size is assigned.
	 */

	size = hdac_hdmi_get_eld_size(codec, nid);
	if (size < ELD_FIXED_BYTES || size > ELD_MAX_SIZE) {
		dev_info(&codec->dev, "HDMI: invalid ELD buf size %d\n", size);
		return -ERANGE;
	}

	/* set ELD buffer */
	for (i = 0; i < size; i++) {
		unsigned int val = hdac_hdmi_get_eld_data(codec, nid, i);
		/*
		 * Graphics driver might be writing to ELD buffer right now.
		 * Just abort. The caller will repoll after a while.
		 */
		if (!(val & AC_ELDD_ELD_VALID)) {
			dev_info(&codec->dev, "HDMI: invalid ELD data byte %d\n", i);
			ret = -EINVAL;
			goto error;
		}
		val &= AC_ELDD_ELD_DATA;
		/*
		 * The first byte cannot be zero. This can happen on some DVI
		 * connections. Some Intel chips may also need some 250ms delay
		 * to return non-zero ELD data, even when the graphics driver
		 * correctly writes ELD content before setting ELD_valid bit.
		 */
		if (!val && !i) {
			dev_dbg(&codec->dev, "HDMI: 0 ELD data\n");
			ret = -EINVAL;
			goto error;
		}
		buf[i] = val;
	}

	*eld_size = size;
error:
	return ret;
}

static int hdac_hdmi_setup_stream(struct hdac_ext_device *hdac,
				hda_nid_t cvt_nid, hda_nid_t pin_nid,
				u32 stream_tag, int format)
{
	unsigned int val;

	dev_dbg(&hdac->hdac.dev, "cvt nid %d pnid %d stream %d format 0x%x\n",
			cvt_nid, pin_nid, stream_tag, format);

	val = (stream_tag << 4);

	snd_hdac_codec_write(&hdac->hdac, cvt_nid, 0,
				AC_VERB_SET_CHANNEL_STREAMID, val);
	snd_hdac_codec_write(&hdac->hdac, cvt_nid, 0,
				AC_VERB_SET_STREAM_FORMAT, format);

	return 0;
}

static void
hdac_hdmi_set_dip_index(struct hdac_ext_device *hdac, hda_nid_t pin_nid,
				int packet_index, int byte_index)
{
	int val;

	val = (packet_index << 5) | (byte_index & 0x1f);

	snd_hdac_codec_write(&hdac->hdac, pin_nid, 0,
				AC_VERB_SET_HDMI_DIP_INDEX, val);
}

struct dp_audio_infoframe {
	u8 type; /* 0x84 */
	u8 len;  /* 0x1b */
	u8 ver;  /* 0x11 << 2 */

	u8 CC02_CT47;	/* match with HDMI infoframe from this on */
	u8 SS01_SF24;
	u8 CXT04;
	u8 CA;
	u8 LFEPBL01_LSV36_DM_INH7;
};

static int hdac_hdmi_setup_audio_infoframe(struct hdac_ext_device *hdac,
				hda_nid_t cvt_nid, hda_nid_t pin_nid)
{
	uint8_t buffer[HDMI_INFOFRAME_HEADER_SIZE + HDMI_AUDIO_INFOFRAME_SIZE];
	struct hdmi_audio_infoframe frame;
	struct dp_audio_infoframe dp_ai;
	struct hdac_hdmi_priv *hdmi = hdac->private_data;
	struct hdac_hdmi_pin *pin;
	u8 *dip;
	int ret;
	int i;
	const u8 *eld_buf;
	u8 conn_type;
	int channels = 2;

	list_for_each_entry(pin, &hdmi->pin_list, head) {
		if (pin->nid == pin_nid)
			break;
	}

	eld_buf = pin->eld.eld_buffer;
	conn_type = drm_eld_get_conn_type(eld_buf);

	/* setup channel count */
	snd_hdac_codec_write(&hdac->hdac, cvt_nid, 0,
			    AC_VERB_SET_CVT_CHAN_COUNT, channels - 1);

	if (conn_type == DRM_ELD_CONN_TYPE_HDMI) {
		hdmi_audio_infoframe_init(&frame);

		/* Default stereo for now */
		frame.channels = channels;

		ret = hdmi_audio_infoframe_pack(&frame, buffer, sizeof(buffer));
		if (ret < 0)
			return ret;

		dip = (u8 *)&frame;

	} else if (conn_type == DRM_ELD_CONN_TYPE_DP) {
		memset(&dp_ai, 0, sizeof(dp_ai));
		dp_ai.type	= 0x84;
		dp_ai.len	= 0x1b;
		dp_ai.ver	= 0x11 << 2;
		dp_ai.CC02_CT47	= channels - 1;
		dp_ai.CA	= 0;

		dip = (u8 *)&dp_ai;
	} else {
		dev_err(&hdac->hdac.dev, "Invalid connection type: %d\n",
						conn_type);
		return -EIO;
	}

	/* stop infoframe transmission */
	hdac_hdmi_set_dip_index(hdac, pin_nid, 0x0, 0x0);
	snd_hdac_codec_write(&hdac->hdac, pin_nid, 0,
			AC_VERB_SET_HDMI_DIP_XMIT, AC_DIPXMIT_DISABLE);


	/*  Fill infoframe. Index auto-incremented */
	hdac_hdmi_set_dip_index(hdac, pin_nid, 0x0, 0x0);
	if (conn_type == DRM_ELD_CONN_TYPE_HDMI) {
		for (i = 0; i < sizeof(frame); i++)
			snd_hdac_codec_write(&hdac->hdac, pin_nid, 0,
				AC_VERB_SET_HDMI_DIP_DATA, dip[i]);
	} else {
		for (i = 0; i < sizeof(dp_ai); i++)
			snd_hdac_codec_write(&hdac->hdac, pin_nid, 0,
				AC_VERB_SET_HDMI_DIP_DATA, dip[i]);
	}

	/* Start infoframe */
	hdac_hdmi_set_dip_index(hdac, pin_nid, 0x0, 0x0);
	snd_hdac_codec_write(&hdac->hdac, pin_nid, 0,
			AC_VERB_SET_HDMI_DIP_XMIT, AC_DIPXMIT_BEST);

	return 0;
}

static void hdac_hdmi_set_power_state(struct hdac_ext_device *edev,
		struct hdac_hdmi_dai_pin_map *dai_map, unsigned int pwr_state)
{
	/* Power up pin widget */
	if (!snd_hdac_check_power_state(&edev->hdac, dai_map->pin->nid,
						pwr_state))
		snd_hdac_codec_write(&edev->hdac, dai_map->pin->nid, 0,
			AC_VERB_SET_POWER_STATE, pwr_state);

	/* Power up converter */
	if (!snd_hdac_check_power_state(&edev->hdac, dai_map->cvt->nid,
						pwr_state))
		snd_hdac_codec_write(&edev->hdac, dai_map->cvt->nid, 0,
			AC_VERB_SET_POWER_STATE, pwr_state);
}

static void hdac_hdmi_enable_cvt(struct hdac_ext_device *edev,
		struct hdac_hdmi_dai_pin_map *dai_map)
{
	/* Enable transmission */
	snd_hdac_codec_write(&edev->hdac, dai_map->cvt->nid, 0,
			AC_VERB_SET_DIGI_CONVERT_1, 1);

	/* Category Code (CC) to zero */
	snd_hdac_codec_write(&edev->hdac, dai_map->cvt->nid, 0,
			AC_VERB_SET_DIGI_CONVERT_2, 0);
}

static int hdac_hdmi_enable_pin(struct hdac_ext_device *hdac,
		struct hdac_hdmi_dai_pin_map *dai_map)
{
	int mux_idx;
	struct hdac_hdmi_pin *pin = dai_map->pin;

	for (mux_idx = 0; mux_idx < pin->num_mux_nids; mux_idx++) {
		if (pin->mux_nids[mux_idx] == dai_map->cvt->nid) {
			snd_hdac_codec_write(&hdac->hdac, pin->nid, 0,
					AC_VERB_SET_CONNECT_SEL, mux_idx);
			break;
		}
	}

	if (mux_idx == pin->num_mux_nids)
		return -EIO;

	/* Enable out path for this pin widget */
	snd_hdac_codec_write(&hdac->hdac, pin->nid, 0,
			AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT);

	hdac_hdmi_set_power_state(hdac, dai_map, AC_PWRST_D0);

	snd_hdac_codec_write(&hdac->hdac, pin->nid, 0,
			AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE);

	return 0;
}

static int hdac_hdmi_query_pin_connlist(struct hdac_ext_device *hdac,
					struct hdac_hdmi_pin *pin)
{
	if (!(get_wcaps(&hdac->hdac, pin->nid) & AC_WCAP_CONN_LIST)) {
		dev_warn(&hdac->hdac.dev,
			"HDMI: pin %d wcaps %#x does not support connection list\n",
			pin->nid, get_wcaps(&hdac->hdac, pin->nid));
		return -EINVAL;
	}

	pin->num_mux_nids = snd_hdac_get_connections(&hdac->hdac, pin->nid,
			pin->mux_nids, HDA_MAX_CONNECTIONS);
	if (pin->num_mux_nids == 0)
		dev_warn(&hdac->hdac.dev, "No connections found for pin: %d\n", pin->nid);

	dev_dbg(&hdac->hdac.dev, "num_mux_nids %d for pin: %d\n",
			pin->num_mux_nids, pin->nid);

	return pin->num_mux_nids;
}

/*
 * This queries pcm list and returns a matching pin widget to which
 * stream is routed to.
 *
 * The converter may be input to multiple pin muxes. So each
 * pin mux (basically each pin widget) is queried to identify if
 * the converter as one of the input, then the first pin match
 * is selected for rendering.
 *
 * Same stream rendering to multiple pins simultaneously can be done
 * possibly, but not supported for now.
 *
 * So return the first pin connected
 */
static struct hdac_hdmi_pin *hdac_hdmi_get_pin_from_cvt(
			struct hdac_ext_device *edev,
			struct hdac_hdmi_priv *hdmi,
			struct hdac_hdmi_cvt *cvt)
{
	struct hdac_hdmi_pcm *pcm;
	struct hdac_hdmi_pin *pin = NULL;
	int ret, i;

	list_for_each_entry(pcm, &hdmi->pcm_list, head) {
		if (pcm->cvt == cvt) {
			pin = pcm->pin;
			break;
		}
	}

	if (pin) {
		ret = hdac_hdmi_query_pin_connlist(edev, pin);
		if (ret < 0)
			return NULL;

		for (i = 0; i < pin->num_mux_nids; i++) {
			if (pin->mux_nids[i] == cvt->nid)
				return pin;
		}
	}

	return NULL;
}

static int hdac_hdmi_playback_prepare(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct hdac_ext_device *hdac = snd_soc_dai_get_drvdata(dai);
	struct hdac_hdmi_priv *hdmi = hdac->private_data;
	struct hdac_hdmi_dai_pin_map *dai_map;
	struct hdac_ext_dma_params *dd;
	int ret;

	dai_map = &hdmi->dai_map[dai->id];

	dd = (struct hdac_ext_dma_params *)snd_soc_dai_get_dma_data(dai, substream);
	dev_dbg(&hdac->hdac.dev, "stream tag from cpu dai %d format in cvt 0x%x\n",
			dd->stream_tag,	dd->format);

	ret = hdac_hdmi_setup_audio_infoframe(hdac, dai_map->cvt->nid,
						dai_map->pin->nid);
	if (ret < 0)
		return ret;

	return hdac_hdmi_setup_stream(hdac, dai_map->cvt->nid,
			dai_map->pin->nid, dd->stream_tag, dd->format);
}

static int hdac_hdmi_trigger(struct snd_pcm_substream *substream, int cmd,
		struct snd_soc_dai *dai)
{
	struct hdac_hdmi_dai_pin_map *dai_map;
	struct hdac_ext_device *hdac = snd_soc_dai_get_drvdata(dai);
	struct hdac_hdmi_priv *hdmi = hdac->private_data;
	int ret;

	dai_map = &hdmi->dai_map[dai->id];
	if (cmd == SNDRV_PCM_TRIGGER_RESUME) {
		ret = hdac_hdmi_enable_pin(hdac, dai_map);
		if (ret < 0)
			return ret;

		return hdac_hdmi_playback_prepare(substream, dai);
	}

	return 0;
}

static void hdac_hdmi_present_sense(struct hdac_hdmi_pin *pin, int repoll);

static int hdac_hdmi_set_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *hparams, struct snd_soc_dai *dai)
{
	struct hdac_ext_device *hdac = snd_soc_dai_get_drvdata(dai);
	struct hdac_hdmi_priv *hdmi = hdac->private_data;
	struct hdac_hdmi_dai_pin_map *dai_map;
	struct hdac_hdmi_cvt *cvt;
	struct hdac_hdmi_pin *pin;
	int ret;
	struct hdac_ext_dma_params *dd;

	dai_map = &hdmi->dai_map[dai->id];

	cvt = dai_map->cvt;
	pin = hdac_hdmi_get_pin_from_cvt(hdac, hdmi, cvt);
	if (!pin)
		return -EIO;

	if ((!pin->eld.monitor_present) || (!pin->eld.eld_valid)) {
		/* one more last try to detect pin sense */
		hdac_hdmi_present_sense(pin, 0);
		if ((!pin->eld.monitor_present) || (!pin->eld.eld_valid)) {
			dev_err(&hdac->hdac.dev,
				"Failed: montior present? %d eld valid?: %d for pin: %d\n",
				pin->eld.monitor_present, pin->eld.eld_valid, pin->nid);
			return -EINVAL;
		}
	}

	dai_map->pin = pin;

	hdac_hdmi_enable_cvt(hdac, dai_map);
	ret = hdac_hdmi_enable_pin(hdac, dai_map);
	if (ret < 0)
		return ret;

	ret = hdac_hdmi_eld_limit_formats(substream->runtime,
				pin->eld.eld_buffer);
	if (ret < 0)
		return ret;

	ret = snd_pcm_hw_constraint_eld(substream->runtime,
			dai_map->pin->eld.eld_buffer);

	if (ret < 0)
		return ret;

	dd = (struct hdac_ext_dma_params *)snd_soc_dai_get_dma_data(dai, substream);
	if (!dd) {
		dd = kzalloc(sizeof(*dd), GFP_KERNEL);
		if (!dd)
			return -ENOMEM;
	}
	dd->format = snd_hdac_calc_stream_format(params_rate(hparams),
			params_channels(hparams), params_format(hparams),
			24, 0);

	snd_soc_dai_set_dma_data(dai, substream, (void *)dd);

	return 0;
}

static int hdac_hdmi_playback_cleanup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct hdac_ext_device *edev = snd_soc_dai_get_drvdata(dai);
	struct hdac_ext_dma_params *dd;
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct hdac_hdmi_dai_pin_map *dai_map;

	dai_map = &hdmi->dai_map[dai->id];

	dd = (struct hdac_ext_dma_params *)snd_soc_dai_get_dma_data(dai, substream);

	if (dd) {
		snd_soc_dai_set_dma_data(dai, substream, NULL);
		kfree(dd);
	}

	if (dai_map->pin) {
		snd_hdac_codec_write(&edev->hdac, dai_map->cvt->nid, 0,
				AC_VERB_SET_CHANNEL_STREAMID, 0);
		snd_hdac_codec_write(&edev->hdac, dai_map->cvt->nid, 0,
				AC_VERB_SET_STREAM_FORMAT, 0);

		hdac_hdmi_set_power_state(edev, dai_map, AC_PWRST_D3);

		snd_hdac_codec_write(&edev->hdac, dai_map->pin->nid, 0,
			AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE);

		dai_map->pin = NULL;
	}

	return 0;
}

static int
hdac_hdmi_query_cvt_params(struct hdac_device *hdac, struct hdac_hdmi_cvt *cvt)
{
	int err;

	/* Only stereo supported as of now */
	cvt->params.channels_min = cvt->params.channels_max = 2;

	err = snd_hdac_query_supported_pcm(hdac, cvt->nid,
			&cvt->params.rates,
			&cvt->params.formats,
			&cvt->params.maxbps);
	if (err < 0)
		dev_err(&hdac->dev,
			"Failed to query pcm params for nid %d: %d\n",
			cvt->nid, err);

	return err;
}

static int hdac_hdmi_fill_widget_info(struct device *dev,
				struct snd_soc_dapm_widget *w,
				enum snd_soc_dapm_type id, void *priv,
				const char *wname, const char *stream,
				struct snd_kcontrol_new *wc, int numkc)
{
	w->id = id;
	w->name = devm_kstrdup(dev, wname, GFP_KERNEL);
	if (!w->name)
		return -ENOMEM;

	w->sname = stream;
	w->reg = SND_SOC_NOPM;
	w->shift = 0;
	w->kcontrol_news = wc;
	w->num_kcontrols = numkc;
	w->priv = priv;

	return 0;
}

static void hdac_hdmi_fill_route(struct snd_soc_dapm_route *route,
		const char *sink, const char *control, const char *src,
		int (*handler)(struct snd_soc_dapm_widget *sr,
			struct snd_soc_dapm_widget *snk))
{
	route->sink = sink;
	route->source = src;
	route->control = control;
	route->connected = handler;
}

static int hdac_hdmi_get_pcm(struct hdac_ext_device *edev,
					struct hdac_hdmi_pin *pin)
{
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct hdac_hdmi_pcm *pcm = NULL;

	list_for_each_entry(pcm, &hdmi->pcm_list, head) {
		if (pcm->pin == pin)
			return pcm->pcm_id;
	}

	return -ENODEV;
}

static void hdac_hdmi_jack_report(struct hdac_ext_device *edev, int pcm,
					int val)
{
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct hdac_hdmi_jack *jack = NULL;

	list_for_each_entry(jack, &hdmi->jack_list, head) {
		if (jack->pcm == pcm) {
			dev_dbg(&edev->hdac.dev,
				"jack report for pcm=%d value=%d\n", pcm, val);
			snd_jack_report(jack->jack, val ? SND_JACK_AVOUT : 0);
			return;
		}
	}
}

static int hdac_hdmi_set_pin_mux(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	int ret;
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	struct snd_soc_dapm_widget *w = snd_soc_dapm_kcontrol_widget(kcontrol);
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct hdac_hdmi_pin *pin = w->priv;
	struct hdac_ext_device *edev = to_hda_ext_device(dapm->dev);
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct hdac_hdmi_pcm *pcm = NULL;
	const char *cvt_name =  e->texts[ucontrol->value.enumerated.item[0]];

	ret = snd_soc_dapm_put_enum_double(kcontrol, ucontrol);
	if (ret < 0)
		return ret;

	mutex_lock(&hdmi->pin_mutex);
	list_for_each_entry(pcm, &hdmi->pcm_list, head) {
		if (pcm->pin == pin)
			pcm->pin = NULL;

		if (!strcmp(cvt_name, pcm->cvt->name) && !pcm->pin) {
			pcm->pin = pin;
			if (pin->eld.monitor_present && pin->eld.eld_valid)
				hdac_hdmi_jack_report(edev, pcm->pcm_id, 1);
			mutex_unlock(&hdmi->pin_mutex);
			return ret;
		}
	}
	mutex_unlock(&hdmi->pin_mutex);

	return ret;
}

/*
 * Ideally the Mux inputs should be based on the num_muxs enumerated, but
 * the display driver seem to be programming the connection list for the pin
 * widget runtime.
 *
 * So programming all the possible inputs for the mux, the user has to take
 * care of selecting the right one and leaving all other inputs selected to
 * "NONE"
 */
static int hdac_hdmi_create_pin_muxs(struct hdac_ext_device *edev,
				struct hdac_hdmi_pin *pin,
				struct snd_soc_dapm_widget *widget,
				const char *widget_name)
{
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct snd_kcontrol_new *kc;
	struct hdac_hdmi_cvt *cvt;
	struct soc_enum *se;
	char kc_name[NAME_SIZE];
	char mux_items[NAME_SIZE];
	/* To hold inputs to the Pin mux */
	char *items[HDA_MAX_CONNECTIONS];
	int i = 0;
	int num_items = hdmi->num_cvt + 1;

	kc = devm_kzalloc(&edev->hdac.dev, sizeof(*kc), GFP_KERNEL);
	if (!kc)
		return -ENOMEM;

	se = devm_kzalloc(&edev->hdac.dev, sizeof(*se), GFP_KERNEL);
	if (!se)
		return -ENOMEM;

	sprintf(kc_name, "Pin %d Input", pin->nid);
	kc->name = devm_kstrdup(&edev->hdac.dev, kc_name, GFP_KERNEL);
	if (!kc->name)
		return -ENOMEM;

	kc->private_value = (long)se;
	kc->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	kc->access = 0;
	kc->info = snd_soc_info_enum_double;
	kc->put = hdac_hdmi_set_pin_mux;
	kc->get = snd_soc_dapm_get_enum_double;

	se->reg = SND_SOC_NOPM;

	/* enum texts: ["NONE", "cvt #", "cvt #", ...] */
	se->items = num_items;
	se->mask = roundup_pow_of_two(se->items) - 1;

	sprintf(mux_items, "NONE");
	items[i] = devm_kstrdup(&edev->hdac.dev, mux_items, GFP_KERNEL);
	if (!items[i])
		return -ENOMEM;

	list_for_each_entry(cvt, &hdmi->cvt_list, head) {
		i++;
		sprintf(mux_items, "cvt %d", cvt->nid);
		items[i] = devm_kstrdup(&edev->hdac.dev, mux_items, GFP_KERNEL);
		if (!items[i])
			return -ENOMEM;
	}

	se->texts = devm_kmemdup(&edev->hdac.dev, items,
			(num_items  * sizeof(char *)), GFP_KERNEL);
	if (!se->texts)
		return -ENOMEM;

	return hdac_hdmi_fill_widget_info(&edev->hdac.dev, widget, snd_soc_dapm_mux,
				pin, widget_name, NULL, kc, 1);
}

/* Add cvt <- input <- mux route map */
static void hdac_hdmi_add_pinmux_cvt_route(struct hdac_ext_device *edev,
		struct snd_soc_dapm_widget *widgets,
		struct snd_soc_dapm_route *route, int rindex)
{
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	const struct snd_kcontrol_new *kc;
	struct soc_enum *se;
	int mux_index = hdmi->num_cvt + hdmi->num_pin;
	int i, j;

	for (i = 0; i < hdmi->num_pin; i++) {
		kc = widgets[mux_index].kcontrol_news;
		se = (struct soc_enum *)kc->private_value;
		for (j = 0; j < hdmi->num_cvt; j++) {
			hdac_hdmi_fill_route(&route[rindex],
					widgets[mux_index].name,
					se->texts[j + 1],
					widgets[j].name, NULL);

			rindex++;
		}

		mux_index++;
	}
}

/*
 * Widgets are added in the below sequence
 *	Converter widgets for num converters enumerated
 *	Pin widgets for num pins enumerated
 *	Pin mux widgets to represent connenction list of pin widget
 *
 * Total widgets elements = num_cvt + num_pin + num_pin;
 *
 * Routes are added as below:
 *	pin mux -> pin (based on num_pins)
 *	cvt -> "Input sel control" -> pin_mux
 *
 * Total route elements:
 *	num_pins + (pin_muxes * num_cvt)
 */
static int create_fill_widget_route_map(struct snd_soc_dapm_context *dapm)
{
	struct snd_soc_dapm_widget *widgets;
	struct snd_soc_dapm_route *route;
	struct hdac_ext_device *edev = to_hda_ext_device(dapm->dev);
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct snd_soc_dai_driver *dai_drv = dapm->component->dai_drv;
	char widget_name[NAME_SIZE];
	struct hdac_hdmi_cvt *cvt;
	struct hdac_hdmi_pin *pin;
	int i = 0;
	int ret;
	int num_routes = 0;

	if (list_empty(&hdmi->cvt_list) || list_empty(&hdmi->pin_list))
		return -EINVAL;

	widgets = devm_kzalloc(dapm->dev,
			(sizeof(*widgets) * ((2 * hdmi->num_pin) + hdmi->num_cvt)),
			GFP_KERNEL);
	if (!widgets)
		return -ENOMEM;

	/* DAPM widgets to represent each converter widget */
	list_for_each_entry(cvt, &hdmi->cvt_list, head) {
		sprintf(widget_name, "Converter %d", cvt->nid);
		ret = hdac_hdmi_fill_widget_info(dapm->dev, &widgets[i],
			snd_soc_dapm_aif_in, &cvt->nid,
			widget_name, dai_drv[i].playback.stream_name, NULL, 0);
		if (ret < 0)
			return ret;
		i++;
	}

	list_for_each_entry(pin, &hdmi->pin_list, head) {
		sprintf(widget_name, "hif%d Output", pin->nid);
		ret = hdac_hdmi_fill_widget_info(dapm->dev, &widgets[i],
				snd_soc_dapm_output, &pin->nid,
				widget_name, NULL, NULL, 0);
		if (ret < 0)
			return ret;
		i++;
	}

	/* DAPM widgets to represent the connection list to pin widget */
	list_for_each_entry(pin, &hdmi->pin_list, head) {
		sprintf(widget_name, "Pin %d Mux", pin->nid);
		ret = hdac_hdmi_create_pin_muxs(edev, pin, &widgets[i],
							widget_name);
		if (ret < 0)
			return ret;
		i++;

		/* For cvt to pin_mux mapping */
		num_routes += hdmi->num_cvt;

		/* For pin_mux to pin mapping */
		num_routes++;
	}

	route = devm_kzalloc(dapm->dev, (sizeof(*route) * num_routes),
							GFP_KERNEL);
	if (!route)
		return -ENOMEM;

	i = 0;
	/* Add pin <- NULL <- mux route map */
	list_for_each_entry(pin, &hdmi->pin_list, head) {
		int sink_index = i + hdmi->num_cvt;
		int src_index = sink_index + hdmi->num_pin;

		hdac_hdmi_fill_route(&route[i], widgets[sink_index].name,
				NULL, widgets[src_index].name,
				NULL);
		i++;

	}

	hdac_hdmi_add_pinmux_cvt_route(edev, widgets, route, i);

	snd_soc_dapm_new_controls(dapm, widgets,
		((2 * hdmi->num_pin) + hdmi->num_cvt));
	snd_soc_dapm_add_routes(dapm, route, num_routes);
	snd_soc_dapm_new_widgets(dapm->card);

	return 0;

}

static int hdac_hdmi_init_dai_map(struct hdac_ext_device *edev)
{
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct hdac_hdmi_dai_pin_map *dai_map;
	struct hdac_hdmi_cvt *cvt;
	int dai_id = 0;

	if (list_empty(&hdmi->cvt_list))
		return -EINVAL;

	list_for_each_entry(cvt, &hdmi->cvt_list, head) {
		dai_map = &hdmi->dai_map[dai_id];
		dai_map->dai_id = dai_id;
		dai_map->cvt = cvt;

		dai_id++;

		if (dai_id == HDA_MAX_CVTS) {
			dev_warn(&edev->hdac.dev, "Max dais supported: %d\n", dai_id);
			break;
		}
	}

	return 0;
}

static int hdac_hdmi_add_cvt(struct hdac_ext_device *edev, hda_nid_t nid)
{
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct hdac_hdmi_cvt *cvt;
	char name[NAME_SIZE];

	cvt = kzalloc(sizeof(*cvt), GFP_KERNEL);
	if (!cvt)
		return -ENOMEM;

	cvt->nid = nid;
	sprintf(name, "cvt %d", cvt->nid);
	cvt->name = devm_kstrdup(&edev->hdac.dev, name, GFP_KERNEL);

	list_add_tail(&cvt->head, &hdmi->cvt_list);
	hdmi->num_cvt++;

	return hdac_hdmi_query_cvt_params(&edev->hdac, cvt);
}

static void hdac_hdmi_present_sense(struct hdac_hdmi_pin *pin, int repoll)
{
	struct hdac_ext_device *edev = pin->edev;
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	int val, pcm_id;

	if (!edev)
		return;

	pin->repoll_count = repoll;

	pm_runtime_get_sync(&edev->hdac.dev);
	val = snd_hdac_codec_read(&edev->hdac, pin->nid, 0,
					AC_VERB_GET_PIN_SENSE, 0);

	dev_dbg(&edev->hdac.dev, "Pin sense val %x for pin: %d\n",
			val, pin->nid);

	mutex_lock(&hdmi->pin_mutex);
	pin->eld.monitor_present = !!(val & AC_PINSENSE_PRESENCE);
	pin->eld.eld_valid = !!(val & AC_PINSENSE_ELDV);

	pcm_id = hdac_hdmi_get_pcm(edev, pin);

	if (!pin->eld.monitor_present || !pin->eld.eld_valid) {
		dev_info(&edev->hdac.dev, "%s: disconnect or eld_invalid\n",
				__func__);
		if (pcm_id >= 0)
			hdac_hdmi_jack_report(edev, pcm_id, 0);

		mutex_unlock(&hdmi->pin_mutex);
		goto put_hdac_device;
	}

	if (pin->eld.monitor_present && pin->eld.eld_valid) {
		/* TODO: Use i915 cmpnt framework when available */
		if (hdac_hdmi_get_eld(&edev->hdac, pin->nid,
				pin->eld.eld_buffer,
				&pin->eld.eld_size) == 0) {
			if (pcm_id >= 0)
				hdac_hdmi_jack_report(edev, pcm_id, 1);
			print_hex_dump_bytes("Eld: ", DUMP_PREFIX_OFFSET,
					pin->eld.eld_buffer, pin->eld.eld_size);
		} else {
			dev_err(&edev->hdac.dev, "ELD invalid\n");
			pin->eld.monitor_present = false;
			pin->eld.eld_valid = false;
			if (pcm_id >= 0)
				hdac_hdmi_jack_report(edev, pcm_id, 0);
		}

	}

	mutex_unlock(&hdmi->pin_mutex);

	/*
	 * Sometimes the pin_sense may present invalid monitor
	 * present and eld_valid. If eld data is not valid loop, few
	 * more times to get correct pin sense and valid eld.
	 */
	if ((!pin->eld.monitor_present || !pin->eld.eld_valid) && repoll)
		schedule_delayed_work(&pin->work, msecs_to_jiffies(300));

put_hdac_device:
	pm_runtime_put_sync(&edev->hdac.dev);
}

static void hdac_hdmi_repoll_eld(struct work_struct *work)
{
	struct hdac_hdmi_pin *pin =
		container_of(to_delayed_work(work), struct hdac_hdmi_pin, work);

	/* picked from legacy */
	if (pin->repoll_count++ > 6)
		pin->repoll_count = 0;

	hdac_hdmi_present_sense(pin, pin->repoll_count);
}

static int hdac_hdmi_add_pin(struct hdac_ext_device *edev, hda_nid_t nid)
{
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct hdac_hdmi_pin *pin;

	pin = kzalloc(sizeof(*pin), GFP_KERNEL);
	if (!pin)
		return -ENOMEM;

	pin->nid = nid;

	list_add_tail(&pin->head, &hdmi->pin_list);
	hdmi->num_pin++;

	pin->edev = edev;
	INIT_DELAYED_WORK(&pin->work, hdac_hdmi_repoll_eld);

	return 0;
}

#define INTEL_VENDOR_NID 0x08
#define INTEL_GET_VENDOR_VERB 0xf81
#define INTEL_SET_VENDOR_VERB 0x781
#define INTEL_EN_DP12			0x02 /* enable DP 1.2 features */
#define INTEL_EN_ALL_PIN_CVTS	0x01 /* enable 2nd & 3rd pins and convertors */

static void hdac_hdmi_skl_enable_all_pins(struct hdac_device *hdac)
{
	unsigned int vendor_param;

	vendor_param = snd_hdac_codec_read(hdac, INTEL_VENDOR_NID, 0,
				INTEL_GET_VENDOR_VERB, 0);
	if (vendor_param == -1 || vendor_param & INTEL_EN_ALL_PIN_CVTS)
		return;

	vendor_param |= INTEL_EN_ALL_PIN_CVTS;
	vendor_param = snd_hdac_codec_read(hdac, INTEL_VENDOR_NID, 0,
				INTEL_SET_VENDOR_VERB, vendor_param);
	if (vendor_param == -1)
		return;
}

static void hdac_hdmi_skl_enable_dp12(struct hdac_device *hdac)
{
	unsigned int vendor_param;

	vendor_param = snd_hdac_codec_read(hdac, INTEL_VENDOR_NID, 0,
				INTEL_GET_VENDOR_VERB, 0);
	if (vendor_param == -1 || vendor_param & INTEL_EN_DP12)
		return;

	/* enable DP1.2 mode */
	vendor_param |= INTEL_EN_DP12;
	vendor_param = snd_hdac_codec_read(hdac, INTEL_VENDOR_NID, 0,
				INTEL_SET_VENDOR_VERB, vendor_param);
	if (vendor_param == -1)
		return;

}

static struct snd_soc_dai_ops hdmi_dai_ops = {
	.hw_params = hdac_hdmi_set_hw_params,
	.prepare = hdac_hdmi_playback_prepare,
	.trigger = hdac_hdmi_trigger,
	.hw_free = hdac_hdmi_playback_cleanup,
};

static int hdac_hdmi_create_dais(struct hdac_device *hdac,
		struct snd_soc_dai_driver **dais,
		struct hdac_hdmi_priv *hdmi, int num_dais)
{
	struct snd_soc_dai_driver *hdmi_dais;
	struct hdac_hdmi_cvt *cvt;
	char name[NAME_SIZE], dai_name[NAME_SIZE];
	int i = 0;
	u32 rates, bps;
	unsigned int rate_max = 384000, rate_min = 8000;
	u64 formats;
	int ret;

	hdmi_dais = devm_kzalloc(&hdac->dev, (sizeof(*hdmi_dais) * num_dais),
			GFP_KERNEL);
	if (!hdmi_dais)
		return -ENOMEM;

	list_for_each_entry(cvt, &hdmi->cvt_list, head) {
		ret = snd_hdac_query_supported_pcm(hdac, cvt->nid, &rates,
			&formats, &bps);
		if (ret)
			return ret;

		sprintf(dai_name, "intel-hdmi-hifi%d", i+1);
		hdmi_dais[i].name = devm_kstrdup(&hdac->dev, dai_name,
						GFP_KERNEL);
		if (!hdmi_dais[i].name)
			return -ENOMEM;

		snprintf(name, sizeof(name), "hifi%d", i+1);
		hdmi_dais[i].playback.stream_name =
				devm_kstrdup(&hdac->dev, name, GFP_KERNEL);
		if (!hdmi_dais[i].playback.stream_name)
			return -ENOMEM;

		hdmi_dais[i].playback.formats = formats;
		hdmi_dais[i].playback.rates = rates;
		hdmi_dais[i].playback.rate_max = rate_max;
		hdmi_dais[i].playback.rate_min = rate_min;
		hdmi_dais[i].playback.channels_min = 2;
		hdmi_dais[i].playback.channels_max = 2;
		hdmi_dais[i].ops = &hdmi_dai_ops;

		i++;
	}

	*dais = hdmi_dais;

	return 0;
}

/*
 * Parse all nodes and store the cvt/pin nids in array
 * Add one time initialization for pin and cvt widgets
 */
static int hdac_hdmi_parse_and_map_nid(struct hdac_ext_device *edev,
		struct snd_soc_dai_driver **dais, int *num_dais)
{
	hda_nid_t nid;
	int i, num_nodes;
	struct hdac_device *hdac = &edev->hdac;
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	int ret;

	hdac_hdmi_skl_enable_all_pins(hdac);
	hdac_hdmi_skl_enable_dp12(hdac);

	num_nodes = snd_hdac_get_sub_nodes(hdac, hdac->afg, &nid);
	if (!nid || num_nodes <= 0) {
		dev_warn(&hdac->dev, "HDMI: failed to get afg sub nodes\n");
		return -EINVAL;
	}

	hdac->num_nodes = num_nodes;
	hdac->start_nid = nid;

	for (i = 0; i < hdac->num_nodes; i++, nid++) {
		unsigned int caps;
		unsigned int type;

		caps = get_wcaps(hdac, nid);
		type = get_wcaps_type(caps);

		if (!(caps & AC_WCAP_DIGITAL))
			continue;

		switch (type) {

		case AC_WID_AUD_OUT:
			ret = hdac_hdmi_add_cvt(edev, nid);
			if (ret < 0)
				return ret;
			break;

		case AC_WID_PIN:
			ret = hdac_hdmi_add_pin(edev, nid);
			if (ret < 0)
				return ret;
			break;
		}
	}

	hdac->end_nid = nid;

	if (!hdmi->num_pin || !hdmi->num_cvt)
		return -EIO;

	ret = hdac_hdmi_create_dais(hdac, dais, hdmi, hdmi->num_cvt);
	if (ret) {
		dev_err(&hdac->dev, "Failed to create dais with err: %d\n",
							ret);
		return ret;
	}

	*num_dais = hdmi->num_cvt;

	return hdac_hdmi_init_dai_map(edev);
}

static void hdac_hdmi_eld_notify_cb(void *aptr, int port)
{
	struct hdac_ext_device *edev = aptr;
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct hdac_hdmi_pin *pin;
	struct snd_soc_codec *codec = edev->scodec;
	/* Don't know how this mapping is derived */
	hda_nid_t pin_nid = port + 0x04;

	dev_dbg(&edev->hdac.dev, "%s: for pin: %d\n", __func__, pin_nid);

	/*
	 * skip notification during system suspend (but not in runtime PM);
	 * the state will be updated at resume. Also since the ELD and
	 * connection states are updated in anyway at the end of the resume,
	 * we can skip it when received during PM process.
	 */
	if (snd_power_get_state(codec->component.card->snd_card) !=
			SNDRV_CTL_POWER_D0)
		return;

	if (atomic_read(&edev->hdac.in_pm))
		return;

	list_for_each_entry(pin, &hdmi->pin_list, head) {
		if (pin->nid == pin_nid)
			hdac_hdmi_present_sense(pin, 1);
	}
}

static struct i915_audio_component_audio_ops aops = {
	.pin_eld_notify	= hdac_hdmi_eld_notify_cb,
};

int hdac_hdmi_jack_init(struct snd_soc_dai *dai, int device)
{
	char jack_name[NAME_SIZE];
	struct snd_soc_codec *codec = dai->codec;
	struct hdac_ext_device *edev = snd_soc_codec_get_drvdata(codec);
	struct snd_soc_dapm_context *dapm =
		snd_soc_component_get_dapm(&codec->component);
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct hdac_hdmi_jack *jack;
	struct hdac_hdmi_pcm *pcm;
	int ret;

	/* this is a new PCM device, create new pcm and
	 * add to the pcm list
	 */
	pcm = kzalloc(sizeof(*pcm), GFP_KERNEL);
	if (!pcm)
		return -ENOMEM;
	pcm->pcm_id = device;
	pcm->cvt = hdmi->dai_map[dai->id].cvt;

	list_add_tail(&pcm->head, &hdmi->pcm_list);

	/* create new jack for this pcm */
	jack = kzalloc(sizeof(*jack), GFP_KERNEL);
	if (!jack)
		return -ENOMEM;

	sprintf(jack_name, "HDMI/DP, pcm=%d Jack", device);
	ret = snd_jack_new(dapm->card->snd_card, jack_name,
		SND_JACK_AVOUT,	&jack->jack, true, false);

	if (ret)
		return ret;
	jack->pcm = device;

	list_add_tail(&jack->head, &hdmi->jack_list);

	return 0;
}
EXPORT_SYMBOL_GPL(hdac_hdmi_jack_init);

static int hdmi_codec_probe(struct snd_soc_codec *codec)
{
	struct hdac_ext_device *edev = snd_soc_codec_get_drvdata(codec);
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct snd_soc_dapm_context *dapm =
		snd_soc_component_get_dapm(&codec->component);
	struct hdac_hdmi_pin *pin;
	int ret;

	edev->scodec = codec;

	ret = create_fill_widget_route_map(dapm);
	if (ret < 0)
		return ret;

	aops.audio_ptr = edev;
	ret = snd_hdac_i915_register_notifier(&aops);
	if (ret < 0) {
		dev_err(&edev->hdac.dev, "notifier register failed: err: %d\n",
				ret);
		return ret;
	}

	list_for_each_entry(pin, &hdmi->pin_list, head)
		hdac_hdmi_present_sense(pin, 1);

	/* Imp: Store the card pointer in hda_codec */
	edev->card = dapm->card->snd_card;

	/*
	 * hdac_device core already sets the state to active and calls
	 * get_noresume. So enable runtime and set the device to suspend.
	 */
	pm_runtime_set_autosuspend_delay(&edev->hdac.dev, 2000);
	pm_runtime_use_autosuspend(&edev->hdac.dev);
	pm_runtime_enable(&edev->hdac.dev);
	pm_runtime_put(&edev->hdac.dev);
	pm_runtime_suspend(&edev->hdac.dev);

	return 0;
}

static int hdmi_codec_remove(struct snd_soc_codec *codec)
{
	struct hdac_ext_device *edev = snd_soc_codec_get_drvdata(codec);

	pm_runtime_disable(&edev->hdac.dev);
	return 0;
}

#ifdef CONFIG_PM
static int hdmi_codec_resume(struct snd_soc_codec *codec)
{
	struct hdac_ext_device *edev = snd_soc_codec_get_drvdata(codec);
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct hdac_hdmi_pin *pin;
	struct hdac_device *hdac = &edev->hdac;
	struct hdac_bus *bus = hdac->bus;
	int err;

	hdac_hdmi_skl_enable_all_pins(&edev->hdac);
	hdac_hdmi_skl_enable_dp12(&edev->hdac);

	/* controller may not have been initialized for the first time */
	if (!bus)
		return 0;


	err = snd_hdac_display_power(bus, true);
	if (err < 0) {
		dev_err(bus->dev, "Cannot turn on display power on i915\n");
		return err;
	}

	/*
	 * As the ELD notify callback request is not entertained while the
	 * device is in suspend state. Need to manually check detection of
	 * all pins here.
	 */
	list_for_each_entry(pin, &hdmi->pin_list, head)
		hdac_hdmi_present_sense(pin, 1);

	/* Power up afg */
	if (!snd_hdac_check_power_state(hdac, hdac->afg, AC_PWRST_D0))
		snd_hdac_codec_write(hdac, hdac->afg, 0,
			AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	return 0;
}
#else
#define hdmi_codec_resume NULL
#endif

static struct snd_soc_codec_driver hdmi_hda_codec = {
	.probe		= hdmi_codec_probe,
	.remove		= hdmi_codec_remove,
	.resume		= hdmi_codec_resume,
	.idle_bias_off	= true,
};

static int hdac_hdmi_dev_probe(struct hdac_ext_device *edev)
{
	struct hdac_device *codec = &edev->hdac;
	struct hdac_hdmi_priv *hdmi_priv;
	struct snd_soc_dai_driver *hdmi_dais = NULL;
	int num_dais = 0;
	int ret = 0;

	hdmi_priv = devm_kzalloc(&codec->dev, sizeof(*hdmi_priv), GFP_KERNEL);
	if (hdmi_priv == NULL)
		return -ENOMEM;

	edev->private_data = hdmi_priv;

	dev_set_drvdata(&codec->dev, edev);

	INIT_LIST_HEAD(&hdmi_priv->pin_list);
	INIT_LIST_HEAD(&hdmi_priv->cvt_list);
	INIT_LIST_HEAD(&hdmi_priv->jack_list);
	INIT_LIST_HEAD(&hdmi_priv->pcm_list);
	mutex_init(&hdmi_priv->pin_mutex);
	/*
	 * Turned off in the runtime_suspend during the first explicit
	 * pm_runtime_suspend call.
	 */
	ret = snd_hdac_display_power(edev->hdac.bus, true);
	if (ret < 0) {
		dev_err(&edev->hdac.dev,
			"Cannot turn on display power on i915 err: %d\n",
			ret);
		return ret;
	}

	ret = hdac_hdmi_parse_and_map_nid(edev, &hdmi_dais, &num_dais);
	if (ret < 0) {
		dev_err(&codec->dev,
			"Failed in parse and map nid with err: %d\n", ret);
		return ret;
	}

	/* ASoC specific initialization */
	return snd_soc_register_codec(&codec->dev, &hdmi_hda_codec,
			hdmi_dais, num_dais);
}

static int hdac_hdmi_dev_remove(struct hdac_ext_device *edev)
{
	struct hdac_hdmi_priv *hdmi = edev->private_data;
	struct hdac_hdmi_pin *pin, *pin_next;
	struct hdac_hdmi_cvt *cvt, *cvt_next;
	struct hdac_hdmi_pcm *pcm, *pcm_next;
	struct hdac_hdmi_jack *jack, *jack_next;

	snd_soc_unregister_codec(&edev->hdac.dev);

	list_for_each_entry_safe(pcm, pcm_next, &hdmi->pcm_list, head) {
		pcm->cvt = NULL;
		pcm->pin = NULL;
		list_del(&pcm->head);
		kfree(pcm);
	}

	list_for_each_entry_safe(jack, jack_next, &hdmi->jack_list, head) {
		list_del(&jack->head);
		kfree(jack);
	}

	list_for_each_entry_safe(cvt, cvt_next, &hdmi->cvt_list, head) {
		list_del(&cvt->head);
		kfree(cvt);
	}

	list_for_each_entry_safe(pin, pin_next, &hdmi->pin_list, head) {
		list_del(&pin->head);
		kfree(pin);
	}

	return 0;
}

#ifdef CONFIG_PM
static int hdac_hdmi_runtime_suspend(struct device *dev)
{
	struct hdac_ext_device *edev = to_hda_ext_device(dev);
	struct hdac_device *hdac = &edev->hdac;
	struct hdac_bus *bus = hdac->bus;
	int err;

	dev_dbg(dev, "Enter: %s\n", __func__);

	/* controller may not have been initialized for the first time */
	if (!bus)
		return 0;

	/* Power down afg */
	if (!snd_hdac_check_power_state(hdac, hdac->afg, AC_PWRST_D3))
		snd_hdac_codec_write(hdac, hdac->afg, 0,
			AC_VERB_SET_POWER_STATE, AC_PWRST_D3);

	err = snd_hdac_display_power(bus, false);
	if (err < 0) {
		dev_err(bus->dev, "Cannot turn on display power on i915\n");
		return err;
	}

	return 0;
}

static int hdac_hdmi_runtime_resume(struct device *dev)
{
	struct hdac_ext_device *edev = to_hda_ext_device(dev);
	struct hdac_device *hdac = &edev->hdac;
	struct hdac_bus *bus = hdac->bus;
	int err;

	dev_dbg(dev, "Enter: %s\n", __func__);

	/* controller may not have been initialized for the first time */
	if (!bus)
		return 0;


	err = snd_hdac_display_power(bus, true);
	if (err < 0) {
		dev_err(bus->dev, "Cannot turn on display power on i915\n");
		return err;
	}

	hdac_hdmi_skl_enable_all_pins(&edev->hdac);
	hdac_hdmi_skl_enable_dp12(&edev->hdac);

	/* Power up afg */
	if (!snd_hdac_check_power_state(hdac, hdac->afg, AC_PWRST_D0))
		snd_hdac_codec_write(hdac, hdac->afg, 0,
			AC_VERB_SET_POWER_STATE, AC_PWRST_D0);

	return 0;
}
#else
#define hdac_hdmi_runtime_suspend NULL
#define hdac_hdmi_runtime_resume NULL
#endif

static const struct dev_pm_ops hdac_hdmi_pm = {
	SET_RUNTIME_PM_OPS(hdac_hdmi_runtime_suspend, hdac_hdmi_runtime_resume, NULL)
};

static const struct hda_device_id hdmi_list[] = {
	HDA_CODEC_EXT_ENTRY(0x80862809, 0x100000, "Skylake HDMI", 0),
	{}
};

MODULE_DEVICE_TABLE(hdaudio, hdmi_list);

static struct hdac_ext_driver hdmi_driver = {
	. hdac = {
		.driver = {
			.name   = "HDMI HDA Codec",
			.pm = &hdac_hdmi_pm,
		},
		.id_table       = hdmi_list,
	},
	.probe          = hdac_hdmi_dev_probe,
	.remove         = hdac_hdmi_dev_remove,
};

static int __init hdmi_init(void)
{
	return snd_hda_ext_driver_register(&hdmi_driver);
}

static void __exit hdmi_exit(void)
{
	snd_hda_ext_driver_unregister(&hdmi_driver);
}

module_init(hdmi_init);
module_exit(hdmi_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("HDMI HD codec");
MODULE_AUTHOR("Samreen Nilofer<samreen.nilofer@intel.com>");
MODULE_AUTHOR("Subhransu S. Prusty<subhransu.s.prusty@intel.com>");