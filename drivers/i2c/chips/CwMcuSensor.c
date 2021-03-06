/* CwMcuSensor.c - driver file for HTC SensorHUB
 *
 * Copyright (C) 2014 HTC Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/vibtrig.h>

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>

#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/CwMcuSensor.h>
#include <linux/gpio.h>
#include <linux/wakelock.h>
#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif
#include <linux/mutex.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

#include <linux/regulator/consumer.h>

#include <linux/firmware.h>

#include <linux/notifier.h>

#include <linux/sensor_hub.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/rtc.h>
#include <linux/ioctl.h>
#include <linux/shub_ctrl.h>
#include <linux/completion.h>
#include <asm/uaccess.h>
#include <linux/miscdevice.h>
 

#ifdef CONFIG_FB
#include <linux/notifier.h>
#include <linux/fb.h>
#endif 

#define D(x...) pr_debug("[S_HUB][CW_MCU] " x)
#define I(x...) pr_info("[S_HUB][CW_MCU] " x)
#define E(x...) pr_err("[S_HUB][CW_MCU] " x)

#define RETRY_TIMES 20
#define LATCH_TIMES  1
#define LATCH_ERROR_NO (-110)
#define ACTIVE_RETRY_TIMES 10
#define DPS_MAX			(1 << (16 - 1))

#define TOUCH_LOG_DELAY		5000
#define CWMCU_BATCH_TIMEOUT_MIN 200
#define MS_TO_PERIOD (1000 * 99 / 100)

#define rel_significant_motion REL_WHEEL

#define ACC_CALIBRATOR_LEN 3
#define ACC_CALIBRATOR_RL_LEN 12
#define MAG_CALIBRATOR_LEN 26
#define GYRO_CALIBRATOR_LEN 3
#define LIGHT_CALIBRATOR_LEN 4
#define PROXIMITY_CALIBRATOR_LEN 6
#define PRESSURE_CALIBRATOR_LEN 4

#define REPORT_EVENT_COMMON_LEN 3
#define REPORT_EVENT_PROXIMITY_LEN 7

#define FW_VER_INFO_LEN 31
#define FW_VER_HEADER_LEN 7
#define FW_VER_COUNT 6
#define FW_I2C_LEN_LIMIT 256

#define ENABLE_LIST_GROUP_NUM 4
#define REACTIVATE_PERIOD (10*HZ)
#define RESET_PERIOD (30*HZ)
#define SYNC_ACK_MAGIC  0x66
#define EXHAUSTED_MAGIC 0x77

#define CALIBRATION_DATA_PATH "/calibration_data"
#define G_SENSOR_FLASH_DATA "gs_flash"
#define GYRO_SENSOR_FLASH_DATA "gyro_flash"
#define LIGHT_SENSOR_FLASH_DATA "als_flash"
#define PROX_SENSOR_FLASH_DATA "ps_flash"
#define BARO_SENSOR_FLASH_DATA "bs_flash"

#define MCU_CHIP_MODE_APPLICATION   0
#define MCU_CHIP_MODE_BOOTLOADER    1

#define VIB_TIME 20

typedef enum {
    MCU_STATE_UNKNOWN = 0,
    MCU_STATE_SHUB_INIT,
    MCU_STATE_SHUB_RUN,
    MCU_STATE_DLOAD,
    MCU_STATE_BOOTLOADER,
    MCU_STATE_MAX,
} MCU_STATE;

static MCU_STATE s_mcu_state = MCU_STATE_UNKNOWN;
#define MCU_IN_UNKNOWN() (MCU_STATE_UNKNOWN == s_mcu_state)
#define MCU_IN_DLOAD() (MCU_STATE_DLOAD == s_mcu_state)
#define MCU_IN_SHUB() (MCU_STATE_SHUB_INIT == s_mcu_state || MCU_STATE_SHUB_RUN == s_mcu_state)
#define MCU_IN_SHUB_INIT() (MCU_STATE_SHUB_INIT == s_mcu_state)
#define MCU_IN_SHUB_RUN() (MCU_STATE_SHUB_RUN == s_mcu_state)
#define MCU_IN_BOOTLOADER() (MCU_STATE_BOOTLOADER == s_mcu_state)
static DEFINE_MUTEX(s_activated_i2c_lock);
static DECLARE_COMPLETION(s_mcu_enter_shub_run);

#ifdef SHUB_DLOAD_SUPPORT
#define MCU2CPU_STATUS_GPIO_LEVEL_SHUB 0
#define MCU2CPU_STATUS_GPIO_LEVEL_DLOAD 1
static DECLARE_COMPLETION(s_mcu_ramdump_avail);
static void mcu_enable_disable_dload_mode(bool en);
#endif 

#ifdef SHUB_LOGGING_SUPPORT
static DECLARE_COMPLETION(s_mcu_log_avail);
static void mcu_set_log_mask(u32 log_mask);
static void mcu_set_log_level(u32 log_level);
#endif 

#ifdef SHUB_EVENT_SUPPORT
static DECLARE_COMPLETION(s_mcu_event_avail);
#endif 

#ifdef CONFIG_CWSTM32_DEBUG  

static int DEBUG_FLAG_GSENSOR;
module_param(DEBUG_FLAG_GSENSOR, int, 0600);

#else

#define DEBUG_FLAG_GSENSOR 0

#endif

static int DEBUG_DISABLE;
module_param(DEBUG_DISABLE, int, 0660);
MODULE_PARM_DESC(DEBUG_DISABLE, "disable " CWMCU_I2C_NAME " driver") ;

static bool g_mcu_ready = 0;
static u8 g_touch_solution = 0xff;

static int p_status = 9;
static u32 adc_table[10] = {0};

struct cwmcu_data {
	struct i2c_client *client;
	atomic_t delay;
	struct mutex mutex_lock;
	struct mutex group_i2c_lock;
	struct mutex power_mode_lock;
	struct iio_trigger  *trig;
	atomic_t pseudo_irq_enable;
	struct mutex lock;

	struct timeval now;
	struct class *sensor_class;
	struct device *sensor_dev;
	u8	acceleration_axes;
	u8	magnetic_axes;
	u8	gyro_axes;

	u32	enabled_list;
	u32	batched_list;
	u32	batch_enabled;

	
	s64	sensors_time[num_sensors];
	s64	time_diff[num_sensors];
	s32	report_period[num_sensors]; 
	u32	update_list;
	s64	sensors_batch_timeout[num_sensors];
	s64	current_timeout;
	int	IRQ;
	struct delayed_work	work;
	struct work_struct	one_shot_work;
	bool w_activated_i2c;
	bool w_re_init;
	bool w_facedown_set;
	bool w_kick_start_mcu;
	bool w_mcu_state_change;

	bool suspended;
	bool probe_success;
	bool is_block_i2c;

	u32 gpio_wake_mcu;
	u32 gpio_reset;
	u32 gpio_chip_mode;
	int gpio_chip_mode_level;
	u32 gpio_mcu_irq;
	u32 gpio_mcu_status;
	int gpio_mcu_status_level;
	s32 gs_chip_layout;
	s32 touch_enable;
	u32 gs_kvalue;
	s16 gs_kvalue_R1;
	s16 gs_kvalue_R2;
	s16 gs_kvalue_R3;
	s16 gs_kvalue_L1;
	s16 gs_kvalue_L2;
	s16 gs_kvalue_L3;
	u32 gy_kvalue;
	u8  als_goldl;
	u8  als_goldh;
	u32 *als_levels;
	u32 als_kvalue;
        u32 ps_kvalue;
        u32 ps_kheader;
        u16 ps_thd_fixed;
        u16 ps_thd_add;
	u32 bs_kvalue;
	u8  bs_kheader;
	u8  gs_calibrated;
	u8  ps_calibrated;
	u8  ls_calibrated;
	u8  bs_calibrated;
	u8  gy_calibrated;

	s32 i2c_total_retry;
	s32 i2c_latch_retry;
	unsigned long i2c_jiffies;
	unsigned long reset_jiffies;

	int disable_access_count;

	s32 iio_data[6];
	struct iio_dev *indio_dev;

	
	int power_on_counter;

	s16 light_last_data[REPORT_EVENT_COMMON_LEN];
	u64 time_base;

	struct workqueue_struct *mcu_wq;
	struct wake_lock significant_wake_lock;
	struct wake_lock any_motion_wake_lock;

	int fw_update_status;

	bool mcu_bootup;
	bool mcu_sensor_ready;
	bool dload_mode_enabled;

	u32 gesture_motion_param;
	int power_key_pressed;

#ifdef SHUB_LOGGING_SUPPORT
	uint32_t mcu_log_mask;
	uint32_t mcu_log_level;
#endif 
	struct pinctrl *pinctrl;
	struct pinctrl_state *gpio_state_init;

#ifdef CONFIG_FB
	struct notifier_block fb_notif;
	struct delayed_work delay_work_register_fb;
	bool is_display_on;
#endif 
};

static struct cwmcu_data *s_mcu_data = NULL;
static struct vib_trigger *vib_trigger = NULL;
BLOCKING_NOTIFIER_HEAD(double_tap_notifier_list);

int register_notifier_by_facedown(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&double_tap_notifier_list, nb);
}
EXPORT_SYMBOL(register_notifier_by_facedown);

int unregister_notifier_by_facedown(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&double_tap_notifier_list,
						  nb);
}
EXPORT_SYMBOL(unregister_notifier_by_facedown);

static int CWMCU_i2c_read(struct cwmcu_data *mcu_data,
			u8 reg_addr, u8 *data, u8 len);
static int CWMCU_i2c_read_power(struct cwmcu_data *mcu_data,
			 u8 reg_addr, u8 *data, u8 len);
static int CWMCU_i2c_write(struct cwmcu_data *mcu_data,
			u8 reg_addr, u8 *data, u8 len);
static int CWMCU_i2c_write_power(struct cwmcu_data *mcu_data,
			u8 reg_addr, u8 *data, u8 len);
static int CWMCU_i2c_write_block(struct cwmcu_data *sensor,
			  u8 reg_addr, u8 *data, u8 len);
static int firmware_odr(struct cwmcu_data *mcu_data, int sensors_id,
			int delay_ms);
static bool reset_hub(struct cwmcu_data *mcu_data, bool force_reset);

#ifdef SHUB_DLOAD_SUPPORT
static int mcu_set_reboot_state(u32 state);
static int mcu_dload_i2c_read(u8 cmd, u8 *data, u8 len);
static int mcu_dload_dump_backup_registers(void);
#ifdef SHUB_DLOAD_DUMP_EXCEPTION_BUFFER
static int mcu_dload_dump_exception_buffer(void);
#endif 
static inline int MCU2CPU_STATUS_GPIO_LEVEL(struct cwmcu_data *mcu_data)
{
    if ((MCU_IN_BOOTLOADER()) || (gpio_get_value_cansleep(mcu_data->gpio_reset) == 0)) {
        return MCU2CPU_STATUS_GPIO_LEVEL_SHUB;
    }

    if (gpio_is_valid(mcu_data->gpio_mcu_status)) {
        return gpio_get_value(mcu_data->gpio_mcu_status);
    }
    else {
        int ret;
        u32 mcu_status_read = 0xff;

        E("gpio_mcu_status is invalid !!!\n");

        ret = mcu_dload_i2c_read(CW_I2C_REG_REBOOT_MODE, (u8*)&mcu_status_read, sizeof(mcu_status_read));
        if (ret > 0) {
            I("MCU2CPU_STATUS_GPIO_LEVEL_DLOAD\n");
            return MCU2CPU_STATUS_GPIO_LEVEL_DLOAD;
        }
        else  {
            I("MCU2CPU_STATUS_GPIO_LEVEL_SHUB\n");
            return MCU2CPU_STATUS_GPIO_LEVEL_SHUB;
        }
    }
}
#endif 

static void gpio_make_falling_edge(int gpio)
{
	if (!gpio_get_value(gpio))
		gpio_set_value(gpio, 1);
	gpio_set_value(gpio, 0);
}

static void mcu_chip_mode_set(struct cwmcu_data *mcu_data, int onoff)
{
    if (mcu_data) {
        gpio_direction_output(mcu_data->gpio_chip_mode, onoff);
        mcu_data->gpio_chip_mode_level = onoff;
    }
}

static int mcu_chip_mode_get(struct cwmcu_data *mcu_data)
{
    if (mcu_data)
        return mcu_data->gpio_chip_mode_level;
    else
        return -1;
}

static inline void mcu_boot_status_reset(struct cwmcu_data *mcu_data)
{
	mcu_data->mcu_bootup = false;
	mcu_data->mcu_sensor_ready = false;
}

static void mcu_state_enter_unknown(struct cwmcu_data *mcu_data)
{
    struct timespec ts;
    struct rtc_time tm;

    
    getnstimeofday(&ts);
    rtc_time_to_tm(ts.tv_sec, &tm);

    INIT_COMPLETION(s_mcu_enter_shub_run);
    s_mcu_state = MCU_STATE_UNKNOWN;
    E("%s[%d]: s_mcu_state enter MCU_STATE_UNKNOWN at (%d-%02d-%02d %02d:%02d:%02d.%09lu UTC) !!!!!!!!!!\n", __func__, __LINE__,
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
}

static void mcu_state_enter_shub_init(struct cwmcu_data *mcu_data)
{
    struct timespec ts;
    struct rtc_time tm;

    
    getnstimeofday(&ts);
    rtc_time_to_tm(ts.tv_sec, &tm);

    I("%s[%d]: s_mcu_state enter MCU_STATE_SHUB_INIT at (%d-%02d-%02d %02d:%02d:%02d.%09lu UTC)\n", __func__, __LINE__,
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
    s_mcu_state = MCU_STATE_SHUB_INIT;

    mcu_data->w_re_init = true;
    queue_work(s_mcu_data->mcu_wq, &s_mcu_data->one_shot_work);
}

static void mcu_state_enter_shub_run(struct cwmcu_data *mcu_data)
{
    struct timespec ts;
    struct rtc_time tm;

    
    getnstimeofday(&ts);
    rtc_time_to_tm(ts.tv_sec, &tm);

    I("%s[%d]: s_mcu_state enter MCU_STATE_SHUB_RUN at (%d-%02d-%02d %02d:%02d:%02d.%09lu UTC)\n", __func__, __LINE__,
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
    s_mcu_state = MCU_STATE_SHUB_RUN;
    complete_all(&s_mcu_enter_shub_run);
}

#ifdef SHUB_DLOAD_SUPPORT
static void mcu_state_enter_dload(struct cwmcu_data *mcu_data)
{
    struct timespec ts;
    struct rtc_time tm;
    int ret;

    if (!mcu_data)
        return;

    if (!mcu_data->dload_mode_enabled) {
        E("%s[%d]: set mcu to SHUB mode\n", __func__, __LINE__);
        ret = mcu_set_reboot_state(MCU_SYS_STATUS_SHUB);
        if (ret >= 0) {
            if (mcu_data) reset_hub(mcu_data, true);
        }
        return;
    }

    I("%s[%d]", __func__, __LINE__);

    ret = mcu_dload_dump_backup_registers();
    if (ret < 0) {
        E("%s[%d]: mcu_dload_dump_backup_registers fails, ret = %d\n", __func__, __LINE__, ret);
        return;
    }

    #ifdef SHUB_DLOAD_DUMP_EXCEPTION_BUFFER
    ret = mcu_dload_dump_exception_buffer();
    if (ret < 0) {
        E("%s[%d]: mcu_dload_dump_exception_buffer fails, ret = %d\n", __func__, __LINE__, ret);
        return;
    }
    #endif 

    
    getnstimeofday(&ts);
    rtc_time_to_tm(ts.tv_sec, &tm);

    mutex_lock(&s_activated_i2c_lock);
    INIT_COMPLETION(s_mcu_enter_shub_run);
    s_mcu_state = MCU_STATE_DLOAD;
    mutex_unlock(&s_activated_i2c_lock);

    E("%s[%d]: s_mcu_state enter MCU_STATE_DLOAD at (%d-%02d-%02d %02d:%02d:%02d.%09lu UTC) !!!!!!!!!!\n", __func__, __LINE__,
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);

    mcu_boot_status_reset(mcu_data);
    complete(&s_mcu_ramdump_avail);
}
#endif 

static void do_mcu_state_transition(struct cwmcu_data *mcu_data)
{
    MCU_STATE pre_mcu_state = s_mcu_state;
#ifdef SHUB_DLOAD_SUPPORT
    int mcu_status_level = mcu_data->gpio_mcu_status_level;
#endif 

    D("%s enter, s_mcu_state:0x%x\n", __func__, pre_mcu_state);

DO_STAT_TRANS:
    switch (s_mcu_state) {
    case MCU_STATE_UNKNOWN:
        {
            #ifdef SHUB_DLOAD_SUPPORT
            if (MCU2CPU_STATUS_GPIO_LEVEL_DLOAD == mcu_status_level) {
                mcu_state_enter_dload(mcu_data);
            }
            #endif 

            if (mcu_data->mcu_bootup) {
                mcu_data->mcu_bootup = false;
                mcu_state_enter_shub_init(mcu_data);
            }
        }
        break;

    case MCU_STATE_SHUB_INIT:
        {
            #ifdef SHUB_DLOAD_SUPPORT
            if (MCU2CPU_STATUS_GPIO_LEVEL_DLOAD == mcu_status_level) {
                mcu_state_enter_dload(mcu_data);
            }
            #endif 

            
            if (mcu_data->mcu_bootup) {
                mcu_state_enter_unknown(mcu_data);
                goto DO_STAT_TRANS;
            }

            if (mcu_data->mcu_sensor_ready) {
                mcu_data->mcu_sensor_ready = false;
                mcu_state_enter_shub_run(mcu_data);
            }
        }
        break;

    case MCU_STATE_SHUB_RUN:
        {
            #ifdef SHUB_DLOAD_SUPPORT
            if (MCU2CPU_STATUS_GPIO_LEVEL_DLOAD == mcu_status_level) {
                mcu_state_enter_dload(mcu_data);
            }
            #endif 

            
            if (mcu_data->mcu_bootup) {
                mcu_state_enter_unknown(mcu_data);
                goto DO_STAT_TRANS;
            }
        }
        break;

#ifdef SHUB_DLOAD_SUPPORT
    case MCU_STATE_DLOAD:
        if ((MCU2CPU_STATUS_GPIO_LEVEL_SHUB == mcu_status_level)) {
            mcu_state_enter_unknown(mcu_data);
            goto DO_STAT_TRANS;
        }
        break;
#endif 

    case MCU_STATE_BOOTLOADER:
        break;

    default:
        E("%s[%d]: incorrect s_mcu_state = 0x%x\n", __func__, __LINE__, s_mcu_state);
        break;
    }

    if (pre_mcu_state != s_mcu_state) {
        I("%s s_mcu_state change 0x%x -> 0x%x\n", __func__, pre_mcu_state, s_mcu_state);
    }

    D("%s exit, s_mcu_state:0x%x\n", __func__, s_mcu_state);
}

static void cwmcu_powermode_switch(struct cwmcu_data *mcu_data, int onoff)
{
	mutex_lock(&mcu_data->power_mode_lock);
	if (onoff) {
		if (mcu_data->power_on_counter == 0) {
			gpio_make_falling_edge(mcu_data->gpio_wake_mcu);
			udelay(10);
			gpio_set_value(mcu_data->gpio_wake_mcu, 1);
			udelay(10);
			gpio_set_value(mcu_data->gpio_wake_mcu, 0);
			D("%s: 11 onoff = %d\n", __func__, onoff);
			usleep_range(500, 600);
		}
		mcu_data->power_on_counter++;
	} else {
		mcu_data->power_on_counter--;
		if (mcu_data->power_on_counter <= 0) {
			mcu_data->power_on_counter = 0;
			gpio_set_value(mcu_data->gpio_wake_mcu, 1);
			D("%s: 22 onoff = %d\n", __func__, onoff);
		}
	}
	mutex_unlock(&mcu_data->power_mode_lock);
	D("%s: onoff = %d, power_counter = %d\n", __func__, onoff,
	  mcu_data->power_on_counter);
}

static int cw_send_event(struct cwmcu_data *mcu_data, u8 id, u16 *data,
			 s64 timestamp)
{
	u8 event[21];

	event[0] = id;
	memcpy(&event[1], data, sizeof(u16)*3);
	memset(&event[7], 0, sizeof(u16)*3);
	memcpy(&event[13], &timestamp, sizeof(s64));

	D("%s: active_scan_mask = 0x%p, masklength = %u, data(x, y, z) ="
	  "(%d, %d, %d)\n",
	  __func__, mcu_data->indio_dev->active_scan_mask,
	  mcu_data->indio_dev->masklength,
	  *(s16 *)&event[1], *(s16 *)&event[3], *(s16 *)&event[5]);

	if (mcu_data->indio_dev->active_scan_mask &&
	    (!bitmap_empty(mcu_data->indio_dev->active_scan_mask,
			   mcu_data->indio_dev->masklength))) {
		mutex_lock(&mcu_data->mutex_lock);
		iio_push_to_buffers(mcu_data->indio_dev, event);
		mutex_unlock(&mcu_data->mutex_lock);
		return 0;
	} else if (mcu_data->indio_dev->active_scan_mask == NULL)
		D("%s: active_scan_mask = NULL, event might be missing\n",
		  __func__);

	return -EIO;
}

static int cw_send_event_special(struct cwmcu_data *mcu_data, u8 id, u16 *data,
				 u16 *bias, s64 timestamp)
{
	u8 event[21];

	event[0] = id;
	memcpy(&event[1], data, sizeof(u16)*3);
	memcpy(&event[7], bias, sizeof(u16)*3);
	memcpy(&event[13], &timestamp, sizeof(s64));

	if (mcu_data->indio_dev->active_scan_mask &&
	    (!bitmap_empty(mcu_data->indio_dev->active_scan_mask,
			   mcu_data->indio_dev->masklength))) {
		mutex_lock(&mcu_data->mutex_lock);
		iio_push_to_buffers(mcu_data->indio_dev, event);
		mutex_unlock(&mcu_data->mutex_lock);
		return 0;
	} else if (mcu_data->indio_dev->active_scan_mask == NULL)
		D("%s: active_scan_mask = NULL, event might be missing\n",
		  __func__);

	return -EIO;
}

static int cwmcu_get_calibrator_status(struct cwmcu_data *mcu_data,
				       u8 sensor_id, u8 *data)
{
	int error_msg = 0;

	if (sensor_id == CW_ACCELERATION)
		error_msg = CWMCU_i2c_read_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_STATUS_ACC,
				data, 1);
	else if (sensor_id == CW_MAGNETIC)
		error_msg = CWMCU_i2c_read_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_STATUS_MAG,
				data, 1);
	else if (sensor_id == CW_GYRO)
		error_msg = CWMCU_i2c_read_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_STATUS_GYRO,
				data, 1);

	return error_msg;
}

static int cwmcu_get_calibrator(struct cwmcu_data *mcu_data, u8 sensor_id,
				s8 *data, u8 len)
{
	int error_msg = 0;

	if ((sensor_id == CW_ACCELERATION) && (len == ACC_CALIBRATOR_LEN))
		error_msg = CWMCU_i2c_read_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_GET_DATA_ACC,
				data, len);
	else if ((sensor_id == CW_MAGNETIC) && (len == MAG_CALIBRATOR_LEN))
		error_msg = CWMCU_i2c_read_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_GET_DATA_MAG,
				data, len);
	else if ((sensor_id == CW_GYRO) && (len == GYRO_CALIBRATOR_LEN))
		error_msg = CWMCU_i2c_read_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_GET_DATA_GYRO,
				data, len);
	else if ((sensor_id == CW_LIGHT) && (len == LIGHT_CALIBRATOR_LEN))
		error_msg = CWMCU_i2c_read_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_GET_DATA_LIGHT,
				data, len);
        else if ((sensor_id == CW_PROXIMITY) && (len == PROXIMITY_CALIBRATOR_LEN))
                error_msg = CWMCU_i2c_read_power(mcu_data,
                                CW_I2C_REG_SENSORS_CALIBRATOR_GET_DATA_PROXIMITY,
                                data, len);
	else if ((sensor_id == CW_PRESSURE) && (len == PRESSURE_CALIBRATOR_LEN))
		error_msg = CWMCU_i2c_read_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_GET_DATA_PRESSURE,
				data, len);
	else
		E("%s: invalid arguments, sensor_id = %u, len = %u\n",
		  __func__, sensor_id, len);

	D("sensors_id = %u, calibrator data = (%d, %d, %d)\n", sensor_id,
	  data[0], data[1], data[2]);
	return error_msg;
}

static ssize_t set_calibrator_en(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data;
	u8 data2;
	unsigned long sensors_id;
	int error;

	error = kstrtoul(buf, 10, &sensors_id);
	if (error) {
		E("%s: kstrtoul fails, error = %d\n", __func__, error);
		return error;
	}

	
	data = (u8)sensors_id;
	D("%s: data(sensors_id) = %u\n", __func__, data);

	cwmcu_powermode_switch(mcu_data, 1);

	mutex_lock(&mcu_data->group_i2c_lock);

	switch (data) {
	case 1:
		error = CWMCU_i2c_read(mcu_data, G_SENSORS_STATUS, &data2, 1);
		if (error < 0)
			goto i2c_fail;
		data = data2 | 16;
		error = CWMCU_i2c_write(mcu_data, G_SENSORS_STATUS, &data, 1);
		if (error < 0)
			goto i2c_fail;
		break;
	case 2:
		error = CWMCU_i2c_read(mcu_data, ECOMPASS_SENSORS_STATUS,
				       &data2, 1);
		if (error < 0)
			goto i2c_fail;
		data = data2 | 16;
		error = CWMCU_i2c_write(mcu_data, ECOMPASS_SENSORS_STATUS,
					&data, 1);
		if (error < 0)
			goto i2c_fail;
		break;
	case 4:
		error = CWMCU_i2c_read(mcu_data, GYRO_SENSORS_STATUS,
				       &data2, 1);
		if (error < 0)
			goto i2c_fail;
		data = data2 | 16;
		error = CWMCU_i2c_write(mcu_data, GYRO_SENSORS_STATUS,
					&data, 1);
		if (error < 0)
			goto i2c_fail;
		break;
	case 7:
		error = CWMCU_i2c_read(mcu_data, LIGHT_SENSORS_STATUS,
				       &data2, 1);
		if (error < 0)
			goto i2c_fail;
		data = data2 | 16;
		error = CWMCU_i2c_write(mcu_data, LIGHT_SENSORS_STATUS,
					&data, 1);
		if (error < 0)
			goto i2c_fail;
		break;
        case 8:
                error =  CWMCU_i2c_read(mcu_data, PROXIMITY_SENSORS_STATUS,
                                        &data2, 1);
		if (error < 0)
			goto i2c_fail;
		data = data2 | 16;
		error = CWMCU_i2c_write(mcu_data, PROXIMITY_SENSORS_STATUS,
					&data,1);
		if (error < 0)
			goto i2c_fail;
		break;
	case 9:
		data = 2; 
		error = CWMCU_i2c_write(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_TARGET_ACC,
				&data, 1);
		error = CWMCU_i2c_read(mcu_data, G_SENSORS_STATUS, &data2, 1);
		if (error < 0)
			goto i2c_fail;
		data = data2 | 16;
		error = CWMCU_i2c_write(mcu_data, G_SENSORS_STATUS, &data, 1);
		if (error < 0)
			goto i2c_fail;
		break;
	case 10:
		data = 1; 
		error = CWMCU_i2c_write(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_TARGET_ACC,
				&data, 1);
		error = CWMCU_i2c_read(mcu_data, G_SENSORS_STATUS, &data2, 1);
		if (error < 0)
			goto i2c_fail;
		data = data2 | 16;
		error = CWMCU_i2c_write(mcu_data, G_SENSORS_STATUS, &data, 1);
		if (error < 0)
			goto i2c_fail;
		break;
	case 11:
		data = 0; 
		error = CWMCU_i2c_write(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_TARGET_ACC,
				&data, 1);
		if (error < 0)
			goto i2c_fail;
		break;
	default:
		mutex_unlock(&mcu_data->group_i2c_lock);
		cwmcu_powermode_switch(mcu_data, 0);
		E("%s: Improper sensor_id = %u\n", __func__, data);
		return -EINVAL;
	}

	error = count;

i2c_fail:
	mutex_unlock(&mcu_data->group_i2c_lock);

	cwmcu_powermode_switch(mcu_data, 0);

	D("%s--: data2 = 0x%x, rc = %d\n", __func__, data2, error);
	return error;
}

static void print_hex_data(char *buf, u32 index, u8 *data, size_t len)
{
	int i;
	int rc;
	char *buf_start;
	size_t buf_remaining =
		3*EXCEPTION_BLOCK_LEN; 

	buf_start = buf;

	for (i = 0; i < len; i++) {
		rc = scnprintf(buf, buf_remaining, "%02x%c", data[i],
				(i == len - 1) ? '\0' : ' ');
		buf += rc;
		buf_remaining -= rc;
	}

	printk(KERN_ERR "[S_HUB][CW_MCU] Exception Buffer[%04d] = %.*s\n",
			index * EXCEPTION_BLOCK_LEN,
			(int)(buf - buf_start),
			buf_start);
}

static ssize_t sprint_data(char *buf, s8 *data, ssize_t len)
{
	int i;
	int rc;
	size_t buf_remaining = PAGE_SIZE;

	for (i = 0; i < len; i++) {
		rc = scnprintf(buf, buf_remaining, "%d%c", data[i],
				(i == len - 1) ? '\n' : ' ');
		buf += rc;
		buf_remaining -= rc;
	}
	return PAGE_SIZE - buf_remaining;
}

static ssize_t show_calibrator_status_acc(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[6] = {0};

	if (cwmcu_get_calibrator_status(mcu_data, CW_ACCELERATION, data) >= 0)
		return scnprintf(buf, PAGE_SIZE, "0x%x\n", data[0]);

	return scnprintf(buf, PAGE_SIZE, "0x1\n");
}

static ssize_t show_calibrator_status_mag(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[6] = {0};

	if (cwmcu_get_calibrator_status(mcu_data, CW_MAGNETIC, data) >= 0)
		return scnprintf(buf, PAGE_SIZE, "0x%x\n", data[0]);

	return scnprintf(buf, PAGE_SIZE, "0x1\n");
}

static ssize_t show_calibrator_status_gyro(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[6] = {0};

	if (cwmcu_get_calibrator_status(mcu_data, CW_GYRO, data) >= 0)
		return scnprintf(buf, PAGE_SIZE, "0x%x\n", data[0]);

	return scnprintf(buf, PAGE_SIZE, "0x1\n");
}

static ssize_t set_k_value(struct cwmcu_data *mcu_data, const char *buf,
			   size_t count, u8 reg_addr, u8 len)
{
	int i;
	long data_temp[len];
	char *str_buf;
	char *running;
	int error;

	D(
	  "%s: count = %lu, strlen(buf) = %lu, PAGE_SIZE = %lu,"
	  " reg_addr = 0x%x\n",
	  __func__, count, strlen(buf), PAGE_SIZE, reg_addr);

	str_buf = kstrndup(buf, count, GFP_KERNEL);
	if (str_buf == NULL) {
		E("%s: cannot allocate buffer\n", __func__);
		return -ENOMEM;
	}
	running = str_buf;

	for (i = 0; i < len; i++) {
		int error;
		char *token;

		token = strsep(&running, " ");

		if (token == NULL) {
			D("%s: i = %d\n", __func__, i);
			break;
		} else {
			if (reg_addr ==
			    CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PRESSURE)
				error = kstrtol(token, 16, &data_temp[i]);
			else
				error = kstrtol(token, 10, &data_temp[i]);
			if (error) {
				E("%s: kstrtol fails, error = %d, i = %d\n",
				  __func__, error, i);
				kfree(str_buf);
				return error;
			}
		}
	}
	kfree(str_buf);

	D("Set calibration by attr (%ld, %ld, %ld), len = %u, reg_addr = 0x%x\n"
	  , data_temp[0], data_temp[1], data_temp[2], len, reg_addr);

	cwmcu_powermode_switch(mcu_data, 1);

	mutex_lock(&mcu_data->group_i2c_lock);
	for (i = 0; i < len; i++) {
		u8 data = (u8)(data_temp[i]);
		
		error = CWMCU_i2c_write(mcu_data, reg_addr, &data, 1);
		if (error < 0) {
			mutex_unlock(&mcu_data->group_i2c_lock);
			cwmcu_powermode_switch(mcu_data, 0);
			E("%s: error = %d, i = %d\n", __func__, error, i);
			return -EIO;
		}
	}
	mutex_unlock(&mcu_data->group_i2c_lock);

	cwmcu_powermode_switch(mcu_data, 0);

	return count;
}

static ssize_t set_k_value_acc_f(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);

	return set_k_value(mcu_data, buf, count,
			   CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_ACC,
			   ACC_CALIBRATOR_LEN);
}


static ssize_t set_k_value_mag_f(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);

	return set_k_value(mcu_data, buf, count,
			   CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_MAG,
			   MAG_CALIBRATOR_LEN);
}

static ssize_t set_k_value_gyro_f(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);

	return set_k_value(mcu_data, buf, count,
			   CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_GYRO,
			   GYRO_CALIBRATOR_LEN);
}

static ssize_t set_k_value_proximity_f(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t count)
{
        struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
        int data_temp[2] = {0};
        int i = 0;
        u8 data[10] = {0};
        sscanf(buf, "%x %x\n", &data_temp[0], &data_temp[1]);
#if 0
	data_temp[2]= mcu_data->ps_thd_add;
#endif
        D("set proximity calibration\n");
        for(i = 0; i < 2; i++){
                data[i] = (u8)(s8) data_temp[i] & 0xFF;
                I("[proximity] data[%d] = %d, data_temp[%d] = %d\n"
			, 2*i, data[i], 2*i, data_temp[i]);
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
				&data[i], 1);
                data[i] = (u8)(s8) (data_temp[i] >> 8) & 0xFF;
                I("[proximity] data[%d] = %d, data_temp[%d] = %d\n"
			, 2*i + 1,data[i] , 2*i + 1, data_temp[i]);
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
				&data[i], 1);
        }

	return count;
#if 0
        return set_k_value(mcu_data, buf, count,
                            CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                            PROXIMITY_CALIBRATOR_LEN);
#endif
}

static ssize_t set_k_value_barometer_f(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);

	return set_k_value(mcu_data, buf, count,
			   CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PRESSURE,
			   PRESSURE_CALIBRATOR_LEN);
}

static ssize_t set_ps_canc(struct device *dev,struct device_attribute *attr,const char *buf, size_t count){

	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	int code = 0;
	u8 PS_canc1, PS_canc2 = 0;
        u8 PS_th1, PS_th2 = 0;
        u8 PS_th_add1, PS_th_add2 = 0;

        sscanf(buf, "%d\n", &code);

        if(code == 1) {
		if (((mcu_data->ps_kvalue >> 16) & 0xFFFF) == 0xFFFF) {
                        PS_canc1 = 0x00;
                        PS_canc2 = 0x00;
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_canc1,1);
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_canc2,1);
                        PS_th1 = (mcu_data->ps_kvalue >> 8) & 0xFF;
                        PS_th2 = 0x00;
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th1,1);
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th2,1);
#if 0
                        PS_th_add1 = mcu_data->ps_thd_add & 0xFF;
                        PS_th_add2 = (mcu_data->ps_thd_add >> 8) & 0xFF;
                        CWMCU_i2c_write(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th_add1,1);
                        CWMCU_i2c_write(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th_add2,1);
#endif
                } else {
                        PS_canc1 = 0x00;
                        PS_canc2 = 0x00;
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_canc1,1);
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_canc2,1);
                        PS_th1 = (mcu_data->ps_kvalue & 0xFF0000) >> 16;
                        PS_th2 = (mcu_data->ps_kvalue & 0xFF000000) >> 24;
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th1,1);
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th2,1);
#if 0
                        PS_th_add1 = mcu_data->ps_thd_add & 0xFF;
                        PS_th_add2 = (mcu_data->ps_thd_add >> 8) & 0xFF;
                        CWMCU_i2c_write(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th_add1,1);
                        CWMCU_i2c_write(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th_add2,1);
#endif
                }
                I("Recover PS_canc1 = %d, PS_canc2 = %d, PS_th1 = %d PS_th2 = %d, PS_th_add1 = %d, PS_th_add2 = %d\n",
				 PS_canc1, PS_canc2, PS_th1, PS_th2, PS_th_add1, PS_th_add2);
        } else {
                if (((mcu_data->ps_kvalue >> 16) & 0xFFFF) == 0xFFFF) {
                        PS_canc1 = (mcu_data->ps_kvalue) & 0xFF;
                        PS_canc2 = 0x00;
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_canc1,1);
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_canc2,1);
                        PS_th1 = (mcu_data->ps_kvalue >>  8) & 0xFF;
                        PS_th2 = 0x00;
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th1,1);
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th2,1);
#if 0
                        PS_th_add1 = mcu_data->ps_thd_add & 0xFF;
                        PS_th_add2 = (mcu_data->ps_thd_add >> 8) & 0xFF;
                        CWMCU_i2c_write(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th_add1,1);
                        CWMCU_i2c_write(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th_add2,1);
#endif
                } else {
                        PS_canc1 = mcu_data->ps_kvalue & 0xFF;
                        PS_canc2 = ((mcu_data->ps_kvalue) & 0xFF00) >> 8;
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_canc1,1);
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_canc2,1);
                        PS_th1 = (mcu_data->ps_kvalue & 0xFF0000) >> 16;
                        PS_th2 = (mcu_data->ps_kvalue & 0xFF000000) >> 24;
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th1,1);
                        CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th2,1);
#if 0
                        PS_th_add1 = mcu_data->ps_thd_add & 0xFF;
                        PS_th_add2 = (mcu_data->ps_thd_add >> 8) & 0xFF;
                        CWMCU_i2c_write(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th_add1,1);
                        CWMCU_i2c_write(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,&PS_th_add2,1);
#endif
                }
                I("Recover PS_canc1 = %d, PS_canc2 = %d, PS_th1 = %d PS_th2 = %d, PS_th_add1 = %d, PS_th_add2 = %d\n",
				 PS_canc1, PS_canc2, PS_th1, PS_th2, PS_th_add1, PS_th_add2);
        }
        return count;
}

static ssize_t led_enable(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	int error;
	u8 data;
	long data_temp = 0;

	error = kstrtol(buf, 10, &data_temp);
	if (error) {
		E("%s: kstrtol fails, error = %d\n", __func__, error);
		return error;
	}

	data = data_temp ? 2 : 4;

	I("LED %s\n", (data == 2) ? "ENABLE" : "DISABLE");

	error = CWMCU_i2c_write_power(mcu_data, 0xD0, &data, 1);
	if (error < 0) {
		cwmcu_powermode_switch(mcu_data, 0);
		E("%s: error = %d\n", __func__, error);
		return -EIO;
	}

	return count;
}

static ssize_t get_k_value(struct cwmcu_data *mcu_data, int type, char *buf,
			   char *data, unsigned len)
{
	if (cwmcu_get_calibrator(mcu_data, type, data, len) < 0)
		memset(data, 0, len);

	return sprint_data(buf, data, len);
}

static ssize_t get_k_value_acc_f(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[ACC_CALIBRATOR_LEN];

	return get_k_value(mcu_data, CW_ACCELERATION, buf, data, sizeof(data));
}

static ssize_t get_k_value_acc_rl_f(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[ACC_CALIBRATOR_RL_LEN] = {0};

	if (CWMCU_i2c_read_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_RESULT_RL_ACC
			   , data, sizeof(data)) >= 0) {

		if (DEBUG_FLAG_GSENSOR == 1) {
			int i;

			for (i = 0; i < sizeof(data); i++)
				D("data[%d]: %u\n", i, data[i]);
		}

		mcu_data->gs_kvalue_L1 = ((s8)data[1] << 8) | data[0];
		mcu_data->gs_kvalue_L2 = ((s8)data[3] << 8) | data[2];
		mcu_data->gs_kvalue_L3 = ((s8)data[5] << 8) | data[4];
		mcu_data->gs_kvalue_R1 = ((s8)data[7] << 8) | data[6];
		mcu_data->gs_kvalue_R2 = ((s8)data[9] << 8) | data[8];
		mcu_data->gs_kvalue_R3 = ((s8)data[11] << 8) | data[10];
	}

	return sprint_data(buf, data, sizeof(data));
}

static ssize_t ap_get_k_value_acc_rl_f(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d %d %d %d %d %d\n",
			 (s16)mcu_data->gs_kvalue_L1,
			 (s16)mcu_data->gs_kvalue_L2,
			 (s16)mcu_data->gs_kvalue_L3,
			 (s16)mcu_data->gs_kvalue_R1,
			 (s16)mcu_data->gs_kvalue_R2,
			 (s16)mcu_data->gs_kvalue_R3);
}

static ssize_t get_k_value_mag_f(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[MAG_CALIBRATOR_LEN];

	return get_k_value(mcu_data, CW_MAGNETIC, buf, data, sizeof(data));
}

static ssize_t get_k_value_gyro_f(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[GYRO_CALIBRATOR_LEN];

	return get_k_value(mcu_data, CW_GYRO, buf, data, sizeof(data));
}

static ssize_t get_k_value_light_f(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[LIGHT_CALIBRATOR_LEN] = {0};

	if (cwmcu_get_calibrator(mcu_data, CW_LIGHT, data, sizeof(data)) < 0) {
		E("%s: Get LIGHT Calibrator fails\n", __func__);
		return -EIO;
	}
	return scnprintf(buf, PAGE_SIZE, "%x %x %x %x\n", data[0], data[1],
			 data[2], data[3]);
}

static ssize_t get_k_value_proximity_f(struct device *dev,
                                       struct device_attribute *attr, char *buf)
{
        struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
        u8 data[PROXIMITY_CALIBRATOR_LEN] = {0};

        if(cwmcu_get_calibrator(mcu_data, CW_PROXIMITY, data, sizeof(data)) < 0) {
                E("%s: Get PROXIMITY Calibrator fails\n", __func__);
                return -EIO;
	}
        return scnprintf(buf, PAGE_SIZE, "%x %x %x %x\n", data[0], data[1],
                        data[2], data[3]);
}

static ssize_t get_k_value_barometer_f(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[PRESSURE_CALIBRATOR_LEN];

	return get_k_value(mcu_data, CW_PRESSURE, buf, data, sizeof(data));
}

static ssize_t trigger_mcu_crash(struct device *dev,
        struct device_attribute *attr,
        const char *buf, size_t count)
{
    int error;
    u8 data;
    long data_temp = 0;

    error = kstrtol(buf, 10, &data_temp);
    if (error) {
        E("%s: kstrtol fails, error = %d\n", __func__, error);
        return error;
    }

    data = data_temp;

    I("%s(%d): crash_type:%d\n", __func__, __LINE__, data);
    if (s_mcu_data) {
        error = CWMCU_i2c_write_power(s_mcu_data, CW_I2C_REG_TRIGGER_CRASH, &data, 1);
        if (error < 0) {
            E("%s: error = %d\n", __func__, error);
            return -EIO;
        }
    }

    return count;
}

static ssize_t trigger_mcu_wakeup(struct device *dev,
        struct device_attribute *attr,
        const char *buf, size_t count)
{
    int error;
    u8 data;
    long data_temp = 0;

    error = kstrtol(buf, 10, &data_temp);
    if (error) {
        E("%s: kstrtol fails, error = %d\n", __func__, error);
        return error;
    }

    data = data_temp;

    I("%s: mcu_wakeup:%d\n", __func__, data);
    if (s_mcu_data) {
        cwmcu_powermode_switch(s_mcu_data, !!data);
    }

    return count;
}

static int CWMCU_i2c_read_power(struct cwmcu_data *mcu_data,
			 u8 reg_addr, u8 *data, u8 len)
{
	int ret;

	cwmcu_powermode_switch(mcu_data, 1);
	ret = CWMCU_i2c_read(mcu_data, reg_addr, data, len);
	cwmcu_powermode_switch(mcu_data, 0);
	return ret;
}

static int CWMCU_i2c_write_power(struct cwmcu_data *mcu_data,
				 u8 reg_addr, u8 *data, u8 len)
{
	int ret;

	cwmcu_powermode_switch(mcu_data, 1);
	ret = CWMCU_i2c_write(mcu_data, reg_addr, data, len);
	cwmcu_powermode_switch(mcu_data, 0);
	return ret;
}

static int CWMCU_i2c_write_block_power(struct cwmcu_data *mcu_data,
				 u8 reg_addr, u8 *data, u8 len)
{
	int ret;

	cwmcu_powermode_switch(mcu_data, 1);
	ret = CWMCU_i2c_write_block(mcu_data, reg_addr, data, len);
	cwmcu_powermode_switch(mcu_data, 0);
	return ret;
}

static ssize_t get_light_kadc(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[4] = {0};
	u16 light_gadc;
	u16 light_kadc;

	CWMCU_i2c_read_power(mcu_data, LIGHT_SENSORS_CALIBRATION_DATA, data,
			     sizeof(data));

	light_gadc = (data[1] << 8) | data[0];
	light_kadc = (data[3] << 8) | data[2];
	return scnprintf(buf, PAGE_SIZE, "gadc = 0x%x, kadc = 0x%x", light_gadc,
			 light_kadc);
}

static ssize_t get_firmware_version(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 firmware_version[FW_VER_COUNT] = {0};

	CWMCU_i2c_read_power(mcu_data, FIRMWARE_VERSION, firmware_version,
			     sizeof(firmware_version));

	return scnprintf(buf, PAGE_SIZE,
			 "Firmware Architecture version %u, "
			 "Sense version %u, Cywee lib version %u,"
			 " Water number %u"
			 ", Active Engine %u, Project Mapping %u\n",
			 firmware_version[0], firmware_version[1],
			 firmware_version[2], firmware_version[3],
			 firmware_version[4], firmware_version[5]);
}

static ssize_t get_firmware_info(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 firmware_info[6] = {0};

	CWMCU_i2c_read_power(mcu_data, FIRMWARE_INFO, firmware_info,
			     sizeof(firmware_info));

	return scnprintf(buf, PAGE_SIZE,
			 "Jenkins build number %u, "
			 "Build time(hh:mm) %02u:%02u, "
			 "CW branch %u, CW mcu type %u\n",
			 (firmware_info[0] << 8) | firmware_info[1],
			 firmware_info[2], firmware_info[3],
			 firmware_info[4], firmware_info[5]);
}

static ssize_t get_hall_sensor(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 hall_sensor = 0;

	CWMCU_i2c_read_power(mcu_data, CWSTM32_READ_Hall_Sensor,
			     &hall_sensor, 1);

	return scnprintf(buf, PAGE_SIZE,
			 "Hall_1(S, N) = (%u, %u), Hall_2(S, N)"
			 " = (%u, %u), Hall_3(S, N) = (%u, %u)\n",
			 !!(hall_sensor & 0x1), !!(hall_sensor & 0x2),
			 !!(hall_sensor & 0x4), !!(hall_sensor & 0x8),
			 !!(hall_sensor & 0x10), !!(hall_sensor & 0x20));
}

static ssize_t get_barometer(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[6] = {0};

	CWMCU_i2c_read_power(mcu_data, CWSTM32_READ_Pressure, data,
			     sizeof(data));

	return scnprintf(buf, PAGE_SIZE, "%x %x %x %x\n", data[0], data[1],
					 data[2], data[3]);
}

static ssize_t get_ps_canc(struct device *dev, struct device_attribute *attr,
                               char *buf)
{
        struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
        u16 PS_canc, PS_th;
        u8 data[6] = {0};
        int ret = 0;

        ret = CWMCU_i2c_read_power(mcu_data, CW_I2C_REG_SENSORS_CALIBRATOR_GET_DATA_PROXIMITY,
							 data, 6);
        PS_canc = (data[1] << 8) | data[0];
        PS_th   = (data[3] << 8) | data[2];
        D("INTE_PS1_CANC = (0x%04X),  INTE_PS2_CANC = (0x%04X)\n", PS_canc, PS_th);

	if (((mcu_data->ps_kheader & (0x50 << 24)) == (0x50 << 24)) &&
		 ((mcu_data->ps_kheader & (0x53 << 16)) == (0x53 << 16))) {
                ret = snprintf(buf, PAGE_SIZE, "P-sensor calibrated,"
                                "INTE_PS1_CANC = (0x%04X), "
                                "INTE_PS2_CANC = (0x%04X)\n",
                                PS_canc, PS_th);
        } else
                ret = snprintf(buf, PAGE_SIZE, "P-sensor NOT calibrated,"
                                "INTE_PS1_CANC = (0x%04X), "
                                "INTE_PS2_CANC = (0x%04X)\n",
                                PS_canc, PS_th);
        return ret;
}

static ssize_t get_proximity(struct device *dev, struct device_attribute *attr,
                            char *buf)
{
        struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
        u8 data[REPORT_EVENT_PROXIMITY_LEN] = {0};
        u16 proximity_adc;

        CWMCU_i2c_read_power(mcu_data, CWSTM32_READ_Proximity,
				 data, sizeof(data));

        proximity_adc = (data[2] << 8) | data[1];
        return snprintf(buf, PAGE_SIZE, "%x %x \n",
				 data[0], proximity_adc);
}

static ssize_t get_proximity_polling(struct device *dev, struct device_attribute *attr,
                                char *buf)
{
        struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
        u8 data[REPORT_EVENT_PROXIMITY_LEN] = {0};
        u8 data_polling_enable = 0;
        u16 proximity_adc;

        data_polling_enable = CW_MCU_BIT_PROXIMITY_POLLING;

        CWMCU_i2c_write_power(mcu_data, PROXIMITY_SENSORS_STATUS,
				 &data_polling_enable, 1);
        CWMCU_i2c_read_power(mcu_data, CWSTM32_READ_Proximity,
				 data, sizeof(data));
        proximity_adc = (data[2] << 8) | data[1];

        return snprintf(buf, PAGE_SIZE, "ADC[0x%02X] status is %d\n",
				 proximity_adc, data[0]);
}

static ssize_t get_light_polling(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[REPORT_EVENT_COMMON_LEN] = {0};
	u8 data_polling_enable;
	u16 light_adc;
	u8 light_enable;
	u32 sensors_bit;
	int rc;

	mutex_lock(&mcu_data->group_i2c_lock);

        
        sensors_bit = CW_MCU_INT_BIT_LIGHT;
        mcu_data->enabled_list &= ~sensors_bit;
        mcu_data->enabled_list |= sensors_bit;

        light_enable = (u8)(mcu_data->enabled_list);
        rc = CWMCU_i2c_write_power(mcu_data, CWSTM32_ENABLE_REG,
				 &light_enable, 1);
        if (rc) {
                E("%s: CWMCU_i2c_write fails, rc = %d\n",
                  __func__, rc);
		mutex_unlock(&mcu_data->group_i2c_lock);
                return -EIO;
        }

	data_polling_enable = CW_MCU_BIT_LIGHT_POLLING;

	rc = CWMCU_i2c_write_power(mcu_data, LIGHT_SENSORS_STATUS,
			&data_polling_enable, 1);
	if (rc < 0) {
		mutex_unlock(&mcu_data->group_i2c_lock);
		E("%s: write fail, rc = %d\n", __func__, rc);
		return rc;
	}
	rc = CWMCU_i2c_read_power(mcu_data, CWSTM32_READ_Light,
				 data, sizeof(data));
	if (rc < 0) {
		mutex_unlock(&mcu_data->group_i2c_lock);
		E("%s: read fail, rc = %d\n", __func__, rc);
		return rc;
	}
	mutex_unlock(&mcu_data->group_i2c_lock);

	light_adc = (data[2] << 8) | data[1];

	return scnprintf(buf, PAGE_SIZE, "ADC[0x%04X] => level %u\n", light_adc,
					 data[0]);
}


static ssize_t read_mcu_data(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	int i;
	u8 reg_addr;
	u8 len;
	long data_temp[2] = {0};
	u8 mcu_rdata[128] = {0};
	char *str_buf;
	char *running;

	str_buf = kstrndup(buf, count, GFP_KERNEL);
	if (str_buf == NULL) {
		E("%s: cannot allocate buffer\n", __func__);
		return -ENOMEM;
	}
	running = str_buf;

	for (i = 0; i < ARRAY_SIZE(data_temp); i++) {
		int error;
		char *token;

		token = strsep(&running, " ");

		if (i == 0)
			error = kstrtol(token, 16, &data_temp[i]);
		else {
			if (token == NULL) {
				data_temp[i] = 1;
				D("%s: token 2 missing\n", __func__);
				break;
			} else
				error = kstrtol(token, 10, &data_temp[i]);
		}
		if (error) {
			E("%s: kstrtol fails, error = %d, i = %d\n",
			  __func__, error, i);
			kfree(str_buf);
			return error;
		}
	}
	kfree(str_buf);

	
	reg_addr = (u8)(data_temp[0]);
	len = (u8)(data_temp[1]);

	if (len < sizeof(mcu_rdata)) {
		CWMCU_i2c_read_power(mcu_data, reg_addr, mcu_rdata, len);

		for (i = 0; i < len; i++)
			D("read mcu reg_addr = 0x%x, reg[%u] = 0x%x\n",
				reg_addr, (reg_addr + i), mcu_rdata[i]);
	} else
		E("%s: len = %u, out of range\n", __func__, len);

	return count;
}

static inline bool retry_exhausted(struct cwmcu_data *mcu_data)
{
	return ((mcu_data->i2c_total_retry > RETRY_TIMES) ||
		(mcu_data->i2c_latch_retry > LATCH_TIMES));
}

static inline void retry_reset(struct cwmcu_data *mcu_data)
{
	mcu_data->i2c_total_retry = 0;
	mcu_data->i2c_latch_retry = 0;
}

static int CWMCU_i2c_write(struct cwmcu_data *mcu_data,
			  u8 reg_addr, u8 *data, u8 len)
{
	s32 write_res;
	int i;

	if (MCU_IN_DLOAD() || MCU_IN_BOOTLOADER()) {
		I("%s[%d], s_mcu_state:%d, return %d\n", __func__, __LINE__, s_mcu_state, -ENOTCONN);
		if (!in_interrupt())msleep(100);
		return -ENOTCONN;
	}

	if (DEBUG_DISABLE) {
		mcu_data->disable_access_count++;
		if ((mcu_data->disable_access_count % 100) == 0)
			I("%s: DEBUG_DISABLE = %d\n", __func__, DEBUG_DISABLE);
		return len;
	}

	if (mcu_data->is_block_i2c) {
		if (time_after(jiffies,
			       mcu_data->reset_jiffies + RESET_PERIOD)) {
			gpio_direction_input(mcu_data->gpio_reset);
			I("%s: gpio_reset = %d\n", __func__,
			  gpio_get_value_cansleep(mcu_data->gpio_reset));

			if (mcu_chip_mode_get(mcu_data) ==
			    MCU_CHIP_MODE_BOOTLOADER)
				msleep(100);
			else {
				
				usleep_range(500000, 1000000);
			}

			mcu_data->is_block_i2c = false;
		}
		return len;
	}

	mutex_lock(&mcu_data->mutex_lock);
	if (mcu_data->suspended) {
		mutex_unlock(&mcu_data->mutex_lock);
		return len;
	}
	mutex_unlock(&mcu_data->mutex_lock);

	mutex_lock(&s_activated_i2c_lock);
	if (retry_exhausted(mcu_data)) {
		mutex_unlock(&s_activated_i2c_lock);
		D("%s: mcu_data->i2c_total_retry = %d, i2c_latch_retry = %d\n",
		  __func__,
		  mcu_data->i2c_total_retry, mcu_data->i2c_latch_retry);
		
		mcu_data->w_activated_i2c = true;
		queue_work(mcu_data->mcu_wq, &mcu_data->one_shot_work);
		return -EIO;
	}

	for (i = 0; i < len; i++) {
		while (!retry_exhausted(mcu_data)) {
			write_res = i2c_smbus_write_byte_data(mcu_data->client,
						  reg_addr, data[i]);
			if (write_res >= 0) {
				retry_reset(mcu_data);
				break;
			}
			gpio_make_falling_edge(mcu_data->gpio_wake_mcu);
			if (write_res == LATCH_ERROR_NO)
				mcu_data->i2c_latch_retry++;
			mcu_data->i2c_total_retry++;
			E(
			  "%s: i2c write error, write_res = %d, total_retry ="
			  " %d, latch_retry = %d\n", __func__, write_res,
				mcu_data->i2c_total_retry,
				mcu_data->i2c_latch_retry);
		}

		if (retry_exhausted(mcu_data)) {
			mutex_unlock(&s_activated_i2c_lock);
			E("%s: mcu_data->i2c_total_retry = %d, "
			  "i2c_latch_retry = %d, EIO\n", __func__,
			  mcu_data->i2c_total_retry, mcu_data->i2c_latch_retry);
			return -EIO;
		}
	}

	mutex_unlock(&s_activated_i2c_lock);

	return 0;
}

static int CWMCU_i2c_write_block(struct cwmcu_data *mcu_data,
			  u8 reg_addr, u8 *data, u8 len)
{
	s32 write_res;

	if (MCU_IN_DLOAD() || MCU_IN_BOOTLOADER()) {
		I("%s[%d], s_mcu_state:%d, return %d\n", __func__, __LINE__, s_mcu_state, -ENOTCONN);
		if (!in_interrupt())msleep(100);
		return -ENOTCONN;
	}

	if (DEBUG_DISABLE) {
		mcu_data->disable_access_count++;
		if ((mcu_data->disable_access_count % 100) == 0)
			I("%s: DEBUG_DISABLE = %d\n", __func__, DEBUG_DISABLE);
		return len;
	}

	if (mcu_data->is_block_i2c) {
		if (time_after(jiffies,
			       mcu_data->reset_jiffies + RESET_PERIOD)) {
			gpio_direction_input(mcu_data->gpio_reset);
			I("%s: gpio_reset = %d\n", __func__,
			  gpio_get_value_cansleep(mcu_data->gpio_reset));

			if (mcu_chip_mode_get(mcu_data) ==
			    MCU_CHIP_MODE_BOOTLOADER)
				msleep(100);
			else {
				
				usleep_range(500000, 1000000);
			}

			mcu_data->is_block_i2c = false;
		}
		return len;
	}

	mutex_lock(&mcu_data->mutex_lock);
	if (mcu_data->suspended) {
		mutex_unlock(&mcu_data->mutex_lock);
		return len;
	}
	mutex_unlock(&mcu_data->mutex_lock);

	mutex_lock(&s_activated_i2c_lock);
	if (retry_exhausted(mcu_data)) {
		mutex_unlock(&s_activated_i2c_lock);
		D("%s: mcu_data->i2c_total_retry = %d, i2c_latch_retry = %d\n",
		  __func__,
		  mcu_data->i2c_total_retry, mcu_data->i2c_latch_retry);
		
		mcu_data->w_activated_i2c = true;
		queue_work(mcu_data->mcu_wq, &mcu_data->one_shot_work);
		return -EIO;
	}

	if (1) {
		while (!retry_exhausted(mcu_data)) {
			write_res = i2c_smbus_write_i2c_block_data(mcu_data->client,
						  reg_addr, len, data);
			if (write_res >= 0) {
				retry_reset(mcu_data);
				break;
			}
			gpio_make_falling_edge(mcu_data->gpio_wake_mcu);
			if (write_res == LATCH_ERROR_NO)
				mcu_data->i2c_latch_retry++;
			mcu_data->i2c_total_retry++;
			E(
			  "%s: i2c write error, write_res = %d, total_retry ="
			  " %d, latch_retry = %d\n", __func__, write_res,
				mcu_data->i2c_total_retry,
				mcu_data->i2c_latch_retry);
		}

		if (retry_exhausted(mcu_data)) {
			mutex_unlock(&s_activated_i2c_lock);
			E("%s: mcu_data->i2c_total_retry = %d, "
			  "i2c_latch_retry = %d, EIO\n", __func__,
			  mcu_data->i2c_total_retry, mcu_data->i2c_latch_retry);
			return -EIO;
		}
	}

	mutex_unlock(&s_activated_i2c_lock);

	return 0;
}

static int CWMCU_i2c_multi_write(struct cwmcu_data *mcu_data,
			  u8 reg_addr, u8 *data, u8 len)
{
	int rc, i;

	mutex_lock(&mcu_data->group_i2c_lock);

	for (i = 0; i < len; i++) {
		rc = CWMCU_i2c_write(mcu_data, reg_addr, &data[i], 1);
		if (rc) {
			mutex_unlock(&mcu_data->group_i2c_lock);
			E("%s: CWMCU_i2c_write fails, rc = %d, i = %d\n",
			  __func__, rc, i);
			return -EIO;
		}
	}

	mutex_unlock(&mcu_data->group_i2c_lock);
	return 0;
}

static int cwmcu_set_sensor_kvalue(struct cwmcu_data *mcu_data)
{
	

	u8 gs_datax = 0, gs_datay = 0, gs_dataz = 0;
	u8 gy_datax = 0, gy_datay = 0, gy_dataz = 0;
        u8 als_goldh = (mcu_data->als_goldh == 0) ? 0x0A : (mcu_data->als_goldh);
        u8 als_goldl = (mcu_data->als_goldl == 0) ? 0x38 : (mcu_data->als_goldl);
	u8 als_datal = 0, als_datah = 0, als_levell = 0, als_levelh = 0;
	u8 ps_cancl = 0, ps_canch = 0, ps_thdl = 0, ps_thdh = 0;
	u8 bs_dataa = 0, bs_datab = 0, bs_datac = 0, bs_datad = 0;
	int i = 0;
	u8 firmware_version[FW_VER_COUNT] = {0};

	mcu_data->gs_calibrated = 0;
	mcu_data->gy_calibrated = 0;
	mcu_data->ls_calibrated = 0;
	mcu_data->bs_calibrated = 0;

	CWMCU_i2c_read_power(mcu_data, FIRMWARE_VERSION, firmware_version,
		       sizeof(firmware_version));
	I(
	  "Firmware Architecture version %u, Sense version %u,"
	  " Cywee lib version %u, Water number %u"
	  ", Active Engine %u, Project Mapping %u\n",
		firmware_version[0], firmware_version[1], firmware_version[2],
		firmware_version[3], firmware_version[4], firmware_version[5]);

	if ((mcu_data->gs_kvalue & (0x67 << 24)) == (0x67 << 24)) {
		gs_datax = (mcu_data->gs_kvalue >> 16) & 0xFF;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_ACC,
				&gs_datax, 1);
		gs_datay = (mcu_data->gs_kvalue >>  8) & 0xFF;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_ACC,
				&gs_datay, 1);
		gs_dataz = (mcu_data->gs_kvalue) & 0xFF;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_ACC,
				&gs_dataz, 1);
		mcu_data->gs_calibrated = 1;
		D("Set g-sensor kvalue x is %x y is %x z is %x\n",
			gs_datax, gs_datay, gs_dataz);
	}

	if ((mcu_data->gy_kvalue & (0x67 << 24)) == (0x67 << 24)) {
		gy_datax = (mcu_data->gy_kvalue >> 16) & 0xFF;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_GYRO,
				&gy_datax, 1);
		gy_datay = (mcu_data->gy_kvalue >>  8) & 0xFF;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_GYRO,
				&gy_datay, 1);
		gy_dataz = (mcu_data->gy_kvalue) & 0xFF;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_GYRO,
				&gy_dataz, 1);
		mcu_data->gy_calibrated = 1;
		D("Set gyro-sensor kvalue x is %x y is %x z is %x\n",
			gy_datax, gy_datay, gy_dataz);
	}

	if ((mcu_data->als_kvalue & 0x6DA50000) == 0x6DA50000) {
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_LIGHT,
				&als_goldl, 1);
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_LIGHT,
				&als_goldh, 1);
		als_datal = (mcu_data->als_kvalue) & 0xFF;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_LIGHT,
				&als_datal, 1);
		als_datah = (mcu_data->als_kvalue >>  8) & 0xFF;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_LIGHT,
				&als_datah, 1);
		mcu_data->ls_calibrated = 1;
		I("Set light-sensor kvalue is %x %x, gvalue is %x %x\n"
				, als_datah, als_datal, als_goldh, als_goldl);
	}
	else {
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_LIGHT,
				&als_goldl, 1);
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_LIGHT,
				&als_goldh, 1);
		als_datal = als_goldl;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_LIGHT,
				&als_datal, 1);
		als_datah = als_goldh;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_LIGHT,
				&als_datah, 1);
		mcu_data->ls_calibrated = 0;
		I("Set light-sensor kvalue is %x %x, gvalue is %x %x\n"
				, als_datah, als_datal, als_goldh, als_goldl);
	}

	for (i = 0; i<10; i++) {
                als_levell = *(mcu_data->als_levels + i) & 0xFF;
                als_levelh = (*(mcu_data->als_levels + i) >> 8) & 0xFF;
                CWMCU_i2c_write_power(mcu_data,
                                CW_I2C_REG_SENSORS_SET_LEVEL_LIGHT,
                                &als_levell, 1);
                CWMCU_i2c_write_power(mcu_data,
                                CW_I2C_REG_SENSORS_SET_LEVEL_LIGHT,
                                &als_levelh, 1);
               D("Set light-sensor level is %d\n", ((als_levelh << 8) | als_levell));
	}

	if((mcu_data->ps_kheader & 0x50530000) == 0x50530000) {
		if (((mcu_data->ps_kvalue >> 16) & 0xFFFF) == 0xFFFF) {
			ps_cancl = (mcu_data->ps_kvalue) & 0xFF;
                        ps_canch = 0x00;
			CWMCU_i2c_write_power(mcu_data,
					CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
					&ps_cancl, 1);
			CWMCU_i2c_write_power(mcu_data,
                                       	CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       	&ps_canch, 1);

                       	if( (mcu_data->ps_kvalue & 0xFF00) != 0X0000) {
				ps_thdl = (mcu_data->ps_kvalue >> 8) & 0xFF;
                               	ps_thdh = 0x00;
                       	}
                       	else {
				ps_thdl = mcu_data->ps_thd_fixed & 0xFF;
                               	ps_thdh = 0x00;
                       	}
			CWMCU_i2c_write_power(mcu_data,
                                       	CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       	&ps_thdl, 1);
			CWMCU_i2c_write_power(mcu_data,
                                       	CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       	&ps_thdh, 1);
#if 0
			ps_thd_addl = mcu_data->ps_thd_add & 0xFF;
			ps_thd_addh = (mcu_data->ps_thd_add >> 8) & 0xFF;
                        CWMCU_i2c_write(mcu_data,
                                       	CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       	&ps_thd_addl, 1);
                        CWMCU_i2c_write(mcu_data,
                                       	CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       	&ps_thd_addh, 1);
                        I("set proximity-sensor kvalue is %x %x %x %x, "
                               "ps_thd_add is %x\n",
                               ps_cancl, ps_canch, ps_thdl, ps_thdh,
                               ((ps_thd_addh << 8) | ps_thd_addl) );
#endif
                        I("set proximity-sensor kvalue is %x %x %x %x\n",
                               ps_cancl, ps_canch, ps_thdl, ps_thdh);
#if 0
                       	CWMCU_i2c_read(mcu_data,
                                       CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       debug_data, 6);
                        I("[AUSTIN DEBUG]  canc: 0x%X, thd: 0x%X, thd_add: 0x%X\n",
                               ((debug_data[1] << 8) | debug_data[0]),
                               ((debug_data[3] << 8) | debug_data[2]),
                               ((debug_data[5] << 8) | debug_data[4]) );
#endif
                } else {
                        ps_cancl = mcu_data->ps_kvalue & 0xFF;
                        ps_canch = ((mcu_data->ps_kvalue) & 0xFF00) >> 8;
                        CWMCU_i2c_write_power(mcu_data,
                                       CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       &ps_cancl, 1);
                        CWMCU_i2c_write_power(mcu_data,
                                       CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       &ps_canch, 1);
                       	if((mcu_data->ps_kvalue & 0xFFFF0000) != 0X00000000) {
                       	        ps_thdl = (mcu_data->ps_kvalue >> 16) & 0xFF;
                       	        ps_thdh = (mcu_data->ps_kvalue >> 24) & 0xFF;;
                       	}
                       	else {
                       	        ps_thdl = mcu_data->ps_thd_fixed & 0xFF;
                       	        ps_thdh = (mcu_data->ps_thd_fixed >> 8) & 0xFF;
                       	}
                        CWMCU_i2c_write_power(mcu_data,
                                       	CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       	&ps_thdl, 1);
                        CWMCU_i2c_write_power(mcu_data,
                                       	CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       	&ps_thdh, 1);
#if 0
			ps_thd_addl = mcu_data->ps_thd_add & 0xFF;
			ps_thd_addh = (mcu_data->ps_thd_add >> 8) & 0xFF;
                        CWMCU_i2c_write(mcu_data,
                                       	CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       	&ps_thd_addl, 1);
                        CWMCU_i2c_write(mcu_data,
                                       	CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       	&ps_thd_addh, 1);
                        I("set proximity-sensor kvalue is %x %x %x %x, "
                               "ps_thd_add is %x\n",
                               ps_cancl, ps_canch, ps_thdl, ps_thdh,
                               ((ps_thd_addh << 8) | ps_thd_addl) );
#endif
                        I("set proximity-sensor kvalue is %x %x %x %x\n",
                               ps_cancl, ps_canch, ps_thdl, ps_thdh);
#if 0
                       	CWMCU_i2c_read(mcu_data,
                                       CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PROXIMITY,
                                       debug_data, 6);
                        I("[AUSTIN DEBUG]  canc: 0x%X, thd: 0x%X, thd_add: 0x%X\n",
                               ((debug_data[1] << 8) | debug_data[0]),
                               ((debug_data[3] << 8) | debug_data[2]),
                               ((debug_data[5] << 8) | debug_data[4]) );
#endif
                }
                mcu_data->ps_calibrated = 1;
        }

	if (mcu_data->bs_kheader == 0x67) {
		bs_dataa = (mcu_data->bs_kvalue >> 24) & 0xFF;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PRESSURE,
				&bs_dataa, 1);
		bs_datab = (mcu_data->bs_kvalue >> 16) & 0xFF;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PRESSURE,
				&bs_datab, 1);
		bs_datac = (mcu_data->bs_kvalue >> 8) & 0xFF;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PRESSURE,
				&bs_datac, 1);
		bs_datad = (mcu_data->bs_kvalue) & 0xFF;
		CWMCU_i2c_write_power(mcu_data,
				CW_I2C_REG_SENSORS_CALIBRATOR_SET_DATA_PRESSURE,
				&bs_datad, 1);
		mcu_data->bs_calibrated = 1;
		D("Set baro-sensor kvalue a is %x b is %x c is %x d is %x\n",
			bs_dataa, bs_datab, bs_datac, bs_datad);
	}
	I("Sensor calibration matrix is (gs %u gy %u ls %u bs %u)\n",
		mcu_data->gs_calibrated, mcu_data->gy_calibrated,
		mcu_data->ls_calibrated, mcu_data->bs_calibrated);
	if(g_touch_solution == 0 || g_touch_solution == 1)
		CWMCU_i2c_write_power(s_mcu_data, TOUCH_SOLUTION_REGISTER, &g_touch_solution, 1);

	return 0;
}


static int cwmcu_sensor_placement(struct cwmcu_data *mcu_data)
{
	int i, rc;
	u8 cali_data_from_fw;

	I("Set Sensor Placement\n");

	for (i = 0; i < 3; i++) {
		CWMCU_i2c_write_power(mcu_data, GENSOR_POSITION,
				&mcu_data->acceleration_axes,
				1);
		rc = CWMCU_i2c_read_power(mcu_data, GENSOR_POSITION,
			       &cali_data_from_fw, 1);
		if (rc >= 0) {
			if (cali_data_from_fw == mcu_data->acceleration_axes)
				break;
			else {
				I("%s: cali_data_from_fw = 0x%x, "
				  "acceleration_axes = 0x%x\n",
				  __func__, cali_data_from_fw,
				  mcu_data->acceleration_axes);
			}
		} else {
			I("%s: GENSOR_POSITION i2c read fails, rc = %d\n",
			  __func__, rc);
		}
	}

	for (i = 0; i < 3; i++) {
		CWMCU_i2c_write_power(mcu_data, COMPASS_POSITION,
				&mcu_data->magnetic_axes, 1);
		rc = CWMCU_i2c_read_power(mcu_data, COMPASS_POSITION,
			       &cali_data_from_fw, 1);
		if (rc >= 0) {
			if (cali_data_from_fw == mcu_data->magnetic_axes)
				break;
			else {
				I("%s: cali_data_from_fw = 0x%x, "
				  "magnetic_axes = 0x%x\n",
				  __func__, cali_data_from_fw,
				  mcu_data->magnetic_axes);
			}
		} else {
			I("%s: COMPASS_POSITION i2c read fails, rc = %d\n",
			  __func__, rc);
		}
	}

	for (i = 0; i < 3; i++) {
		CWMCU_i2c_write_power(mcu_data, GYRO_POSITION,
				&mcu_data->gyro_axes, 1);
		rc = CWMCU_i2c_read_power(mcu_data, GYRO_POSITION,
			       &cali_data_from_fw, 1);
		if (rc >= 0) {
			if (cali_data_from_fw == mcu_data->gyro_axes)
				break;
			else {
				I("%s: cali_data_from_fw = 0x%x, "
				  "gyro_axes = 0x%x\n",
				  __func__, cali_data_from_fw,
				  mcu_data->gyro_axes);
			}
		} else {
			I("%s: GYRO_POSITION i2c read fails, rc = %d\n",
			  __func__, rc);
		}
	}
	return 0;
}

static int cwmcu_restore_status(struct cwmcu_data *mcu_data)
{
	int i, rc;
	u8 data;
	u8 reg_value = 0;

	D("Restore status\n");

	for (i = 0; i < ENABLE_LIST_GROUP_NUM; i++) {
		data = (u8)(mcu_data->enabled_list>>(i*8));
		CWMCU_i2c_write_power(mcu_data, CWSTM32_ENABLE_REG+i, &data, 1);
		I("%s: write_addr = 0x%x, write_val = 0x%x\n",
		  __func__, (CWSTM32_ENABLE_REG+i), data);
	}

	D("%s: enable_list = 0x%x\n", __func__, mcu_data->enabled_list);

	for (i = 0; i <= CW_GEOMAGNETIC_ROTATION_VECTOR; i++) {
		rc = firmware_odr(mcu_data, i,
			     mcu_data->report_period[i] / MS_TO_PERIOD);
		if (rc) {
			E("%s: firmware_odr fails, rc = %d, i = %d\n",
			  __func__, rc, i);
			return -EIO;
		}
	}

#ifdef MCU_WARN_MSGS
	reg_value = 1;
	rc = CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_WARN_MSG_ENABLE,
			     &reg_value, 1);
	if (rc) {
		E("%s: CWMCU_i2c_write(WARN_MSG) fails, rc = %d, i = %d\n",
		  __func__, rc, i);
		return -EIO;
	}
	D("%s: WARN_MSGS enabled\n", __func__);
#endif

	reg_value = 1;
	rc = CWMCU_i2c_write_power(mcu_data, CW_I2C_REG_WATCH_DOG_ENABLE,
			     &reg_value, 1);
	if (rc) {
		E("%s: CWMCU_i2c_write(WATCH_DOG) fails, rc = %d\n",
		  __func__, rc);
		return -EIO;
	}
	D("%s: Watch dog enabled\n", __func__);

	return 0;
}

static void mcu_set_display_state(bool on_off)
{
    int ret;
    u8 display_state = (u8)on_off;
    ret = CWMCU_i2c_write_block_power(s_mcu_data, CW_I2C_REG_DISPLAY_STATE, (u8*)&display_state, sizeof(display_state));
    I("%s(%d): display_state:0x%x ret:%d\n", __func__, __LINE__, display_state, ret);
}

static int CWMCU_i2c_read(struct cwmcu_data *mcu_data,
			 u8 reg_addr, u8 *data, u8 len)
{
	s32 rc = 0;

	if (MCU_IN_DLOAD() || MCU_IN_BOOTLOADER()) {
		I("%s[%d], s_mcu_state:%d, return %d\n", __func__, __LINE__, s_mcu_state, -ENOTCONN);
		if (!in_interrupt()) msleep(100);
		return -ENOTCONN;
	}

	if (DEBUG_DISABLE) {
		mcu_data->disable_access_count++;
		if ((mcu_data->disable_access_count % 100) == 0)
			I("%s: DEBUG_DISABLE = %d\n", __func__, DEBUG_DISABLE);
		return len;
	}

	if (mcu_data->is_block_i2c) {
		if (time_after(jiffies,
			       mcu_data->reset_jiffies + RESET_PERIOD)) {
			gpio_direction_input(mcu_data->gpio_reset);
			I("%s: gpio_reset = %d\n", __func__,
			  gpio_get_value_cansleep(mcu_data->gpio_reset));

			if (mcu_chip_mode_get(mcu_data) ==
			    MCU_CHIP_MODE_BOOTLOADER)
				msleep(100);
			else {
				
				usleep_range(500000, 1000000);
			}

			mcu_data->is_block_i2c = false;
		}
		return len;
	}

	mutex_lock(&mcu_data->mutex_lock);
	if (mcu_data->suspended) {
		mutex_unlock(&mcu_data->mutex_lock);
		return len;
	}
	mutex_unlock(&mcu_data->mutex_lock);

	mutex_lock(&s_activated_i2c_lock);
	if (retry_exhausted(mcu_data)) {
		for (rc = 0; rc < len; rc++)
			data[rc] = 0; 

		
		D(
		  "%s: mcu_data->i2c_total_retry = %d, "
		  "mcu_data->i2c_latch_retry = %d\n", __func__,
		  mcu_data->i2c_total_retry,
		  mcu_data->i2c_latch_retry);
		mcu_data->w_activated_i2c = true;
		queue_work(mcu_data->mcu_wq, &mcu_data->one_shot_work);

		mutex_unlock(&s_activated_i2c_lock);
		return len;
	}

	while (!retry_exhausted(mcu_data)) {
		rc = i2c_smbus_read_i2c_block_data(mcu_data->client, reg_addr,
						   len, data);
		if (rc == len) {
			retry_reset(mcu_data);
			break;
		} else {
			gpio_make_falling_edge(mcu_data->gpio_wake_mcu);
			mcu_data->i2c_total_retry++;
			if (rc == LATCH_ERROR_NO)
				mcu_data->i2c_latch_retry++;
			E("%s: rc = %d, total_retry = %d, latch_retry = %d\n",
			  __func__,
			  rc, mcu_data->i2c_total_retry,
			  mcu_data->i2c_latch_retry);
		}
	}

	if (retry_exhausted(mcu_data)) {
		E("%s: total_retry = %d, latch_retry = %d, return\n",
		  __func__, mcu_data->i2c_total_retry,
		  mcu_data->i2c_latch_retry);
	}

	mutex_unlock(&s_activated_i2c_lock);

	return rc;
}

static bool reset_hub(struct cwmcu_data *mcu_data, bool force_reset)
{
	if (force_reset || time_after(jiffies, mcu_data->reset_jiffies + RESET_PERIOD)) {
		gpio_direction_output(mcu_data->gpio_reset, 0);
		I("%s: gpio_reset = %d\n", __func__,
		  gpio_get_value_cansleep(mcu_data->gpio_reset));
		usleep_range(10000, 15000);
		gpio_direction_input(mcu_data->gpio_reset);
		I("%s: gpio_reset = %d\n", __func__,
		  gpio_get_value_cansleep(mcu_data->gpio_reset));

		retry_reset(mcu_data);
		mcu_boot_status_reset(mcu_data);
		mcu_data->i2c_jiffies = jiffies;

		if (mcu_chip_mode_get(mcu_data) == MCU_CHIP_MODE_BOOTLOADER) {
			msleep(100);
		}
		else {
			
			usleep_range(500000, 1000000);
		}

		mcu_data->is_block_i2c = false;
	} else {
		gpio_direction_output(mcu_data->gpio_reset, 0);
		I("%s: else: gpio_reset = %d\n", __func__,
		  gpio_get_value_cansleep(mcu_data->gpio_reset));
		mcu_data->is_block_i2c = true;
	}

	mcu_data->reset_jiffies = jiffies;
	return !mcu_data->is_block_i2c;
}

static int firmware_odr(struct cwmcu_data *mcu_data, int sensors_id,
			int delay_ms)
{
	u8 reg_addr;
	u8 reg_value;
	int rc;

	switch (sensors_id) {
	case CW_ACCELERATION:
		reg_addr = ACCE_UPDATE_RATE;
		break;
	case CW_MAGNETIC:
		reg_addr = MAGN_UPDATE_RATE;
		break;
	case CW_GYRO:
		reg_addr = GYRO_UPDATE_RATE;
		break;
	case CW_ORIENTATION:
		reg_addr = ORIE_UPDATE_RATE;
		break;
	case CW_ROTATIONVECTOR:
		reg_addr = ROTA_UPDATE_RATE;
		break;
	case CW_LINEARACCELERATION:
		reg_addr = LINE_UPDATE_RATE;
		break;
	case CW_GRAVITY:
		reg_addr = GRAV_UPDATE_RATE;
		break;
	case CW_MAGNETIC_UNCALIBRATED:
		reg_addr = MAGN_UNCA_UPDATE_RATE;
		break;
	case CW_GYROSCOPE_UNCALIBRATED:
		reg_addr = GYRO_UNCA_UPDATE_RATE;
		break;
	case CW_GAME_ROTATION_VECTOR:
		reg_addr = GAME_ROTA_UPDATE_RATE;
		break;
	case CW_GEOMAGNETIC_ROTATION_VECTOR:
		reg_addr = GEOM_ROTA_UPDATE_RATE;
		break;
	case CW_SIGNIFICANT_MOTION:
		reg_addr = SIGN_UPDATE_RATE;
		break;
	case CW_PRESSURE:
		reg_addr = PRESSURE_UPDATE_RATE;
		break;
	default:
		reg_addr = 0;
		D("%s: Only report_period changed, sensors_id = %d,"
			" delay_us = %6d\n",
			__func__, sensors_id,
			mcu_data->report_period[sensors_id]);
		return 0;
	}

	if (delay_ms >= 200)
		reg_value = UPDATE_RATE_NORMAL;
	else if (delay_ms >= 100)
		reg_value = UPDATE_RATE_RATE_10Hz;
	else if (delay_ms >= 60)
		reg_value = UPDATE_RATE_UI;
	else if (delay_ms >= 40)
		reg_value = UPDATE_RATE_RATE_25Hz;
	else if (delay_ms >= 20)
		reg_value = UPDATE_RATE_GAME;
	else
		reg_value = UPDATE_RATE_FASTEST;

	D("%s: reg_addr = 0x%x, reg_value = 0x%x\n",
	  __func__, reg_addr, reg_value);
	rc = CWMCU_i2c_write_power(mcu_data, reg_addr, &reg_value, 1);
	if (rc) {
		E("%s: CWMCU_i2c_write fails, rc = %d\n", __func__, rc);
		return -EIO;
	}

	return 0;
}

int is_continuous_sensor(int sensors_id)
{
	switch (sensors_id) {
	case CW_ACCELERATION:
	case CW_MAGNETIC:
	case CW_GYRO:
	case CW_PRESSURE:
	case CW_ORIENTATION:
	case CW_ROTATIONVECTOR:
	case CW_LINEARACCELERATION:
	case CW_GRAVITY:
	case CW_MAGNETIC_UNCALIBRATED:
	case CW_GYROSCOPE_UNCALIBRATED:
	case CW_GAME_ROTATION_VECTOR:
	case CW_GEOMAGNETIC_ROTATION_VECTOR:
		return 1;
		break;
	default:
		return 0;
		break;
	}
}

static void setup_delay(struct cwmcu_data *mcu_data)
{
	u8 i;
	int delay_ms;
	int delay_candidate_ms;

	delay_candidate_ms = CWMCU_NO_POLLING_DELAY;
	for (i = 0; i < CW_SENSORS_ID_END; i++) {
		if ((mcu_data->enabled_list & (1 << i)) &&
		    is_continuous_sensor(i) &&
		    !(mcu_data->batched_list & (1 << i))) {
			D("%s: report_period[%d] = %d\n", __func__, i,
			  mcu_data->report_period[i]);

			delay_ms = mcu_data->report_period[i] / MS_TO_PERIOD;
			if (delay_ms > CWMCU_MAX_DELAY)
				delay_ms = CWMCU_MAX_DELAY;

			if (delay_candidate_ms > delay_ms)
				delay_candidate_ms = delay_ms;
			}
	}

	if (delay_candidate_ms != atomic_read(&mcu_data->delay)) {
		atomic_set(&mcu_data->delay, delay_candidate_ms);
		cancel_delayed_work_sync(&mcu_data->work);
		queue_delayed_work(mcu_data->mcu_wq, &mcu_data->work, 0);
	}

	D("%s: Minimum delay = %dms\n", __func__,
	  atomic_read(&mcu_data->delay));

}

static int handle_batch_params(struct cwmcu_data *mcu_data,
			       size_t count,
			       int sensors_id,
			       s64 timeout)
{
	int rc;
	u8 i;
	__le32 timeout_data;
	u8 data;
	u32 continuous_sensor_count;

	switch (sensors_id) {
	case CW_ACCELERATION:
	case CW_MAGNETIC:
	case CW_GYRO:
	case CW_PRESSURE:
	case CW_ORIENTATION:
	case CW_ROTATIONVECTOR:
	case CW_LINEARACCELERATION:
	case CW_GRAVITY:
	case CW_MAGNETIC_UNCALIBRATED:
	case CW_GYROSCOPE_UNCALIBRATED:
	case CW_GAME_ROTATION_VECTOR:
	case CW_GEOMAGNETIC_ROTATION_VECTOR:
	case CW_STEP_DETECTOR:
	case CW_STEP_COUNTER:
		break;
	case CW_LIGHT:
	case CW_SIGNIFICANT_MOTION:
	default:
		D("%s: Batch not supported for this sensor_id = 0x%x\n",
		  __func__, sensors_id);
		return count;
	}

	mcu_data->sensors_batch_timeout[sensors_id] = timeout;

	for (continuous_sensor_count = 0, i = 0; i < CW_SENSORS_ID_END; i++) {
		if (mcu_data->sensors_batch_timeout[i] != 0) {
			if ((mcu_data->current_timeout >
			     mcu_data->sensors_batch_timeout[i]) ||
			    (mcu_data->current_timeout == 0)) {
				mcu_data->current_timeout =
					mcu_data->sensors_batch_timeout[i];
			}
			D("sensorid = %d, current_timeout = %lld\n",
			  i, mcu_data->current_timeout);
		} else
			continuous_sensor_count++;
	}

	if (continuous_sensor_count != CW_SENSORS_ID_END)
		mcu_data->batch_enabled = true;
	else {
		mcu_data->batch_enabled = false;
		mcu_data->current_timeout = 0;
	}

	if ((timeout > 0) && (timeout < CWMCU_BATCH_TIMEOUT_MIN))
		timeout = CWMCU_BATCH_TIMEOUT_MIN;

	mcu_data->sensors_batch_timeout[sensors_id] = timeout;

	timeout_data = cpu_to_le32(mcu_data->current_timeout);

	mcu_data->batched_list &= ~(1<<sensors_id);
	if (timeout > 0) {
		mcu_data->batched_list |= (mcu_data->batch_enabled <<
						sensors_id);
	}

	setup_delay(mcu_data);

	i = (sensors_id / 8);

	data = (u8)(mcu_data->batched_list>>(i*8));

	D("%s: Writing, addr = 0x%x, data = 0x%x, i = %d\n", __func__,
	  (CW_BATCH_ENABLE_REG+i), data, i);

	cwmcu_powermode_switch(mcu_data, 1);

	rc = CWMCU_i2c_write_power(mcu_data,
			     CW_BATCH_ENABLE_REG+i,
			     &data, 1);
	if (rc)
		E("%s: CWMCU_i2c_write fails, rc = %d\n",
		  __func__, rc);

	D(
	  "%s: Writing, write_addr = 0x%x, current_timeout = %lld,"
	  " timeout_data = 0x%x\n",
	  __func__, CWSTM32_BATCH_MODE_TIMEOUT, mcu_data->current_timeout,
	  timeout_data);

	rc = CWMCU_i2c_multi_write(mcu_data,
				   CWSTM32_BATCH_MODE_TIMEOUT,
				   (u8 *)&timeout_data,
				   sizeof(timeout_data));
	cwmcu_powermode_switch(mcu_data, 0);
	if (rc)
		E("%s: CWMCU_i2c_write fails, rc = %d\n", __func__, rc);
	else
		rc = count;

	return rc;
}
#if defined(CONFIG_SYNC_TOUCH_STATUS)
int touch_status(u8 status){
    int ret = -1;

    if (s_mcu_data->probe_success != 1)
        return ret;

    if(status == 1 || status == 0){
        ret = CWMCU_i2c_write_power(s_mcu_data, TOUCH_STATUS_REGISTER, &status, 1);
        I("[TP][SensorHub] touch_status = %d\n", status);
    }
    return ret;
}
#endif

int touch_solution(u8 solution){
	if(g_mcu_ready && (solution == 1 || solution == 0) ){
		CWMCU_i2c_write_power(s_mcu_data, TOUCH_SOLUTION_REGISTER, &solution, 1);
		I("[TP][SensorHub] touch_solution = %d\n", solution);
	}
	g_touch_solution = solution;
	return 0;
}

static ssize_t active_set(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	long enabled = 0;
	long sensors_id = 0;
	u8 data;
	u8 i;
	char *str_buf;
	char *running;
	u32 sensors_bit;
	int rc;

	str_buf = kstrndup(buf, count, GFP_KERNEL);
	if (str_buf == NULL) {
		E("%s: cannot allocate buffer\n", __func__);
		return -ENOMEM;
	}
	running = str_buf;

	for (i = 0; i < 2; i++) {
		int error;
		char *token;

		token = strsep(&running, " ");

		if (i == 0)
			error = kstrtol(token, 10, &sensors_id);
		else {
			if (token == NULL) {
				enabled = sensors_id;
				sensors_id = 0;
			} else
				error = kstrtol(token, 10, &enabled);
		}
		if (error) {
			E("%s: kstrtol fails, error = %d, i = %d\n",
				__func__, error, i);
			kfree(str_buf);
			return error;
		}
	}
	kfree(str_buf);

	if (!mcu_data->probe_success)
		return -EBUSY;

	if((!(mcu_data->touch_enable)) && sensors_id == 24){
		return 0;
	}
	if ((sensors_id >= CW_SENSORS_ID_END) ||
	    (sensors_id < 0)
	   ) {
		E("%s: Invalid sensors_id = %ld\n", __func__, sensors_id);
		return -EINVAL;
	}

	if ((sensors_id == HTC_ANY_MOTION) && mcu_data->power_key_pressed) {
		I("%s: Any_Motion && power_key_pressed\n", __func__);
		return count;
	}

	sensors_bit = (1 << sensors_id);
	mcu_data->enabled_list &= ~sensors_bit;
	mcu_data->enabled_list |= enabled ? sensors_bit : 0;

	if ((mcu_data->batched_list & sensors_bit) && (!enabled))
		handle_batch_params(mcu_data, 0, sensors_id, 0);

	
	if (enabled == 0) {
		mcu_data->sensors_batch_timeout[sensors_id] = 0;
		mcu_data->sensors_time[sensors_id] = 0;
	} else {
		do_gettimeofday(&mcu_data->now);
		mcu_data->sensors_time[sensors_id] =
			(mcu_data->now.tv_sec * NS_PER_US) +
			mcu_data->now.tv_usec;
	}

	cwmcu_powermode_switch(mcu_data, 1);

	i = sensors_id / 8;
	data = (u8)(mcu_data->enabled_list>>(i*8));
	D("%s: i2c_write: data = 0x%x, CWSTM32_ENABLE_REG+i = 0x%x\n",
	  __func__, data, CWSTM32_ENABLE_REG+i);
	rc = CWMCU_i2c_write_power(mcu_data, CWSTM32_ENABLE_REG+i, &data, 1);
	if (rc) {
		E("%s: CWMCU_i2c_write fails, rc = %d\n",
		  __func__, rc);
		cwmcu_powermode_switch(mcu_data, 0);
		return -EIO;
	}

	if (enabled == 0)
		mcu_data->report_period[sensors_id] = 200000 * MS_TO_PERIOD;

	if ((sensors_id == CW_STEP_COUNTER) && (!!enabled)) {
		__le16 data[2] = {0};

		rc = CWMCU_i2c_read(mcu_data,
				    CWSTM32_READ_STEP_COUNTER,
				    (u8 *)data, sizeof(data));
		if (rc >= 0) {
			u16 data_buff[REPORT_EVENT_COMMON_LEN];

			mcu_data->sensors_time[step_counter] = 0;

			data_buff[0] = le16_to_cpup(data);
			data_buff[1] = le16_to_cpup(data + 1);
			cw_send_event(mcu_data, CW_STEP_COUNTER, data_buff, 0);

			D("%s: Initial Step Counter, step(data_buf) = %u\n",
			  __func__, *(u32 *)&data_buff[0]);
		} else
			D("%s: Step Counter i2c read fails, rc = %d\n",
			  __func__, rc);
	}

	setup_delay(mcu_data);

	rc = firmware_odr(mcu_data, sensors_id,
			  mcu_data->report_period[sensors_id] / MS_TO_PERIOD);
	cwmcu_powermode_switch(mcu_data, 0);
	if (rc) {
		E("%s: firmware_odr fails, rc = %d\n", __func__, rc);
	}

        
        if (sensors_id == CW_PROXIMITY) {
                I("%s: Initial proximitysensor = %d\n",
                  __func__, mcu_data->light_last_data[0]);
                if (!enabled)
                        p_status = 1;
                else
                        p_status = 9;
        }

	if ((sensors_id == CW_LIGHT) && (!!enabled)) {
		D("%s: Initial lightsensor = %d\n",
		  __func__, mcu_data->light_last_data[0]);
		cw_send_event(mcu_data, CW_LIGHT, &mcu_data->light_last_data[0],
			      0);
	}

	D("%s: sensors_id = %ld, enable = %ld, enable_list = 0x%x\n",
		__func__, sensors_id, enabled, mcu_data->enabled_list);

	return count;
}

static ssize_t active_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data;

	CWMCU_i2c_read_power(mcu_data, CWSTM32_ENABLE_REG, &data, sizeof(data));

	D("%s: enable[1] = 0x%x\n", __func__, data);

	return scnprintf(buf, PAGE_SIZE, "%u\n", mcu_data->enabled_list);
}

static ssize_t interval_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&mcu_data->delay));
}

static ssize_t interval_set(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	long val = 0;
	long sensors_id = 0;
	int i, rc;
	char *str_buf;
	char *running;

	str_buf = kstrndup(buf, count, GFP_KERNEL);
	if (str_buf == NULL) {
		E("%s: cannot allocate buffer\n", __func__);
		return -ENOMEM;
	}
	running = str_buf;

	for (i = 0; i < 2; i++) {
		int error;
		char *token;

		token = strsep(&running, " ");

		if (i == 0)
			error = kstrtol(token, 10, &sensors_id);
		else {
			if (token == NULL) {
				val = 66;
				D("%s: delay set to 66\n", __func__);
			} else
				error = kstrtol(token, 10, &val);
		}
		if (error) {
			E("%s: kstrtol fails, error = %d, i = %d\n",
				__func__, error, i);
			kfree(str_buf);
			return error;
		}
	}
	kfree(str_buf);

	if ((sensors_id < 0) || (sensors_id >= num_sensors)) {
		D("%s: Invalid sensors_id = %ld\n", __func__, sensors_id);
		return -EINVAL;
	}

	if (mcu_data->report_period[sensors_id] != val * MS_TO_PERIOD) {
		
		mcu_data->report_period[sensors_id] = val * MS_TO_PERIOD;

		setup_delay(mcu_data);

		cwmcu_powermode_switch(mcu_data, 1);
		rc = firmware_odr(mcu_data, sensors_id, val);
		cwmcu_powermode_switch(mcu_data, 0);
		if (rc) {
			E("%s: firmware_odr fails, rc = %d\n", __func__, rc);
			return rc;
		}
	}

	return count;
}


static ssize_t batch_set(struct device *dev,
		     struct device_attribute *attr,
		     const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	s64 timeout = 0;
	int sensors_id = 0, flag = 0, delay_ms = 0;
	u8 i;
	int retry;
	int rc;
	char *token;
	char *str_buf;
	char *running;
	long input_val;
	unsigned long long input_val_l;
	bool need_update_fw_odr;
	s32 period;

	if (!mcu_data->probe_success) {
		E("%s: probe_success = %d\n", __func__,
		  mcu_data->probe_success);
		return -1;
	}

	for (retry = 0; retry < ACTIVE_RETRY_TIMES; retry++) {
		mutex_lock(&mcu_data->mutex_lock);
		if (mcu_data->suspended) {
			mutex_unlock(&mcu_data->mutex_lock);
			D("%s: suspended, retry = %d\n",
				__func__, retry);
			usleep_range(5000, 10000);
		} else {
			mutex_unlock(&mcu_data->mutex_lock);
			break;
		}
	}
	if (retry >= ACTIVE_RETRY_TIMES) {
		D("%s: resume not completed, retry = %d, retry fails!\n",
			__func__, retry);
		return -ETIMEDOUT;
	}

	str_buf = kstrndup(buf, count, GFP_KERNEL);
	if (str_buf == NULL) {
		E("%s: cannot allocate buffer\n", __func__);
		return -1;
	}
	running = str_buf;

	for (i = 0; i < 4; i++) {
		token = strsep(&running, " ");
		if (token == NULL) {
			E("%s: token = NULL, i = %d\n", __func__, i);
			break;
		}

		switch (i) {
		case 0:
			rc = kstrtol(token, 10, &input_val);
			sensors_id = (int)input_val;
			break;
		case 1:
			rc = kstrtol(token, 10, &input_val);
			flag = (int)input_val;
			break;
		case 2:
			rc = kstrtol(token, 10, &input_val);
			delay_ms = (int)input_val;
			break;
		case 3:
			rc = kstrtoull(token, 10, &input_val_l);
			timeout = (s64)input_val_l;
			break;
		default:
			E("%s: Unknown i = %d\n", __func__, i);
			break;
		}

		if (rc) {
			E("%s: kstrtol fails, rc = %d, i = %d\n",
			  __func__, rc, i);
			kfree(str_buf);
			return rc;
		}
	}
	kfree(str_buf);

	D("%s: sensors_id = 0x%x, flag = %d, delay_ms = %d, timeout = %lld\n",
	  __func__, sensors_id, flag, delay_ms, timeout);

	
	period = delay_ms * MS_TO_PERIOD;
	need_update_fw_odr = mcu_data->report_period[sensors_id] != period;
	mcu_data->report_period[sensors_id] = period;

	rc = handle_batch_params(mcu_data, count, sensors_id, timeout);

	if ((need_update_fw_odr == true) &&
	    (mcu_data->enabled_list & (1 << sensors_id))) {
		cwmcu_powermode_switch(mcu_data, 1);
		rc = firmware_odr(mcu_data, sensors_id, delay_ms);
		cwmcu_powermode_switch(mcu_data, 0);
		if (rc) {
			E("%s: firmware_odr fails, rc = %d\n", __func__, rc);
		}
	}

	D(
	  "%s: sensors_id = %d, timeout = %lld, batched_list = 0x%x,"
	  " delay_ms = %d\n",
	  __func__, sensors_id, timeout, mcu_data->batched_list,
	  delay_ms);

	return rc;
}

static ssize_t batch_show(struct device *dev, struct device_attribute *attr,
		      char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u64 timestamp = 0;
	struct timespec kt;
	u64 k_timestamp;

	kt = current_kernel_time();

	CWMCU_i2c_read_power(mcu_data, CW_I2C_REG_MCU_TIME, (u8 *)&timestamp,
			     sizeof(timestamp));

	le64_to_cpus(&timestamp);

	k_timestamp = (u64)(kt.tv_sec*NSEC_PER_SEC) + (u64)kt.tv_nsec;

	return scnprintf(buf, PAGE_SIZE, "%llu", timestamp);
}


static ssize_t flush_show(struct device *dev, struct device_attribute *attr,
		      char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	int ret;
	u8 data[4] = {0};

	ret = CWMCU_i2c_read_power(mcu_data, CWSTM32_BATCH_MODE_DATA_COUNTER,
			     data, sizeof(data));
	if (ret < 0)
		D("%s: Read Counter fail, ret = %d\n", __func__, ret);

	D("%s: DEBUG: Queue counter = %d\n", __func__,
	  *(u32 *)&data[0]);

	return scnprintf(buf, PAGE_SIZE, "Queue counter = %d\n",
			 *(u32 *)&data[0]);
}

static void cwmcu_send_flush(struct cwmcu_data *mcu_data, int id)
{
	u8 type = CW_META_DATA;
	u16 data[REPORT_EVENT_COMMON_LEN];
	s64 timestamp = 0;
	int rc;

	data[0] = (u16)id;
	data[1] = data[2] = 0;

	D("%s: flush sensor: %d!!\n", __func__, id);

	mutex_lock(&mcu_data->lock);
	rc = cw_send_event(mcu_data, type, data, timestamp);
	mutex_unlock(&mcu_data->lock);
	if (rc < 0)
		E("%s: send_event fails, rc = %d\n", __func__, rc);

	D("%s--:\n", __func__);
}

static ssize_t flush_set(struct device *dev, struct device_attribute *attr,
		     const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data;
	unsigned long handle;
	int rc;

	rc = kstrtoul(buf, 10, &handle);
	if (rc) {
		E("%s: kstrtoul fails, rc = %d\n", __func__, rc);
		return rc;
	}

	D("%s: handle = %lu\n", __func__, handle);

	cwmcu_powermode_switch(mcu_data, 1);

	if (handle == TIMESTAMP_SYNC_CODE) {
		data = SYNC_TIMESTAMP_BIT;
		rc = CWMCU_i2c_write(mcu_data,
				     CWSTM32_BATCH_MODE_COMMAND,
				     &data, 1);
		if (rc)
			E("%s: i2c_write fails22, rc = %d\n", __func__, rc);
	} else {
		data = 0x01;

		D("%s: addr = 0x%x, data = 0x%x\n", __func__,
		  CWSTM32_BATCH_MODE_COMMAND, data);

		rc = CWMCU_i2c_write(mcu_data, CWSTM32_BATCH_MODE_COMMAND,
				     &data, 1);
		if (rc)
			E("%s: CWMCU_i2c_write fails, rc = %d\n", __func__, rc);

		cwmcu_send_flush(mcu_data, handle);
	}

	cwmcu_powermode_switch(mcu_data, 0);

	return count;
}

static ssize_t facedown_set(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t size)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	bool on;

	if (strtobool(buf, &on) < 0)
		return -EINVAL;

	if (!!on == !!(mcu_data->enabled_list & (1 << HTC_FACEDOWN_DETECTION)))
		return size;

	if (on)
		mcu_data->enabled_list |= (1 << HTC_FACEDOWN_DETECTION);
	else
		mcu_data->enabled_list &= ~(1 << HTC_FACEDOWN_DETECTION);

	mcu_data->w_facedown_set = true;
	queue_work(mcu_data->mcu_wq, &mcu_data->one_shot_work);

	return size;
}

static ssize_t facedown_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n",
		!!(mcu_data->enabled_list & (1U << HTC_FACEDOWN_DETECTION)));
}

static void report_iio(struct cwmcu_data *mcu_data, int *i, u8 *data,
		       __le64 *data64, u32 *event_count)
{
	s32 ret;
	u8 data_buff;
	s16 data_event[3];
	s16 bias_event[3];
	u16 timestamp_event;

	if (data[0] == CW_META_DATA) {
		__le16 *data16 = (__le16 *)(data + 1);

		data_event[0] = le16_to_cpup(data16 + 1);
		D("CW_META_DATA return flush event_id = %d complete~!!\n",
		  data[3]);
		cw_send_event(mcu_data, data[0], data_event, 0);
	} else if (data[0] == CW_TIME_BASE) {
		u64 timestamp;

		timestamp = le64_to_cpup(data64 + 1);

		D("CW_TIME_BASE = %llu\n", timestamp);
		mcu_data->time_base = timestamp;
	} else if ((data[0] == CW_MAGNETIC_UNCALIBRATED_BIAS) ||
		   (data[0] == CW_GYROSCOPE_UNCALIBRATED_BIAS)) {
		__le16 *data16 = (__le16 *)(data + 1);

		data_buff = (data[0] == CW_MAGNETIC_UNCALIBRATED_BIAS) ?
				CW_MAGNETIC_UNCALIBRATED :
				CW_GYROSCOPE_UNCALIBRATED;

		bias_event[0] = le16_to_cpup(data16 + 1);
		bias_event[1] = le16_to_cpup(data16 + 2);
		bias_event[2] = le16_to_cpup(data16 + 3);

		ret = CWMCU_i2c_read(mcu_data, CWSTM32_BATCH_MODE_DATA_QUEUE,
				     data, 9);
		if (ret >= 0) {
			(*i)++;
			timestamp_event = le16_to_cpup(data16);
			data_event[0] = le16_to_cpup(data16 + 1);
			data_event[1] = le16_to_cpup(data16 + 2);
			data_event[2] = le16_to_cpup(data16 + 3);

			if (DEBUG_FLAG_GSENSOR == 1) {
				I(
				  "Batch data: total count = %u, current "
				  "count = %d, event_id = %d, data(x, y, z) = "
				  "(%d, %d, %d), bias(x, y,  z) = "
				  "(%d, %d, %d)\n"
				  , *event_count, *i, data_buff
				  , data_event[0], data_event[1], data_event[2]
				  , bias_event[0], bias_event[1]
				  , bias_event[2]);
			}

			cw_send_event_special(mcu_data, data_buff,
					      data_event,
					      bias_event,
					      timestamp_event +
					      mcu_data->time_base);
		} else {
			E("Read Uncalibrated data fails, ret = %d\n", ret);
		}
	} else {
		__le16 *data16 = (__le16 *)(data + 1);

		timestamp_event = le16_to_cpup(data16);
		data_event[0] = le16_to_cpup(data16 + 1);
		data_event[1] = le16_to_cpup(data16 + 2);
		data_event[2] = le16_to_cpup(data16 + 3);

		if (DEBUG_FLAG_GSENSOR == 1) {
			I(
			  "Batch data: total count = %u, current count = %d, "
			  "event_id = %d, data(x, y, z) = (%d, %d, %d), "
			  "timediff = %d, time_base = %llu, r_time = %llu\n"
			  , *event_count, *i, data[0]
			  , data_event[0], data_event[1], data_event[2]
			  , timestamp_event
			  , mcu_data->time_base
			  , mcu_data->time_base + timestamp_event
			  );
		}
		if ((data[0] == CW_MAGNETIC) || (data[0] == CW_ORIENTATION)) {
			int rc;
			u8 accuracy;
			u16 bias_event[REPORT_EVENT_COMMON_LEN] = {0};

			rc = CWMCU_i2c_read(mcu_data,
					    CW_I2C_REG_SENSORS_ACCURACY_MAG,
					    &accuracy, 1);
			if (rc < 0) {
				E(
				  "%s: read ACCURACY_MAG fails, rc = "
				  "%d\n", __func__, rc);
				accuracy = 3;
			}
			bias_event[0] = accuracy;

			cw_send_event_special(mcu_data, data[0], data_event,
					      bias_event,
					      timestamp_event +
					      mcu_data->time_base);
		} else {
			cw_send_event(mcu_data, data[0], data_event,
				      timestamp_event + mcu_data->time_base);
		}
	}
}

static void cwmcu_batch_read(struct cwmcu_data *mcu_data)
{
	s32 ret;
	int i;
	u32 *event_count;
	u8 event_count_data[4] = {0};

	


	ret = CWMCU_i2c_read(mcu_data, CWSTM32_BATCH_MODE_DATA_COUNTER,
			     event_count_data,
			     sizeof(event_count_data));
	if (ret < 0)
		D("Read Batched data Counter fail, ret = %d\n", ret);

	event_count = (u32 *)(&event_count_data[0]);

	if (DEBUG_FLAG_GSENSOR == 1)
		I("%s: event_count = %u\n", __func__, *event_count);

	for (i = 0; i < *event_count; i++) {
		__le64 data64[2];
		u8 *data = (u8 *)data64;

		data = data + 7;

		ret = CWMCU_i2c_read(mcu_data,
				     CWSTM32_BATCH_MODE_DATA_QUEUE,
				     data, 9);
		if (ret >= 0) {
			
			if (data[0] != CWMCU_NODATA) {
				report_iio(mcu_data, &i, data, data64,
					   event_count);
			}
		} else
			E("Read Queue fails, ret = %d\n", ret);
	}
}

static void cwmcu_check_sensor_update(struct cwmcu_data *mcu_data)
{
	int id;
	s64 temp;

	do_gettimeofday(&mcu_data->now);
	temp = (mcu_data->now.tv_sec * NS_PER_US) + mcu_data->now.tv_usec;

	for (id = 0; id < CW_SENSORS_ID_END; id++) {
		mcu_data->time_diff[id] = temp - mcu_data->sensors_time[id];

		if ((mcu_data->time_diff[id] >= mcu_data->report_period[id])
		    && (mcu_data->enabled_list & (1 << id))) {
			mcu_data->sensors_time[id] = temp;
			mcu_data->update_list |= (1 << id);
		} else
			mcu_data->update_list &= ~(1 << id);
	}
}

static void cwmcu_read(struct cwmcu_data *mcu_data, struct iio_poll_func *pf)
{
	int id_check;

	if (!mcu_data->probe_success) {
		E("%s: probe_success = %d\n", __func__,
		  mcu_data->probe_success);
		return;
	}

	if (mcu_data->enabled_list) {

		cwmcu_check_sensor_update(mcu_data);

		for (id_check = 0 ;
		     (id_check < CW_SENSORS_ID_END)
		     ; id_check++) {
			if ((is_continuous_sensor(id_check)) &&
			    (mcu_data->update_list & (1<<id_check)) &&
			    (mcu_data->sensors_batch_timeout[id_check] == 0)) {
				cwmcu_powermode_switch(mcu_data, 1);
				cwmcu_batch_read(mcu_data);
				cwmcu_powermode_switch(mcu_data, 0);
			}
		}
	}

}

static int cwmcu_suspend(struct device *dev)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	int i;

	D("[CWMCU] %s\n", __func__);

	disable_irq(mcu_data->IRQ);

	mutex_lock(&mcu_data->mutex_lock);
	mcu_data->suspended = true;
	mutex_unlock(&mcu_data->mutex_lock);

	for (i = 0; (mcu_data->power_on_counter != 0) &&
		    (gpio_get_value(mcu_data->gpio_wake_mcu) != 1) &&
		    (i < ACTIVE_RETRY_TIMES); i++)
		usleep_range(10, 20);

	gpio_set_value(mcu_data->gpio_wake_mcu, 1);
	mcu_data->power_on_counter = 0;

	return 0;
}


static int cwmcu_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct cwmcu_data *mcu_data = i2c_get_clientdata(client);

	D("[CWMCU] %s++\n", __func__);

	mutex_lock(&mcu_data->mutex_lock);
	mcu_data->suspended = false;
	mutex_unlock(&mcu_data->mutex_lock);

	enable_irq(mcu_data->IRQ);

	D("[CWMCU] %s--\n", __func__);
	return 0;
}


#ifdef MCU_WARN_MSGS
static void print_warn_msg(struct cwmcu_data *mcu_data,
			   char *buf, u32 len, u32 index)
{
	int ret;
	char *buf_start = buf;

	while ((buf - buf_start) < len) {
		ret = min((u32)WARN_MSG_BLOCK_LEN,
			  (u32)(len - (buf - buf_start)));
		ret = CWMCU_i2c_read(mcu_data,
				     CW_I2C_REG_WARN_MSG_BUFFER,
				     buf, ret);
		if (ret == 0) {
			break;
		} else if (ret < 0) {
			E("%s: warn i2c_read: ret = %d\n", __func__, ret);
			break;
		} else
			buf += ret;
	}
	printk(KERN_WARNING "[S_HUB][CW_MCU] Warning MSG[%d] = %.*s",
			index, (int)(buf - buf_start), buf_start);
}
#endif

void activate_double_tap(u8 facedown)
{
	blocking_notifier_call_chain(&double_tap_notifier_list, facedown, NULL);
	return;
}

void easy_access_irq_handler(struct cwmcu_data *mcu_data, u8 easy_access_type)
{
	int ret, retry;
	u8 clear_intr;
	u8 reg_addr;
	u8 sensor_id;
	u8 data[6];
	s32 data_event;
	u16 *u16_data_p = (u16 *)&data_event;
	__le16 data_buff[REPORT_EVENT_COMMON_LEN] = {0};
	char easy_access_type_str[32];

	switch (easy_access_type) {
	case CW_MCU_INT_BIT_HTC_GESTURE_MOTION:
		scnprintf(easy_access_type_str, 32, "GESTURE_MOTION");
		clear_intr = CW_MCU_INT_BIT_HTC_GESTURE_MOTION;
		reg_addr = CWSTM32_READ_Gesture_Motion;
		sensor_id = HTC_GESTURE_MOTION;
		break;
	default:
		E("%s: Unknown easy_access_type = 0x%x\n", __func__,
		  easy_access_type);
		return;
	}

	for (retry = 0; retry < RETRY_TIMES; retry++) {
		ret = i2c_smbus_read_i2c_block_data(mcu_data->client,
					reg_addr, sizeof(data), data);
		if (ret == sizeof(data))
			break;
		mdelay(5);
	}
	D("[CWMCU] i2c bus read %d bytes, retry = %d\n", ret, retry);
	data_event = (s32)((data[0] & 0x1F) |
			   (((data[1] | (data[2] << 8)) & 0x3FF) << 5) |
			   (data[3] << 15) |
			   (data[4] << 23));
	data_buff[0] = cpu_to_le16p((u16 *)&u16_data_p[0]);
	data_buff[1] = cpu_to_le16p((u16 *)&u16_data_p[2]);
	D("%s: data_event = 0x%x, data_buff(0, 1) = (0x%x, 0x%x)\n",
	  __func__, data_event, data_buff[0], data_buff[1]);
	if (vib_trigger) {
		if (data[0] == 14) {
			
			vib_trigger_event(vib_trigger, VIB_TIME);
			D("%s: %s is detected, vibrate for %d ms!\n",
			  __func__, easy_access_type_str, VIB_TIME);
		} else if (data[0] == 6 || data[0] == 15 ||
			   data[0] == 18 || data[0] == 19 ||
			   data[0] == 24 || data[0] == 25 ||
			   data[0] == 26 || data[0] == 27) {
			
			vib_trigger_event(vib_trigger, VIB_TIME);
			mcu_data->sensors_time[sensor_id] = 0;
			cw_send_event(mcu_data, sensor_id, data_buff, 0);
			mcu_data->power_key_pressed = 0;
			D(
			  "%s: [vib_trigger] %s: df0: %d, "
			  "d0: %d, d1: %d\n", __func__, easy_access_type_str,
			  data_buff[0], data[0], data[1]);
			D(
			  "%s: [vib_trigger] %s: data_buff:"
			  " %d, data_event: %d\n", __func__,
			  easy_access_type_str, data_buff[1], data_event);
		} else {
			vib_trigger_event(vib_trigger, VIB_TIME);
			mcu_data->sensors_time[sensor_id] = 0;
			cw_send_event(mcu_data, sensor_id, data_buff, 0);
			mcu_data->power_key_pressed = 0;
			D(
			  "%s: [disable vib_trigger] %s: "
			  "df0: %d, d0: %d, d1: %d\n", __func__,
			  easy_access_type_str, data_buff[0], data[0], data[1]);
			D(
			  "%s: [disable vib_trigger] %s: "
			  "data_buff: %d, data_event: %d\n", __func__,
			  easy_access_type_str, data_buff[1], data_event);
		}
	} else {
		mcu_data->sensors_time[sensor_id] = 0;
		cw_send_event(mcu_data, sensor_id, data_buff, 0);
		mcu_data->power_key_pressed = 0;
		I("%s: %s: df0: %d, d0: %d, d1: %d\n",
		  __func__, easy_access_type_str,
		  data_buff[0], data[0], data[1]);
		I("%s: %s: data_buff: %d, data_event: %d\n",
		  __func__, easy_access_type_str, data_buff[1], data_event);
	}
	ret = CWMCU_i2c_write_power(mcu_data, CWSTM32_INT_ST4, &clear_intr, 1);
}

static irqreturn_t cwmcu_irq_handler(int irq, void *handle)
{
	struct cwmcu_data *mcu_data = handle;
	s32 ret;
	u8 INT_st1 = 0, INT_st2 = 0, INT_st3 = 0, INT_st4 = 0, err_st = 0, batch_st = 0;
	u8 clear_intr;
	u16 light_adc = 0;
	u16 ps_autok_thd = 0 , ps_min_adc = 0;
	u16 ps_adc = 0;
	u16 p_status = 0;
#ifdef SHUB_DLOAD_SUPPORT
	int mcu_status_level;
#endif 

	if (!mcu_data->probe_success) {
		D("%s: probe not completed\n", __func__);
		return IRQ_HANDLED;
	}

	D("[CWMCU] %s\n", __func__);

#ifdef SHUB_DLOAD_SUPPORT
	mcu_status_level = MCU2CPU_STATUS_GPIO_LEVEL(mcu_data);
	
	if (mcu_data->gpio_mcu_status_level != mcu_status_level) {
		mcu_data->gpio_mcu_status_level = mcu_status_level;
		I("%s gpio_mcu_status_level:%d\n", __func__, mcu_data->gpio_mcu_status_level);
		mcu_data->w_mcu_state_change = true;
		queue_work(mcu_data->mcu_wq, &mcu_data->one_shot_work);
	}
#endif 

	if (MCU_IN_DLOAD() || MCU_IN_BOOTLOADER()) {
		I("%s skip, s_mcu_state:%d\n", __func__, s_mcu_state);
		goto EXIT;
	}

	cwmcu_powermode_switch(mcu_data, 1);

	ret = CWMCU_i2c_read(mcu_data, CWSTM32_INT_ST1, &INT_st1, 1);
	if (ret < 0)INT_st1 = 0;

	ret = CWMCU_i2c_read(mcu_data, CWSTM32_INT_ST2, &INT_st2, 1);
	if (ret < 0)INT_st2 = 0;

	ret = CWMCU_i2c_read(mcu_data, CWSTM32_INT_ST3, &INT_st3, 1);
	if (ret < 0)INT_st3 = 0;

	ret = CWMCU_i2c_read(mcu_data, CWSTM32_INT_ST4, &INT_st4, 1);
	if (ret < 0)INT_st4 = 0;

	ret = CWMCU_i2c_read(mcu_data, CWSTM32_ERR_ST, &err_st, 1);
	if (ret < 0)err_st = 0;

	ret = CWMCU_i2c_read(mcu_data, CWSTM32_BATCH_MODE_COMMAND, &batch_st, 1);
	if (ret < 0)batch_st = 0;

	I(
	  "%s: INT_st(1, 2, 3, 4) = (0x%x, 0x%x, 0x%x, 0x%x), err_st = 0x%x"
	  ", batch_st = 0x%x\n",
	  __func__, INT_st1, INT_st2, INT_st3, INT_st4, err_st, batch_st);

	
	if (INT_st1 & CW_MCU_INT_BIT_PROXIMITY) {
		u8 data[REPORT_EVENT_PROXIMITY_LEN] = {0};
		s16 data_buff[REPORT_EVENT_PROXIMITY_LEN] = {0};

		if(mcu_data->enabled_list & (1<<proximity)) {
			CWMCU_i2c_read(mcu_data, CWSTM32_READ_Proximity, data, 7);
			if(data[0] < 2) {
				mcu_data->sensors_time[proximity] =
					mcu_data->sensors_time[proximity] -
					mcu_data->report_period[proximity];

                                ps_autok_thd    = (data[6] << 8) | data[5];
                                ps_min_adc      = (data[4] << 8) | data[3];
                                ps_adc          = (data[2] << 8) | data[1];
                                p_status        = data[0];
                                data_buff[0]    = data[0];
				cw_send_event(mcu_data, CW_PROXIMITY, data_buff, 0);

				I("Proximity interrupt occur value is %d adc is"
     	                            " 0x%X, while min_adc is 0x%X, autok_thd is 0x%X"
	                            " ps_calibration is %d\n", data[0], ps_adc,
          	                    ps_min_adc, ps_autok_thd, mcu_data->ps_calibrated);
			}
		}
		else {
			D("Proximity interrupt occur value is %d adc is"
			  " 0x%X, while min_adc is 0x%X, autok_thd is 0x%X"
           	          " ps_calibration is %d (message only)\n",
			  data[0], ps_adc, ps_min_adc, ps_autok_thd,
			  mcu_data->ps_calibrated);
		}

		if(data[0] < 2) {
		clear_intr = CW_MCU_INT_BIT_PROXIMITY;
			ret = CWMCU_i2c_write(mcu_data, CWSTM32_INT_ST1, &clear_intr, 1);
			CWMCU_i2c_write(mcu_data, CWSTM32_READ_Proximity, &data[0], 1);
		}
	}

	
	if (INT_st1 & CW_MCU_INT_BIT_LIGHT) {
		u8 data[REPORT_EVENT_COMMON_LEN] = {0};
		s16 data_buff[REPORT_EVENT_COMMON_LEN] = {0};

		if (mcu_data->enabled_list & (1<<light)) {
			CWMCU_i2c_read(mcu_data, CWSTM32_READ_Light, data, 3);
			if (data[0] < 11) {
				mcu_data->sensors_time[light] =
						mcu_data->sensors_time[light] -
						mcu_data->report_period[light];
				light_adc = (data[2] << 8) | data[1];

				data_buff[0] = data[0];
				mcu_data->light_last_data[0] = data_buff[0];
				cw_send_event(mcu_data, CW_LIGHT, data_buff, 0);
				D(
				  "light interrupt occur value is %u, adc "
				  "is %x ls_calibration is %u\n",
					data[0], light_adc,
					mcu_data->ls_calibrated);
			} else {
				D(
				  "light interrupt occur value is %u, adc is"
				  " %x ls_calibration is %u (message only)\n",
					data[0], light_adc,
					mcu_data->ls_calibrated);
			}
		}
		if (data[0] < 11) {
			clear_intr = CW_MCU_INT_BIT_LIGHT;
			CWMCU_i2c_write(mcu_data, CWSTM32_INT_ST1, &clear_intr,
					1);
		}
	}

	
	if (INT_st2 & CW_MCU_INT_BIT_SHUB_BOOTUP) {
		I("%s: CW_MCU_INT_BIT_SHUB_BOOTUP\n", __func__);

		mcu_data->mcu_bootup = true;
		mcu_data->w_mcu_state_change = true;
		queue_work(mcu_data->mcu_wq, &mcu_data->one_shot_work);

		clear_intr = CW_MCU_INT_BIT_SHUB_BOOTUP;
		ret = CWMCU_i2c_write(mcu_data, CWSTM32_INT_ST2, &clear_intr, 1);
	}

	
	if (INT_st2 & CW_MCU_INT_BIT_LOG_AVAILABLE) {
		I("%s: CW_MCU_INT_BIT_LOG_AVAILABLE\n", __func__);

#ifdef SHUB_LOGGING_SUPPORT
		complete(&s_mcu_log_avail);
#endif 

		clear_intr = CW_MCU_INT_BIT_LOG_AVAILABLE;
		ret = CWMCU_i2c_write(mcu_data, CWSTM32_INT_ST2, &clear_intr, 1);
	}

	
	if (INT_st3 & CW_MCU_INT_BIT_SIGNIFICANT_MOTION) {
		if (mcu_data->enabled_list & (1<<significant_motion)) {
			s16 data_buff[REPORT_EVENT_COMMON_LEN] = {0};

			mcu_data->sensors_time[significant_motion] = 0;

			wake_lock_timeout(&mcu_data->significant_wake_lock,
					  1 * HZ);

			data_buff[0] = 1;
			cw_send_event(mcu_data, CW_SIGNIFICANT_MOTION, data_buff
				      , 0);

			D("%s: Significant Motion interrupt occurs!!\n",
					__func__);
		}
		clear_intr = CW_MCU_INT_BIT_SIGNIFICANT_MOTION;
		CWMCU_i2c_write(mcu_data, CWSTM32_INT_ST3, &clear_intr, 1);
	}

	
	if (INT_st3 & CW_MCU_INT_BIT_STEP_DETECTOR) {
		if (mcu_data->enabled_list & (1<<step_detector)) {
			u8 data = 0;
			s16 data_buff[REPORT_EVENT_COMMON_LEN] = {0};

			ret = CWMCU_i2c_read(mcu_data,
					     CWSTM32_READ_STEP_DETECTOR,
					     &data, 1);
			if (ret >= 0) {
				mcu_data->sensors_time[step_detector] = 0;

				data_buff[0] = data;
				cw_send_event(mcu_data, CW_STEP_DETECTOR,
					      data_buff, 0);

				D("%s: Step Detector INT, timestamp = %u\n",
						__func__, data);
			} else
				D("%s: Step Detector i2c read fail, ret = %d\n",
						__func__, ret);

		}
		clear_intr = CW_MCU_INT_BIT_STEP_DETECTOR;
		CWMCU_i2c_write(mcu_data, CWSTM32_INT_ST3, &clear_intr, 1);
	}

	
	if (INT_st3 & CW_MCU_INT_BIT_STEP_COUNTER) {
		if (mcu_data->enabled_list & (1<<step_counter)) {
			u8 data[4] = {0};
			__le16 data_buff[REPORT_EVENT_COMMON_LEN] = {0};

			ret = CWMCU_i2c_read(mcu_data,
					     CWSTM32_READ_STEP_COUNTER,
					     data, sizeof(data));
			if (ret >= 0) {
				mcu_data->sensors_time[step_counter] = 0;

				data_buff[0] = cpu_to_le16p((u16 *)&data[0]);
				data_buff[1] = cpu_to_le16p((u16 *)&data[2]);
				cw_send_event(mcu_data, CW_STEP_COUNTER,
					      data_buff, 0);

				D(
				  "%s: Step Counter interrupt, step(data_buf) ="
				  " %u\n",
				  __func__, *(u32 *)&data_buff[0]);
			} else
				D("%s: Step Counter i2c read fails, ret = %d\n",
						__func__, ret);

		}
		clear_intr = CW_MCU_INT_BIT_STEP_COUNTER;
		CWMCU_i2c_write(mcu_data, CWSTM32_INT_ST3, &clear_intr, 1);
	}

	
	if (INT_st3 & CW_MCU_INT_BIT_FACEDOWN_DETECTION) {
		if (mcu_data->enabled_list & (1<<HTC_FACEDOWN_DETECTION)) {
			u8 data;

			ret = CWMCU_i2c_read(mcu_data,
					     CWSTM32_READ_FACEDOWN_DETECTION,
					     &data, sizeof(data));
			if (ret >= 0) {
				D("%s: FACEDOWN = %u\n", __func__, data);
				activate_double_tap(data);
			} else
				E("%s: FACEDOWN i2c read fails, ret = %d\n",
						__func__, ret);

		}
		clear_intr = CW_MCU_INT_BIT_FACEDOWN_DETECTION;
		CWMCU_i2c_write(mcu_data, CWSTM32_INT_ST3, &clear_intr, 1);
	}

	
	if (INT_st4 & CW_MCU_INT_BIT_HTC_GESTURE_MOTION) {
		easy_access_irq_handler(mcu_data,
					CW_MCU_INT_BIT_HTC_GESTURE_MOTION);
	}

	
	if (INT_st4 & CW_MCU_INT_BIT_ANY_MOTION) {
		if (mcu_data->enabled_list & (1 << HTC_ANY_MOTION)) {
			s16 data_buff[REPORT_EVENT_COMMON_LEN] = {0};

			mcu_data->sensors_time[HTC_ANY_MOTION] = 0;

			wake_lock_timeout(&mcu_data->any_motion_wake_lock,
					  1 * HZ);

			data_buff[0] = 1;
			cw_send_event(mcu_data, HTC_ANY_MOTION, data_buff, 0);

			D("%s: HTC_ANY_MOTION occurs!\n", __func__);
		}
		clear_intr = CW_MCU_INT_BIT_ANY_MOTION;
		ret = CWMCU_i2c_write(mcu_data, CWSTM32_INT_ST4, &clear_intr,
				      1);
	}

#ifdef MCU_WARN_MSGS
	
	if (err_st & CW_MCU_INT_BIT_ERROR_WARN_MSG) {
		u8 buf_len[WARN_MSG_BUFFER_LEN_SIZE] = {0};

		ret = CWMCU_i2c_read(mcu_data, CW_I2C_REG_WARN_MSG_BUFFER_LEN,
				     buf_len, sizeof(buf_len));
		if (ret >= 0) {
			int i;
			char buf[WARN_MSG_PER_ITEM_LEN];

			for (i = 0; i < WARN_MSG_BUFFER_LEN_SIZE; i++) {
				if (buf_len[i] <= WARN_MSG_PER_ITEM_LEN)
					print_warn_msg(mcu_data, buf,
						       buf_len[i], i);
			}
		} else {
			E("%s: Warn MSG read fails, ret = %d\n",
						__func__, ret);
		}
		clear_intr = CW_MCU_INT_BIT_ERROR_WARN_MSG;
		ret = CWMCU_i2c_write(mcu_data, CWSTM32_ERR_ST, &clear_intr, 1);
	}
#endif

	
	if (err_st & CW_MCU_INT_BIT_ERROR_MCU_EXCEPTION) {
		u8 buf_len[EXCEPTION_BUFFER_LEN_SIZE] = {0};
		bool reset_done;

		ret = CWMCU_i2c_read(mcu_data, CW_I2C_REG_EXCEPTION_BUFFER_LEN,
				     buf_len, sizeof(buf_len));
		if (ret >= 0) {
			__le32 exception_len;
			u8 data[EXCEPTION_BLOCK_LEN];
			int i;

			exception_len = cpu_to_le32p((u32 *)&buf_len[0]);
			E("%s: exception_len = %u\n", __func__, exception_len);

			for (i = 0; exception_len >= EXCEPTION_BLOCK_LEN; i++) {
				memset(data, 0, sizeof(data));
				ret = CWMCU_i2c_read(mcu_data,
						    CW_I2C_REG_EXCEPTION_BUFFER,
						    data, sizeof(data));
				if (ret >= 0) {
					char buf[3*EXCEPTION_BLOCK_LEN];

					print_hex_data(buf, i, data,
							EXCEPTION_BLOCK_LEN);
					exception_len -= EXCEPTION_BLOCK_LEN;
				} else {
					E(
					  "%s: i = %d, excp1 i2c_read: ret = %d"
					  "\n", __func__, i, ret);
					goto exception_end;
				}
			}
			if ((exception_len > 0) &&
			    (exception_len < sizeof(data))) {
				ret = CWMCU_i2c_read(mcu_data,
						    CW_I2C_REG_EXCEPTION_BUFFER,
						    data, exception_len);
				if (ret >= 0) {
					char buf[3*EXCEPTION_BLOCK_LEN];

					print_hex_data(buf, i, data,
						       exception_len);
				} else {
					E(
					  "%s: i = %d, excp2 i2c_read: ret = %d"
					  "\n", __func__, i, ret);
				}
			}
		} else {
			E("%s: Exception status dump fails, ret = %d\n",
						__func__, ret);
		}
exception_end:
		mutex_lock(&s_activated_i2c_lock);
		reset_done = reset_hub(mcu_data, false);
		mutex_unlock(&s_activated_i2c_lock);

		if (reset_done) {
			mcu_data->w_re_init = true;
			queue_work(mcu_data->mcu_wq, &mcu_data->one_shot_work);
			E("%s: reset after exception done\n", __func__);
		}

		clear_intr = CW_MCU_INT_BIT_ERROR_MCU_EXCEPTION;
		ret = CWMCU_i2c_write(mcu_data, CWSTM32_ERR_ST, &clear_intr, 1);
	}

	
	if (err_st & CW_MCU_INT_BIT_ERROR_WATCHDOG_RESET) {
		u8 data[WATCHDOG_STATUS_LEN] = {0};

		E("[CWMCU] Watch Dog Reset\n");
		msleep(5);

		ret = CWMCU_i2c_read(mcu_data, CW_I2C_REG_WATCHDOG_STATUS,
				     data, WATCHDOG_STATUS_LEN);
		if (ret >= 0) {
			int i;

			for (i = 0; i < WATCHDOG_STATUS_LEN; i++) {
				E("%s: Watchdog Status[%d] = 0x%x\n",
					__func__, i, data[i]);
			}
		} else {
			E("%s: Watchdog status dump fails, ret = %d\n",
						__func__, ret);
		}

		mcu_data->w_re_init = true;
		queue_work(mcu_data->mcu_wq, &mcu_data->one_shot_work);

		clear_intr = CW_MCU_INT_BIT_ERROR_WATCHDOG_RESET;
		ret = CWMCU_i2c_write(mcu_data, CWSTM32_ERR_ST, &clear_intr, 1);
	}

	
	if (batch_st & 0x1C) {
		if (batch_st & 0x8) { 
			u16 data_buff[REPORT_EVENT_COMMON_LEN] = {0};

			data_buff[0] = EXHAUSTED_MAGIC;
			cw_send_event(mcu_data, TIME_DIFF_EXHAUSTED, data_buff,
				      0);

			clear_intr = 0x8;
		} else if (batch_st & 0x14) { 
			cwmcu_batch_read(mcu_data);
			clear_intr = 0x14;
		}
		D("%s: clear_intr = 0x%x, write_addr = 0x%x", __func__,
		  clear_intr, CWSTM32_BATCH_MODE_COMMAND);
		ret = CWMCU_i2c_write(mcu_data,
				      CWSTM32_BATCH_MODE_COMMAND,
				      &clear_intr, 1);
	}

	cwmcu_powermode_switch(mcu_data, 0);

EXIT:
	return IRQ_HANDLED;
}

static int cw_data_rdy_trig_poll(struct iio_dev *indio_dev)
{
	struct cwmcu_data *mcu_data = iio_priv(indio_dev);

	disable_irq_nosync(mcu_data->trig->subirq_base);

	iio_trigger_poll(mcu_data->trig, iio_get_time_ns());

	enable_irq(mcu_data->trig->subirq_base);

	return 0;
}

static irqreturn_t cw_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct cwmcu_data *mcu_data = iio_priv(indio_dev);

	mutex_lock(&mcu_data->lock);
	cwmcu_read(mcu_data, pf);
	iio_trigger_notify_done(mcu_data->indio_dev->trig);
	mutex_unlock(&mcu_data->lock);

	return IRQ_HANDLED;
}

static const struct iio_buffer_setup_ops cw_buffer_setup_ops = {
	.preenable = &iio_sw_buffer_preenable,
	.postenable = &iio_triggered_buffer_postenable,
	.predisable = &iio_triggered_buffer_predisable,
};

static int cw_pseudo_irq_enable(struct iio_dev *indio_dev)
{
	struct cwmcu_data *mcu_data = iio_priv(indio_dev);

	if (!atomic_cmpxchg(&mcu_data->pseudo_irq_enable, 0, 1)) {
		D("%s:\n", __func__);
		cancel_delayed_work_sync(&mcu_data->work);
		queue_delayed_work(mcu_data->mcu_wq, &mcu_data->work, 0);
	}

	return 0;
}

static int cw_pseudo_irq_disable(struct iio_dev *indio_dev)
{
	struct cwmcu_data *mcu_data = iio_priv(indio_dev);

	if (atomic_cmpxchg(&mcu_data->pseudo_irq_enable, 1, 0)) {
		cancel_delayed_work_sync(&mcu_data->work);
		D("%s:\n", __func__);
	}
	return 0;
}

static int cw_set_pseudo_irq(struct iio_dev *indio_dev, int enable)
{
	if (enable)
		cw_pseudo_irq_enable(indio_dev);
	else
		cw_pseudo_irq_disable(indio_dev);

	return 0;
}

static int cw_data_rdy_trigger_set_state(struct iio_trigger *trig,
		bool state)
{
	struct iio_dev *indio_dev =
			(struct iio_dev *)iio_trigger_get_drvdata(trig);
	struct cwmcu_data *mcu_data = iio_priv(indio_dev);

	mutex_lock(&mcu_data->mutex_lock);
	cw_set_pseudo_irq(indio_dev, state);
	mutex_unlock(&mcu_data->mutex_lock);

	return 0;
}

static const struct iio_trigger_ops cw_trigger_ops = {
	.owner = THIS_MODULE,
	.set_trigger_state = &cw_data_rdy_trigger_set_state,
};

static int cw_probe_trigger(struct iio_dev *iio_dev)
{
	struct cwmcu_data *mcu_data = iio_priv(iio_dev);
	int ret;

	iio_dev->pollfunc = iio_alloc_pollfunc(&iio_pollfunc_store_time,
			&cw_trigger_handler, IRQF_ONESHOT, iio_dev,
			"%s_consumer%d", iio_dev->name, iio_dev->id);
	if (iio_dev->pollfunc == NULL) {
		ret = -ENOMEM;
		goto error_ret;
	}
	mcu_data->trig = iio_trigger_alloc("%s-dev%d",
			iio_dev->name,
			iio_dev->id);
	if (!mcu_data->trig) {
		ret = -ENOMEM;
		goto error_dealloc_pollfunc;
	}
	mcu_data->trig->dev.parent = &mcu_data->client->dev;
	mcu_data->trig->ops = &cw_trigger_ops;
	iio_trigger_set_drvdata(mcu_data->trig, iio_dev);

	ret = iio_trigger_register(mcu_data->trig);
	if (ret)
		goto error_free_trig;

	return 0;

error_free_trig:
	iio_trigger_free(mcu_data->trig);
error_dealloc_pollfunc:
	iio_dealloc_pollfunc(iio_dev->pollfunc);
error_ret:
	return ret;
}

static int cw_probe_buffer(struct iio_dev *iio_dev)
{
	int ret;
	struct iio_buffer *buffer;

	buffer = iio_kfifo_allocate(iio_dev);
	if (!buffer) {
		ret = -ENOMEM;
		goto error_ret;
	}

	buffer->scan_timestamp = true;
	iio_dev->buffer = buffer;
	iio_dev->setup_ops = &cw_buffer_setup_ops;
	iio_dev->modes |= INDIO_BUFFER_TRIGGERED;
	ret = iio_buffer_register(iio_dev, iio_dev->channels,
				  iio_dev->num_channels);
	if (ret)
		goto error_free_buf;

	iio_scan_mask_set(iio_dev, iio_dev->buffer, CW_SCAN_ID);
	iio_scan_mask_set(iio_dev, iio_dev->buffer, CW_SCAN_X);
	iio_scan_mask_set(iio_dev, iio_dev->buffer, CW_SCAN_Y);
	iio_scan_mask_set(iio_dev, iio_dev->buffer, CW_SCAN_Z);
	return 0;

error_free_buf:
	iio_kfifo_free(iio_dev->buffer);
error_ret:
	return ret;
}


static int cw_read_raw(struct iio_dev *indio_dev,
		       struct iio_chan_spec const *chan,
		       int *val,
		       int *val2,
		       long mask) {
	struct cwmcu_data *mcu_data = iio_priv(indio_dev);
	int ret = -EINVAL;

	if (chan->type != IIO_ACCEL)
		return ret;

	mutex_lock(&mcu_data->lock);
	switch (mask) {
	case 0:
		*val = mcu_data->iio_data[chan->channel2 - IIO_MOD_X];
		ret = IIO_VAL_INT;
		break;
	case IIO_CHAN_INFO_SCALE:
		
		
		*val = 0;
		*val2 = 1000;
		ret = IIO_VAL_INT_PLUS_MICRO;
		break;
	}
	mutex_unlock(&mcu_data->lock);

	return ret;
}

#define CW_CHANNEL(axis)			\
{						\
	.type = IIO_ACCEL,			\
	.modified = 1,				\
	.channel2 = axis+1,			\
	.info_mask = BIT(IIO_CHAN_INFO_SCALE),	\
	.scan_index = axis,			\
	.scan_type = IIO_ST('u', 32, 32, 0)	\
}

static const struct iio_chan_spec cw_channels[] = {
	CW_CHANNEL(CW_SCAN_ID),
	CW_CHANNEL(CW_SCAN_X),
	CW_CHANNEL(CW_SCAN_Y),
	CW_CHANNEL(CW_SCAN_Z),
	IIO_CHAN_SOFT_TIMESTAMP(CW_SCAN_TIMESTAMP)
};

static const struct iio_info cw_info = {
	.read_raw = &cw_read_raw,
	.driver_module = THIS_MODULE,
};

static int mcu_parse_dt(struct device *dev, struct cwmcu_data *pdata)
{
	struct property *prop;
	struct device_node *dt = dev->of_node;
	u32 buf = 0;
	struct device_node *g_sensor_offset;
	int g_sensor_cali_size = 0;
	unsigned char *g_sensor_cali_data = NULL;
	struct device_node *gyro_sensor_offset;
	int gyro_sensor_cali_size = 0;
	unsigned char *gyro_sensor_cali_data = NULL;
	struct device_node *light_sensor_offset = NULL;
	int light_sensor_cali_size = 0;
	unsigned char *light_sensor_cali_data = NULL;
        struct device_node *p_sensor_offset = NULL;
        int p_sensor_cali_size = 0;
        unsigned char *p_sensor_cali_data = NULL;
	struct device_node *baro_sensor_offset;
	int baro_sensor_cali_size = 0;
	unsigned char *baro_sensor_cali_data = NULL;

	int i;

	g_sensor_offset = of_find_node_by_path(CALIBRATION_DATA_PATH);
	if (g_sensor_offset) {
		g_sensor_cali_data = (unsigned char *)
				     of_get_property(g_sensor_offset,
						     G_SENSOR_FLASH_DATA,
						     &g_sensor_cali_size);
		I("%s: cali_size = %d\n", __func__, g_sensor_cali_size);
		if (g_sensor_cali_data) {
			for (i = 0; (i < g_sensor_cali_size) && (i < 4); i++) {
				I("g sensor cali_data[%d] = %02x\n", i,
						g_sensor_cali_data[i]);
				pdata->gs_kvalue |= (g_sensor_cali_data[i] <<
						    (i * 8));
			}
		}

	} else
		E("%s: G-sensor Calibration data offset not found\n", __func__);

	gyro_sensor_offset = of_find_node_by_path(CALIBRATION_DATA_PATH);
	if (gyro_sensor_offset) {
		gyro_sensor_cali_data = (unsigned char *)
					of_get_property(gyro_sensor_offset,
							GYRO_SENSOR_FLASH_DATA,
							&gyro_sensor_cali_size);
		I("%s:gyro cali_size = %d\n", __func__, gyro_sensor_cali_size);
		if (gyro_sensor_cali_data) {
			for (i = 0; (i < gyro_sensor_cali_size) && (i < 4);
				     i++) {
				I("gyro sensor cali_data[%d] = %02x\n", i,
					gyro_sensor_cali_data[i]);
				pdata->gy_kvalue |= (gyro_sensor_cali_data[i] <<
						     (i * 8));
			}
			pdata->gs_kvalue_L1 = (gyro_sensor_cali_data[5] << 8) |
						gyro_sensor_cali_data[4];
			I("g sensor cali_data L1 = %x\n", pdata->gs_kvalue_L1);
			pdata->gs_kvalue_L2 = (gyro_sensor_cali_data[7] << 8) |
						gyro_sensor_cali_data[6];
			I("g sensor cali_data L2 = %x\n", pdata->gs_kvalue_L2);
			pdata->gs_kvalue_L3 = (gyro_sensor_cali_data[9] << 8) |
						gyro_sensor_cali_data[8];
			I("g sensor cali_data L3 = %x\n", pdata->gs_kvalue_L3);
			pdata->gs_kvalue_R1 = (gyro_sensor_cali_data[11] << 8) |
						gyro_sensor_cali_data[10];
			I("g sensor cali_data R1 = %x\n", pdata->gs_kvalue_R1);
			pdata->gs_kvalue_R2 = (gyro_sensor_cali_data[13] << 8) |
						gyro_sensor_cali_data[12];
			I("g sensor cali_data R2 = %x\n", pdata->gs_kvalue_R2);
			pdata->gs_kvalue_R3 = (gyro_sensor_cali_data[15] << 8) |
						gyro_sensor_cali_data[14];
			I("g sensor cali_data R3 = %x\n", pdata->gs_kvalue_R3);
		}

	} else
		E("%s: GYRO-sensor Calibration data offset not found\n",
				__func__);

	light_sensor_offset = of_find_node_by_path(CALIBRATION_DATA_PATH);
	if (light_sensor_offset) {
		light_sensor_cali_data = (unsigned char *)
					 of_get_property(light_sensor_offset,
						       LIGHT_SENSOR_FLASH_DATA,
						       &light_sensor_cali_size);
		I("%s:light cali_size = %d\n", __func__,
				light_sensor_cali_size);
		if (light_sensor_cali_data) {
			for (i = 0; (i < light_sensor_cali_size) && (i < 4);
			     i++) {
				I("light sensor cali_data[%d] = %02x\n", i,
					light_sensor_cali_data[i]);
				pdata->als_kvalue |=
					(light_sensor_cali_data[i] << (i * 8));
			}
		}
	} else
		E("%s: LIGHT-sensor Calibration data offset not found\n",
			__func__);

        p_sensor_offset = of_find_node_by_path(CALIBRATION_DATA_PATH);
        if (p_sensor_offset) {
               p_sensor_cali_data = (unsigned char*)
                                       of_get_property(p_sensor_offset,
                                                       PROX_SENSOR_FLASH_DATA,
                                                       &p_sensor_cali_size);
               I("%s:proximity cali_size = %d\n", __func__,
                                 p_sensor_cali_size);
                if (p_sensor_cali_data) {
                       for (i = 0; (i < p_sensor_cali_size) && (i < 8); i++) {
                               I("proximity sensor cali_data[%d] = %02x \n", i,
                                       p_sensor_cali_data[i]);
                                if(i < 4)
                                        pdata->ps_kheader |= (p_sensor_cali_data[i] << (i * 8));
                                else
                                        pdata->ps_kvalue |= (p_sensor_cali_data[i] << ((i-4) * 8));
                        }
                }
        } else
                E("%s: PROXIMITY-sensor Calibration data offset not found\n",
			__func__);


	baro_sensor_offset = of_find_node_by_path(CALIBRATION_DATA_PATH);
	if (baro_sensor_offset) {
		baro_sensor_cali_data = (unsigned char *)
					of_get_property(baro_sensor_offset,
							BARO_SENSOR_FLASH_DATA,
							&baro_sensor_cali_size);
		D("%s: cali_size = %d\n", __func__, baro_sensor_cali_size);
		if (baro_sensor_cali_data) {
			for (i = 0; (i < baro_sensor_cali_size) &&
			     (i < 5); i++) {
				I("baro sensor cali_data[%d] = %02x\n", i,
					baro_sensor_cali_data[i]);
				if (i == baro_sensor_cali_size - 1)
					pdata->bs_kheader =
						baro_sensor_cali_data[i];
				else
					pdata->bs_kvalue |=
						(baro_sensor_cali_data[i] <<
						(i * 8));
			}
		}
	} else
		E("%s: Barometer-sensor Calibration data offset not found\n",
			__func__);

	pdata->gpio_mcu_status = of_get_named_gpio(dt, "mcu,mcu_status-gpio",
					0);
	if (!gpio_is_valid(pdata->gpio_mcu_status)) {
		E("DT:gpio_mcu_status value is not valid %d\n", pdata->gpio_mcu_status);
	}
	else
		D("DT:gpio_mcu_status=%d\n", pdata->gpio_mcu_status);

	pdata->gpio_wake_mcu = of_get_named_gpio(dt, "mcu,Cpu_wake_mcu-gpio",
					0);
	if (!gpio_is_valid(pdata->gpio_wake_mcu))
		E("DT:gpio_wake_mcu value is not valid\n");
	else
		D("DT:gpio_wake_mcu=%d\n", pdata->gpio_wake_mcu);

	pdata->gpio_mcu_irq = of_get_named_gpio(dt, "mcu,intr-gpio", 0);
	if (!gpio_is_valid(pdata->gpio_mcu_irq))
		E("DT:gpio_mcu_irq value is not valid\n");
	else
		D("DT:gpio_mcu_irq=%d\n", pdata->gpio_mcu_irq);

	pdata->gpio_reset = of_get_named_gpio(dt, "mcu,Reset-gpio", 0);
	if (!gpio_is_valid(pdata->gpio_reset))
		E("DT:gpio_reset value is not valid\n");
	else
		D("DT:gpio_reset=%d\n", pdata->gpio_reset);

	pdata->gpio_chip_mode = of_get_named_gpio(dt, "mcu,Chip_mode-gpio", 0);
	if (!gpio_is_valid(pdata->gpio_chip_mode))
		E("DT:gpio_chip_mode value is not valid\n");
	else
		D("DT:gpio_chip_mode=%d\n", pdata->gpio_chip_mode);

	prop = of_find_property(dt, "mcu,gs_chip_layout", NULL);
	if (prop) {
		of_property_read_u32(dt, "mcu,gs_chip_layout", &buf);
		pdata->gs_chip_layout = buf;
		D("%s: chip_layout = %d\n", __func__, pdata->gs_chip_layout);
	} else
		E("%s: g_sensor,chip_layout not found", __func__);

        prop = of_find_property(dt, "touch_enable", NULL);
        if (prop) {
                of_property_read_u32(dt, "touch_enable", &buf);
                pdata->touch_enable = buf;
                I("%s: touch_enable = %d\n", __func__, pdata->touch_enable);
        } else
                E("%s: touch_enable not found", __func__);


	prop = of_find_property(dt, "mcu,acceleration_axes", NULL);
	if (prop) {
		of_property_read_u32(dt, "mcu,acceleration_axes", &buf);
		pdata->acceleration_axes = buf;
		I("%s: acceleration axes = %u\n", __func__,
			pdata->acceleration_axes);
	} else
		E("%s: g_sensor axes not found", __func__);

	prop = of_find_property(dt, "mcu,magnetic_axes", NULL);
	if (prop) {
		of_property_read_u32(dt, "mcu,magnetic_axes", &buf);
		pdata->magnetic_axes = buf;
		I("%s: Compass axes = %u\n", __func__, pdata->magnetic_axes);
	} else
		E("%s: Compass axes not found", __func__);

	prop = of_find_property(dt, "mcu,gyro_axes", NULL);
	if (prop) {
		of_property_read_u32(dt, "mcu,gyro_axes", &buf);
		pdata->gyro_axes = buf;
		I("%s: gyro axes = %u\n", __func__, pdata->gyro_axes);
	} else
		E("%s: gyro axes not found", __func__);

        prop = of_find_property(dt, "mcu,als_levels", NULL);
        if (prop) {
                of_property_read_u32_array(dt, "mcu,als_levels", adc_table, 10);
                pdata->als_levels = &adc_table[0];
	} else
		E("%s: Light sensor level not found", __func__);

        prop = of_find_property(dt, "mcu,als_goldl", NULL);
        if (prop) {
                of_property_read_u32(dt, "mcu,als_goldl", &buf);
                pdata->als_goldl = buf;
                I("%s: als_goldl = 0x%x\n",__func__, pdata->als_goldl);
        } else
                E("%s: als_goldl not found", __func__);

        prop = of_find_property(dt, "mcu,als_goldh", NULL);
        if (prop) {
                of_property_read_u32(dt, "mcu,als_goldh", &buf);
                pdata->als_goldh = buf;
                I("%s: als_goldh = 0x%x\n",__func__, pdata->als_goldh);
        } else
                E("%s: als_goldh not found", __func__);

        prop = of_find_property(dt, "mcu,ps_thd_fixed", NULL);
        if (prop) {
                of_property_read_u32(dt, "mcu,ps_thd_fixed", &buf);
                pdata->ps_thd_fixed = buf;
                I("%s: ps_thd_fixed = 0x%x\n",__func__, pdata->ps_thd_fixed);
        } else
                E("%s: ps_thd_fixed not found", __func__);

        prop = of_find_property(dt, "mcu,ps_thd_add", NULL);
        if (prop) {
                of_property_read_u32(dt, "mcu,ps_thd_add", &buf);
                pdata->ps_thd_add = buf;
                I("%s: ps_thd_add = 0x%x\n",__func__, pdata->ps_thd_add);
        } else
                E("%s: ps_thd_add not found", __func__);

	return 0;
}
#if 0
static ssize_t bma250_clear_powerkey_pressed(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	unsigned long value;
	int error;

	error = kstrtol(buf, 10, &value);
	if (error) {
		E("%s: kstrtol fails, error = %d\n", __func__, error);
		return error;
	}

	if (value == 1)
		mcu_data->power_key_pressed = 1;
	else if (value == 0)
		mcu_data->power_key_pressed = 0;

	return count;
}

static ssize_t bma250_get_powerkry_pressed(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", mcu_data->power_key_pressed);
}
static DEVICE_ATTR(clear_powerkey_flag, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP,
		bma250_get_powerkry_pressed, bma250_clear_powerkey_pressed);
#endif
static ssize_t p_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", p_status);
}

static DEVICE_ATTR(p_status, 0444, p_status_show, NULL);

static ssize_t set_gesture_motion(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 *data;
	unsigned long val = 0;
	int i, rc;

	rc = kstrtol(buf, 16, &val);
	if (rc) {
		E("%s: kstrtol fails, error = %d\n", __func__, rc);
		return rc;
	}

	data = (u8 *)&val;

	I("%s: Gesture motion parameter = 0x%lx\n", __func__, val);
	I("%s: data(0, 1, 2, 3) = (0x%x, 0x%x, 0x%x, 0x%x)\n",
	  __func__, data[0], data[1], data[2], data[3]);
	mcu_data->gesture_motion_param = val;

	cwmcu_powermode_switch(mcu_data, 1);
	for (i = 0; i < GESTURE_MOTION_UPDATE_ATTRIBUTE_LEN; i++) {
		I("%s: writing 0x%x to 0xC9\n", __func__, data[i]);
		CWMCU_i2c_write(mcu_data,
				  GESTURE_MOTION_UPDATE_ATTRIBUTE,
				  &data[i], 1);
	}
	cwmcu_powermode_switch(mcu_data, 0);

	return count;
}

static ssize_t get_gesture_motion(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);
	u8 data[GESTURE_MOTION_UPDATE_ATTRIBUTE_LEN] = {0};

	if (CWMCU_i2c_read_power(mcu_data, GESTURE_MOTION_UPDATE_ATTRIBUTE, data, 4)
	    >= 0) {
		I(
		  "%s: gesture_motion_param = 0x%08X, data(0, 1, 2, 3) = "
		  "(0x%x, 0x%x, 0x%x, 0x%x), cpu_to_le32p((__u32 *)&data)"
		  " = 0x%08X\n", __func__,
		  mcu_data->gesture_motion_param, data[0], data[1], data[2],
		  data[3], cpu_to_le32p((__u32 *)&data));
		return snprintf(buf, PAGE_SIZE, "0x%08X\n",
				cpu_to_le32p((__u32 *)&data));
	}
	return snprintf(buf, PAGE_SIZE, "0x%08X\n", 0xFFFFFFFF);
}

static bool is_htc_dbg_flag_set(void)
{
#if 0 
    
    if (get_radio_flag() & 0x8) {
        I("%s: true\n", __func__);
        return true;
    }
    else {
        I("%s: false\n", __func__);
        return false;
    }
#endif
	return false; 
}

static ssize_t dbg_flag_show(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf)
{
    int dbg_flag_set;

    if (is_htc_dbg_flag_set())
        dbg_flag_set = 1;
    else
        dbg_flag_set = 0;

    return snprintf(buf, PAGE_SIZE, "%d\n", dbg_flag_set);
}

#ifdef SHUB_LOGGING_SUPPORT
static ssize_t log_mask_show(struct device *dev, struct device_attribute *attr,char *buf);
static ssize_t log_mask_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
static ssize_t log_level_show(struct device *dev, struct device_attribute *attr,char *buf);
static ssize_t log_level_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
#endif 
static ssize_t sensor_placement_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
static struct device_attribute attributes[] = {

	__ATTR(enable, 0666, active_show,
			active_set),
	__ATTR(batch_enable, 0666, batch_show, batch_set),
	__ATTR(delay_ms, 0666, interval_show,
			interval_set),
	__ATTR(flush, 0666, flush_show, flush_set),
	__ATTR(calibrator_en, 0220, NULL, set_calibrator_en),
	__ATTR(calibrator_status_acc, 0440, show_calibrator_status_acc, NULL),
	__ATTR(calibrator_status_mag, 0440, show_calibrator_status_mag, NULL),
	__ATTR(calibrator_status_gyro, 0440, show_calibrator_status_gyro, NULL),
	__ATTR(calibrator_data_acc, 0666, get_k_value_acc_f, set_k_value_acc_f),
	__ATTR(calibrator_data_acc_rl, 0440, get_k_value_acc_rl_f, NULL),
	__ATTR(ap_calibrator_data_acc_rl, 0440, ap_get_k_value_acc_rl_f, NULL),
	__ATTR(calibrator_data_mag, 0666, get_k_value_mag_f, set_k_value_mag_f),
	__ATTR(calibrator_data_gyro, 0666, get_k_value_gyro_f,
			set_k_value_gyro_f),
	__ATTR(calibrator_data_light, 0440, get_k_value_light_f, NULL),
        __ATTR(calibrator_data_proximity, 0666, get_k_value_proximity_f, set_k_value_proximity_f),
	__ATTR(calibrator_data_barometer, 0666, get_k_value_barometer_f,
			set_k_value_barometer_f),
	__ATTR(gesture_motion, 0660, get_gesture_motion, set_gesture_motion),
	__ATTR(data_barometer, 0440, get_barometer, NULL),
        __ATTR(data_proximity, 0666, get_proximity, NULL),
        __ATTR(data_proximity_polling, 0666, get_proximity_polling, NULL),
	__ATTR(data_light_polling, 0440, get_light_polling, NULL),
	__ATTR(sensor_hub_rdata, 0220, NULL, read_mcu_data),
        __ATTR(ps_canc, 0666, get_ps_canc, set_ps_canc),
	__ATTR(data_light_kadc, 0440, get_light_kadc, NULL),
	__ATTR(firmware_version, 0440, get_firmware_version, NULL),
	__ATTR(firmware_info, 0440, get_firmware_info, NULL),
	__ATTR(hall_sensor, 0440, get_hall_sensor, NULL),
	__ATTR(led_en, 0220, NULL, led_enable),
	__ATTR(facedown_enabled, 0660, facedown_show, facedown_set),
	__ATTR(trigger_crash, 0220, NULL, trigger_mcu_crash),
	__ATTR(mcu_wakeup, 0220, NULL, trigger_mcu_wakeup),
#ifdef SHUB_LOGGING_SUPPORT
	__ATTR(mcu_log_mask, 0660, log_mask_show, log_mask_store),
	__ATTR(mcu_log_level, 0660, log_level_show, log_level_store),
#endif 
	__ATTR(dbg_flag, 0440, dbg_flag_show, NULL),
	__ATTR(sensor_placement, 0660, NULL, sensor_placement_store),
};


static int create_sysfs_interfaces(struct cwmcu_data *mcu_data)
{
	int i;
	int res;
#if 0
	struct class *bma250_powerkey_class = NULL;
	struct device *bma250_powerkey_dev = NULL;
#endif
	struct class *optical_class = NULL;
	struct device *proximity_dev = NULL;
#if 0
	bma250_powerkey_class = class_create(THIS_MODULE, "bma250_powerkey");
	if (IS_ERR(bma250_powerkey_class)) {
		res = PTR_ERR(bma250_powerkey_class);
		bma250_powerkey_class = NULL;
		E("%s: could not allocate bma250_powerkey_class, res = %d\n",
		  __func__, res);
		goto error_powerkey_class;
	}

	bma250_powerkey_dev = device_create(bma250_powerkey_class,
					    NULL, 0, "%s", "bma250");
	res = device_create_file(bma250_powerkey_dev,
				 &dev_attr_clear_powerkey_flag);
	if (res) {
		E("%s, create bma250_device_create_file fail!\n", __func__);
		goto err_create_bma250_device_file;
	}
#endif
	optical_class = class_create(THIS_MODULE, "optical_sensors");
	if (IS_ERR(optical_class)) {
		res = PTR_ERR(optical_class);
		optical_class = NULL;
		E("%s: could not allocate optical_class, res = %d\n", __func__,
		  res);
		goto error_optical_class_create;
	}

	proximity_dev = device_create(optical_class, NULL, 0, "%s",
				      "proximity");
	res = device_create_file(proximity_dev, &dev_attr_p_status);
	if (res) {
		E("%s, create proximty_device_create_file fail!\n", __func__);
		goto err_create_proximty_device_file;
	}

	mcu_data->sensor_class = class_create(THIS_MODULE, "htc_sensorhub");
	if (IS_ERR(mcu_data->sensor_class)) {
		res = PTR_ERR(mcu_data->sensor_class);
		goto err_class_create;
	}

	mcu_data->sensor_dev = device_create(mcu_data->sensor_class, NULL, 0,
					     "%s", "sensor_hub");
	if (IS_ERR(mcu_data->sensor_dev)) {
		res = PTR_ERR(mcu_data->sensor_dev);
		goto err_device_create;
	}

	res = dev_set_drvdata(mcu_data->sensor_dev, mcu_data);
	if (res)
		goto err_set_drvdata;

	for (i = 0; i < ARRAY_SIZE(attributes); i++)
		if (device_create_file(mcu_data->sensor_dev, attributes + i))
			goto error;

	res = sysfs_create_link(&mcu_data->sensor_dev->kobj,
				&mcu_data->indio_dev->dev.kobj, "iio");
	if (res < 0)
		goto error;

	return 0;

error:
	while (--i >= 0)
		device_remove_file(mcu_data->sensor_dev, attributes + i);
err_set_drvdata:
	put_device(mcu_data->sensor_dev);
	device_unregister(mcu_data->sensor_dev);
err_device_create:
	class_destroy(mcu_data->sensor_class);
err_class_create:
	device_remove_file(proximity_dev, &dev_attr_p_status);
err_create_proximty_device_file:
	class_destroy(optical_class);

error_optical_class_create:
#if 0
	device_remove_file(bma250_powerkey_dev, &dev_attr_clear_powerkey_flag);
err_create_bma250_device_file:
	class_destroy(bma250_powerkey_class);
error_powerkey_class:
	dev_err(&mcu_data->client->dev, "%s:Unable to create interface\n",
		__func__);
#endif
	return res;
}

static void destroy_sysfs_interfaces(struct cwmcu_data *mcu_data)
{
	int i;

	sysfs_remove_link(&mcu_data->sensor_dev->kobj, "iio");
	for (i = 0; i < ARRAY_SIZE(attributes); i++)
		device_remove_file(mcu_data->sensor_dev, attributes + i);
	put_device(mcu_data->sensor_dev);
	device_unregister(mcu_data->sensor_dev);
	class_destroy(mcu_data->sensor_class);
}

static void cwmcu_remove_trigger(struct iio_dev *indio_dev)
{
	struct cwmcu_data *mcu_data = iio_priv(indio_dev);

	iio_trigger_unregister(mcu_data->trig);
	iio_trigger_free(mcu_data->trig);
	iio_dealloc_pollfunc(indio_dev->pollfunc);
}
static void cwmcu_remove_buffer(struct iio_dev *indio_dev)
{
	iio_buffer_unregister(indio_dev);
	iio_kfifo_free(indio_dev->buffer);
}

static void cwmcu_one_shot(struct work_struct *work)
{
	struct cwmcu_data *mcu_data = container_of((struct work_struct *)work,
			struct cwmcu_data, one_shot_work);

	if (mcu_data->w_activated_i2c == true) {
		mcu_data->w_activated_i2c = false;

		mutex_lock(&s_activated_i2c_lock);
		if (retry_exhausted(mcu_data) &&
		    time_after(jiffies, mcu_data->i2c_jiffies +
					REACTIVATE_PERIOD)) {

			
			if (MCU_IN_DLOAD() || MCU_IN_BOOTLOADER()) {
				E("%s[%d]: skip reset_hub, s_mcu_state:%x\n", __func__, __LINE__, s_mcu_state);
			} else {
				bool reset_done;
				reset_done = reset_hub(mcu_data, false);
				E("%s[%d]: reset_hub, reset_done:%d\n", __func__, __LINE__, reset_done);
			}
		}

		if (retry_exhausted(mcu_data)) {
			I("%s: i2c_total_retry = %d, i2c_latch_retry = %d\n",
			  __func__, mcu_data->i2c_total_retry,
			  mcu_data->i2c_latch_retry);
		}
		else {
			
			mcu_data->i2c_total_retry++;
			mcu_data->i2c_jiffies = jiffies;
		}

		mutex_unlock(&s_activated_i2c_lock);
		D(
		  "%s--: mcu_data->i2c_total_retry = %d, "
		  "mcu_data->i2c_latch_retry = %d\n", __func__,
		  mcu_data->i2c_total_retry, mcu_data->i2c_latch_retry);
	}

	if (mcu_data->w_re_init == true) {
		mcu_data->w_re_init = false;

		cwmcu_powermode_switch(mcu_data, 1);

		cwmcu_sensor_placement(mcu_data);
		cwmcu_set_sensor_kvalue(mcu_data);
		cwmcu_restore_status(mcu_data);

#ifdef SHUB_DLOAD_SUPPORT
		mcu_enable_disable_dload_mode(mcu_data->dload_mode_enabled);
#endif 

#ifdef SHUB_LOGGING_SUPPORT
		mcu_set_log_mask(mcu_data->mcu_log_mask);
		mcu_set_log_level(mcu_data->mcu_log_level);
#endif 

#ifdef CONFIG_FB
		mcu_set_display_state(mcu_data->is_display_on);
#endif 

		
		mcu_data->mcu_sensor_ready = true;
		mcu_data->w_mcu_state_change = true;
		queue_work(mcu_data->mcu_wq, &mcu_data->one_shot_work);

		cwmcu_powermode_switch(mcu_data, 0);
	}

	if (mcu_data->w_facedown_set == true) {
		u8 data;
		int i;

		mcu_data->w_facedown_set = false;

		i = (HTC_FACEDOWN_DETECTION / 8);

		data = (u8)(mcu_data->enabled_list >> (i * 8));
		CWMCU_i2c_write_power(mcu_data, CWSTM32_ENABLE_REG + i, &data,
				      1);
	}

	if (mcu_data->w_kick_start_mcu == true) {
		mcu_data->w_kick_start_mcu = false;
		reset_hub(mcu_data, true);
		g_mcu_ready = true;
		enable_irq(mcu_data->IRQ);
		cwmcu_sensor_placement(mcu_data);
		cwmcu_set_sensor_kvalue(mcu_data);
	}

	if (mcu_data->w_mcu_state_change == true) {
		mcu_data->w_mcu_state_change = false;
		do_mcu_state_transition(mcu_data);
	}
}


static void cwmcu_work_report(struct work_struct *work)
{
	struct cwmcu_data *mcu_data = container_of((struct delayed_work *)work,
			struct cwmcu_data, work);
	struct iio_dev *indio_dev = iio_priv_to_dev(mcu_data);

	if (atomic_read(&mcu_data->pseudo_irq_enable)) {
		unsigned long jiff;

		jiff = msecs_to_jiffies(atomic_read(&mcu_data->delay));
		if (!jiff)
			jiff = 1;
		cw_data_rdy_trig_poll(indio_dev);
		queue_delayed_work(mcu_data->mcu_wq, &mcu_data->work, jiff);
	}
}

#if defined(SHUB_DLOAD_SUPPORT) || defined(SHUB_FIRMWARE_UPDATE_SUPPORT)
static int mcu_i2c_rx(const struct i2c_client *client, u8 *buf, int len)
{
    int ret;
    int i;

    if (!client) return -ENXIO;

    mutex_lock(&s_activated_i2c_lock);

    for (i = 0; i < RETRY_TIMES; i++) {
        D("%s(%d) len:%d\n", __func__, __LINE__, len);
        ret = i2c_master_recv(client, (char *)buf, (int)len);
        if (unlikely(ret < 0)) {
            E("%s(%d) fail addr:0x%x ret:%d\n", __func__, __LINE__, client->addr, ret);
            msleep(1);
        }
        else {
            break;
        }
    }

    mutex_unlock(&s_activated_i2c_lock);
    return ret;
}

static int mcu_i2c_tx(const struct i2c_client *client, u8 *buf, int len)
{
    int ret;
    int i;

    if (!client) return -ENXIO;

    mutex_lock(&s_activated_i2c_lock);

    for (i = 0; i < RETRY_TIMES; i++) {
        D("%s(%d) len:%d\n", __func__, __LINE__, len);
        ret = i2c_master_send(client, (char *)buf, (int)len);
        if (unlikely(ret < 0)) {
            E("%s(%d) fail addr:0x%x ret:%d\n", __func__, __LINE__, client->addr, ret);
            msleep(1);
        }
        else {
            break;
        }
    }

    mutex_unlock(&s_activated_i2c_lock);
    return ret;
}
#endif

#ifdef SHUB_DLOAD_SUPPORT
#define MCU_I2C_TX_CMD_LEN_MAX     32
static int mcu_i2c_tx_cmd(const struct i2c_client *client, u8 cmd, u8 *buf, int len)
{
    int ret;
    u8 i2c_data[MCU_I2C_TX_CMD_LEN_MAX+1] = {0};

    D("%s: addr:0x%x cmd:0x%x len:%d\n", __func__, client->addr, cmd, len);

    if (!client) return -ENXIO;

    if (unlikely(len > MCU_I2C_TX_CMD_LEN_MAX)) {
        E("%s[%d]: invalid len:%d !!\n", __func__, __LINE__, len);
        return -EINVAL;
    }

    i2c_data[0] = cmd;
    memcpy(&(i2c_data[1]), buf, len);

    ret = mcu_i2c_tx(client, i2c_data, len+1);
    if (unlikely(ret < 0)) {
        E("%s[%d]: addr:%x cmd:%x fail ret:%d!!\n", __func__, __LINE__, client->addr, cmd, ret);
        return ret;
    }

    return ret;
}

static int mcu_i2c_rx_cmd(const struct i2c_client *client, u8 cmd, u8 *buf, int len)
{
    int ret;
    u8 i2c_data[1];

    D("%s: addr:0x%x cmd:0x%x len:%d\n", __func__, client->addr, cmd, len);

    if (!client) return -ENXIO;

    i2c_data[0] = cmd;

    ret = mcu_i2c_tx(client, i2c_data, 1);
    if (unlikely(ret < 0)) {
        E("%s[%d]: addr:%x cmd:%x fail ret:%d!!\n", __func__, __LINE__, client->addr, cmd, ret);
        return ret;
    }

    ret = mcu_i2c_rx(client, buf, len);

    return ret;
}
#endif 

#ifdef SHUB_FIRMWARE_UPDATE_SUPPORT


#define STM32MCUF383_FLASH_START_ADDR       (0x08000000)
#define STM32MCUF383_FLASH_END_ADDR         (0x0803FFFF)
#define STM32MCUF383_PID                    (0x0432)

#define STM32MCUF401_FLASH_START_ADDR       (0x08000000)
#define STM32MCUF401_FLASH_END_ADDR         (0x0803FFFF)
#define STM32MCUF401_PID                    (0x0423)

#define STM32MCUF411_FLASH_START_ADDR       (0x08000000)
#define STM32MCUF411_FLASH_END_ADDR         (0x0807FFFF)
#define STM32MCUF411_PID                    (0x0431)

#define STM32MCUF383_HTC_PARAM_START_ADDR   (0x2000ff00)
#define STM32MCUF401_HTC_PARAM_START_ADDR   (0x2000ff00)
#define STM32MCUF411_HTC_PARAM_START_ADDR   (0x2001ff00)
#define HTC_PARAM_SIZE                      (0x00000100)

#define MCU_BL_RESP_ACK  0x79
#define MCU_BL_RESP_NACK 0x1F
#define MCU_BL_RESP_BUSY 0x76

#define MCU_I2C_FLASH_NAME "i2c-mcu-flash"
#define MCU_FW_UPDATE_I2C_LEN_LIMIT 256
#define MCU_FW_UPDATE_TIMEOUT_SEC   300

static struct i2c_client *mcu_fw_flash_i2c_client = NULL;
static u8 stm32mcu_bl_ver = 0x11;
static u16 stm32mcu_pid = STM32MCUF411_PID;
static u32 stm32mcu_flash_start_addr = STM32MCUF411_FLASH_START_ADDR;
static u32 stm32mcu_flash_end_addr = STM32MCUF411_FLASH_END_ADDR;
static u32 stm32mcu_htc_param_start_addr = STM32MCUF411_HTC_PARAM_START_ADDR;
static u8 stm32mcu_flash_readout_protect_enable = 0;
static u8 stm32mcu_flash_support_crc_checksum = 0;

#define FW_UPDATE_STATUS_INIT   0
#define FW_UPDATE_ONGOING       1
#define FW_UPDATE_DONE          2
static unsigned char  mcu_fw_flash_status = FW_UPDATE_DONE;
static unsigned char  mcu_fw_flash_progress = 0;

static unsigned long  mcu_fw_flash_start_jiffies;
static uint32_t  mcu_fw_img_size = 0;
static uint32_t  mcu_fw_img_crc_checksum = 0;

static int mcu_bl_wait_for_ack(int wait_ms)
{
    u8 i2c_data[1] = {0};
    unsigned long  start_jiffies = jiffies;
    int count = 0;
    unsigned int elapsed_ms;

    D("%s\n", __func__);

    do {
        mcu_i2c_rx(mcu_fw_flash_i2c_client, i2c_data, 1);
        D("%s ack:0x%x\n", __func__, i2c_data[0]);
        if (i2c_data[0] == MCU_BL_RESP_ACK || i2c_data[0] == MCU_BL_RESP_NACK)
            break;

        if (jiffies_to_msecs(jiffies - start_jiffies) > wait_ms)
            break;

        msleep(10);

        count++;
    } while(1);

    elapsed_ms = jiffies_to_msecs(jiffies - start_jiffies);
    if (elapsed_ms > 50) {
        I("%s done, count:%d elapsed_ms:%u ret:0x%x\n", __func__, count, elapsed_ms, i2c_data[0]);
    }

    return i2c_data[0];
}

static int mcu_bl_get_version(void)
{
    u8  i2c_data[10];
    int rc = 0;
    int id_num_of_byte;
    int tx_bytes;

    I("%s[%d]: send GV cmd\n", __func__, __LINE__);
    i2c_data[0] = 0x01;
    i2c_data[1] = 0xFE;
    tx_bytes = 2;
    rc = mcu_i2c_tx(mcu_fw_flash_i2c_client, i2c_data, tx_bytes);
    if (rc != tx_bytes) {
        E("%s[%d]: Failed to write 0x%02x 0x%02x, rc = %d\n", __func__, __LINE__, i2c_data[0], i2c_data[1], rc);
        return rc;
    }

    rc = mcu_bl_wait_for_ack(500);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }

    rc = mcu_i2c_rx(mcu_fw_flash_i2c_client, i2c_data, 1);
    if (rc != 1) {
        E("%s: Failed to read ver, rc = %d\n", __func__, rc);
        return rc;
    }

    stm32mcu_bl_ver = i2c_data[0];
    I("%s[%d]: stm32mcu_bl_ver:0x%x\n", __func__, __LINE__, stm32mcu_bl_ver);

    rc = mcu_bl_wait_for_ack(500);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }

    I("%s[%d]: Get ID cmd\n", __func__, __LINE__);
    i2c_data[0] = 0x02;
    i2c_data[1] = 0xFD;
    tx_bytes = 2;
    rc = mcu_i2c_tx(mcu_fw_flash_i2c_client, i2c_data, tx_bytes);
    if (rc != tx_bytes) {
        E("%s[%d]: Failed to write 0x%02x 0x%02x, rc = %d\n", __func__, __LINE__, i2c_data[0], i2c_data[1], rc);
        return rc;
    }

    rc = mcu_bl_wait_for_ack(500);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }

    rc = mcu_i2c_rx(mcu_fw_flash_i2c_client, i2c_data, 3);
    if (rc != 3) {
        E("%s: Failed to read pid, rc = %d\n", __func__, rc);
        return rc;
    }

    id_num_of_byte = i2c_data[0] + 1;
    I("%s[%d]: id_num_of_byte:%x\n", __func__, __LINE__, id_num_of_byte);

    stm32mcu_pid = (i2c_data[1] << 8) | i2c_data[2];
    I("%s[%d]: Product ID:0x%04x\n", __func__, __LINE__, stm32mcu_pid);

    rc = mcu_bl_wait_for_ack(500);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }

    
    switch (stm32mcu_pid) {
    case STM32MCUF383_PID:
        I("%s[%d]: STM32MCUF383_PID\n", __func__, __LINE__);
        stm32mcu_flash_start_addr = STM32MCUF383_FLASH_START_ADDR;
        stm32mcu_flash_end_addr = STM32MCUF383_FLASH_END_ADDR;
        stm32mcu_htc_param_start_addr = STM32MCUF383_HTC_PARAM_START_ADDR;
        stm32mcu_flash_readout_protect_enable = 1;
        stm32mcu_flash_support_crc_checksum = 0;
        break;

    case STM32MCUF401_PID:
        I("%s[%d]: STM32MCUF401_PID\n", __func__, __LINE__);
        stm32mcu_flash_start_addr = STM32MCUF401_FLASH_START_ADDR;
        stm32mcu_flash_end_addr = STM32MCUF401_FLASH_END_ADDR;
        stm32mcu_htc_param_start_addr = STM32MCUF401_HTC_PARAM_START_ADDR;
        stm32mcu_flash_readout_protect_enable = 0;
        stm32mcu_flash_support_crc_checksum = 1;
        break;

    case STM32MCUF411_PID:
    default:
        I("%s[%d]: STM32MCUF411_PID\n", __func__, __LINE__);
        stm32mcu_flash_start_addr = STM32MCUF411_FLASH_START_ADDR;
        stm32mcu_flash_end_addr = STM32MCUF411_FLASH_END_ADDR;
        stm32mcu_htc_param_start_addr = STM32MCUF411_HTC_PARAM_START_ADDR;
        stm32mcu_flash_readout_protect_enable = 0;
        stm32mcu_flash_support_crc_checksum = 1;
        break;
    }

    return 0;
}

static int mcu_bl_read_protect_enable(void)
{
    uint8_t i2c_data[2];
    int rc;
    int tx_bytes;

    i2c_data[0] = (stm32mcu_bl_ver == 0x11) ? 0x83 : 0x82;
    i2c_data[1] = (stm32mcu_bl_ver == 0x11) ? 0x7C : 0x7D;
    I("%s: Send command = 0x%02x 0x%02x \n", __func__, i2c_data[0], i2c_data[1]);
    tx_bytes = 2;
    rc = mcu_i2c_tx(mcu_fw_flash_i2c_client, i2c_data, tx_bytes);
    if (rc != tx_bytes) {
        E("%s[%d]: Failed to write 0x%02x 0x%02x, rc = %d\n", __func__, __LINE__, i2c_data[0], i2c_data[1], rc);
        return rc;
    }

    I("%s[%d]: wait ack1\n", __func__, __LINE__);
    rc = mcu_bl_wait_for_ack(500);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }

    I("%s[%d]: wait ack2\n", __func__, __LINE__);
    rc = mcu_bl_wait_for_ack(5000);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }

    msleep(500);	

    return 0;
}

static int mcu_bl_read_protect_disable(void)
{
    uint8_t i2c_data[2];
    int rc;
    int tx_bytes;

    i2c_data[0] = (stm32mcu_bl_ver == 0x11) ? 0x93 : 0x92;
    i2c_data[1] = (stm32mcu_bl_ver == 0x11) ? 0x6C : 0x6D;
    I("%s: Send command = 0x%02x 0x%02x \n", __func__, i2c_data[0], i2c_data[1]);
    tx_bytes = 2;
    rc = mcu_i2c_tx(mcu_fw_flash_i2c_client, i2c_data, tx_bytes);
    if (rc != tx_bytes) {
        E("%s[%d]: Failed to write 0x%02x 0x%02x, rc = %d\n", __func__, __LINE__, i2c_data[0], i2c_data[1], rc);
        return rc;
    }

    I("%s[%d]: wait ack1\n", __func__, __LINE__);
    rc = mcu_bl_wait_for_ack(500);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }

    I("%s[%d]: wait ack2\n", __func__, __LINE__);
    rc = mcu_bl_wait_for_ack(20000);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }

    usleep_range(41*1000, 42*1000);

    return 0;
}

static int mcu_bl_erase_flash_mem(void)
{
    u8 i2c_data[128] = {0};
    int rc;
    int tx_bytes;

    if (!mcu_fw_flash_i2c_client)
        return -ENOENT;

    i2c_data[0] = (stm32mcu_bl_ver == 0x11) ? 0x45 : 0x44;
    i2c_data[1] = (stm32mcu_bl_ver == 0x11) ? 0xBA : 0xBB;
    I("%s: Send command = 0x%02x 0x%02x \n", __func__, i2c_data[0], i2c_data[1]);
    tx_bytes = 2;
    rc = mcu_i2c_tx(mcu_fw_flash_i2c_client, i2c_data, tx_bytes);
    if (rc != tx_bytes) {
        E("%s[%d]: Failed to write 0x%02x 0x%02x, rc = %d\n", __func__, __LINE__, i2c_data[0], i2c_data[1], rc);
        return rc;
    }

    rc = mcu_bl_wait_for_ack(500);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }

    I("%s[%d]: start global mass erase\n", __func__, __LINE__);
    i2c_data[0] = 0xFF;
    i2c_data[1] = 0xFF;
    i2c_data[2] = 0;
    tx_bytes = 3;
    rc = mcu_i2c_tx(mcu_fw_flash_i2c_client, i2c_data, tx_bytes);
    if (rc != tx_bytes) {
        E("%s[%d]: Failed to write 0x%02x 0x%02x, rc = %d\n", __func__, __LINE__, i2c_data[0], i2c_data[1], rc);
        return rc;
    }

    I("%s: wait for erase...\n", __func__);
    rc = mcu_bl_wait_for_ack(20000);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }
    I("%s: erase done\n", __func__);

    return 0;
}

static int mcu_bl_write_memory(u32 start_address,
          u8 write_buf[],
          int numberofbyte)
{
    u8 i2c_data[MCU_FW_UPDATE_I2C_LEN_LIMIT+2] = {0};
    __be32 to_i2c_command;
    int data_len, checksum;
    int i;
    int rc;
    int tx_bytes;

    if (!mcu_fw_flash_i2c_client)
        return -ENOENT;

    if (numberofbyte > 256) {
        E("%s[%d]: numberofbyte(%d) > 256\n", __func__, __LINE__, numberofbyte);
        return -EINVAL;
    }

    i2c_data[0] = (stm32mcu_bl_ver == 0x11) ? 0x32 : 0x31;
    i2c_data[1] = (stm32mcu_bl_ver == 0x11) ? 0xCD : 0xCE;
    tx_bytes = 2;
    rc = mcu_i2c_tx(mcu_fw_flash_i2c_client, i2c_data, tx_bytes);
    if (rc != tx_bytes) {
        E("%s[%d]: Failed to write 0x%02x 0x%02x, rc = %d\n", __func__, __LINE__, i2c_data[0], i2c_data[1], rc);
        return rc;
    }

    rc = mcu_bl_wait_for_ack(500);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }

    to_i2c_command = cpu_to_be32(start_address);
    memcpy(i2c_data, &to_i2c_command, sizeof(__be32));
    i2c_data[4] = i2c_data[0] ^ i2c_data[1] ^ i2c_data[2] ^ i2c_data[3];
    tx_bytes = 5;
    rc = mcu_i2c_tx(mcu_fw_flash_i2c_client, i2c_data, tx_bytes);
    if (rc != tx_bytes) {
        E("%s[%d]: Failed to write 0x%02x 0x%02x, rc = %d\n", __func__, __LINE__, i2c_data[0], i2c_data[1], rc);
        return rc;
    }

    rc = mcu_bl_wait_for_ack(500);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }


    checksum = 0x0;
    data_len = numberofbyte + 2;

    i2c_data[0] = numberofbyte - 1;

    for (i = 0; i < numberofbyte; i++)
        i2c_data[i+1] = write_buf[i];

    for (i = 0; i < (data_len - 1); i++)
        checksum ^= i2c_data[i];

    i2c_data[i] = checksum;
    rc = mcu_i2c_tx(mcu_fw_flash_i2c_client, i2c_data, data_len);
    if (rc != data_len) {
        E("%s[%d]: Failed to write 0x%02x 0x%02x, rc = %d\n", __func__, __LINE__, i2c_data[0], i2c_data[1], rc);
        return rc;
    }

    rc = mcu_bl_wait_for_ack(3000);
    if (rc != MCU_BL_RESP_ACK) {
        E("%s[%d]: FW wait ACK fail, rc = 0x%x\n", __func__, __LINE__, rc);
        return -1;
    }

    return 0;
}

static int mcu_bl_erase_htc_param(void)
{
    int ret;
    u8 buffer[HTC_PARAM_SIZE] = {0};

    ret = mcu_bl_write_memory(stm32mcu_htc_param_start_addr, buffer, HTC_PARAM_SIZE);
    if (ret) {
        E("%s(%d): mcu_bl_write_memory fails,"
          "ret = %d\n", __func__, __LINE__, ret);
        return -EIO;
    }
    else {
        I("%s: done, addr:0x%08x, size:%u\n", __func__, stm32mcu_htc_param_start_addr, HTC_PARAM_SIZE);
    }

    return ret;
}

static void mcu_bl_enter_leave(uint8_t enter)
{
    
    mutex_lock(&s_activated_i2c_lock);

    
    if (enter) {
        I("%s(%d) : enter\n", __func__, __LINE__);
        s_mcu_state = MCU_STATE_BOOTLOADER;
        mcu_chip_mode_set(s_mcu_data, MCU_CHIP_MODE_BOOTLOADER);
    }
    else {
        I("%s(%d) : leave\n", __func__, __LINE__);
        mcu_state_enter_unknown(s_mcu_data);
        mcu_chip_mode_set(s_mcu_data, MCU_CHIP_MODE_APPLICATION);
    }

    I("%s[%d]: gpio_chip_mode value = %d\n", __func__, __LINE__,
      gpio_get_value_cansleep(s_mcu_data->gpio_chip_mode));

    usleep_range(10000, 15000);

    
    reset_hub(s_mcu_data, true);
    mutex_unlock(&s_activated_i2c_lock);
}

static uint32_t mcu_get_firmware_checksum(uint32_t firmware_size)
{
    mcu_fw_checksum_t fw_checksum = {0, 0};
    unsigned long  start_jiffies = jiffies;

    I("%s[%d]: start get checksum firmware_size:%d\n", __func__, __LINE__, firmware_size);

    CWMCU_i2c_write_block_power(s_mcu_data, CW_I2C_REG_FLASH_CHECKSUM, (u8*)&firmware_size, 4);

    while (fw_checksum.calculate_done != 1) {
        CWMCU_i2c_read_power(s_mcu_data, CW_I2C_REG_FLASH_CHECKSUM, (u8*)&fw_checksum, sizeof(fw_checksum));

        if (jiffies_to_msecs(jiffies - start_jiffies) > 3000) {
            E("%s[%d]: wait checksum failed !!!\n", __func__, __LINE__);
            break;
        }

        if (fw_checksum.calculate_done != 1)
            usleep_range(10000, 15000);
    }

    I("%s[%d]: done get checksum:0x%X ...\n", __func__, __LINE__, fw_checksum.check_sum);

    return fw_checksum.check_sum;
}

static ssize_t fw_update_status_show(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf)
{
        return snprintf(buf, PAGE_SIZE, "%d\n", mcu_fw_flash_status);
}

static ssize_t fw_update_status_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf,
                                  size_t count)
{
    unsigned long tmp;
    int error;
    error = kstrtoul(buf, 10, &tmp);
    if (error) {
        E("%s[%d]: kstrtoul fails, error = %d\n", __func__, __LINE__, error);
    }
    else {
        mcu_fw_flash_status = tmp;
    }

    return count;
}

static ssize_t fw_update_progress_show(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf)
{
        return snprintf(buf, PAGE_SIZE, "%d\n", mcu_fw_flash_progress);
}

static ssize_t fw_update_progress_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf,
                                  size_t count)
{
    unsigned long tmp;
    int error;
    error = kstrtoul(buf, 10, &tmp);
    if (error) {
        E("%s[%d]: kstrtoul fails, error = %d\n", __func__, __LINE__, error);
    }
    else {
        mcu_fw_flash_progress = tmp;
    }

    return count;
}

static ssize_t fw_update_timeout_show(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf)
{
        return snprintf(buf, PAGE_SIZE, "%d\n", MCU_FW_UPDATE_TIMEOUT_SEC);
}

static DEVICE_ATTR(fw_update_status, S_IRUSR | S_IWUSR, fw_update_status_show, fw_update_status_store);
static DEVICE_ATTR(fw_update_timeout, S_IRUSR | S_IWUSR, fw_update_timeout_show, NULL);
static DEVICE_ATTR(fw_update_progress, S_IRUSR | S_IWUSR, fw_update_progress_show, fw_update_progress_store);

static int shub_fw_flash_open(struct inode *inode, struct file *file)
{
    mcu_fw_flash_start_jiffies = jiffies;
    I("%s(%d) done\n", __func__, __LINE__);
    return 0;
}

static int shub_fw_flash_release(struct inode *inode, struct file *file)
{
    I("%s(%d) done, elapsed_ms:%u\n", __func__, __LINE__, jiffies_to_msecs(jiffies - mcu_fw_flash_start_jiffies));
    return 0;
}

static ssize_t shub_fw_flash_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    return -ENODEV;
}

static ssize_t shub_fw_flash_write(struct file *file, const char __user *buf,
        size_t count, loff_t *ppos)
{
    size_t written = 0;
    size_t to_write;
    u8 write_buf[MCU_FW_UPDATE_I2C_LEN_LIMIT];
    int ret;
    unsigned long  start_jiffies = jiffies;
    u32 fw_flash_addr = stm32mcu_flash_start_addr + *ppos;

    if ( fw_flash_addr > stm32mcu_flash_end_addr) {
        E("%s(%d) invalid fw_flash_addr:0x%08x *ppos(0x%08x)\n", __func__, __LINE__, fw_flash_addr, (unsigned int)(*ppos));
        return written;
    }

    D("%s(%d): count(%u) *ppos(0x%08x)\n", __func__, __LINE__, (unsigned int)count, (unsigned int)(*ppos));

    while (count > 0) {
        to_write = min_t(size_t, count, (size_t)MCU_FW_UPDATE_I2C_LEN_LIMIT);
        to_write = min_t(size_t, to_write, (size_t)(stm32mcu_flash_end_addr-fw_flash_addr+1));
        if (to_write == 0) {
            E("%s(%d) reach mcu flash limit, fw_flash_addr:0x%08x\n", __func__, __LINE__, fw_flash_addr);
            break;
        }

        if (unlikely(copy_from_user(write_buf, buf, to_write))) {
            E("%s(%d) Unable to copy from user space !!\n", __func__, __LINE__);
            return -EFAULT;
        }

        ret = mcu_bl_write_memory(fw_flash_addr,
                     write_buf,
                     to_write);
        if (ret) {
            E("%s(%d): mcu_bl_write_memory fails,"
              "ret = %d\n", __func__, __LINE__, ret);
            return -EIO;
        }
        else {
            D("%s: mcu_bl_write_memory done, addr:0x%08x, size:%u\n", __func__, fw_flash_addr, (unsigned int)to_write);
        }

        buf += to_write;
        count -= to_write;
        written += to_write;
        *ppos += to_write;
        mcu_fw_img_size += to_write;
        fw_flash_addr = stm32mcu_flash_start_addr + *ppos;
    }

    I("%s(%d): return written(%u), elapsed_ms:%u\n", __func__, __LINE__, (unsigned int)written, jiffies_to_msecs(jiffies - start_jiffies));

    return written;
}

static long shub_fw_flash_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret = 0;

    if (!s_mcu_data) {
        E("%s(%d): s_mcu_data is NULL, return\n", __func__, __LINE__);
        return -EINVAL;
    }

    switch (cmd) {

    case SHUB_FW_IOCTL_PRE_FLASH:
        {
            I("%s(%d): SHUB_FW_IOCTL_PRE_FLASH\n", __func__, __LINE__);

            
            mcu_fw_img_size = 0;

            
            mcu_bl_enter_leave(1);

            
            ret = mcu_bl_get_version();
            if (ret < 0) {
                E("%s(%d): mcu_bl_get_version fails, ret = %d\n", __func__, __LINE__, ret);
                return ret;
            }

            
            if (stm32mcu_flash_readout_protect_enable) {
                ret = mcu_bl_read_protect_enable();
                if (ret < 0) {
                    I("%s(%d): mcu_bl_read_protect_enable fails, ret = %d\n", __func__, __LINE__, ret);
                }

                
                ret = mcu_bl_read_protect_disable();
                if (ret < 0) {
                    E("%s(%d): read_protect_disable fails, ret = %d\n", __func__, __LINE__, ret);
                    return ret;
                }
            }
            else {
                
                ret = mcu_bl_erase_flash_mem();
                if (ret < 0) {
                    E("%s(%d): erase mcu flash memory fails, ret = %d\n", __func__, __LINE__, ret);
                    return ret;
                }
            }

            
            mcu_bl_erase_htc_param();
        }
        break;

    case SHUB_FW_IOCTL_POST_FLASH:
        {
            unsigned long  start_jiffies;
            unsigned long timeout;

            I("%s(%d): SHUB_FW_IOCTL_POST_FLASH\n", __func__, __LINE__);


            if (stm32mcu_flash_support_crc_checksum) {
                
                mcu_bl_enter_leave(0);

                
                timeout = wait_for_completion_timeout(&s_mcu_enter_shub_run, (msecs_to_jiffies(3000)));
                if (timeout == 0) {
                    E("%s(%d): wait_for s_mcu_enter_shub_run timeout !!!\n", __func__, __LINE__);
                } else {
                    I("%s(%d): s_mcu_enter_shub_run completely\n", __func__, __LINE__);
                }

                if(!MCU_IN_BOOTLOADER()) {
                    
                    mcu_fw_img_crc_checksum = mcu_get_firmware_checksum(mcu_fw_img_size);
                    I("%s(%d): firmware checksum:0x%X\n", __func__, __LINE__, mcu_fw_img_crc_checksum);
                }

                
                mcu_bl_enter_leave(1);

                
                mcu_bl_erase_htc_param();
            }

            
            if (stm32mcu_flash_readout_protect_enable) {
                I("%s(%d): enable read_protect\n", __func__, __LINE__);
                ret = mcu_bl_read_protect_enable();
                if (ret < 0) {
                    E("%s(%d): read_protect_disable fails, ret = %d\n", __func__, __LINE__, ret);
                }
            }

            
            mcu_bl_enter_leave(0);

            
            I("%s(%d): After leave bootloader, wait mcu enter SHUB state\n", __func__, __LINE__);
            start_jiffies = jiffies;
            while (!MCU_IN_SHUB() && (jiffies_to_msecs(jiffies - start_jiffies) < 3000)) {
                msleep(5);
            }

            I("%s(%d): SHUB_FW_IOCTL_POST_FLASH done, s_mcu_state:%d\n", __func__, __LINE__, s_mcu_state);
        }
        break;

    case SHUB_FW_IOCTL_GET_FW_VERSION:
        {
            u8 fw_ver_from_mcu[6] = {0};
            mcu_fw_version_t fw_version;

            I("%s(%d): SHUB_FW_IOCTL_GET_FW_VERSION\n", __func__, __LINE__);

            ret = CWMCU_i2c_read(s_mcu_data, FIRMWARE_VERSION, fw_ver_from_mcu, sizeof(fw_ver_from_mcu));
            if (ret < 0) {
                E("%s(%d): Read FIRMWARE_VERSION err:%d\n", __func__, __LINE__, ret);
            }
            else {
                memset(&fw_version, 0, sizeof(fw_version));
                fw_version.arch = fw_ver_from_mcu[0];
                fw_version.sense= fw_ver_from_mcu[1];
                fw_version.cw_lib= fw_ver_from_mcu[2];
                fw_version.water= fw_ver_from_mcu[3];
                fw_version.active_engine= fw_ver_from_mcu[4];
                fw_version.project_mapping= fw_ver_from_mcu[5];

                ret = copy_to_user((void *)arg, &fw_version, sizeof(fw_version));
                if (ret) {
                    E("%s(%d): copy_to_user failed err:%d\n", __func__, __LINE__, ret);
                }
                else {
                    I("%s(%d): fw_version, arch:%d sense:%d cw_lib:%d water:%d active:%d project:%d\n", __func__, __LINE__,
                        fw_version.arch, fw_version.sense, fw_version.cw_lib, fw_version.water, fw_version.active_engine, fw_version.project_mapping );
                }
            }
        }
        break;

    case SHUB_FW_IOCTL_GET_FW_CHECKSUM:
        {
            I("%s(%d): SHUB_FW_IOCTL_GET_FW_CHECKSUM\n", __func__, __LINE__);

            if (put_user(mcu_fw_img_crc_checksum, (uint32_t __user *)arg)) {
                E("%s(%d): put_user mcu_fw_img_crc_checksum failed\n", __func__, __LINE__);
                return -EFAULT;
            }
        }
        break;

    default:
        E("%s(%d): INVALID param:0x%x\n", __func__, __LINE__, cmd);
        ret = -EINVAL;
        break;
    }

    return ret;
}

static const struct file_operations shub_fw_flash_fops = {
    .owner = THIS_MODULE,
    .read  = shub_fw_flash_read,
    .write = shub_fw_flash_write,
    .open  = shub_fw_flash_open,
    .release = shub_fw_flash_release,
    .unlocked_ioctl = shub_fw_flash_ioctl,
    .compat_ioctl = shub_fw_flash_ioctl
};

static struct miscdevice shub_fw_flash_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = SHUB_FIRMWARE_UPDATE_DEVICE_NAME,
    .fops = &shub_fw_flash_fops
};

static int mcu_pinctrl_init(struct cwmcu_data *mcu_data)
{
    int retval;
    struct i2c_client *client = mcu_data->client;
    int ret;
    I("mcu_pinctrl_init");
    
    mcu_data->pinctrl = devm_pinctrl_get(&client->dev);
    if (IS_ERR_OR_NULL(mcu_data->pinctrl)) {
        E("Target does not use pinctrl\n");
        retval = PTR_ERR(mcu_data->pinctrl);
        mcu_data->pinctrl = NULL;
        return retval;
    }

    mcu_data->gpio_state_init = pinctrl_lookup_state(mcu_data->pinctrl, "mcu_gpio_init");
    if (IS_ERR_OR_NULL(mcu_data->gpio_state_init)) {
        E("Can not get ts default pinstate\n");
        retval = PTR_ERR(mcu_data->gpio_state_init);
        mcu_data->pinctrl = NULL;
        return retval;
    }

    ret = pinctrl_select_state(mcu_data->pinctrl, mcu_data->gpio_state_init);
    if (ret) {
        E("can not init gpio\n");
        return ret;
    }

    return 0;
}


static int mcu_fw_flash_i2c_probe(struct i2c_client *client,
                                  const struct i2c_device_id *id)
{
    int ret;
    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        E("%s: i2c_check_functionality error\n", __func__);
        return -EIO;
    }

    mcu_fw_flash_i2c_client = client;
    I("%s: addr:%x\n", __func__, mcu_fw_flash_i2c_client->addr);

    
    ret = misc_register(&shub_fw_flash_miscdev);
    if (ret < 0) {
        E("%s: failed to register misc device for '%s'!\n", __func__, shub_fw_flash_miscdev.name);
        return ret;
    }

    
    I("%s: create device attribute %s %s\n", __func__, dev_name(shub_fw_flash_miscdev.this_device), dev_attr_fw_update_status.attr.name);
    ret = device_create_file(shub_fw_flash_miscdev.this_device, &dev_attr_fw_update_status);
    if (ret < 0) {
        E("%s: cant create device attribute %s %s\n", __func__, dev_name(shub_fw_flash_miscdev.this_device), dev_attr_fw_update_status.attr.name);
        return ret;
    }

    I("%s: create device attribute %s %s\n", __func__, dev_name(shub_fw_flash_miscdev.this_device), dev_attr_fw_update_timeout.attr.name);
    ret = device_create_file(shub_fw_flash_miscdev.this_device, &dev_attr_fw_update_timeout);
    if (ret < 0) {
        E("%s: cant create device attribute %s %s\n", __func__, dev_name(shub_fw_flash_miscdev.this_device), dev_attr_fw_update_timeout.attr.name);
        return ret;
    }

    I("%s: create device attribute %s %s\n", __func__, dev_name(shub_fw_flash_miscdev.this_device), dev_attr_fw_update_progress.attr.name);
    ret = device_create_file(shub_fw_flash_miscdev.this_device, &dev_attr_fw_update_progress);
    if (ret < 0) {
        E("%s: cant create device attribute %s %s\n", __func__, dev_name(shub_fw_flash_miscdev.this_device), dev_attr_fw_update_progress.attr.name);
        return ret;
    }

    return ret;
}

static int mcu_fw_flash_i2c_remove(struct i2c_client *client)
{
    I("%s\n", __func__);

    misc_deregister(&shub_fw_flash_miscdev);

    return 0;
}

static const struct i2c_device_id mcu_fw_flash_id[] = {
    {MCU_I2C_FLASH_NAME, 0},
    { }
};

#ifdef CONFIG_OF
static struct of_device_id mcu_fw_flash_match_table[] = {
    {.compatible = "htc_mcu_flash" },
    {},
};
#else
#define mcu_fw_flash_match_table NULL
#endif

static struct i2c_driver mcu_fw_flash_i2c_driver = {
    .driver = {
        .name = MCU_I2C_FLASH_NAME,
        .owner = THIS_MODULE,
        .of_match_table = mcu_fw_flash_match_table,
    },
    .probe    = mcu_fw_flash_i2c_probe,
    .remove   = mcu_fw_flash_i2c_remove,
    .id_table = mcu_fw_flash_id,
};
#endif 

#ifdef SHUB_LOGGING_SUPPORT


static void mcu_set_log_mask(u32 log_mask)
{
    int ret;

    s_mcu_data->mcu_log_mask = log_mask;
    if (MCU_IN_SHUB()) {
        ret = CWMCU_i2c_write_block_power(s_mcu_data, CW_I2C_REG_LOG_MASK, (u8*)&log_mask, sizeof(log_mask));
        I("%s(%d): log_mask:0x%x ret:%d\n", __func__, __LINE__, log_mask, ret);
    }
    else {
        I("%s(%d): MCU not in SHUB state\n", __func__, __LINE__);
    }
}

static int mcu_get_log_mask(u32 *log_mask_ptr)
{
    int ret = -1;

    if (MCU_IN_SHUB()) {
        ret = CWMCU_i2c_read_power(s_mcu_data, CW_I2C_REG_LOG_MASK, (u8*)log_mask_ptr, sizeof(u32));
        I("%s(%d): log_mask:0x%x ret:%d\n", __func__, __LINE__, *log_mask_ptr, ret);
        s_mcu_data->mcu_log_mask = *log_mask_ptr;
    }
    else {
        I("%s(%d): MCU not in SHUB state\n", __func__, __LINE__);
    }

    return ret;
}

static ssize_t log_mask_show(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf)
{
    uint32_t log_mask = 0;
    int ret;
    ret = mcu_get_log_mask(&log_mask);
    if (ret < 0)
        log_mask = 0;

    return snprintf(buf, PAGE_SIZE, "0x%x\n", (int)(log_mask));
}

static ssize_t log_mask_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf,
                                  size_t count)
{
    uint32_t log_mask = 0;
    int error;

    error = kstrtou32(buf, 16, &log_mask);
    if (error) {
        E("%s[%d]: kstrtoul fails, error = %d\n", __func__, __LINE__, error);
    }
    else {
        I("%s(%d): 0x%x\n", __func__, __LINE__, log_mask);
        mcu_set_log_mask(log_mask);
    }

    return count;
}

static void mcu_set_log_level(u32 log_level)
{
    int ret;

    s_mcu_data->mcu_log_level = log_level;
    if (MCU_IN_SHUB()) {
        ret = CWMCU_i2c_write_block_power(s_mcu_data, CW_I2C_REG_LOG_LEVEL, (u8*)&log_level, sizeof(log_level));
        I("%s(%d): log_level:0x%x ret:%d\n", __func__, __LINE__, log_level, ret);
    }
    else {
        I("%s(%d): MCU not in SHUB state\n", __func__, __LINE__);
    }
}

static int mcu_get_log_level(u32 *log_level_ptr)
{
    int ret = -1;

    if (MCU_IN_SHUB()) {
        ret = CWMCU_i2c_read_power(s_mcu_data, CW_I2C_REG_LOG_LEVEL, (u8*)log_level_ptr, sizeof(u32));
        I("%s(%d): log_level:0x%x ret:%d\n", __func__, __LINE__, *log_level_ptr, ret);
        s_mcu_data->mcu_log_level = *log_level_ptr;
    }
    else {
        I("%s(%d): MCU not in SHUB state\n", __func__, __LINE__);
    }

    return ret;
}

static ssize_t log_level_show(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf)
{
    uint32_t log_level = 0;
    int ret;
    ret = mcu_get_log_level(&log_level);
    if (ret < 0)
        log_level = 0;

    return snprintf(buf, PAGE_SIZE, "%d\n", (int)(log_level));
}

static ssize_t log_level_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf,
                                  size_t count)
{
    uint32_t log_level = 0;
    int error;

    error = kstrtou32(buf, 10, &log_level);
    if (error) {
        E("%s[%d]: kstrtoul fails, error = %d\n", __func__, __LINE__, error);
    }
    else {
        I("%s(%d): %d\n", __func__, __LINE__, log_level);
        mcu_set_log_level(log_level);
    }

    return count;
}

static ssize_t sensor_placement_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf,
			size_t count)
{
	char *str_buf;
	char *running;
	long sensors_id = 0;
	long placement = 0;
	uint8_t position, cali_data_from_fw;
	int i, rc;
	struct cwmcu_data *mcu_data = dev_get_drvdata(dev);

	str_buf = kstrndup(buf, count, GFP_KERNEL);
	if (str_buf == NULL) {
		E("%s: cannot allocate buffer\n", __func__);
		return -ENOMEM;
	}
        running = str_buf;

	for (i = 0; i < 2; i++) {
		int error;
		char *token;

		token = strsep(&running, " ");

		if (i == 0)
		        error = kstrtol(token, 10, &sensors_id);
		else {
		        if (token == NULL) {
		                placement = sensors_id;
		                sensors_id = 0;
		        } else
		                error = kstrtol(token, 10, &placement);
		}
		if (error) {
		        E("%s: kstrtol fails, error = %d, i = %d\n",
		                __func__, error, i);
		        kfree(str_buf);
		        return error;
		}
	}
	switch(sensors_id){
		case(1):
			position = GENSOR_POSITION;
			break;
		case(2):
			position = COMPASS_POSITION;
			break;
		case(3):
			position = GYRO_POSITION;
			break;
		default:
			E("Sensor id %d is not in range, please in range 1-3\n", (int)sensors_id);
			return count;
	}
	for (i = 0; i < 3; i++) {
		CWMCU_i2c_write_power(mcu_data, position,
				(uint8_t*)&placement,
				1);
		rc = CWMCU_i2c_read_power(mcu_data, position,
			       &cali_data_from_fw, 1);
		if (rc >= 0) {
			if (cali_data_from_fw == placement)
				break;
			else {
				I("%s: cali_data_from_fw = 0x%x, "
				  "sensors_axes = 0x%x\n",
				  __func__, cali_data_from_fw,
				  (unsigned int)placement);
			}
		} else {
			I("%s: sensors_id %d  i2c read fails, rc = %d\n",
			  __func__, (int)sensors_id, rc);
		}
	}

	return count;
}

static u32 mcu_get_log_size(void)
{
    int ret;
    struct log_size_struct {
        u32 log_size;
        u32 drop_count;
    } cwmcu_log_size = {0, 0};

    if (MCU_IN_SHUB()) {
        ret = CWMCU_i2c_read_power(s_mcu_data, CW_I2C_REG_LOG_SIZE, (u8*)&cwmcu_log_size, sizeof(cwmcu_log_size));
        I("%s: ret:%d, log_size = %d, drop_count = %d\n", __func__, ret, cwmcu_log_size.log_size, cwmcu_log_size.drop_count);
    }
    else {
        I("%s: MCU not in SHUB state\n", __func__);
    }

    return cwmcu_log_size.log_size;
}

static int shub_log_open(struct inode *inode, struct file *file)
{
    if (!file)
        return -ENODEV;

    I("%s(%d)\n", __func__, __LINE__);

    return 0;
}

static int shub_log_release(struct inode *inode, struct file *file)
{
    file->private_data = NULL;
    return 0;
}

static ssize_t shub_log_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    int ret;
    int read_len = 0;
    int total_read_len = 0;
    unsigned long  start_jiffies = jiffies;
    D("%s(%d): count(%lu)\n", __func__, __LINE__, count);

    if (MCU_IN_SHUB()) {
        u8 i2c_log_buf[I2C_LOG_READ_LEN];

        while (count > 0) {
            if (count < I2C_LOG_READ_LEN) {
                D("%s(%d): count(%u) < I2C_LOG_READ_LEN(%u)\n", __func__, __LINE__, (unsigned int)count, I2C_LOG_READ_LEN);
                goto EXIT;
            }
            ret = CWMCU_i2c_read_power(s_mcu_data, CW_I2C_REG_LOG_DATA, i2c_log_buf, I2C_LOG_READ_LEN);

            if (ret < 0) {
                E("%s: Read LOG_DATA fails, ret = %d\n", __func__, ret);
                goto EXIT;
            } else {
                read_len = ret;
                ret = copy_to_user(buf, i2c_log_buf, read_len);
                if (ret) {
                    E("%s(%d): copy_to_user failed err:%d\n", __func__, __LINE__, ret);
                    goto EXIT;
                }
            }

            D("%s: Read LOG_DATA, len:%d, log:%s\n", __func__, read_len, i2c_log_buf);
            count -= read_len;
            total_read_len += read_len;
            buf += read_len;
        }
    }

EXIT:
    I("%s(%d): return total_read_len(%d), elapsed_ms:%u\n", __func__, __LINE__, total_read_len, jiffies_to_msecs(jiffies - start_jiffies));
    return total_read_len;
}

static ssize_t shub_log_write(struct file *file, const char __user * buf, size_t size, loff_t *pos)
{
    return -ENODEV;
}

static long shub_log_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret = 0;

    
    while (!MCU_IN_SHUB()) {
        unsigned long timeout = wait_for_completion_timeout(&s_mcu_enter_shub_run, (msecs_to_jiffies(30000)));
        if (timeout == 0) {
            E("%s(%d): wait_for s_mcu_enter_shub_run timeout !!!\n", __func__, __LINE__);
        } else {
            I("%s(%d): s_mcu_enter_shub_run completely\n", __func__, __LINE__);
            break;
        }
    }

    switch (cmd) {
    case SHUB_LOG_IOCTL_WAIT_FOR_NOTIFY:
        {
            uint32_t mcu_log_type = 0;
            ret = wait_for_completion_interruptible(&s_mcu_log_avail);
            if (ret != -ERESTARTSYS) {
                I("%s(%d): s_mcu_log_avail retval:%d, mcu_log_type:0x%x\n", __func__, __LINE__, ret, mcu_log_type);
            }
            if (!ret) {
                put_user(mcu_log_type, (unsigned int __user *)arg);
            }
            INIT_COMPLETION(s_mcu_log_avail);
        }
        break;

    case SHUB_LOG_IOCTL_GET_LOG_SIZE:
        {
            unsigned int mcu_log_size = 0;
            mcu_log_size = mcu_get_log_size();
            put_user(mcu_log_size, (unsigned int __user *)arg);
        }
        break;


    case SHUB_LOG_IOCTL_SET_LOGMASK:
        {
            uint32_t log_mask = 0;
            if (copy_from_user(&log_mask, (unsigned int __user *)arg, sizeof(log_mask))) {
                E("%s(%d): CWMCU_IOCTL_SET_LOGMASK invalid param\n", __func__, __LINE__);
                return -EFAULT;
            }
            mcu_set_log_mask(log_mask);
        }
        break;

    case SHUB_LOG_IOCTL_GET_LOGMASK:
        {
            uint32_t log_mask = 0;
            int ret;
            ret = mcu_get_log_mask(&log_mask);
            if (ret < 0)
                log_mask = 0;
            put_user(log_mask, (unsigned int __user *)arg);
        }
        break;

    case SHUB_LOG_IOCTL_SET_LOGLEVEL:
        {
            uint32_t log_level = 0;
            if (copy_from_user(&log_level, (unsigned int __user *)arg, sizeof(log_level))) {
                E("%s(%d): CWMCU_IOCTL_SET_LOGMASK invalid param\n", __func__, __LINE__);
                return -EFAULT;
            }
            mcu_set_log_level(log_level);
        }
        break;

    case SHUB_LOG_IOCTL_GET_LOGLEVEL:
        {
            uint32_t log_level = 0;
            int ret;
            ret = mcu_get_log_level(&log_level);
            put_user(log_level, (unsigned int __user *)arg);
        }
        break;

    case SHUB_LOG_IOCTL_GET_LOG_START:
        {
        }
        break;

    case SHUB_LOG_IOCTL_GET_LOG_DONE:
        {
        }
        break;

    default:
        E("%s(%d): INVALID param:0x%x\n", __func__, __LINE__, cmd);
        ret = -EINVAL;
        break;
    }

    return ret;
}

static const struct file_operations shub_log_fops = {
    .owner = THIS_MODULE,
    .read  = shub_log_read,
    .write = shub_log_write,
    .unlocked_ioctl = shub_log_ioctl,
    .compat_ioctl = shub_log_ioctl,
    .open  = shub_log_open,
    .release = shub_log_release,
};

static struct miscdevice shub_log_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = SHUB_LOG_DEVICE_NAME,
    .fops = &shub_log_fops
};
#endif 

#ifdef SHUB_EVENT_SUPPORT

static u32 mcu_get_event_size(void)
{
    int ret;
    u32 event_size = 0;

    if (MCU_IN_SHUB()) {
        ret = CWMCU_i2c_read_power(s_mcu_data, CW_I2C_REG_EVENT_SIZE, (u8*)&event_size, sizeof(event_size));
        I("%s: ret:%d, event_size = %d\n", __func__, ret, event_size);
    }
    else {
        I("%s: MCU not in SHUB state\n", __func__);
    }

    return event_size;
}


static int shub_event_open(struct inode *inode, struct file *file)
{
    if (!file)
        return -ENODEV;

    I("%s(%d)\n", __func__, __LINE__);

    return 0;
}

static int shub_event_release(struct inode *inode, struct file *file)
{
    file->private_data = NULL;
    return 0;
}

static ssize_t shub_event_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    int ret;
    int read_len = 0;
    int total_read_len = 0;
    unsigned long  start_jiffies = jiffies;
    D("%s(%d): count(%lu)\n", __func__, __LINE__, count);

    if (MCU_IN_SHUB()) {
        u8 i2c_event_buf[I2C_EVENT_READ_LEN];

        while (count > 0) {
            if (count < I2C_EVENT_READ_LEN) {
                D("%s(%d): count(%u) < I2C_EVENT_READ_LEN(%u)\n", __func__, __LINE__, (unsigned int)count, I2C_EVENT_READ_LEN);
                goto EXIT;
            }
            ret = CWMCU_i2c_read_power(s_mcu_data, CW_I2C_REG_EVENT_DATA, i2c_event_buf, I2C_EVENT_READ_LEN);

            if (ret < 0) {
                E("%s: Read EVENT_DATA fails, ret = %d\n", __func__, ret);
                goto EXIT;
            } else {
                read_len = ret;
                ret = copy_to_user(buf, i2c_event_buf, read_len);
                if (ret) {
                    E("%s(%d): copy_to_user failed err:%d\n", __func__, __LINE__, ret);
                    goto EXIT;
                }
            }

            count -= read_len;
            total_read_len += read_len;
            buf += read_len;
        }
    }

EXIT:
    I("%s(%d): return total_read_len(%d), elapsed_ms:%u\n", __func__, __LINE__, total_read_len, jiffies_to_msecs(jiffies - start_jiffies));
    return total_read_len;
}

static ssize_t shub_event_write(struct file *file, const char __user * buf, size_t size, loff_t *pos)
{
    return -ENODEV;
}

static long shub_event_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret = 0;

    
    while (!MCU_IN_SHUB()) {
        unsigned long timeout = wait_for_completion_timeout(&s_mcu_enter_shub_run, (msecs_to_jiffies(30000)));
        if (timeout == 0) {
            E("%s(%d): wait_for s_mcu_enter_shub_run timeout !!!\n", __func__, __LINE__);
        } else {
            I("%s(%d): s_mcu_enter_shub_run completely\n", __func__, __LINE__);
            break;
        }
    }

    switch (cmd) {
    case SHUB_EVENT_IOCTL_WAIT_FOR_NOTIFY:
        {
            uint32_t mcu_event_type = 0;
            ret = wait_for_completion_interruptible(&s_mcu_event_avail);
            I("%s(%d): s_mcu_event_avail retval:%d, mcu_event_type:0x%x\n", __func__, __LINE__, ret, mcu_event_type);
            if (!ret) {
                put_user(mcu_event_type, (unsigned int __user *)arg);
            }
            INIT_COMPLETION(s_mcu_event_avail);
        }
        break;

    case SHUB_EVENT_IOCTL_GET_EVENT_SIZE:
        {
            unsigned int mcu_event_size = 0;
            mcu_event_size = mcu_get_event_size();
            put_user(mcu_event_size, (unsigned int __user *)arg);
        }
        break;

    case SHUB_EVENT_IOCTL_GET_EVENT_START:
        {
        }
        break;

    case SHUB_EVENT_IOCTL_GET_EVENT_DONE:
        {
        }
        break;

    default:
        E("%s(%d): INVALID param:0x%x\n", __func__, __LINE__, cmd);
        ret = -EINVAL;
        break;
    }

    return ret;
}

static const struct file_operations shub_event_fops = {
    .owner = THIS_MODULE,
    .read  = shub_event_read,
    .write = shub_event_write,
    .unlocked_ioctl = shub_event_ioctl,
    .compat_ioctl = shub_event_ioctl,
    .open  = shub_event_open,
    .release = shub_event_release,
};

static struct miscdevice shub_event_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = SHUB_EVENT_DEVICE_NAME,
    .fops = &shub_event_fops
};
#endif 

#ifdef SHUB_DLOAD_SUPPORT
#define MCU_I2C_DLOAD_NAME "i2c-mcu-dload"
#define STM32MCUF411_RAM_START_ADDR       (0x20000000)
#define STM32MCUF411_RAM_SIZE             (0x20000)

static struct i2c_client *dload_i2c_client = NULL;

static unsigned int shub_ramdump_size = 0;
static unsigned int ramdump_capture_inprogress = 0;



static int mcu_dload_i2c_write(u8 cmd, u8 *data, u8 len)
{
    int ret;

    if (!dload_i2c_client) return -EIO;

    ret = mcu_i2c_tx_cmd(dload_i2c_client, cmd, data, len);
    if (ret < 0) {
        E("%s error, cmd = 0x%x, ret = %d\n", __func__, cmd, ret);
    }

    return ret;
}

static int mcu_dload_i2c_read(u8 cmd, u8 *data, u8 len)
{
    int ret;

    if (!dload_i2c_client) return -EIO;

    ret = mcu_i2c_rx_cmd(dload_i2c_client, cmd, data, len);
    if (ret < 0) {
        E("%s error: cmd = 0x%x, ret = %d\n", __func__, cmd, ret);
    }

    return ret;
}

static int mcu_dload_dump_backup_registers(void)
{
    int ret = 0;
    u8 bkp_reg;
    u32 data;

    for (bkp_reg = 0; bkp_reg <= 19; bkp_reg++) {
        ret = mcu_dload_i2c_write(CW_I2C_REG_DUMP_BACKUP_REG, (u8*)&bkp_reg, sizeof(bkp_reg));
        if (ret < 0) {
            E("%s: write CW_I2C_REG_DUMP_BACKUP_REG [%d] fails, ret = %d\n",
                __func__, bkp_reg, ret);
            break;
        }

        ret = mcu_dload_i2c_read(CW_I2C_REG_DUMP_BACKUP_REG, (u8*)&data, sizeof(data));
        if (ret < 0) {
            E("%s: BackupRegister[%d] dump fails, ret = %d\n",
                __func__, bkp_reg, ret);
            break;
        } else {
            I("%s: BackupRegister[%d] = 0x%x\n",
                __func__, bkp_reg, data);
        }
    }

    return ret;
}

#ifdef SHUB_DLOAD_DUMP_EXCEPTION_BUFFER
static int mcu_dload_dump_exception_buffer(void)
{
    int ret;
    u32 exception_len = 0;

    ret = mcu_dload_i2c_read(CW_I2C_REG_EXCEPTION_BUFFER_LEN, (u8*)&exception_len, sizeof(exception_len));
    if (ret >= 0) {
        u8 data[EXCEPTION_BLOCK_LEN];
        int i;

        E("%s: exception_len = %u\n", __func__, exception_len);
        if (exception_len > EXCEPTION_LEN_MAX)
            exception_len = EXCEPTION_LEN_MAX;

        for (i = 0; exception_len > 0 ; i++) {
            memset(data, 0, sizeof(data));
            ret = mcu_dload_i2c_read(CW_I2C_REG_EXCEPTION_BUFFER, data, sizeof(data));
            if (ret >= 0) {
                char buf[3*EXCEPTION_BLOCK_LEN];
                u32 print_len = ((exception_len > EXCEPTION_BLOCK_LEN) ? EXCEPTION_BLOCK_LEN : exception_len);

                print_hex_data(buf, i, data, print_len);
                exception_len -= print_len;
            } else {
                E("%s: i = %d, excp1 i2c_read: ret = %d\n", __func__, i, ret);
                break;
            }
        }
    } else {
        E("%s: Exception status dump fails, ret = %d\n", __func__, ret);
    }

    return ret;
}
#endif 

static inline int MCU_I2C_WRITE(u8 reg_addr, u8 *data, u8 len)
{
    if (MCU_IN_DLOAD() || (MCU_IN_UNKNOWN()&&(s_mcu_data->gpio_mcu_status_level==MCU2CPU_STATUS_GPIO_LEVEL_DLOAD)))
        return mcu_dload_i2c_write(reg_addr, data, len);
    else if (MCU_IN_SHUB() || (MCU_IN_UNKNOWN()&&(s_mcu_data->gpio_mcu_status_level==MCU2CPU_STATUS_GPIO_LEVEL_SHUB)))
        return CWMCU_i2c_write_block_power(s_mcu_data, reg_addr, data, len);
    else
        return 0;
}

static inline int MCU_I2C_READ(u8 reg_addr, u8 *data, u8 len)
{
    if (MCU_IN_DLOAD() || (MCU_IN_UNKNOWN()&&(s_mcu_data->gpio_mcu_status_level==MCU2CPU_STATUS_GPIO_LEVEL_DLOAD)))
        return mcu_dload_i2c_read(reg_addr, data, len);
    else if (MCU_IN_SHUB() || (MCU_IN_UNKNOWN()&&(s_mcu_data->gpio_mcu_status_level==MCU2CPU_STATUS_GPIO_LEVEL_SHUB)))
        return CWMCU_i2c_read_power(s_mcu_data, reg_addr, data, len);
    else
        return 0;
}

static int mcu_set_reboot_state(u32 state)
{
    u32 reboot_state_read = 0xff;
    int ret;

    I("%s: 0x%x s_mcu_state:%d\n", __func__, state, s_mcu_state);

    if (state != MCU_SYS_STATUS_DLOAD && state != MCU_SYS_STATUS_SHUB) {
        I("%s: Invalid state:0x%x\n", __func__, state);
        return -EINVAL;
    }

    ret = MCU_I2C_WRITE(CW_I2C_REG_REBOOT_MODE, (u8*)&state, sizeof(state));
    if (ret < 0) {
        E("%s[%d]: failed, ret=%d\n", __func__, __LINE__, ret);
        return ret;
    }

    ret = MCU_I2C_READ(CW_I2C_REG_REBOOT_MODE, (u8*)&reboot_state_read, sizeof(reboot_state_read));
    if (ret < 0) {
        E("%s[%d]: failed, ret=%d\n", __func__, __LINE__, ret);
        return ret;
    }

    if (reboot_state_read != state) {
        E("%s: mode_read(0x%x) != mode(0x%x)\n", __func__, reboot_state_read, state);
        ret = -EIO;
    }

    return ret;
}

static void mcu_enable_disable_dload_mode(bool en)
{
    s_mcu_data->dload_mode_enabled = en;
    if (s_mcu_data->dload_mode_enabled) {
        I("%s(%d): ramdump dbg mode is enabled, set hub reboot state to MCU_STATE_DLOAD\n", __func__, __LINE__);
        mcu_set_reboot_state(MCU_SYS_STATUS_DLOAD);
    }
    else {
        I("%s(%d): ramdump dbg mode is disabled, set hub reboot state to MCU_STATE_NORMAL\n", __func__, __LINE__);
        mcu_set_reboot_state(MCU_SYS_STATUS_SHUB);
    }
}



static ssize_t dload_enable_show(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf)
{
    return snprintf(buf, PAGE_SIZE, "%d\n", (int)(s_mcu_data->dload_mode_enabled));
}

static ssize_t dload_enable_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf,
                                  size_t count)
{
    unsigned long tmp;
    int error;
    error = kstrtoul(buf, 10, &tmp);
    if (error) {
        E("%s[%d]: kstrtoul fails, error = %d\n", __func__, __LINE__, error);
    }
    else {
        I("%s(%d): dload_mode_enabled:%lu\n", __func__, __LINE__, tmp);
        mcu_enable_disable_dload_mode(tmp);
    }

    return count;
}

static DEVICE_ATTR(dload_enable, S_IRUSR | S_IWUSR, dload_enable_show, dload_enable_store);

static int shub_dload_open(struct inode *inode, struct file *file)
{
    if (!file)
        return -ENODEV;

    I("%s(%d)\n", __func__, __LINE__);

    return 0;
}

static int shub_dload_release(struct inode *inode, struct file *file)
{
    file->private_data = NULL;
    return 0;
}

static ssize_t shub_dload_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    int ret;
    int read_len = 0;
    int total_read_len = 0;
    unsigned long  start_jiffies = jiffies;
    D("%s(%d): count(%lu) ramdump_capture_inprogress(%d)\n", __func__, __LINE__, count, ramdump_capture_inprogress);

    if (ramdump_capture_inprogress ){
        u8 i2c_ramdump_buf[I2C_RAMDUMP_READ_LEN];

        while (count > 0) {
            if (count < I2C_RAMDUMP_READ_LEN) {
                E("%s(%d): count(%u) < I2C_RAMDUMP_READ_LEN(%u)\n", __func__, __LINE__, (unsigned int)count, I2C_RAMDUMP_READ_LEN);
                goto EXIT;
            }
            ret = mcu_dload_i2c_read(CW_I2C_REG_CAPTURE_RAMDUMP, i2c_ramdump_buf, I2C_RAMDUMP_READ_LEN);

            if (ret < 0) {
                E("%s: Read RAMDUMP fails, ret = %d\n", __func__, ret);
                goto EXIT;
            } else {
                read_len = ret;
                ret = copy_to_user(buf, i2c_ramdump_buf, read_len);
                if (ret) {
                    E("%s(%d): copy_to_user failed err:%d\n", __func__, __LINE__, ret);
                    goto EXIT;
                }
            }

            D("%s: Read RAMDUMP, len = %d\n", __func__, read_len);
            count -= read_len;
            total_read_len += read_len;
            buf += read_len;
        }
    }

EXIT:
    I("%s(%d): return total_read_len(%d), elapsed_ms:%u\n", __func__, __LINE__, total_read_len, jiffies_to_msecs(jiffies - start_jiffies));
    return total_read_len;
}

static ssize_t shub_dload_write(struct file *file, const char __user * buf, size_t size, loff_t *pos)
{
    return -ENODEV;
}

static long shub_dload_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    static unsigned long  ramdump_start_jiffies = 0;

    switch (cmd) {
    case SHUB_DLOAD_IOCTL_WAIT_FOR_NOTIFY:
        {
            uint32_t mcu_dload_type = 0;
            ret = wait_for_completion_interruptible(&s_mcu_ramdump_avail);
            if (ret != -ERESTARTSYS) {
                I("%s(%d): s_mcu_ramdump_avail retval:%d mcu_dload_type:%d\n", __func__, __LINE__, ret, mcu_dload_type);
            }
            if (!ret) {
                put_user(mcu_dload_type, (unsigned int __user *)arg);
            }
            INIT_COMPLETION(s_mcu_ramdump_avail);
        }
        break;

    case SHUB_DLOAD_IOCTL_GET_RAMDUMP_SIZE:
        {
            put_user(shub_ramdump_size, (unsigned int __user *)arg);
        }
        break;

    case SHUB_DLOAD_IOCTL_RAMDUMP_START:
        {
            struct cwmcu_ramdump_param ramdump_param;

            ramdump_capture_inprogress = 1;

            if (is_htc_dbg_flag_set()) {
                ramdump_param.start_addr = STM32MCUF411_RAM_START_ADDR;
                ramdump_param.size = STM32MCUF411_RAM_SIZE;

                mcu_dload_i2c_write(CW_I2C_REG_CAPTURE_RAMDUMP, ((u8*)&ramdump_param), sizeof(struct cwmcu_ramdump_param));
                shub_ramdump_size = ramdump_param.size;
                I("%s(%d): RAMDUMP_START start_addr:0x%x, size:0x%x\n", __func__, __LINE__, ramdump_param.start_addr, ramdump_param.size);
            }
            ramdump_start_jiffies = jiffies;
        }
        break;

    case SHUB_DLOAD_IOCTL_RAMDUMP_DONE:
        {
            unsigned long timeout;
            unsigned long  elapsed_ms = jiffies_to_msecs(jiffies - ramdump_start_jiffies);
            ramdump_capture_inprogress = 0;

            I("%s(%d): RAMDUMP_DONE, elapsed_ms:%lu\n", __func__, __LINE__, elapsed_ms);

            if (MCU_IN_DLOAD()) {
                I("%s(%d): reset mcu to SHUB state\n", __func__, __LINE__);
                mcu_set_reboot_state(MCU_SYS_STATUS_SHUB);
                if (s_mcu_data) reset_hub(s_mcu_data, true);
                timeout = wait_for_completion_timeout(&s_mcu_enter_shub_run, (msecs_to_jiffies(5000)));
                if (timeout == 0) {
                    E("%s(%d): wait_for s_mcu_enter_shub_run timeout !!!\n", __func__, __LINE__);
                } else {
                    I("%s(%d): s_mcu_enter_shub_run completely\n", __func__, __LINE__);
                }
            }
        }
        break;

    case SHUB_DLOAD_IOCTL_ENABLE_DLOAD:
        {
            uint8_t en;
            if (copy_from_user(&(en), (unsigned int __user *)arg, sizeof(en))) {
                E("%s(%d): SHUB_DLOAD_IOCTL_ENABLE_DLOAD invalid param\n", __func__, __LINE__);
                return -EFAULT;
            }

            I("%s(%d): SHUB_DLOAD_IOCTL_ENABLE_DLOAD en:%d\n", __func__, __LINE__, en);

            if (!MCU_IN_SHUB()) {
                unsigned long timeout = wait_for_completion_timeout(&s_mcu_enter_shub_run, (msecs_to_jiffies(30000)));
                if (timeout == 0) {
                    E("%s(%d): wait_for s_mcu_enter_shub_run timeout !!!\n", __func__, __LINE__);
                } else {
                    I("%s(%d): s_mcu_enter_shub_run completely\n", __func__, __LINE__);
                }
            }

            mcu_enable_disable_dload_mode(en);
        }
        break;

    default:
        E("%s(%d): INVALID param:0x%x\n", __func__, __LINE__, cmd);
        ret = -EINVAL;
        break;
    }

    return ret;
}

static const struct file_operations shub_dload_fops = {
    .owner = THIS_MODULE,
    .read  = shub_dload_read,
    .write = shub_dload_write,
    .unlocked_ioctl = shub_dload_ioctl,
    .compat_ioctl = shub_dload_ioctl,
    .open  = shub_dload_open,
    .release = shub_dload_release,
};

static struct miscdevice shub_dload_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = SHUB_DLOAD_DEVICE_NAME,
    .fops = &shub_dload_fops
};


static int mcu_dload_i2c_probe(struct i2c_client *client,
            const struct i2c_device_id *id)
{
    int ret;

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        E("%s: i2c_check_functionality error\n", __func__);
        return -EIO;
    }

    dload_i2c_client = client;
    I("%s: addr:%x\n", __func__,dload_i2c_client->addr);

    
    ret = misc_register(&shub_dload_miscdev);
    if (ret < 0) {
        E("%s: failed to register misc device for '%s'!\n", __func__, shub_dload_miscdev.name);
        return ret;
    }

    
    I("%s: create device attribute %s %s\n", __func__, dev_name(shub_dload_miscdev.this_device), dev_attr_dload_enable.attr.name);
    ret = device_create_file(shub_dload_miscdev.this_device, &dev_attr_dload_enable);
    if (ret < 0) {
        E("%s: cant create device attribute %s %s\n", __func__, dev_name(shub_dload_miscdev.this_device), dev_attr_dload_enable.attr.name);
        return ret;
    }

    return 0;
}

static int mcu_dload_i2c_remove(struct i2c_client *client)
{
    I("%s\n", __func__);

    misc_deregister(&shub_dload_miscdev);

    return 0;
}

static const struct i2c_device_id mcu_dload_id[] = {
    {MCU_I2C_DLOAD_NAME, 0},
    { }
};

#ifdef CONFIG_OF
static struct of_device_id mcu_dload_match_table[] = {
    {.compatible = "htc_mcu_dload" },
    {},
};
#else
#define mcu_dload_match_table NULL
#endif

static struct i2c_driver mcu_dload_i2c_driver = {
    .driver = {
        .name = MCU_I2C_DLOAD_NAME,
        .owner = THIS_MODULE,
        .of_match_table = mcu_dload_match_table,
    },
    .probe    = mcu_dload_i2c_probe,
    .remove   = mcu_dload_i2c_remove,
    .id_table = mcu_dload_id,
};
#endif 

#ifdef CONFIG_FB
static int fb_notifier_callback(struct notifier_block *self,
                                 unsigned long event, void *data)
{
    struct fb_event *evdata = data;
    int *blank;

    D("%s\n", __func__);
    if (evdata && evdata->data && event == FB_EVENT_BLANK && s_mcu_data &&
                    s_mcu_data->client) {
        blank = evdata->data;
        switch (*blank) {
        case FB_BLANK_UNBLANK:
            I("MCU late_resume\n");
            if (!s_mcu_data->is_display_on) {
                s_mcu_data->is_display_on = true;
                mcu_set_display_state(s_mcu_data->is_display_on);

                #ifdef SHUB_LOGGING_SUPPORT
                complete(&s_mcu_log_avail);
                #endif 

                #ifdef SHUB_EVENT_SUPPORT
                complete(&s_mcu_event_avail);
                #endif 
            }
            break;

        case FB_BLANK_POWERDOWN:
        case FB_BLANK_HSYNC_SUSPEND:
        case FB_BLANK_VSYNC_SUSPEND:
        case FB_BLANK_NORMAL:
            I("MCU early_suspend\n");
            if (s_mcu_data->is_display_on) {
                s_mcu_data->is_display_on = false;
                mcu_set_display_state(s_mcu_data->is_display_on);
            }
            break;
        }
    }
    return 0;
}

static void mcu_fb_register(struct work_struct *work)
{
    int ret = 0;
    struct cwmcu_data *mcu_data = container_of((struct delayed_work *)work,
                                    struct cwmcu_data, delay_work_register_fb);
    I("%s in", __func__);

    mcu_data->fb_notif.notifier_call = fb_notifier_callback;
    ret = fb_register_client(&mcu_data->fb_notif);
    if (ret)
        E("MCU ERR:Unable to register fb_notifier: %d\n", ret);
}
#endif 

static int CWMCU_i2c_probe(struct i2c_client *client,
				       const struct i2c_device_id *id)
{
	struct cwmcu_data *mcu_data;
	struct iio_dev *indio_dev;
	int error;
	int i;

	I("%s++: Support 25Hz and 10Hz sampling rate\n", __func__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		E("%s: i2c_check_functionality error\n", __func__);
		return -EIO;
	}

	D("%s: sizeof(*mcu_data) = %lu\n", __func__, sizeof(*mcu_data));

	indio_dev = iio_device_alloc(sizeof(*mcu_data));
	if (!indio_dev) {
		I("%s: iio_device_alloc failed\n", __func__);
		return -ENOMEM;
	}

	i2c_set_clientdata(client, indio_dev);

	indio_dev->name = CWMCU_I2C_NAME;
	indio_dev->dev.parent = &client->dev;
	indio_dev->info = &cw_info;
	indio_dev->channels = cw_channels;
	indio_dev->num_channels = ARRAY_SIZE(cw_channels);
	indio_dev->modes |= INDIO_BUFFER_TRIGGERED;

	mcu_data = iio_priv(indio_dev);
	mcu_data->client = client;
	mcu_data->indio_dev = indio_dev;

	s_mcu_data = mcu_data;

	if (client->dev.of_node) {
		D("Device Tree parsing.");

		error = mcu_parse_dt(&client->dev, mcu_data);
		if (error) {
			dev_err(&client->dev,
				"%s: mcu_parse_dt for pdata failed. err = %d\n"
					, __func__, error);
			goto exit_mcu_parse_dt_fail;
		}
	} else {
		if (client->dev.platform_data != NULL) {
			mcu_data->acceleration_axes =
				((struct cwmcu_platform_data *)
				 mcu_data->client->dev.platform_data)
				->acceleration_axes;
			mcu_data->magnetic_axes =
				((struct cwmcu_platform_data *)
				 mcu_data->client->dev.platform_data)
				->magnetic_axes;
			mcu_data->gyro_axes =
				((struct cwmcu_platform_data *)
				 mcu_data->client->dev.platform_data)
				->gyro_axes;
			mcu_data->gpio_wake_mcu =
				((struct cwmcu_platform_data *)
				 mcu_data->client->dev.platform_data)
				->gpio_wake_mcu;
		}
	}

	error = gpio_request(mcu_data->gpio_reset, "cwmcu_reset");
	if (error)
		E("%s : request reset gpio fail\n", __func__);

#ifdef SHUB_DLOAD_SUPPORT
	if (gpio_is_valid(mcu_data->gpio_mcu_status)) {
		error = gpio_request(mcu_data->gpio_mcu_status, "cwmcu_status");
		E("%s : request cwmcu_status\n", __func__);
		if (error)
			E("%s : request gpio_mcu_status gpio fail\n", __func__);

		gpio_direction_input(mcu_data->gpio_mcu_status);
	}
	mcu_data->gpio_mcu_status_level = MCU2CPU_STATUS_GPIO_LEVEL(mcu_data);
	I("%s : gpio_mcu_status_level:%d\n", __func__, mcu_data->gpio_mcu_status_level);
#endif 

	error = gpio_request(mcu_data->gpio_wake_mcu, "cwmcu_CPU2MCU");
	if (error)
		E("%s : request gpio_wake_mcu gpio fail\n", __func__);

	error = gpio_request(mcu_data->gpio_chip_mode, "cwmcu_hub_boot_mode");
	if (error)
		E("%s : request ghip mode gpio fail\n", __func__);

	gpio_direction_output(mcu_data->gpio_reset, 0);
	gpio_direction_output(mcu_data->gpio_wake_mcu, 0);
	mcu_chip_mode_set(mcu_data, MCU_CHIP_MODE_APPLICATION);

	error = gpio_request(mcu_data->gpio_mcu_irq, "cwmcu_int");
	if (error) {
		E("%s : request irq gpio fail\n", __func__);
	}

	mcu_pinctrl_init(mcu_data);
	mutex_init(&mcu_data->mutex_lock);
	mutex_init(&mcu_data->group_i2c_lock);
	mutex_init(&mcu_data->power_mode_lock);
	mutex_init(&mcu_data->lock);

	INIT_DELAYED_WORK(&mcu_data->work, cwmcu_work_report);
	INIT_WORK(&mcu_data->one_shot_work, cwmcu_one_shot);

	error = cw_probe_buffer(indio_dev);
	if (error) {
		E("%s: iio yas_probe_buffer failed\n", __func__);
		goto error_free_dev;
	}
	error = cw_probe_trigger(indio_dev);
	if (error) {
		E("%s: iio yas_probe_trigger failed\n", __func__);
		goto error_remove_buffer;
	}
	error = iio_device_register(indio_dev);
	if (error) {
		E("%s: iio iio_device_register failed\n", __func__);
		goto error_remove_trigger;
	}

	error = create_sysfs_interfaces(mcu_data);
	if (error)
		goto err_free_mem;

	for (i = 0; i < num_sensors; i++) {
		mcu_data->sensors_time[i] = 0;
		mcu_data->report_period[i] = 200000 * MS_TO_PERIOD;
	}

	wake_lock_init(&mcu_data->significant_wake_lock, WAKE_LOCK_SUSPEND,
		       "significant_wake_lock");
	wake_lock_init(&mcu_data->any_motion_wake_lock, WAKE_LOCK_SUSPEND,
		       "any_motion_wake_lock");

	atomic_set(&mcu_data->delay, CWMCU_MAX_DELAY);

	mcu_data->mcu_wq = create_singlethread_workqueue("htc_mcu");
	i2c_set_clientdata(client, mcu_data);
	pm_runtime_enable(&client->dev);

	client->irq = gpio_to_irq(mcu_data->gpio_mcu_irq);

	mcu_data->IRQ = client->irq;
	D("Requesting irq = %d\n", mcu_data->IRQ);
	error = request_threaded_irq(mcu_data->IRQ, NULL, cwmcu_irq_handler,
			IRQF_TRIGGER_RISING | IRQF_ONESHOT, "cwmcu", mcu_data);
	if (error)
		E("[CWMCU] could not request irq %d\n", error);
	error = enable_irq_wake(mcu_data->IRQ);
	if (error < 0)
		E("[CWMCU] could not enable irq as wakeup source %d\n", error);

	mutex_lock(&mcu_data->mutex_lock);
	mcu_data->suspended = false;
	mutex_unlock(&mcu_data->mutex_lock);

	
	vib_trigger_register_simple("vibrator", &vib_trigger);

#ifdef SHUB_LOGGING_SUPPORT
	error = misc_register(&shub_log_miscdev);
	if (error < 0) {
		E("%s: failed to register misc device for '%s'!\n", __func__, shub_log_miscdev.name);
	}
#endif 

#ifdef SHUB_EVENT_SUPPORT
	error = misc_register(&shub_event_miscdev);
	if (error < 0) {
		E("%s: failed to register misc device for '%s'!\n", __func__, shub_event_miscdev.name);
	}
#endif 

#ifdef CONFIG_FB
	INIT_DELAYED_WORK(&mcu_data->delay_work_register_fb, mcu_fb_register);
	queue_delayed_work(mcu_data->mcu_wq, &mcu_data->delay_work_register_fb, msecs_to_jiffies(15000));
#endif 

	mcu_data->probe_success = true;
	I("CWMCU_i2c_probe success!\n");

	
	mcu_data->w_kick_start_mcu = true;
	queue_work(mcu_data->mcu_wq, &mcu_data->one_shot_work);
	disable_irq(mcu_data->IRQ);
	return 0;

err_free_mem:
	if (indio_dev)
		iio_device_unregister(indio_dev);
error_remove_trigger:
	if (indio_dev)
		cwmcu_remove_trigger(indio_dev);
error_remove_buffer:
	if (indio_dev)
		cwmcu_remove_buffer(indio_dev);
error_free_dev:
	if (client->dev.of_node &&
	    ((struct cwmcu_platform_data *)mcu_data->client->dev.platform_data))
		kfree(mcu_data->client->dev.platform_data);
exit_mcu_parse_dt_fail:
	if (indio_dev)
		iio_device_free(indio_dev);
	i2c_set_clientdata(client, NULL);

	return error;
}


static int CWMCU_i2c_remove(struct i2c_client *client)
{
	struct cwmcu_data *mcu_data = i2c_get_clientdata(client);

	gpio_set_value(mcu_data->gpio_wake_mcu, 1);

#ifdef SHUB_LOGGING_SUPPORT
	misc_deregister(&shub_log_miscdev);
#endif 

#ifdef SHUB_EVENT_SUPPORT
	misc_deregister(&shub_event_miscdev);
#endif 

#ifdef CONFIG_FB
	if (fb_unregister_client(&mcu_data->fb_notif))
		E("%s: Error occurred while unregistering fb_notifier\n", __func__);
#endif 

	wake_lock_destroy(&mcu_data->significant_wake_lock);
	wake_lock_destroy(&mcu_data->any_motion_wake_lock);
	destroy_sysfs_interfaces(mcu_data);
	kfree(mcu_data);
	return 0;
}

static const struct dev_pm_ops cwmcu_pm_ops = {
	.suspend = cwmcu_suspend,
	.resume = cwmcu_resume
};


static const struct i2c_device_id cwmcu_id[] = {
	{CWMCU_I2C_NAME, 0},
	{ }
};
#ifdef CONFIG_OF
static struct of_device_id mcu_match_table[] = {
	{.compatible = "htc_mcu" },
	{},
};
#else
#define mcu_match_table NULL
#endif

MODULE_DEVICE_TABLE(i2c, cwmcu_id);

static struct i2c_driver cwmcu_driver = {
	.driver = {
		.name = CWMCU_I2C_NAME,
		   .owner = THIS_MODULE,
		.pm = &cwmcu_pm_ops,
		.of_match_table = mcu_match_table,
	},
	.probe    = CWMCU_i2c_probe,
	.remove   = CWMCU_i2c_remove,
	.id_table = cwmcu_id,
};

static int __init CWMCU_i2c_init(void)
{
#ifdef SHUB_DLOAD_SUPPORT
    i2c_add_driver(&mcu_dload_i2c_driver);
#endif 

#ifdef SHUB_FIRMWARE_UPDATE_SUPPORT
    i2c_add_driver(&mcu_fw_flash_i2c_driver);
#endif 

	return i2c_add_driver(&cwmcu_driver);
}
module_init(CWMCU_i2c_init);

static void __exit CWMCU_i2c_exit(void)
{
#ifdef SHUB_DLOAD_SUPPORT
    i2c_del_driver(&mcu_dload_i2c_driver);
#endif 

#ifdef SHUB_FIRMWARE_UPDATE_SUPPORT
    i2c_del_driver(&mcu_fw_flash_i2c_driver);
#endif 

    i2c_del_driver(&cwmcu_driver);
}
module_exit(CWMCU_i2c_exit);

MODULE_DESCRIPTION("CWMCU I2C Bus Driver V1.6");
MODULE_AUTHOR("CyWee Group Ltd.");
MODULE_LICENSE("GPL");
