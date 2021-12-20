// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux driver for ZOOM devices (L-8 only at the moment)
 *
 * Copyright 2021 (C) Sebastian Reimers
 *
 * Authors:  Sebastian Reimers <hallo@studio-link.de>
 *
 */

#include <linux/slab.h>
#include <sound/pcm.h>

#include "pcm.h"
#include "driver.h"

#define IN_EP           0x82
#define OUT_EP          0x01
#define PCM_N_URBS      4
#define PCM_URB_SIZE 512
#define PCM_PACKET_SIZE (4 * 4) /* 32 Bit x Frames/URB */

struct pcm_urb {
	struct zoom_chip *chip;

	struct urb instance;
	struct usb_anchor submitted;
	u8 *buffer;
};

struct pcm_substream {
	spinlock_t lock;
	struct snd_pcm_substream *instance;

	bool active;
	snd_pcm_uframes_t dma_off;    /* current position in alsa dma_area */
	snd_pcm_uframes_t period_off; /* current position in current period */
};

enum { /* pcm streaming states */
	STREAM_DISABLED, /* no pcm streaming */
	STREAM_STARTING, /* pcm streaming requested, waiting to become ready */
	STREAM_RUNNING,  /* pcm streaming running */
	STREAM_STOPPING
};

struct pcm_runtime {
	struct zoom_chip *chip;
	struct snd_pcm *instance;

	struct pcm_substream playback;
	struct pcm_substream capture;
	bool panic; /* if set driver won't do anymore pcm on device */

	struct pcm_urb out_urbs[PCM_N_URBS];
	struct pcm_urb in_urbs[PCM_N_URBS];

	struct mutex stream_mutex;
	u8 stream_state; /* one of STREAM_XXX */
	wait_queue_head_t stream_wait_queue;
	bool stream_wait_cond;
};

static const unsigned int rates[] = { 48000 };
static const struct snd_pcm_hw_constraint_list constraints_extra_rates = {
	.count = ARRAY_SIZE(rates),
	.list = rates,
	.mask = 0,
};

static const struct snd_pcm_hardware pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH,

	.formats = SNDRV_PCM_FMTBIT_S32_LE,

	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 4,
	.buffer_bytes_max = 1024 * 1024,
	.period_bytes_min = PCM_PACKET_SIZE * 2,
	.period_bytes_max = 512 * 1024,
	.periods_min = 2,
	.periods_max = 1024
};

static const struct snd_pcm_hardware pcm_hw_rec = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH,

	.formats = SNDRV_PCM_FMTBIT_S32_LE,

	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 1,
	.channels_max = 12,
	.buffer_bytes_max = 1024 * 1024,
	.period_bytes_min = PCM_PACKET_SIZE * 12,
	.period_bytes_max = 512 * 1024,
	.periods_min = 2,
	.periods_max = 1024
};

static struct pcm_substream *zoom_pcm_get_substream(struct snd_pcm_substream
						      *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct device *device = &rt->chip->dev->dev;

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return &rt->playback;

	if (alsa_sub->stream == SNDRV_PCM_STREAM_CAPTURE)
		return &rt->capture;

	dev_err(device, "Error getting pcm substream slot.\n");
	return NULL;
}

/* call with stream_mutex locked */
static void zoom_pcm_stream_stop(struct pcm_runtime *rt)
{
	int i, time;

	if (rt->stream_state != STREAM_DISABLED) {
		rt->stream_state = STREAM_STOPPING;

		for (i = 0; i < PCM_N_URBS; i++) {
			time = usb_wait_anchor_empty_timeout(
					&rt->out_urbs[i].submitted, 100);
			if (!time)
				usb_kill_anchored_urbs(
					&rt->out_urbs[i].submitted);
			usb_kill_urb(&rt->out_urbs[i].instance);
		}

		for (i = 0; i < PCM_N_URBS; i++) {
			time = usb_wait_anchor_empty_timeout(
					&rt->in_urbs[i].submitted, 100);
			if (!time)
				usb_kill_anchored_urbs(
					&rt->in_urbs[i].submitted);
			usb_kill_urb(&rt->in_urbs[i].instance);
		}

		rt->stream_state = STREAM_DISABLED;
	}
}

static int zoom_interface_init(struct pcm_runtime *rt)
{
	int ret = 0;

	ret = usb_set_interface(rt->chip->dev, 1, 3); /* ALT=1 EP1 OUT 32 bit */
	if (ret != 0) {
		zoom_pcm_stream_stop(rt);
		dev_err(&rt->chip->dev->dev,
				"can't set first interface for device.\n");
		return -EIO;
	}

	ret = usb_set_interface(rt->chip->dev, 2, 3); /* ALT=2 EP2 IN 32 bit */
	if (ret != 0) {
		zoom_pcm_stream_stop(rt);
		dev_err(&rt->chip->dev->dev,
				"can't set second interface for device.\n");
		return -EIO;
	}

	return 0;
}

/* call with stream_mutex locked */
static int zoom_pcm_stream_start(struct pcm_runtime *rt)
{
	int ret = 0;
	int i;

	if (rt->stream_state == STREAM_DISABLED) {

		/* reset panic state when starting a new stream */
		rt->panic = false;
		
		/* the device is rather forgetful, after some time without
		 * URBs the device fallbacks to 16bit mode */
		ret = zoom_interface_init(rt);
		if (ret)
			return ret;

		/* submit our out urbs zero init */
		rt->stream_state = STREAM_STARTING;
		for (i = 0; i < PCM_N_URBS; i++) {
			memset(rt->out_urbs[i].buffer, 0, PCM_URB_SIZE);
			usb_anchor_urb(&rt->out_urbs[i].instance,
				       &rt->out_urbs[i].submitted);
			ret = usb_submit_urb(&rt->out_urbs[i].instance,
					     GFP_ATOMIC);
			if (ret) {
				zoom_pcm_stream_stop(rt);
				return ret;
			}

			usb_anchor_urb(&rt->in_urbs[i].instance,
				       &rt->in_urbs[i].submitted);
			ret = usb_submit_urb(&rt->in_urbs[i].instance,
					     GFP_ATOMIC);
			if (ret) {
				zoom_pcm_stream_stop(rt);
				return ret;
			}
		}

		/* wait for first out urb to return (sent in in urb handler) */
		wait_event_timeout(rt->stream_wait_queue, rt->stream_wait_cond,
				   HZ);
		if (rt->stream_wait_cond) {
			struct device *device = &rt->chip->dev->dev;
			dev_info(device, "%s: Stream is running wakeup event\n",
				 __func__);
			rt->stream_state = STREAM_RUNNING;
		} else {
			zoom_pcm_stream_stop(rt);
			return -EIO;
		}
	}
	return ret;
}

/* The hardware wants 4x32ch (512 byte) values */
static void memcpy_pcm_playback(u8 *dest, u8 *src, u8 ch)
{
	unsigned int i, c, o = 0;

	for (i = 0; i < (PCM_URB_SIZE/4); i++) {
		if (i % 32) {
			((u32 *)dest)[i] = 0; /* Padding */
			continue;
		}

		for (c = 0; c < ch; c++) {
			((u32 *)dest)[i++] = ((u32 *)src)[o++];
		}
	}
}

static void memcpy_pcm_capture(u8 *dest, u8 *src, u8 ch, unsigned int skip, unsigned int l)
{
	unsigned int i, c, o = 0;

	for (i = 0; i < (PCM_URB_SIZE/4); i++) {
		if (i % 32)
			continue;

		for (c = 0; c < ch; c++) {
			if (skip && i < skip/4) {
				i++;
				continue;
			}
			if (l && o >= l/4)
				return;

			((u32 *)dest)[o++] = ((u32 *)src)[i++];
		}
	}
}

/* call with substream locked */
/* returns true if a period elapsed */
static bool zoom_pcm_capture(struct pcm_substream *sub, struct pcm_urb *urb)
{
	struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
	struct device *device = &urb->chip->dev->dev;
	u8 *source;
	unsigned int pcm_buffer_size, pcm_len, len;

	WARN_ON(alsa_rt->format != SNDRV_PCM_FORMAT_S32_LE);

	pcm_buffer_size = snd_pcm_lib_buffer_bytes(sub->instance);

	pcm_len = 4 * alsa_rt->channels * 4; /* 4 Byte (32Bit) * CH * 4 Frames */

	if (sub->dma_off + pcm_len <= pcm_buffer_size) {
		dev_dbg(device, "%s: (1) buffer_size %#x dma_offset %#x\n", __func__,
			 (unsigned int) pcm_buffer_size,
			 (unsigned int) sub->dma_off);

		source = alsa_rt->dma_area + sub->dma_off;
		memcpy_pcm_capture(source, urb->buffer, alsa_rt->channels, 0, 0);
	} else {
		/* wrap around at end of ring buffer */
		dev_dbg(device, "%s: (2) buffer_size %#x dma_offset %#x\n", __func__,
			 (unsigned int) pcm_buffer_size,
			 (unsigned int) sub->dma_off);
		
		len = pcm_buffer_size - sub->dma_off;
		source = alsa_rt->dma_area + sub->dma_off;
		memcpy_pcm_capture(source, urb->buffer, alsa_rt->channels, 0, len);

		source = alsa_rt->dma_area;
		memcpy_pcm_capture(source, urb->buffer, alsa_rt->channels, len, 0);

	}
	sub->dma_off += pcm_len;
	if (sub->dma_off >= pcm_buffer_size)
		sub->dma_off -= pcm_buffer_size;

	sub->period_off += pcm_len;
	if (sub->period_off >= alsa_rt->period_size) {
		sub->period_off %= alsa_rt->period_size;
		return true;
	}
	return false;
}

/* call with substream locked */
/* returns true if a period elapsed */
static bool zoom_pcm_playback(struct pcm_substream *sub, struct pcm_urb *urb)
{
	struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
	struct device *device = &urb->chip->dev->dev;
	u8 *source;
	unsigned int pcm_buffer_size, pcm_len;

	WARN_ON(alsa_rt->format != SNDRV_PCM_FORMAT_S32_LE);

	pcm_buffer_size = snd_pcm_lib_buffer_bytes(sub->instance);

	pcm_len = 4 * alsa_rt->channels * 4; /* 4 Byte (32Bit) * 2 CH * 4 Frames */

	if (sub->dma_off + pcm_len <= pcm_buffer_size) {
		dev_dbg(device, "%s: (1) buffer_size %#x dma_offset %#x\n", __func__,
			 (unsigned int) pcm_buffer_size,
			 (unsigned int) sub->dma_off);

		source = alsa_rt->dma_area + sub->dma_off;
		memcpy_pcm_playback(urb->buffer, source, alsa_rt->channels);
	} else {
		/* wrap around at end of ring buffer */
		dev_info(device, "%s: (2) buffer_size %#x dma_offset %#x\n", __func__,
			 (unsigned int) pcm_buffer_size,
			 (unsigned int) sub->dma_off);
	}
	sub->dma_off += pcm_len;
	if (sub->dma_off >= pcm_buffer_size)
		sub->dma_off -= pcm_buffer_size;

	sub->period_off += pcm_len;
	if (sub->period_off >= alsa_rt->period_size) {
		sub->period_off %= alsa_rt->period_size;
		return true;
	}
	return false;
}

static void zoom_pcm_in_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *in_urb = usb_urb->context;
	struct pcm_runtime *rt = in_urb->chip->pcm;
	struct pcm_substream *sub;
	bool do_period_elapsed = false;
	unsigned long flags;
	int ret;

	if (rt->panic || rt->stream_state == STREAM_STOPPING)
		return;

	if (unlikely(usb_urb->status == -ENOENT ||	/* unlinked */
		     usb_urb->status == -ENODEV ||	/* device removed */
		     usb_urb->status == -ECONNRESET ||	/* unlinked */
		     usb_urb->status == -ESHUTDOWN)) {	/* device disabled */
		goto out_fail;
	}

	sub = &rt->capture;
#if 1
	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active) {
		do_period_elapsed = zoom_pcm_capture(sub, in_urb);
	}
	spin_unlock_irqrestore(&sub->lock, flags);
	if (do_period_elapsed)
		snd_pcm_period_elapsed(sub->instance);

#endif
	ret = usb_submit_urb(&in_urb->instance, GFP_ATOMIC);
	if (ret < 0)
		goto out_fail;

	return;

out_fail:
	rt->panic = true;
}
	
static void zoom_pcm_out_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *out_urb = usb_urb->context;
	struct pcm_runtime *rt = out_urb->chip->pcm;
	struct pcm_substream *sub;
	bool do_period_elapsed = false;
	unsigned long flags;
	int ret;

	if (rt->panic || rt->stream_state == STREAM_STOPPING)
		return;

	if (unlikely(usb_urb->status == -ENOENT ||	/* unlinked */
		     usb_urb->status == -ENODEV ||	/* device removed */
		     usb_urb->status == -ECONNRESET ||	/* unlinked */
		     usb_urb->status == -ESHUTDOWN)) {	/* device disabled */
		goto out_fail;
	}

	if (rt->stream_state == STREAM_STARTING) {
		rt->stream_wait_cond = true;
		wake_up(&rt->stream_wait_queue);
	}

	/* now send our playback data (if a free out urb was found) */
	sub = &rt->playback;
	spin_lock_irqsave(&sub->lock, flags);

	if (sub->active) {
		do_period_elapsed = zoom_pcm_playback(sub, out_urb);
	}
	else
		memset(out_urb->buffer, 0, PCM_URB_SIZE);

	spin_unlock_irqrestore(&sub->lock, flags);

	if (do_period_elapsed)
		snd_pcm_period_elapsed(sub->instance);

	ret = usb_submit_urb(&out_urb->instance, GFP_ATOMIC);
	if (ret < 0)
		goto out_fail;

	return;

out_fail:
	rt->panic = true;
}

static int zoom_pcm_open(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = NULL;
	struct snd_pcm_runtime *alsa_rt = alsa_sub->runtime;

	if (rt->panic)
		return -EPIPE;

	mutex_lock(&rt->stream_mutex);

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		alsa_rt->hw = pcm_hw;
		sub = &rt->playback;
	}

	if (alsa_sub->stream == SNDRV_PCM_STREAM_CAPTURE) {
		alsa_rt->hw = pcm_hw_rec;
		sub = &rt->capture;
	}

	if (!sub) {
		struct device *device = &rt->chip->dev->dev;
		mutex_unlock(&rt->stream_mutex);
		dev_err(device, "Invalid stream type\n");
		return -EINVAL;
	}

	sub->instance = alsa_sub;
	sub->active = false;
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int zoom_pcm_close(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = zoom_pcm_get_substream(alsa_sub);
	unsigned long flags;

	if (rt->panic)
		return 0;

	mutex_lock(&rt->stream_mutex);
	if (sub) {
		zoom_pcm_stream_stop(rt);

		/* deactivate substream */
		spin_lock_irqsave(&sub->lock, flags);
		sub->instance = NULL;
		sub->active = false;
		spin_unlock_irqrestore(&sub->lock, flags);

	}
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int zoom_pcm_prepare(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = zoom_pcm_get_substream(alsa_sub);
	int ret;

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	mutex_lock(&rt->stream_mutex);

	zoom_pcm_stream_stop(rt);

	sub->dma_off = 0;
	sub->period_off = 0;

	if (rt->stream_state == STREAM_DISABLED) {

		ret = zoom_pcm_stream_start(rt);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			return ret;
		}
	}
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int zoom_pcm_trigger(struct snd_pcm_substream *alsa_sub, int cmd)
{
	struct pcm_substream *sub = zoom_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		spin_lock_irq(&sub->lock);
		sub->active = true;
		spin_unlock_irq(&sub->lock);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		spin_lock_irq(&sub->lock);
		sub->active = false;
		spin_unlock_irq(&sub->lock);
		return 0;

	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t zoom_pcm_pointer(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_substream *sub = zoom_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	unsigned long flags;
	snd_pcm_uframes_t dma_offset;

	if (rt->panic || !sub)
		return SNDRV_PCM_POS_XRUN;

	spin_lock_irqsave(&sub->lock, flags);
	dma_offset = sub->dma_off;
	spin_unlock_irqrestore(&sub->lock, flags);
	return bytes_to_frames(alsa_sub->runtime, dma_offset);
}

static const struct snd_pcm_ops pcm_ops = {
	.open = zoom_pcm_open,
	.close = zoom_pcm_close,
	.prepare = zoom_pcm_prepare,
	.trigger = zoom_pcm_trigger,
	.pointer = zoom_pcm_pointer,
};

static int zoom_pcm_init_urb_out(struct pcm_urb *urb,
			       struct zoom_chip *chip,
			       unsigned int ep,
			       void (*handler)(struct urb *))
{
	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(PCM_URB_SIZE, GFP_KERNEL);
	if (!urb->buffer)
		return -ENOMEM;

	usb_fill_bulk_urb(&urb->instance, chip->dev,
			  usb_sndbulkpipe(chip->dev, ep), (void *)urb->buffer,
			  PCM_URB_SIZE, handler, urb);
	if (usb_urb_ep_type_check(&urb->instance))
		return -EINVAL;
	init_usb_anchor(&urb->submitted);

	return 0;
}

static int zoom_pcm_init_urb_in(struct pcm_urb *urb,
			       struct zoom_chip *chip,
			       unsigned int ep,
			       void (*handler)(struct urb *))
{
	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(PCM_URB_SIZE, GFP_KERNEL);
	if (!urb->buffer)
		return -ENOMEM;

	usb_fill_bulk_urb(&urb->instance, chip->dev,
			  usb_rcvbulkpipe(chip->dev, ep), (void *)urb->buffer,
			  PCM_URB_SIZE, handler, urb);
	if (usb_urb_ep_type_check(&urb->instance))
		return -EINVAL;
	init_usb_anchor(&urb->submitted);

	return 0;
}

void zoom_pcm_abort(struct zoom_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;

	if (rt) {
		rt->panic = true;

		mutex_lock(&rt->stream_mutex);
		zoom_pcm_stream_stop(rt);
		mutex_unlock(&rt->stream_mutex);
	}
}

static void zoom_pcm_destroy(struct zoom_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;
	int i;

	for (i = 0; i < PCM_N_URBS; i++) {
		kfree(rt->out_urbs[i].buffer);
		kfree(rt->in_urbs[i].buffer);
	}

	kfree(chip->pcm);
	chip->pcm = NULL;
}

static void zoom_pcm_free(struct snd_pcm *pcm)
{
	struct pcm_runtime *rt = pcm->private_data;

	if (rt)
		zoom_pcm_destroy(rt->chip);
}

int zoom_pcm_init(struct zoom_chip *chip)
{
	int i;
	int ret;
	struct snd_pcm *pcm;
	struct pcm_runtime *rt;

	rt = kzalloc(sizeof(*rt), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;

	rt->chip = chip;
	rt->stream_state = STREAM_DISABLED;

	init_waitqueue_head(&rt->stream_wait_queue);
	mutex_init(&rt->stream_mutex);
	spin_lock_init(&rt->playback.lock);
	spin_lock_init(&rt->capture.lock);

	ret = zoom_interface_init(rt);
	if (ret)
		return ret;

	for (i = 0; i < PCM_N_URBS; i++) {
		ret = zoom_pcm_init_urb_out(&rt->out_urbs[i], chip, OUT_EP,
				    zoom_pcm_out_urb_handler);
		if (ret < 0) {
			printk("zoom_pcm_init_urb_out\n");
			goto error;
		}
	}

	for (i = 0; i < PCM_N_URBS; i++) {
		ret = zoom_pcm_init_urb_in(&rt->in_urbs[i], chip, IN_EP,
				    zoom_pcm_in_urb_handler);
		if (ret < 0) {
			printk("zoom_pcm_init_urb_in\n");
			goto error;
		}
	}

	ret = snd_pcm_new(chip->card, "USB Audio", 0, 1, 1, &pcm);
	if (ret < 0) {
		dev_err(&chip->dev->dev, "Cannot create pcm instance\n");
		goto error;
	}

	pcm->private_data = rt;
	pcm->private_free = zoom_pcm_free;

	strscpy(pcm->name, "USB Audio", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC,
				       NULL, 0, 0);

	rt->instance = pcm;

	chip->pcm = rt;
	return 0;

error:
	for (i = 0; i < PCM_N_URBS; i++)
		kfree(rt->out_urbs[i].buffer);
	for (i = 0; i < PCM_N_URBS; i++)
		kfree(rt->in_urbs[i].buffer);
	kfree(rt);
	return ret;
}
