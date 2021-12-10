// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux driver for ZOOM devices (L-8 only at the moment)
 *
 * Copyright 2021 (C) Sebastian Reimers
 *
 * Authors:  Sebastian Reimers <hallo@studio-link.de>
 *
 */

#ifndef ZOOM_CHIP_H
#define ZOOM_CHIP_H

#include <linux/usb.h>
#include <sound/core.h>

struct pcm_runtime;

struct zoom_chip {
	struct usb_device *dev;
	struct snd_card *card;
	struct pcm_runtime *pcm;
};
#endif /* ZOOM_CHIP_H */
