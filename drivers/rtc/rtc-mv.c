/*
 * Driver for the RTC in Marvell SoCs.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <common.h>
#include <init.h>
#include <driver.h>
#include <xfuncs.h>
#include <errno.h>
#include <io.h>
#include <rtc.h>
#include <linux/rtc.h>
#include <linux/bcd.h>


#define RTC_TIME_REG_OFFS	0
#define RTC_SECONDS_OFFS	0
#define RTC_MINUTES_OFFS	8
#define RTC_HOURS_OFFS		16
#define RTC_WDAY_OFFS		24
#define RTC_HOURS_12H_MODE	BIT(22) /* 12 hour mode */

#define RTC_DATE_REG_OFFS	4
#define RTC_MDAY_OFFS		0
#define RTC_MONTH_OFFS		8
#define RTC_YEAR_OFFS		16

struct rtc_plat_data {
	struct rtc_device rtc;
	void __iomem *ioaddr;
};

static inline struct rtc_plat_data *to_mv_rtc_priv(struct rtc_device *rtcdev)
{
	return container_of(rtcdev, struct rtc_plat_data, rtc);
}

static int mv_rtc_set_time(struct rtc_device *dev, struct rtc_time *tm)
{
	struct rtc_plat_data *pdata = to_mv_rtc_priv(dev);
	void __iomem *ioaddr = pdata->ioaddr;
	u32	rtc_reg;

	rtc_reg = (bin2bcd(tm->tm_sec) << RTC_SECONDS_OFFS) |
		(bin2bcd(tm->tm_min) << RTC_MINUTES_OFFS) |
		(bin2bcd(tm->tm_hour) << RTC_HOURS_OFFS) |
		(bin2bcd(tm->tm_wday) << RTC_WDAY_OFFS);
	writel(rtc_reg, ioaddr + RTC_TIME_REG_OFFS);

	rtc_reg = (bin2bcd(tm->tm_mday) << RTC_MDAY_OFFS) |
		(bin2bcd(tm->tm_mon + 1) << RTC_MONTH_OFFS) |
		(bin2bcd(tm->tm_year % 100) << RTC_YEAR_OFFS);
	writel(rtc_reg, ioaddr + RTC_DATE_REG_OFFS);

	return 0;
}

static int mv_rtc_read_time(struct rtc_device *dev, struct rtc_time *tm)
{
	struct rtc_plat_data *pdata = to_mv_rtc_priv(dev);
	void __iomem *ioaddr = pdata->ioaddr;
	u32	rtc_time, rtc_date;
	unsigned int year, month, day, hour, minute, second, wday;

	rtc_time = readl(ioaddr + RTC_TIME_REG_OFFS);
	rtc_date = readl(ioaddr + RTC_DATE_REG_OFFS);

	second = rtc_time & 0x7f;
	minute = (rtc_time >> RTC_MINUTES_OFFS) & 0x7f;
	hour = (rtc_time >> RTC_HOURS_OFFS) & 0x3f; /* assume 24 hour mode */
	wday = (rtc_time >> RTC_WDAY_OFFS) & 0x7;

	day = rtc_date & 0x3f;
	month = (rtc_date >> RTC_MONTH_OFFS) & 0x3f;
	year = (rtc_date >> RTC_YEAR_OFFS) & 0xff;

	tm->tm_sec = bcd2bin(second);
	tm->tm_min = bcd2bin(minute);
	tm->tm_hour = bcd2bin(hour);
	tm->tm_mday = bcd2bin(day);
	tm->tm_wday = bcd2bin(wday);
	tm->tm_mon = bcd2bin(month) - 1;
	/* hw counts from year 2000, but tm_year is relative to 1900 */
	tm->tm_year = bcd2bin(year) + 100;

	return rtc_valid_tm(tm);
}

static struct rtc_class_ops mv_rtc_ops = {
	.read_time	= mv_rtc_read_time,
	.set_time	= mv_rtc_set_time,
};

static int mv_rtc_probe(struct device_d *dev)
{
	struct resource *res;
	struct rtc_plat_data *pdata;
	void __iomem *ioaddr;
	u32 rtc_time;
	u32 rtc_date;
	int ret = 0;

	res = dev_request_mem_resource(dev, 0);
	if (IS_ERR(res)) {
		dev_err(dev, "could not get memory region\n");
		return PTR_ERR(res);
	}
	ioaddr = dev_request_mem_region(dev, 0);
	if (IS_ERR(ioaddr)){
		dev_err(dev, "could not get memory start\n");
		return PTR_ERR(ioaddr);
	}

	/* make sure the 24 hour mode is enabled */
	rtc_time = readl(ioaddr + RTC_TIME_REG_OFFS);
	if (rtc_time & RTC_HOURS_12H_MODE) {
		dev_err(dev, "12 Hour mode is enabled but not supported.\n");
		ret = -EINVAL;
		goto out;
	}

	/* make sure it is actually functional */
	if (rtc_time == 0x01000000) {
		rtc_time = readl(ioaddr + RTC_TIME_REG_OFFS);
		if (rtc_time == 0x01000000) {
			dev_err(dev, "internal RTC not ticking\n");
			ret = -ENODEV;
			goto out;
		}
	}

	/*
	 * A date after January 19th, 2038 does not fit on 32 bits and
	 * will confuse the kernel and userspace. Reset to a sane date
	 * (January 1st, 2013) if we're after 2038.
	 */
	rtc_date = readl(ioaddr + RTC_DATE_REG_OFFS);
	if (bcd2bin((rtc_date >> RTC_YEAR_OFFS) & 0xff) >= 38) {
		dev_info(dev, "invalid RTC date, resetting to January 1st, 2013\n");
		writel(0x130101, ioaddr + RTC_DATE_REG_OFFS);
	}
	
	pdata = xzalloc(sizeof(*pdata));

	pdata->ioaddr = ioaddr;
	pdata->rtc.ops = &mv_rtc_ops;
	pdata->rtc.dev = dev;

	ret = rtc_register(&pdata->rtc);
	if (ret) {
		dev_err(dev, "Failed to register rtc device: %d\n", ret);
		return ret;
	}

	return 0;
out:
	return ret;
}

static __maybe_unused struct of_device_id rtc_mv_dt_ids[] = {
	{ .compatible = "marvell,orion-rtc", },
	{}
};

static struct driver_d mv_rtc_driver = {
	.name	= "rtc-mv",
	.probe	 = mv_rtc_probe,
	.of_compatible = DRV_OF_COMPAT(rtc_mv_dt_ids),
};
device_platform_driver(mv_rtc_driver);
