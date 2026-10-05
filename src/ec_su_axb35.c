// ec_su_axb35.c

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>

extern int ec_read(u8 addr, u8 *val);
extern int ec_write(u8 addr, u8 val);

enum fan_mode { AUTO, FIXED, CURVE };

struct ec_fan {
    const char    *name;
    u8             speed_reg_high;
    u8             speed_reg_low;
    u8             mode_reg;
    u8             rampup_curve[6];
    u8             rampdown_curve[6];
    enum fan_mode  mode;
    struct device *dev;
};

struct ec_temp {
    const char    *name;
    u8             reg;
    u8             temp_min;
    u8             temp_max;
    struct device *dev;
};

struct ec_apu {
    const char    *name;
    u8             power_mode_reg;
    struct device *dev;
};

static struct class *ec_class;

static struct ec_fan ec_fans[] = {
    { .name           = "fan1",
      .speed_reg_high = 0x35,
      .speed_reg_low  = 0x36,
      .mode_reg       = 0x21,
      .rampup_curve   = { 0, 60, 70, 83, 95, 97 },
      .rampdown_curve = { 0, 40, 50, 80, 94, 96 } },
    { .name           = "fan2",
      .speed_reg_high = 0x37,
      .speed_reg_low  = 0x38,
      .mode_reg       = 0x23,
      .rampup_curve   = { 0, 60, 70, 83, 95, 97 },
      .rampdown_curve = { 0, 40, 50, 80, 94, 96 } },
    { .name           = "fan3",
      .speed_reg_high = 0x28,
      .speed_reg_low  = 0x29,
      .mode_reg       = 0x25,
      .rampup_curve   = { 0, 20, 60, 83, 95, 97 },
      .rampdown_curve = { 0, 0, 50, 80, 94, 96 } },
};

static struct ec_temp ec_temp = {
    .name = "temp1",
    .reg  = 0x70,
};

static struct ec_apu ec_apu = {
    .name           = "apu",
    .power_mode_reg = 0x31,
};

static int ec_fan_read_rpm(const struct ec_fan *fan, u16 *rpm)
{
    u8  hi;
    u8  lo;
    int ret;

    ret = ec_read(fan->speed_reg_high, &hi);
    if (ret)
        return ret;

    ret = ec_read(fan->speed_reg_low, &lo);
    if (ret)
        return ret;

    *rpm = (hi << 8) | lo;

    // wired fan3 behavior, displaying 8000 before turning to 0
    if (strcmp(fan->name, "fan3") == 0 && *rpm == 8000)
        *rpm = 0;

    return 0;
}

static ssize_t fan_rpm_show(struct device *dev, struct device_attribute *attr,
                            char *buf)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    u16            rpm;
    int            ret;

    ret = ec_fan_read_rpm(fan, &rpm);
    if (ret)
        return ret;

    return sprintf(buf, "%u\n", rpm);
}

static struct device_attribute dev_attr_fan_rpm =
    __ATTR(rpm, 0444, fan_rpm_show, NULL);

static void update_fan_mode(struct ec_fan *fan)
{
    u8 val;
    // TODO: handle error
    ec_read(fan->mode_reg, &val);

    switch (val) {
    case 0x10:
    case 0x20:
    case 0x30:
        fan->mode = AUTO;
        break;
    case 0x11:
    case 0x21:
    case 0x31:
        // curve and fixed use the value in the EC register
        // so fixed is only allowed if it was already know as
        // FIXED to the driver
        if (fan->mode != FIXED) {
            fan->mode = CURVE;
        } else {
            fan->mode = FIXED;
        }
        break;
    }
}

static ssize_t fan_mode_show(struct device *dev, struct device_attribute *attr,
                             char *buf)
{
    struct ec_fan *fan = dev_get_drvdata(dev);

    update_fan_mode(fan);

    const char *mode = "unknown";
    switch (fan->mode) {
    case AUTO:
        mode = "auto";
        break;
    case FIXED:
        mode = "fixed";
        break;
    case CURVE:
        mode = "curve";
        break;
    }

    return sprintf(buf, "%s\n", mode);
}

static u8 read_fan_level(struct ec_fan *fan)
{
    u8 val;
    // TODO: handle error
    ec_read(fan->mode_reg + 1, &val);

    switch (val & 0xF) {
    case 0x2: // 20%
        return 1;
    case 0x3: // 40%
        return 2;
    case 0x4: // 60%
        return 3;
    case 0x5: // 80%
        return 4;
    case 0x6: // 100%
        return 5;
    case 0x7:
    default: // off
        return 0;
    }
}

static ssize_t fan_level_show(struct device *dev, struct device_attribute *attr,
                              char *buf)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    return sprintf(buf, "%u\n", read_fan_level(fan));
}

static void write_fan_level(struct ec_fan *fan, u8 level)
{
    u8 val;

    switch (fan->mode_reg) {
    case 0x21:
        val = 0x10;
        break;
    case 0x23:
        val = 0x20;
        break;
    case 0x25:
        val = 0x30;
        break;
    }

    switch (level) {
    case 0:
        val += 0x7;
        break;
    case 1:
        val += 0x2;
        break;
    case 2:
        val += 0x3;
        break;
    case 3:
        val += 0x4;
        break;
    case 4:
        val += 0x5;
        break;
    case 5:
    default:
        val += 0x6;
    }

    // TODO: handle error
    ec_write(fan->mode_reg + 1, val);
}

static ssize_t fan_level_store(struct device           *dev,
                               struct device_attribute *attr, const char *buf,
                               size_t count)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    u8             val;

    if (kstrtou8(buf, 10, &val))
        return -EINVAL;

    write_fan_level(fan, val);

    return count;
}

static struct device_attribute dev_attr_fan_level =
    __ATTR(level, 0644, fan_level_show, fan_level_store);

static ssize_t fan_mode_store(struct device *dev, struct device_attribute *attr,
                              const char *buf, size_t count)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    u8             val;

    if (sysfs_streq(buf, "auto")) {
        fan->mode = AUTO;
    } else if (sysfs_streq(buf, "fixed")) {
        fan->mode = FIXED;
    } else if (sysfs_streq(buf, "curve")) {
        fan->mode = CURVE;
    } else {
        return -EINVAL;
    }

    switch (fan->mode) {
    case AUTO:
        fan->mode = AUTO;
        switch (fan->mode_reg) {
        case 0x21:
            val = 0x10;
            break;
        case 0x23:
            val = 0x20;
            break;
        case 0x25:
            val = 0x30;
            break;
        default:
            return -EINVAL;
        }
        break;
    case FIXED:
    case CURVE:
        switch (fan->mode_reg) {
        case 0x21:
            val = 0x11;
            break;
        case 0x23:
            val = 0x21;
            break;
        case 0x25:
            val = 0x31;
            break;
        default:
            return -EINVAL;
        }
        break;
    }

    // TODO: handle error
    ec_write(fan->mode_reg, val);

    // When switching to CURVE mode, set initial fan level based on current temperature
    // to prevent RPM burst from inappropriate starting level
    if (fan->mode == CURVE) {
        u8 temp;
        // TODO: handle error
        if (ec_read(ec_temp.reg, &temp) == 0) {
            u8 initial_level = 0;
            int i;
            
            // Find the appropriate level based on current temperature
            // Use rampup curve for initial positioning to be more responsive
            for (i = 5; i > 0; i--) {
                if (temp >= fan->rampup_curve[i]) {
                    initial_level = i;
                    break;
                }
            }
            
            write_fan_level(fan, initial_level);
        }
    }

    return count;
}

static struct device_attribute dev_attr_fan_mode =
    __ATTR(mode, 0644, fan_mode_show, fan_mode_store);

static ssize_t fan_curve_show(u8 *curve, char *buf)
{
    int   i;
    char *p = buf;

    for (i = 1; i < 6; i++) {
        p += sprintf(p, "%d", curve[i]);
        if (i < 5) {
            *p++ = ',';
        }
    }
    *p++ = '\n';
    *p   = '\0';

    return p - buf;
}

static ssize_t fan_curve_store(u8 *curve, const char *buf, size_t count)
{
    char *str = kstrndup(buf, count, GFP_KERNEL);
    char *token;
    int   values[5];
    int   i   = 0;
    int   ret = 0;

    if (!str)
        return -ENOMEM;

    token = strsep(&str, ",");
    while (token && i < 5) {
        if (kstrtoint(token, 10, &values[i]) < 0) {
            ret = -EINVAL;
            break;
        }
        if (values[i] < 0 || values[i] > 100) {
            ret = -EINVAL;
            break;
        }
        i++;
        token = strsep(&str, ",");
    }

    if (i != 5)
        ret = -EINVAL;

    if (ret == 0) {
        for (i = 0; i < 5; i++) {
            curve[i + 1] = values[i];
        }
    }

    kfree(str);
    return ret ? ret : count;
}

static ssize_t fan_rampup_curve_show(struct device           *dev,
                                     struct device_attribute *attr, char *buf)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    return fan_curve_show(fan->rampup_curve, buf);
}

static ssize_t fan_rampup_curve_store(struct device           *dev,
                                      struct device_attribute *attr,
                                      const char *buf, size_t count)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    return fan_curve_store(fan->rampup_curve, buf, count);
}

static struct device_attribute dev_attr_fan_rampup_curve =
    __ATTR(rampup_curve, 0644, fan_rampup_curve_show, fan_rampup_curve_store);

static ssize_t fan_rampdown_curve_show(struct device           *dev,
                                       struct device_attribute *attr, char *buf)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    return fan_curve_show(fan->rampdown_curve, buf);
}

static ssize_t fan_rampdown_curve_store(struct device           *dev,
                                        struct device_attribute *attr,
                                        const char *buf, size_t count)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    return fan_curve_store(fan->rampdown_curve, buf, count);
}

static struct device_attribute dev_attr_fan_rampdown_curve = __ATTR(
    rampdown_curve, 0644, fan_rampdown_curve_show, fan_rampdown_curve_store);

static ssize_t temp_current_show(struct device           *dev,
                                 struct device_attribute *attr, char *buf)
{
    struct ec_temp *temp = dev_get_drvdata(dev);
    u8              val;
    // TODO: handle error
    ec_read(temp->reg, &val);
    return sprintf(buf, "%u\n", val);
}

static struct device_attribute dev_attr_temp_cur =
    __ATTR(temp, 0444, temp_current_show, NULL);

static ssize_t temp_min_show(struct device *dev, struct device_attribute *attr,
                             char *buf)
{
    struct ec_temp *temp = dev_get_drvdata(dev);
    return sprintf(buf, "%u\n", temp->temp_min);
}

static struct device_attribute dev_attr_temp_min =
    __ATTR(min, 0444, temp_min_show, NULL);

static ssize_t temp_max_show(struct device *dev, struct device_attribute *attr,
                             char *buf)
{
    struct ec_temp *temp = dev_get_drvdata(dev);
    return sprintf(buf, "%u\n", temp->temp_max);
}

static struct device_attribute dev_attr_temp_max =
    __ATTR(max, 0444, temp_max_show, NULL);

static ssize_t apu_power_mode_show(struct device           *dev,
                                   struct device_attribute *attr, char *buf)
{
    struct ec_apu *apu = dev_get_drvdata(dev);
    u8             val;
    // TODO: handle error
    ec_read(apu->power_mode_reg, &val);

    const char *mode = "unknown";
    switch (val) {
    case 0x00:
        mode = "balanced";
        break;
    case 0x01:
        mode = "performance";
        break;
    case 0x02:
        mode = "quiet";
        break;
    default:
        return -EINVAL;
    }

    return sprintf(buf, "%s\n", mode);
}

static ssize_t apu_power_mode_store(struct device           *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count)
{
    struct ec_apu *apu = dev_get_drvdata(dev);
    u8             val;
    if (sysfs_streq(buf, "balanced")) {
        val = 0x00;
    } else if (sysfs_streq(buf, "performance")) {
        val = 0x01;
    } else if (sysfs_streq(buf, "quiet")) {
        val = 0x02;
    } else {
        return -EINVAL;
    }

    ec_write(apu->power_mode_reg, val);
    return count;
}

static struct device_attribute dev_attr_apu_power_mode =
    __ATTR(power_mode, 0644, apu_power_mode_show, apu_power_mode_store);

/*
 * hwmon interface.
 *
 * Exposes the same EC sensors through the standard hwmon API, so that they
 * show up in 'sensors' and any other hwmon consumer.
 */
static struct device *ec_hwmon_parent;
static struct device *ec_hwmon_dev;

static umode_t ec_hwmon_is_visible(const void *data,
                                   enum hwmon_sensor_types type, u32 attr,
                                   int channel)
{
    switch (type) {
    case hwmon_temp:
        if (attr == hwmon_temp_input)
            return 0444;
        break;
    case hwmon_fan:
        if (attr == hwmon_fan_input && channel < (int) ARRAY_SIZE(ec_fans))
            return 0444;
        break;
    default:
        break;
    }
    return 0;
}

static int ec_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
                         u32 attr, int channel, long *val)
{
    switch (type) {
    case hwmon_temp:
        if (attr == hwmon_temp_input) {
            u8  temp;
            int ret;

            ret = ec_read(ec_temp.reg, &temp);
            if (ret)
                return ret;

            *val = temp * 1000; // hwmon reports millidegrees
            return 0;
        }
        break;
    case hwmon_fan:
        if (attr == hwmon_fan_input && channel < (int) ARRAY_SIZE(ec_fans)) {
            u16 rpm;
            int ret;

            ret = ec_fan_read_rpm(&ec_fans[channel], &rpm);
            if (ret)
                return ret;

            *val = rpm;
            return 0;
        }
        break;
    default:
        break;
    }
    return -EOPNOTSUPP;
}

static const struct hwmon_ops ec_hwmon_ops = {
    .is_visible = ec_hwmon_is_visible,
    .read       = ec_hwmon_read,
};

static const u32 ec_hwmon_temp_config[] = {
    HWMON_T_INPUT,
    0,
};

static const u32 ec_hwmon_fan_config[] = {
    HWMON_F_INPUT,
    HWMON_F_INPUT,
    HWMON_F_INPUT,
    0,
};

static const struct hwmon_channel_info ec_hwmon_temp_info = {
    .type   = hwmon_temp,
    .config = ec_hwmon_temp_config,
};

static const struct hwmon_channel_info ec_hwmon_fan_info = {
    .type   = hwmon_fan,
    .config = ec_hwmon_fan_config,
};

static const struct hwmon_channel_info *ec_hwmon_info[] = {
    &ec_hwmon_temp_info,
    &ec_hwmon_fan_info,
    NULL,
};

static const struct hwmon_chip_info ec_hwmon_chip_info = {
    .ops  = &ec_hwmon_ops,
    .info = ec_hwmon_info,
};

static struct delayed_work ec_update_work;

static void ec_update_worker(struct work_struct *work)
{
    u8 temp;
    // TODO: handle error
    ec_read(ec_temp.reg, &temp);
    int i;

    // Update min/max
    if (ec_temp.temp_min == 0 || temp < ec_temp.temp_min)
        ec_temp.temp_min = temp;

    if (temp > ec_temp.temp_max)
        ec_temp.temp_max = temp;

    // update fan level if curve mode is active
    for (i = 0; i < ARRAY_SIZE(ec_fans); i++) {
        struct ec_fan *fan = &ec_fans[i];

        if (fan->mode == CURVE) {
            u8 level = read_fan_level(fan);
            if (level < 5 && temp >= fan->rampup_curve[level + 1]) {
                write_fan_level(fan, level + 1);
            } else if (level > 0 && temp <= fan->rampdown_curve[level])
                write_fan_level(fan, level - 1);
        }
    }

    // Requeue the work
    schedule_delayed_work(&ec_update_work,
                          msecs_to_jiffies(1000)); // every 1 sec
}

static dev_t ec_su_axb35_dev;

/* Minor numbers of the devices created in ec_class */
#define EC_MINOR_TEMP  (ARRAY_SIZE(ec_fans))
#define EC_MINOR_APU   (ARRAY_SIZE(ec_fans) + 1)
#define EC_MINOR_HWMON (ARRAY_SIZE(ec_fans) + 2)
#define EC_NUM_DEVICES (ARRAY_SIZE(ec_fans) + 3)

static int __init ec_su_axb35_init(void)
{
    int i;
    int ret;

    BUILD_BUG_ON(ARRAY_SIZE(ec_hwmon_fan_config) != ARRAY_SIZE(ec_fans) + 1);

    ret = alloc_chrdev_region(&ec_su_axb35_dev, 0, EC_NUM_DEVICES,
                              "ec_su_axb35");
    if (ret < 0) {
        pr_err("ec_su_axb35: Failed to allocation major number\n");
        return ret;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    ec_class = class_create("ec_su_axb35");
#else
    ec_class = class_create(THIS_MODULE, "ec_su_axb35");
#endif

    if (IS_ERR(ec_class)) {
        unregister_chrdev_region(ec_su_axb35_dev, EC_NUM_DEVICES);
        return PTR_ERR(ec_class);
    }

    for (i = 0; i < ARRAY_SIZE(ec_fans); i++) {
        struct ec_fan *fan = &ec_fans[i];

        fan->dev = device_create(
            ec_class, NULL, MKDEV(MAJOR(ec_su_axb35_dev), i), fan, fan->name);
        if (IS_ERR(fan->dev))
            continue;

        dev_set_drvdata(fan->dev, fan);
        device_create_file(fan->dev, &dev_attr_fan_rpm);
        device_create_file(fan->dev, &dev_attr_fan_mode);
        device_create_file(fan->dev, &dev_attr_fan_level);
        device_create_file(fan->dev, &dev_attr_fan_rampup_curve);
        device_create_file(fan->dev, &dev_attr_fan_rampdown_curve);
        update_fan_mode(fan);
    }

    ec_temp.dev = device_create(
        ec_class, NULL, MKDEV(MAJOR(ec_su_axb35_dev), EC_MINOR_TEMP), &ec_temp,
        ec_temp.name);
    if (!IS_ERR(ec_temp.dev)) {
        dev_set_drvdata(ec_temp.dev, &ec_temp);
        device_create_file(ec_temp.dev, &dev_attr_temp_cur);
        device_create_file(ec_temp.dev, &dev_attr_temp_min);
        device_create_file(ec_temp.dev, &dev_attr_temp_max);
    }

    ec_apu.dev = device_create(ec_class, NULL,
                               MKDEV(MAJOR(ec_su_axb35_dev), EC_MINOR_APU),
                               &ec_apu, ec_apu.name);
    if (!IS_ERR(ec_apu.dev)) {
        dev_set_drvdata(ec_apu.dev, &ec_apu);
        device_create_file(ec_apu.dev, &dev_attr_apu_power_mode);
    }

    /*
     * The hwmon core requires a parent device, so create a plain class
     * device to hang the hwmon device off.
     */
    ec_hwmon_parent = device_create(ec_class, NULL,
                                    MKDEV(MAJOR(ec_su_axb35_dev),
                                          EC_MINOR_HWMON),
                                    NULL, "ec_hwmon");
    if (IS_ERR(ec_hwmon_parent)) {
        ec_hwmon_parent = NULL;
    } else {
        ec_hwmon_dev = hwmon_device_register_with_info(
            ec_hwmon_parent, "ec_su_axb35", NULL, &ec_hwmon_chip_info, NULL);
        if (IS_ERR(ec_hwmon_dev)) {
            pr_warn("ec_su_axb35: Failed to register hwmon device: %ld\n",
                    PTR_ERR(ec_hwmon_dev));
            ec_hwmon_dev = NULL;
        }
    }

    INIT_DELAYED_WORK(&ec_update_work, ec_update_worker);
    schedule_delayed_work(&ec_update_work, msecs_to_jiffies(1000));

    pr_info("ec_su_axb35: Sixunited AXB35-02 EC driver loaded\n");
    return 0;
}

static void __exit ec_su_axb35_exit(void)
{
    int i;

    if (ec_hwmon_dev) {
        hwmon_device_unregister(ec_hwmon_dev);
        ec_hwmon_dev = NULL;
    }
    if (ec_hwmon_parent) {
        device_destroy(ec_class, MKDEV(MAJOR(ec_su_axb35_dev), EC_MINOR_HWMON));
        ec_hwmon_parent = NULL;
    }

    /* Reset all fans to AUTO mode before unloading */
    for (i = 0; i < ARRAY_SIZE(ec_fans); i++) {
        struct ec_fan *fan = &ec_fans[i];
        u8 val;

        fan->mode = AUTO;

        switch (fan->mode_reg) {
        case 0x21: // Fan 1
            val = 0x10;
            break;
        case 0x23: // Fan 2
            val = 0x20;
            break;
        case 0x25: // Fan 3
            val = 0x30;
            break;
        default:
            continue;
        }
        ec_write(fan->mode_reg, val);
        pr_info("ec_su_axb35: %s reset to AUTO mode\n", fan->name);
    }

    for (i = 0; i < ARRAY_SIZE(ec_fans); i++) {
        if (!IS_ERR(ec_fans[i].dev)) {
            device_remove_file(ec_fans[i].dev, &dev_attr_fan_rpm);
            device_remove_file(ec_fans[i].dev, &dev_attr_fan_mode);
            device_remove_file(ec_fans[i].dev, &dev_attr_fan_level);
            device_remove_file(ec_fans[i].dev, &dev_attr_fan_rampup_curve);
            device_remove_file(ec_fans[i].dev, &dev_attr_fan_rampdown_curve);
            device_destroy(ec_class, MKDEV(MAJOR(ec_su_axb35_dev), i));
        }
    }

    if (!IS_ERR(ec_temp.dev)) {
        device_remove_file(ec_temp.dev, &dev_attr_temp_cur);
        device_remove_file(ec_temp.dev, &dev_attr_temp_min);
        device_remove_file(ec_temp.dev, &dev_attr_temp_max);
        device_destroy(ec_class, MKDEV(MAJOR(ec_su_axb35_dev), EC_MINOR_TEMP));
    }

    if (!IS_ERR(ec_apu.dev)) {
        device_remove_file(ec_apu.dev, &dev_attr_apu_power_mode);
        device_destroy(ec_class, MKDEV(MAJOR(ec_su_axb35_dev), EC_MINOR_APU));
    }

    cancel_delayed_work_sync(&ec_update_work);

    class_destroy(ec_class);
    unregister_chrdev_region(ec_su_axb35_dev, EC_NUM_DEVICES);
    pr_info("ec_su_axb35: Module unloaded\n");
}

module_init(ec_su_axb35_init);
module_exit(ec_su_axb35_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("loom@mopper.de");
MODULE_DESCRIPTION("Sixunited AXB35-02 Embedded Controller driver");
