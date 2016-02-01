/*
 *  Arcade Joystick Driver for RaspberryPi
 *
 *  Copyright (c) 2014 Matthieu Proucelle
 *
 *  With tweaks by Temia Eszteri
 *
 *  Based on the gamecon driver by Vojtech Pavlik, and Markus Hiienkari
 */


/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include <linux/ioport.h>
#include <asm/io.h>


MODULE_AUTHOR("Matthieu Proucelle & Temia Eszteri");
MODULE_DESCRIPTION("GPIO Arcade Joystick Driver");
MODULE_LICENSE("GPL");

#define MK_MAX_DEVICES		2

#ifdef RPI2
#define PERI_BASE        0x3F000000
#else
#define PERI_BASE        0x20000000
#endif

#define GPIO_BASE                (PERI_BASE + 0x200000) /* GPIO controller */

#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define GPIO_READ(g)  *(gpio + 13) &= (1<<(g))

#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET *(gpio+7)
#define GPIO_CLR *(gpio+10)

static volatile unsigned *gpio;

struct mk_config {
    int args[MK_MAX_DEVICES];
    unsigned int nargs;
};

static struct mk_config mk_cfg __initdata;

module_param_array_named(map, mk_cfg.args, int, &(mk_cfg.nargs), 0);
MODULE_PARM_DESC(map, "Enable or disable GPIO Arcade Joystick");

enum mk_type {
    MK_NONE = 0,
    MK_ARCADE_GPIO,
    MK_MAX
};

#define MK_REFRESH_TIME	HZ/100

struct mk_pad {
    struct input_dev *dev;
    enum mk_type type;
    char phys[32];
    int mcp23017addr;
};

struct mk {
    struct mk_pad pads[MK_MAX_DEVICES];
    struct timer_list timer;
    int pad_count[MK_MAX];
    int used;
    struct mutex mutex;
};

static struct mk *mk_base;

static const int mk_max_arcade_buttons = 14;

// Map of the gpios :                     u,  d,  l,  r,  str, sct, a, b, tr, y,  x,  tl, tr2, tl2
static const int mk_arcade_gpio_maps[] = {26, 20, 16, 21, 22,  27,  5, 6, 23, 19, 13, 18, 24,  17};

static const short mk_arcade_gpio_btn[] = {
    BTN_START, BTN_SELECT, BTN_A, BTN_B, BTN_TR, BTN_Y, BTN_X, BTN_TL, BTN_TR2, BTN_TL2
};

static const char *mk_names[] = {
    NULL, "GPIO Controller 1"
};

/* GPIO UTILS */
static void setGpioPullUps(int pullUps) {
    *(gpio + 37) = 0x02;
    udelay(10);
    *(gpio + 38) = pullUps;
    udelay(10);
    *(gpio + 37) = 0x00;
    *(gpio + 38) = 0x00;
}

static void setGpioAsInput(int gpioNum) {
    INP_GPIO(gpioNum);
}

/*---------------------------------------------------------------------------*/

static void mk_gpio_read_packet(struct mk_pad * pad, unsigned char *data) {
    int i;
    // Duplicate code to avoid the if at each loop
    if (pad->type == MK_ARCADE_GPIO) {
        for (i = 0; i < mk_max_arcade_buttons; i++) {
            int read = GPIO_READ(mk_arcade_gpio_maps[i]);
            if (read == 0) data[i] = 1;
            else data[i] = 0;
        }
    }
}

static void mk_input_report(struct mk_pad * pad, unsigned char * data) {
    struct input_dev * dev = pad->dev;
    int j;
    input_report_abs(dev, ABS_Y, !data[0]-!data[1]);
    input_report_abs(dev, ABS_X, !data[2]-!data[3]);
    for (j = 4; j < mk_max_arcade_buttons; j++) {
        input_report_key(dev, mk_arcade_gpio_btn[j - 4], data[j]);
    }
    input_sync(dev);
}

static void mk_process_packet(struct mk *mk) {

    unsigned char data[mk_max_arcade_buttons];
    struct mk_pad *pad;
    int i;

    for (i = 0; i < MK_MAX_DEVICES; i++) {
        pad = &mk->pads[i];
        if (pad->type == MK_ARCADE_GPIO) {
            mk_gpio_read_packet(pad, data);
            mk_input_report(pad, data);
        }
    }

}

/*
 * mk_timer() initiates reads of console pads data.
 */

static void mk_timer(unsigned long private) {
    struct mk *mk = (void *) private;
    mk_process_packet(mk);
    mod_timer(&mk->timer, jiffies + MK_REFRESH_TIME);
}

static int mk_open(struct input_dev *dev) {
    struct mk *mk = input_get_drvdata(dev);
    int err;

    err = mutex_lock_interruptible(&mk->mutex);
    if (err)
        return err;

    if (!mk->used++)
        mod_timer(&mk->timer, jiffies + MK_REFRESH_TIME);

    mutex_unlock(&mk->mutex);
    return 0;
}

static void mk_close(struct input_dev *dev) {
    struct mk *mk = input_get_drvdata(dev);

    mutex_lock(&mk->mutex);
    if (!--mk->used) {
        del_timer_sync(&mk->timer);
    }
    mutex_unlock(&mk->mutex);
}

static int __init mk_setup_pad(struct mk *mk, int idx, int pad_type_arg) {
    struct mk_pad *pad = &mk->pads[idx];
    struct input_dev *input_dev;
    int i, pad_type;
    int err;
    char FF = 0xFF;
    pr_err("pad type : %d\n",pad_type_arg);

    if (pad_type_arg == MK_ARCADE_GPIO) {
        pad_type = MK_ARCADE_GPIO;
    }
    if (pad_type < 1) {
        pr_err("Pad type %d unknown\n", pad_type);
        return -EINVAL;
    }
    pr_err("pad type : %d\n",pad_type);
    pad->dev = input_dev = input_allocate_device();
    if (!input_dev) {
        pr_err("Not enough memory for input device\n");
        return -ENOMEM;
    }

    pad->type = pad_type;
    pad->mcp23017addr = pad_type_arg;
    snprintf(pad->phys, sizeof (pad->phys),
            "input%d", idx);

    input_dev->name = mk_names[pad_type];
    input_dev->phys = pad->phys;
    input_dev->id.bustype = BUS_PARPORT;
    input_dev->id.vendor = 0x0001;
    input_dev->id.product = pad_type;
    input_dev->id.version = 0x0100;

    input_set_drvdata(input_dev, mk);

    input_dev->open = mk_open;
    input_dev->close = mk_close;

    input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);

    for (i = 0; i < 2; i++)
        input_set_abs_params(input_dev, ABS_X + i, -1, 1, 0, 0);
    for (i = 0; i < mk_max_arcade_buttons - 4; i++)
        __set_bit(mk_arcade_gpio_btn[i], input_dev->keybit);

    mk->pad_count[pad_type]++;

    switch (pad_type) {
        case MK_ARCADE_GPIO:
            for (i = 0; i < mk_max_arcade_buttons; i++) {
                setGpioAsInput(mk_arcade_gpio_maps[i]);
            }
            setGpioPullUps(0x2df72060); // really, this should be generated on the fly
            printk("GPIO configured for pad%d\n", idx);
            break;
    }

    err = input_register_device(pad->dev);
    if (err)
        goto err_free_dev;

    return 0;

err_free_dev:
    input_free_device(pad->dev);
    pad->dev = NULL;
    return err;
}

static struct mk __init *mk_probe(int *pads, int n_pads) {
    struct mk *mk;
    int i;
    int count = 0;
    int err;

    mk = kzalloc(sizeof (struct mk), GFP_KERNEL);
    if (!mk) {
        pr_err("Not enough memory\n");
        err = -ENOMEM;
        goto err_out;
    }

    mutex_init(&mk->mutex);
    setup_timer(&mk->timer, mk_timer, (long) mk);

    for (i = 0; i < n_pads && i < MK_MAX_DEVICES; i++) {
        if (!pads[i])
            continue;

        err = mk_setup_pad(mk, i, pads[i]);
        if (err)
            goto err_unreg_devs;

        count++;
    }

    if (count == 0) {
        pr_err("No valid devices specified\n");
        err = -EINVAL;
        goto err_free_mk;
    }

    return mk;

err_unreg_devs:
    while (--i >= 0)
        if (mk->pads[i].dev)
            input_unregister_device(mk->pads[i].dev);
err_free_mk:
    kfree(mk);
err_out:
    return ERR_PTR(err);
}

static void mk_remove(struct mk *mk) {
    int i;

    for (i = 0; i < MK_MAX_DEVICES; i++)
        if (mk->pads[i].dev)
            input_unregister_device(mk->pads[i].dev);
    kfree(mk);
}

static int __init mk_init(void) {
    /* Set up gpio pointer for direct register access */
    if ((gpio = ioremap(GPIO_BASE, 0xB0)) == NULL) {
        pr_err("io remap failed\n");
        return -EBUSY;
    }
    if (mk_cfg.nargs < 1) {
        pr_err("at least one device must be specified\n");
        return -EINVAL;
    } else {
        mk_base = mk_probe(mk_cfg.args, mk_cfg.nargs);
        if (IS_ERR(mk_base))
            return -ENODEV;
    }
    return 0;
}

static void __exit mk_exit(void) {
    if (mk_base)
        mk_remove(mk_base);

    iounmap(gpio);
}

module_init(mk_init);
module_exit(mk_exit);
