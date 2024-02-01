/*
 * ASoC SPRD sound card support
 *
 * SPDX-FileCopyrightText: 2015 Spreadtrum Communications (Shanghai) Co., Ltd
 * SPDX-FileCopyrightText: 2016 Unisoc (Shanghai) Technologies Co., Ltd
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */
#include "sprd-asoc-debug.h"
#define pr_fmt(fmt) pr_sprd_fmt("BOARD")""fmt

#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/kallsyms.h>

#include "sprd-asoc-card-utils.h"
#include "sprd-asoc-common.h"

// Add For FSM FS1815N
#ifdef CONFIG_SND_SOC_FS1815
extern void fsm_speaker_onn(void);
extern void fsm_speaker_off(void);
extern void fsm_init(void);
extern void fsm_set_scene(int scene);
extern int fsm_dev_count(void);
#endif

struct sprd_asoc_ext_hook_map {
	const char *name;
	sprd_asoc_hook_func hook;
	int en_level;
};

enum {
	/* ext_ctrl_type */
	CELL_CTRL_TYPE,
	/* pa type select */
	CELL_HOOK,
	/* select mode */
	CELL_PRIV,
	/* share gpio with  */
	CELL_SHARE_GPIO,
	CELL_NUMBER,
};

/* audio_sense begin */
enum {
	AUDIO_SENSE_CLOSE = -1,
	AUDIO_SENSE_MUSIC = 0,
	AUDIO_SENSE_VOICE,
	AUDIO_SENSE_VOIP,
	AUDIO_SENSE_FM,
	AUDIO_SENSE_RCV,
	AUDIO_SENSE_UP_SPK_BYPASS,
	AUDIO_SENSE_DOWN_SPK_BYPASS,
	AUDIO_SENSE_MAX,
};

const char * const audio_sense_texts[] = {
	"Music",
	"Voice",
	"Voip",
	"Fm",
	"Receiver",
	"UpSpkBypass",
	"DownSpkBypass",
};

const struct soc_enum audio_sense_enum =
	SOC_ENUM_SINGLE_VIRT(ARRAY_SIZE(audio_sense_texts),
			audio_sense_texts);
/* audio_sense end */
enum {
	LEFT_CHANNEL = 0,
	RIRHT_CHANNEL = 1,
};

struct sprd_asoc_hook_spk_priv {
	int gpio[BOARD_FUNC_MAX];
	int priv_data[BOARD_FUNC_MAX];
	bool gpio_requested[BOARD_FUNC_MAX];
	int state[BOARD_FUNC_MAX];
	int rcv_shared_multi_spk;
	int hook_audio_sense[BOARD_FUNC_MAX];	// record what audio sense is applied in every hook
	int audio_sense;			// audio_sense set by hal
	spinlock_t lock;
};

static struct sprd_asoc_hook_spk_priv hook_spk_priv;

#define GENERAL_SPK_MODE 10

#define EN_LEVEL 1

static int select_mode;
static u32 extral_iic_pa_en;

static ssize_t select_mode_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buff)
{
	return sprintf(buff, "%d\n", select_mode);
}

static ssize_t select_mode_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buff, size_t len)
{
	unsigned long level;
	int ret;


	ret = kstrtoul(buff, 10, &level);
	if (ret) {
		pr_err("%s kstrtoul failed!(%d)\n", __func__, ret);
		return len;
	}
	select_mode = level;
	pr_info("speaker ext pa select_mode = %d\n", select_mode);

	return len;
}

#ifdef CONFIG_SND_SOC_FS1815
static ssize_t pa_info_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buff)
{
	return sprintf(buff, "%s\n", fsm_dev_count() <= 0 ? "unknown" : "fs1815");
}

static ssize_t fsm_init_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buff, size_t len)
{
	pr_info("%s enter\n", __func__);
	fsm_init();
	return len;
}

static ssize_t fsm_scene_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buff, size_t len)
{
	unsigned long scene;
	int ret;

	ret = kstrtoul(buff, 10, &scene);
	if (ret) {
		pr_err("%s kstrtoul failed!(%d)\n", __func__, ret);
		return len;
	}

	pr_info("%s set_scene: %ul\n", scene);
	fsm_set_scene((int)scene);

	return len;
}
#endif

static int ext_debug_sysfs_init(void)
{
	int ret;
	static struct kobject *ext_debug_kobj;
	static struct kobj_attribute ext_debug_attr =
		__ATTR(select_mode, 0644,
		select_mode_show,
		select_mode_store);

#ifdef CONFIG_SND_SOC_FS1815
	static struct kobj_attribute ext_info_attr =
		__ATTR(pa_info, 0644,
		pa_info_show,
		NULL);

	static struct kobj_attribute fsm_init_attr =
		__ATTR(fsm_init, 0644,
		NULL,
		fsm_init_store);

	static struct kobj_attribute fsm_scene_attr =
		__ATTR(fsm_scene, 0644,
		NULL,
		fsm_scene_store);
#endif

	if (ext_debug_kobj)
		return 0;
	ext_debug_kobj = kobject_create_and_add("extpa", kernel_kobj);
	if (ext_debug_kobj == NULL) {
		ret = -ENOMEM;
		pr_err("register sysfs failed. ret = %d\n", ret);
		return ret;
	}

	ret = sysfs_create_file(ext_debug_kobj, &ext_debug_attr.attr);
	if (ret) {
		pr_err("create sysfs failed. ret = %d\n", ret);
		return ret;
	}

#ifdef CONFIG_SND_SOC_FS1815
	ret = sysfs_create_file(ext_debug_kobj, &ext_info_attr.attr);
	if (ret) {
		pr_err("create sysfs failed. ret = %d\n", ret);
		return ret;
	}

	ret = sysfs_create_file(ext_debug_kobj, &fsm_init_attr.attr);
	if (ret) {
		pr_err("create fsm_init sysfs failed. ret = %d\n", ret);
		return ret;
	}

	ret = sysfs_create_file(ext_debug_kobj, &fsm_scene_attr.attr);
	if (ret) {
		pr_err("create fsm_scene sysfs failed. ret = %d\n", ret);
		return ret;
	}
#endif

	return ret;
}

static void hook_gpio_pulse_control(unsigned int gpio, unsigned int mode)
{
	int i = 1;
	spinlock_t *lock = &hook_spk_priv.lock;
	unsigned long flags;

	spin_lock_irqsave(lock, flags);
	for (i = 1; i < mode; i++) {
		gpio_set_value(gpio, EN_LEVEL);
		udelay(2);
		gpio_set_value(gpio, !EN_LEVEL);
		udelay(2);
	}

	gpio_set_value(gpio, EN_LEVEL);
	spin_unlock_irqrestore(lock, flags);
}

static int hook_general_spk(int id, int on)
{
	int gpio, mode;
#if 0
	sp_asoc_pr_info("%s enter\n", __func__);
	if (extral_iic_pa_en == 1) {
		static int (*extral_i2c_pa_function)(int);

		extral_i2c_pa_function = (void *)kallsyms_lookup_name("aw87xxx_i2c_pa");
		if (!extral_i2c_pa_function) {
			sp_asoc_pr_info("%s extral_i2c_pa is not prepare\n", __func__);
		} else {
			sp_asoc_pr_info("%s extral_i2c_pa, on %d\n", __func__, on);
			extral_i2c_pa_function(on);
		}
		return HOOK_OK;
	} else if (extral_iic_pa_en == 2) {
		static int (*extral_i2c_pa_function)(int, int);

		extral_i2c_pa_function =
			(void *)kallsyms_lookup_name("aw87xxx_audio_scene_load");
		if (!extral_i2c_pa_function) {
			sp_asoc_pr_info("%s extral_i2c_pa is not prepare\n", __func__);
		} else {
			sp_asoc_pr_info("%s extral_i2c_pa, on %d\n", __func__, on);
			extral_i2c_pa_function(on, LEFT_CHANNEL);
			extral_i2c_pa_function(on, RIRHT_CHANNEL);
		}
		return HOOK_OK;
	}
#endif
	gpio = hook_spk_priv.gpio[id];
	if (gpio < 0) {
		pr_err("%s gpio is invalid!\n", __func__);
		return -EINVAL;
	}
	mode = hook_spk_priv.priv_data[id];
	if (mode > GENERAL_SPK_MODE)
		mode = 0;
	pr_info("%s id: %d, gpio: %d, mode: %d, on: %d\n",
		 __func__, id, gpio, mode, on);

	/* Off */
	if (!on) {
		gpio_set_value(gpio, !EN_LEVEL);
		/* aviod pulses less than 1ms */
		msleep(2);
		return HOOK_OK;
	}

	/* On */
	if (select_mode) {
		mode = select_mode;
		pr_info("%s mode: %d, select_mode: %d\n",
			__func__, mode, select_mode);
	}
	hook_gpio_pulse_control(gpio, mode);

	/* When the first time open speaker path and play a very short sound,
	 * the sound can't be heard. So add a delay here to make sure the AMP
	 * is ready.
	 */
	msleep(22);

	return HOOK_OK;
}

/* audio_sense begin */
void audio_sense_put(int audio_sense)
{
	if(AUDIO_SENSE_MUSIC > audio_sense || AUDIO_SENSE_MAX <= audio_sense) {
		audio_sense = AUDIO_SENSE_MUSIC;
	}
	sp_asoc_pr_info("%s put %d\n", __func__, audio_sense);
	hook_spk_priv.audio_sense = audio_sense;
}

int audio_sense_get(void)
{
	return hook_spk_priv.audio_sense;
}

int audio_sense_need_update(int id)
{
	return (hook_spk_priv.audio_sense != hook_spk_priv.hook_audio_sense[id]) &&
		(AUDIO_SENSE_CLOSE != hook_spk_priv.hook_audio_sense[id]);
}
/* audio_sense end */

#ifdef CONFIG_SND_SOC_FS1815
static void set_fs1815_scene(int audio_sense)
{
	pr_info("%s audio_sense: %d\n", __func__, audio_sense);
	switch (audio_sense) {
		case AUDIO_SENSE_MUSIC:
			fsm_set_scene(0);
			break;
		case AUDIO_SENSE_VOICE:
			fsm_set_scene(1);
			break;
		case AUDIO_SENSE_VOIP:
			fsm_set_scene(2);
			break;
		case AUDIO_SENSE_FM:
			fsm_set_scene(0);
			break;
		case AUDIO_SENSE_RCV:
			fsm_set_scene(15);
			break;
		case AUDIO_SENSE_UP_SPK_BYPASS:
			fsm_set_scene(7);
			break;
		case AUDIO_SENSE_DOWN_SPK_BYPASS:
			fsm_set_scene(13);
			break;
		default:
			fsm_set_scene(0);
	};
}

static int hook_spk_i2c_fs1815(int id, int on)
{
	int mode;
	int audio_sense = hook_spk_priv.audio_sense;

	mode = hook_spk_priv.priv_data[id];
	if (mode > GENERAL_SPK_MODE)
		mode = 0;
	pr_info("%s id: %d, mode: %d, on: %d\n", __func__, id, mode, on);

	if (on) {
		if (select_mode) {
			mode = select_mode;
			pr_info("%s mode: %d, select_mode: %d\n", __func__, mode, select_mode);
		}
		hook_spk_priv.state[id] = mode;
		if ((id == 2) && (audio_sense==AUDIO_SENSE_VOIP  || audio_sense==AUDIO_SENSE_VOICE)) {
			hook_spk_priv.hook_audio_sense[id] =AUDIO_SENSE_RCV;
			set_fs1815_scene(AUDIO_SENSE_RCV);
		} else if ((id == 0 || id == 1) && (audio_sense==AUDIO_SENSE_VOIP || audio_sense==AUDIO_SENSE_VOICE)) {
			hook_spk_priv.hook_audio_sense[id] =audio_sense;
			set_fs1815_scene(AUDIO_SENSE_VOICE);
		} else {
			hook_spk_priv.hook_audio_sense[id] = audio_sense;
			set_fs1815_scene(audio_sense);
		}
		//enable PA
		fsm_speaker_onn();
	} else {
		//disable PA
		fsm_speaker_off();
		hook_spk_priv.state[id] = 0;
		hook_spk_priv.hook_audio_sense[id] = AUDIO_SENSE_CLOSE;
		set_fs1815_scene(AUDIO_SENSE_CLOSE);
		msleep(1); // avoid handset/handsfree switched to fast
	}
	pr_info("%s id=%d  on=%d  hook state {%d, %d, %d} audio_sense: %s", __func__, id, on,
			hook_spk_priv.state[0], hook_spk_priv.state[1], hook_spk_priv.state[2],
			(hook_spk_priv.hook_audio_sense[id] > AUDIO_SENSE_CLOSE && hook_spk_priv.hook_audio_sense[id] < AUDIO_SENSE_MAX) ?
			audio_sense_texts[hook_spk_priv.hook_audio_sense[id]] : "NULL");
	return HOOK_OK;
}

static int hook_spk_i2c(int id, int on)
{
	// BOARD_FUNC_EAR only shared with BOARD_FUNC_SPK
	// if BOARD_FUNC_EAR is shared with both BOARD_FUNC_SPK1 and BOARD_FUNC_SPK,
	// SPK1 should be processed by SPK or EAR("speaker1 function" will be invalid)
	// SPK1 mode is default(dts) when spk on; same with ear mode when ear real on
	// dts: sprd,rcv_shared_multi_spk;
	if (hook_spk_priv.rcv_shared_multi_spk && BOARD_FUNC_SPK1 == id) {
		return HOOK_OK;
	}
	pr_info("%s id: %d, on: %d\n", __func__, id, on);
	switch (id) {
		case BOARD_FUNC_SPK:
			// when spk on, if ear is real on: ear switch to virtual on
			if (on && 0 < hook_spk_priv.state[BOARD_FUNC_EAR]) {
				hook_spk_i2c_fs1815(BOARD_FUNC_EAR, 0);
				if (hook_spk_priv.rcv_shared_multi_spk) {
					select_mode = 0;
					hook_spk_i2c_fs1815(BOARD_FUNC_SPK1, 0);
				}
				hook_spk_priv.state[BOARD_FUNC_EAR] = -1; // ear virtual on
				//hook_spk_priv.hook_audio_sense[BOARD_FUNC_EAR] = audio_sense;
			}
			// spk on/off
			hook_spk_i2c_fs1815(id, on);
			if (hook_spk_priv.rcv_shared_multi_spk) {
				select_mode = 0;
				hook_spk_i2c_fs1815(BOARD_FUNC_SPK1, on);
			}
			// when spk off, if ear is virtual on: ear switch to real on
			if (!on && -1 == hook_spk_priv.state[BOARD_FUNC_EAR]) {
				hook_spk_i2c_fs1815(BOARD_FUNC_EAR, 1); // ear real on
				if (hook_spk_priv.rcv_shared_multi_spk) {
					select_mode = hook_spk_priv.priv_data[BOARD_FUNC_EAR];
					hook_spk_i2c_fs1815(BOARD_FUNC_SPK1, 1);
					select_mode = 0;
				}
			}
			break;
		case BOARD_FUNC_EAR:
			// when ear on/off, if spk is on: ear virtual on/off; else ear real on/off
			if (0 < hook_spk_priv.state[BOARD_FUNC_SPK]) {
				hook_spk_priv.state[BOARD_FUNC_EAR] = ((on > 0) ? -1:0); // ear virtual on/off
				//hook_spk_priv.hook_audio_sense[BOARD_FUNC_EAR] = ((on > 0) ? audio_sense : AUDIO_SENSE_CLOSE);
			} else {
				hook_spk_i2c_fs1815(BOARD_FUNC_EAR, on); // ear real on/off
				if (hook_spk_priv.rcv_shared_multi_spk) {
					select_mode = ((on > 0) ? hook_spk_priv.priv_data[BOARD_FUNC_EAR]:0);
					hook_spk_i2c_fs1815(BOARD_FUNC_SPK1, on);
					select_mode = 0;
				}
			}
			break;
		default:
			hook_spk_i2c_fs1815(id, on);
			break;
	}
	pr_info("%s id=%d  on=%d  hook state {%d, %d, %d}", __func__, id, on,
			hook_spk_priv.state[0], hook_spk_priv.state[1], hook_spk_priv.state[2]);
	pr_info("%s hook sense {%d, %d, %d}", __func__,
			hook_spk_priv.hook_audio_sense[0],
			hook_spk_priv.hook_audio_sense[1],
			hook_spk_priv.hook_audio_sense[2]);
	return HOOK_OK;
}
#endif

static struct sprd_asoc_ext_hook_map ext_hook_arr[] = {
	{"general_speaker", hook_general_spk, EN_LEVEL},
#ifdef CONFIG_SND_SOC_FS1815
	{"i2c_speaker", hook_spk_i2c, EN_LEVEL},
#endif
};

static int sprd_asoc_card_parse_hook(struct device *dev,
					 struct sprd_asoc_ext_hook *ext_hook)
{
	struct device_node *np = dev->of_node;
	const char *prop_pa_info = "sprd,spk-ext-pa-info";
	const char *prop_pa_gpio = "sprd,spk-ext-pa-gpio";
	const char *extral_iic_pa_info = "extral-iic-pa";
	int spk_cnt, elem_cnt, i;
	int ret = 0;
	unsigned long gpio_flag;
	unsigned int ext_ctrl_type, share_gpio, hook_sel, priv_data;
	u32 *buf;
	u32 extral_iic_pa = 0;

	// init audio sense
	hook_spk_priv.audio_sense = AUDIO_SENSE_MUSIC;
	for (i = 0; i < BOARD_FUNC_MAX; i++) {
		hook_spk_priv.hook_audio_sense[i] = AUDIO_SENSE_CLOSE;
	}

	ret = of_property_read_u32(np, extral_iic_pa_info, &extral_iic_pa);
	if (!ret) {
		sp_asoc_pr_info("%s hook aw87xx iic pa!\n", __func__);
		extral_iic_pa_en = extral_iic_pa;
		ext_hook->ext_ctrl[BOARD_FUNC_SPK] = ext_hook_arr[BOARD_FUNC_SPK].hook;
		return 0;
	}

	elem_cnt = of_property_count_u32_elems(np, prop_pa_info);
	if (elem_cnt <= 0) {
		dev_info(dev,
			"Count '%s' failed!(%d)\n", prop_pa_info, elem_cnt);
		return -EINVAL;
	}

	if (elem_cnt % CELL_NUMBER) {
		dev_err(dev, "Spk pa info is not a multiple of %d.\n",
			CELL_NUMBER);
		return -EINVAL;
	}

	spk_cnt = elem_cnt / CELL_NUMBER;
	if (spk_cnt > BOARD_FUNC_MAX) {
		dev_warn(dev, "Speaker count %d is greater than %d!\n",
			 spk_cnt, BOARD_FUNC_MAX);
		spk_cnt = BOARD_FUNC_MAX;
	}

	spin_lock_init(&hook_spk_priv.lock);

	buf = devm_kmalloc(dev, elem_cnt * sizeof(u32), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = of_property_read_u32_array(np, prop_pa_info, buf, elem_cnt);
	if (ret < 0) {
		dev_err(dev, "Read property '%s' failed!\n", prop_pa_info);
		//return ret;
	}

	if (of_property_read_bool(np, "sprd,rcv_shared_multi_spk"))
		hook_spk_priv.rcv_shared_multi_spk = 1;
	else
		hook_spk_priv.rcv_shared_multi_spk = 0;
	dev_info(dev, "[%s] rcv shared with multi spk: %d\n", __func__, hook_spk_priv.rcv_shared_multi_spk);

	for (i = 0; i < spk_cnt; i++) {
		int num = i * CELL_NUMBER;

		/* Get the ctrl type */
		ext_ctrl_type = buf[CELL_CTRL_TYPE + num];
		if (ext_ctrl_type >= BOARD_FUNC_MAX) {
			dev_err(dev, "Ext ctrl type %d is invalid!\n",
				ext_ctrl_type);
			return -EINVAL;
		}

		/* Get the selection of hook function */
		hook_sel = buf[CELL_HOOK + num];
		if (hook_sel >= ARRAY_SIZE(ext_hook_arr)) {
			dev_err(dev,
				"Hook selection %d is invalid!\n", hook_sel);
			return -EINVAL;
		}
		ext_hook->ext_ctrl[ext_ctrl_type] = ext_hook_arr[hook_sel].hook;

		/* Get the private data */
		priv_data = buf[CELL_PRIV + num];
		hook_spk_priv.priv_data[ext_ctrl_type] = priv_data;

#ifdef CONFIG_SND_SOC_FS1815
		if (1 == hook_sel) {
			dev_warn(dev, "%s pa use i2c\n", __func__);
			continue;
		}
#endif

		/* Process the shared gpio */
		share_gpio = buf[CELL_SHARE_GPIO + num];
		if (share_gpio > 0) {
			if (share_gpio > spk_cnt) {
				dev_err(dev, "share_gpio %d is bigger than spk_cnt!\n",
					share_gpio);
				ext_hook->ext_ctrl[ext_ctrl_type] = NULL;
				return -EINVAL;
			}
			hook_spk_priv.gpio[ext_ctrl_type] =
				hook_spk_priv.gpio[share_gpio - 1];
			continue;
		}

		ret = of_get_named_gpio_flags(np, prop_pa_gpio, i, NULL);
		if (ret < 0) {
			dev_err(dev, "Get gpio failed:%d!\n", ret);
			ext_hook->ext_ctrl[ext_ctrl_type] = NULL;
			return ret;
		}
		hook_spk_priv.gpio[ext_ctrl_type] = ret;

		pr_info("ext_ctrl_type %d hook_sel %d priv_data %d gpio %d",
			ext_ctrl_type, hook_sel, priv_data, ret);

		gpio_flag = GPIOF_DIR_OUT;
		gpio_flag |= ext_hook_arr[hook_sel].en_level ?
			GPIOF_INIT_HIGH : GPIOF_INIT_LOW;
		ret = gpio_request_one(hook_spk_priv.gpio[ext_ctrl_type],
				       gpio_flag,  "audio:pa_ctrl");
		dev_info(dev, "Gpio request[%d] ret:%d! hook_p = %p, ext_ctrl_p = %p \n",
			ext_ctrl_type, ret, ext_hook, ext_hook->ext_ctrl[ext_ctrl_type]);
		if (ret == 0) {
			hook_spk_priv.gpio_requested[ext_ctrl_type] = true;
		} else if (ret < 0) {
			dev_err(dev, "Gpio request[%d] failed:%d!\n",
				ext_ctrl_type, ret);
			hook_spk_priv.gpio_requested[ext_ctrl_type] = false;
			ext_hook->ext_ctrl[ext_ctrl_type] = NULL;
			return ret;
		}
	}

	return 0;
}
int sprd_asoc_card_parse_ext_hook(struct device *dev,
				  struct sprd_asoc_ext_hook *ext_hook)
{
	ext_debug_sysfs_init();
	return sprd_asoc_card_parse_hook(dev, ext_hook);
}

int sprd_asoc_card_ext_hook_free_gpio(struct device *dev)
{
	struct device_node *np = dev->of_node;
	const char *prop_pa_info = "sprd,spk-ext-pa-info";
	int spk_cnt, elem_cnt, i;
	unsigned int ext_ctrl_type;
	int ret = 0;
	u32 *buf;
	bool is_freed = false;

	elem_cnt = of_property_count_u32_elems(np, prop_pa_info);
	if (elem_cnt <= 0) {
		dev_info(dev,
			"Count '%s' failed!(%d)\n", prop_pa_info, elem_cnt);
		return -EINVAL;
	}

	if (elem_cnt % CELL_NUMBER) {
		dev_err(dev, "Spk pa info is not a multiple of %d.\n",
			CELL_NUMBER);
		return -EINVAL;
	}

	spk_cnt = elem_cnt / CELL_NUMBER;
	if (spk_cnt > BOARD_FUNC_MAX) {
		dev_warn(dev, "Speaker count %d is greater than %d!\n",
			 spk_cnt, BOARD_FUNC_MAX);
		spk_cnt = BOARD_FUNC_MAX;
	}

	buf = devm_kmalloc(dev, elem_cnt * sizeof(u32), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = of_property_read_u32_array(np, prop_pa_info, buf, elem_cnt);
	if (ret < 0) {
		dev_err(dev, "Read property '%s' failed!\n", prop_pa_info);
		//return ret;
	}

	for (i = 0; i < spk_cnt; i++) {
		int num = i * CELL_NUMBER;

		/* Get the ctrl type */
		ext_ctrl_type = buf[CELL_CTRL_TYPE + num];
		if (ext_ctrl_type >= BOARD_FUNC_MAX) {
			dev_err(dev, "Ext ctrl type %d is invalid!\n",
				ext_ctrl_type);
			return -EINVAL;
		}

		pr_info("%s, spk cnt = %d, ext_ctrl_type = %d\n", __func__, spk_cnt, ext_ctrl_type);

		if (hook_spk_priv.gpio_requested[ext_ctrl_type]) {
			gpio_free(hook_spk_priv.gpio[ext_ctrl_type]);
			hook_spk_priv.gpio_requested[ext_ctrl_type] = false;
			pr_info("%s, gpio_freed\n", __func__);
			is_freed = true;
		}
	}

	if (is_freed) {
		pr_info("%s, delay 10ms after gpio freed\n", __func__);
		msleep(10);
        }

	return 0;
}

MODULE_ALIAS("platform:asoc-sprd-card");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ASoC SPRD Sound Card Utils - Hooks");
MODULE_AUTHOR("Peng Lee <peng.lee@spreadtrum.com>");
