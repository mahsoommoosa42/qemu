/*
 * ESP32-S3 RTC clock selection and nominal frequency tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "hw/misc/esp32s3_rtc_cntl.h"

#define RTC_BASE 0x60008000

static QTestState *rtc_init(void)
{
    return qtest_init("-machine esp32s3 -display none -serial null");
}

static uint32_t rtc_read(QTestState *qts, uint32_t addr)
{
    return qtest_readl(qts, RTC_BASE + addr);
}

static uint64_t rtc_time(QTestState *qts)
{
    qtest_writel(qts, RTC_BASE + A_RTC_CNTL_TIME_UPDATE,
                 R_RTC_CNTL_TIME_UPDATE_UPDATE_MASK);
    return rtc_read(qts, A_RTC_CNTL_TIME0) |
           ((uint64_t)rtc_read(qts, A_RTC_CNTL_TIME1) << 32);
}

static void rtc_clock(QTestState *qts, unsigned int slow, unsigned int fast)
{
    uint32_t value = FIELD_DP32(0, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL, slow);

    value = FIELD_DP32(value, RTC_CNTL_CLK_CONF, FAST_CLK_RTC_SEL, fast);
    qtest_writel(qts, RTC_BASE + A_RTC_CNTL_CLK_CONF, value);
}

static void test_reset_clock(void)
{
    QTestState *qts = rtc_init();
    uint32_t value = rtc_read(qts, A_RTC_CNTL_CLK_CONF);

    g_assert_cmpuint(FIELD_EX32(value, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL),
                     ==, 0);
    g_assert_cmpuint(FIELD_EX32(value, RTC_CNTL_CLK_CONF, FAST_CLK_RTC_SEL),
                     ==, 0);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    g_assert_cmpuint(rtc_time(qts), ==, 136000);
    qtest_quit(qts);
}

static void test_slow_clock(const void *data)
{
    static const uint32_t nominal_hz[] = {136000, 32768, 17500000 / 256};
    unsigned int selection = GPOINTER_TO_UINT(data);
    unsigned int slow = selection / 2;
    unsigned int fast = selection % 2;
    QTestState *qts = rtc_init();
    uint32_t value;

    rtc_clock(qts, slow, fast);
    value = rtc_read(qts, A_RTC_CNTL_CLK_CONF);
    g_assert_cmpuint(FIELD_EX32(value, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL),
                     ==, slow);
    g_assert_cmpuint(FIELD_EX32(value, RTC_CNTL_CLK_CONF, FAST_CLK_RTC_SEL),
                     ==, fast);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    g_assert_cmpuint(rtc_time(qts), ==, nominal_hz[slow]);
    qtest_quit(qts);
}

static void test_invalid_slow_clock(const void *data)
{
    unsigned int slow = GPOINTER_TO_UINT(data);
    QTestState *qts = rtc_init();
    uint64_t before;
    uint32_t value;

    rtc_clock(qts, slow, 0);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    before = rtc_time(qts);
    /* Selection 3 is invalid: keep the previous source and update FAST. */
    rtc_clock(qts, 3, 1);
    value = rtc_read(qts, A_RTC_CNTL_CLK_CONF);
    g_assert_cmpuint(FIELD_EX32(value, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL),
                     ==, slow);
    g_assert_cmpuint(FIELD_EX32(value, RTC_CNTL_CLK_CONF, FAST_CLK_RTC_SEL),
                     ==, 1);
    g_assert_cmpuint(rtc_time(qts), ==, before);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    g_assert_cmpuint(rtc_time(qts), ==, before * 2);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    unsigned int i;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/esp32s3/rtc-clock/reset", test_reset_clock);
    for (i = 0; i < 6; i++) {
        g_autofree char *name = g_strdup_printf(
            "/esp32s3/rtc-clock/slow-%u-fast-%u", i / 2, i % 2);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_slow_clock);
    }
    for (i = 0; i < 3; i++) {
        g_autofree char *name = g_strdup_printf(
            "/esp32s3/rtc-clock/invalid-after-%u", i);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_invalid_slow_clock);
    }
    return g_test_run();
}
