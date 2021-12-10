// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux driver for ZOOM devices (L-8 only at the moment)
 *
 * Copyright 2021 (C) Sebastian Reimers
 *
 * Authors:  Sebastian Reimers <hallo@studio-link.de>
 *
 */

#ifndef ZOOM_PCM_H
#define ZOOM_PCM_H

struct zoom_chip;

int zoom_pcm_init(struct zoom_chip *chip, u8 extra_freq);
void zoom_pcm_abort(struct zoom_chip *chip);
#endif /* ZOOM_PCM_H */
