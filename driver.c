// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux driver for ZOOM devices (L-8 only at the moment)
 *
 * Copyright 2021 (C) Sebastian Reimers
 *
 * Authors:  Sebastian Reimers <hallo@studio-link.de>
 *
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <sound/initval.h>

#include "driver.h"
#include "pcm.h"

MODULE_AUTHOR("Sebastian Reimers <hallo@studio-link.de>");
MODULE_DESCRIPTION("ZOOM L-8 USB audio driver");
MODULE_LICENSE("GPL");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX; /* Index 0-max */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR; /* Id for card */
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP; /* Enable this card */

#define DRIVER_NAME "snd-usb-zoom"
#define CARD_NAME "ZOOM L-8"

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for " CARD_NAME " soundcard.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for " CARD_NAME " soundcard.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable " CARD_NAME " soundcard.");

static DEFINE_MUTEX(register_mutex);

struct zoom_vendor_quirk {
	const char *device_name;
};

static int zoom_chip_create(struct usb_interface *intf,
			      struct usb_device *device, int idx,
			      const struct zoom_vendor_quirk *quirk,
			      struct zoom_chip **rchip)
{
	struct snd_card *card = NULL;
	struct zoom_chip *chip;
	int ret;
	int len;

	*rchip = NULL;

	/* if we are here, card can be registered in alsa. */
	ret = snd_card_new(&intf->dev, index[idx], id[idx], THIS_MODULE,
			   sizeof(*chip), &card);
	if (ret < 0) {
		dev_err(&device->dev, "cannot create alsa card.\n");
		return ret;
	}

	strscpy(card->driver, DRIVER_NAME, sizeof(card->driver));

	if (quirk && quirk->device_name)
		strscpy(card->shortname, quirk->device_name, sizeof(card->shortname));
	else
		strscpy(card->shortname, "Zoom generic audio", sizeof(card->shortname));

	strlcat(card->longname, card->shortname, sizeof(card->longname));
	len = strlcat(card->longname, " at ", sizeof(card->longname));
	if (len < sizeof(card->longname))
		usb_make_path(device, card->longname + len,
			      sizeof(card->longname) - len);

	chip = card->private_data;
	chip->dev = device;
	chip->card = card;

	*rchip = chip;
	return 0;
}

static int zoom_chip_probe(struct usb_interface *intf,
			     const struct usb_device_id *usb_id)
{
	const struct zoom_vendor_quirk *quirk = (struct zoom_vendor_quirk *)usb_id->driver_info;
	int ret;
	int i;
	struct zoom_chip *chip;
	struct usb_device *device = interface_to_usbdev(intf);
	u8 buffer[32]; //@TODO FIX buffer size!!!

	dev_info(&device->dev, "zoom chip probe\n");
#if 0
	/* Initialize device / @TODO verify samplerate */
	ret = usb_control_msg_recv(device, 0, 1, 0xa1, 256, 10240,
			buffer, 32, 1000, GFP_KERNEL);
	dev_info(&device->dev, "zoom chip CT1: %d\n", ret);
	ret = usb_control_msg_recv(device, 0, 2, 0xa1, 256, 10240,
			buffer, 32, 1000, GFP_KERNEL);
	dev_info(&device->dev, "zoom chip CT2: %d\n", ret);
#endif

	/* check whether the card is already registered */
	chip = NULL;
	mutex_lock(&register_mutex);

	for (i = 0; i < SNDRV_CARDS; i++)
		if (enable[i])
			break;

	if (i >= SNDRV_CARDS) {
		dev_err(&device->dev, "no available " CARD_NAME " audio device\n");
		ret = -ENODEV;
		goto err;
	}

	ret = zoom_chip_create(intf, device, i, quirk, &chip);
	if (ret < 0) {
		dev_err(&device->dev, "zoom_chip_create\n");
		goto err;
	}

	ret = zoom_pcm_init(chip, 0);
	if (ret < 0) {
		dev_err(&device->dev, "zoom_pcm_init\n");
		goto err_chip_destroy;
	}

	ret = snd_card_register(chip->card);
	if (ret < 0) {
		dev_err(&device->dev, "cannot register " CARD_NAME " card\n");
		goto err_chip_destroy;
	}

	mutex_unlock(&register_mutex);

	usb_set_intfdata(intf, chip);
	return 0;

err_chip_destroy:
	snd_card_free(chip->card);
err:
	mutex_unlock(&register_mutex);
	return ret;
}

static void zoom_chip_disconnect(struct usb_interface *intf)
{
	struct zoom_chip *chip;
	struct snd_card *card;

	chip = usb_get_intfdata(intf);
	if (!chip)
		return;

	card = chip->card;

	/* Make sure that the userspace cannot create new request */
	snd_card_disconnect(card);

	zoom_pcm_abort(chip);
	snd_card_free_when_closed(card);
}

static const struct usb_device_id device_table[] = {
	{
		USB_DEVICE_INTERFACE_NUMBER(0x1686, 0x0525, 2),
		.driver_info = (unsigned long)&(const struct zoom_vendor_quirk) {
			.device_name = "ZOOM L-8"
		}
	},
	{}
};

MODULE_DEVICE_TABLE(usb, device_table);

static struct usb_driver zoom_usb_driver = {
	.name = DRIVER_NAME,
	.probe = zoom_chip_probe,
	.disconnect = zoom_chip_disconnect,
	.id_table = device_table,
};

module_usb_driver(zoom_usb_driver);
