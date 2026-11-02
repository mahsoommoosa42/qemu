/*
 * ESP32-S3 RTC timer and deep-sleep tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/misc/esp32s3_rtc_cntl.h"

#define RTC_BASE 0x60008000
#define RTC_PATH "/machine/soc/rtc_cntl"
#define TIMER_WAKEUP BIT(3)
#define SLOW_HZ 32768

/*
 * RTC slow-memory reset stub at 0x50000000. Increment STORE1 at each boot,
 * enter deep sleep on the first boot, and loop on the second. Falling
 * through the sleep request writes STORE0 = 1, like a returning sleep HAL.
 *
 * j boot; .align 4; .word 0x60008000, 0x80000000;
 * boot: l32r a2, rtcbase; l32i a3, a2, 0x54; addi a3, a3, 1;
 * s32i a3, a2, 0x54; beqi a3, 2, awake; l32r a3, sleepbit;
 * s32i a3, a2, 0x90; memw; s32i a3, a2, 0x18; memw;
 * failed: movi a3, 1; s32i a3, a2, 0x50; j failed;
 * awake: j awake;
 */
static const uint8_t sleep_guest[] = {
    0x06, 0x02, 0x00, 0x00, 0x00, 0x80, 0x00, 0x60,
    0x00, 0x00, 0x00, 0x80, 0x21, 0xfe, 0xff, 0x32,
    0x22, 0x15, 0x1b, 0x33, 0x32, 0x62, 0x15, 0x26,
    0x23, 0x15, 0x31, 0xfb, 0xff, 0x32, 0x62, 0x24,
    0xc0, 0x20, 0x00, 0x39, 0x62, 0xc0, 0x20, 0x00,
    0x0c, 0x13, 0x32, 0x62, 0x14, 0xc6, 0xfd, 0xff,
    0x06, 0xff, 0xff,
};

static uint32_t rtc_read(QTestState *qts, uint32_t addr)
{
    return qtest_readl(qts, RTC_BASE + addr);
}

static void rtc_write(QTestState *qts, uint32_t addr, uint32_t value)
{
    qtest_writel(qts, RTC_BASE + addr, value);
}

static uint64_t rtc_time(QTestState *qts)
{
    rtc_write(qts, A_RTC_CNTL_TIME_UPDATE, R_RTC_CNTL_TIME_UPDATE_UPDATE_MASK);
    return rtc_read(qts, A_RTC_CNTL_TIME0) |
           ((uint64_t)rtc_read(qts, A_RTC_CNTL_TIME1) << 32);
}

static QTestState *rtc_init(void)
{
    QTestState *qts = qtest_init("-machine esp32s3 -display none -serial null");

    rtc_write(qts, A_RTC_CNTL_CLK_CONF,
              FIELD_DP32(0, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL,
                         ESP32_SLOW_CLK_32KXTAL));
    return qts;
}

static void rtc_alarm(QTestState *qts, uint64_t target)
{
    rtc_write(qts, A_RTC_CNTL_SLP_TIMER0, target);
    rtc_write(qts, A_RTC_CNTL_SLP_TIMER1,
              (target >> 32) | R_RTC_CNTL_SLP_TIMER1_MAIN_TIMER_ALARM_EN_MASK);
}

static void rtc_sleep(QTestState *qts, uint32_t wakeup)
{
    rtc_write(qts, A_RTC_CNTL_WAKEUP_STATE,
              FIELD_DP32(0, RTC_CNTL_WAKEUP_STATE, WAKEUP_ENA, wakeup));
    rtc_write(qts, A_RTC_CNTL_DIG_PWC, R_RTC_CNTL_DIG_PWC_DG_WRAP_PD_EN_MASK);
    rtc_write(qts, A_RTC_CNTL_STATE0, R_RTC_CNTL_STATE0_SLEEP_EN_MASK);
}

static void test_deep_sleep_timer(void)
{
    QTestState *qts = rtc_init();
    uint64_t before;
    uint32_t reset;

    qtest_irq_intercept_out_named(qts, RTC_PATH, ESP32S3_RTC_CPU_STALL_GPIO);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    before = rtc_time(qts);
    rtc_write(qts, A_RTC_CNTL_STORE0, 0x12345678);
    qtest_writel(qts, 0x50000000, 0xaabbccdd);
    qtest_writel(qts, 0x600fe000, 0x10203040);
    rtc_alarm(qts, before + SLOW_HZ);
    rtc_sleep(qts, TIMER_WAKEUP);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND - 1);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_STATE0), ==,
                    R_RTC_CNTL_STATE0_SLEEP_EN_MASK);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_SLP_WAKEUP_CAUSE), ==, 0);
    qtest_clock_step(qts, 2);
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_STATE0), ==, 0);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_SLP_WAKEUP_CAUSE), ==,
                    TIMER_WAKEUP);
    reset = rtc_read(qts, A_RTC_CNTL_RESET_STATE);
    g_assert_cmpuint(FIELD_EX32(reset, RTC_CNTL_RESET_STATE,
                               RESET_CAUSE_PROCPU), ==, ESP32_DEEPSLEEP_RESET);
    g_assert_cmpuint(FIELD_EX32(reset, RTC_CNTL_RESET_STATE,
                               RESET_CAUSE_APPCPU), ==, ESP32_DEEPSLEEP_RESET);
    g_assert_cmpuint(rtc_time(qts), ==, before + SLOW_HZ);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_STORE0), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, 0x50000000), ==, 0xaabbccdd);
    g_assert_cmphex(qtest_readl(qts, 0x600fe000), ==, 0x10203040);
    qtest_quit(qts);
}

static void test_deep_sleep_no_wakeup(const void *with_alarm)
{
    QTestState *qts = rtc_init();

    qtest_irq_intercept_out_named(qts, RTC_PATH, ESP32S3_RTC_CPU_STALL_GPIO);
    if (with_alarm) {
        rtc_alarm(qts, rtc_time(qts) + SLOW_HZ);
    }
    rtc_sleep(qts, 0);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND * 2);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_SLP_WAKEUP_CAUSE), ==, 0);
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmpuint(rtc_time(qts), ==, SLOW_HZ * 2);
    qtest_quit(qts);
}

static void test_clock_write_preserves_phase(void)
{
    QTestState *qts = rtc_init();
    int64_t tick_ns = (NANOSECONDS_PER_SECOND + SLOW_HZ - 1) / SLOW_HZ;

    rtc_alarm(qts, 1);
    qtest_clock_step(qts, tick_ns / 2);
    g_assert_cmpuint(rtc_time(qts), ==, 0);
    /* Changing only FAST must not restart the unchanged slow oscillator. */
    rtc_write(qts, A_RTC_CNTL_CLK_CONF,
              FIELD_DP32(0, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL,
                         ESP32_SLOW_CLK_32KXTAL) |
              R_RTC_CNTL_CLK_CONF_FAST_CLK_RTC_SEL_MASK);
    qtest_clock_step(qts, tick_ns - tick_ns / 2);
    g_assert_cmpuint(rtc_time(qts), ==, 1);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_INT_RAW), ==,
                    R_RTC_CNTL_INT_RAW_MAIN_TIMER_MASK);
    qtest_quit(qts);
}

static void test_rearm_with_pending_interrupt(void)
{
    QTestState *qts = rtc_init();
    uint64_t before;

    rtc_alarm(qts, SLOW_HZ);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND + 1);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_INT_RAW), ==,
                    R_RTC_CNTL_INT_RAW_MAIN_TIMER_MASK);
    before = rtc_time(qts);
    rtc_alarm(qts, before + SLOW_HZ);
    /* Sleep entry clears wake status, not the latched main-timer IRQ. */
    rtc_write(qts, A_RTC_CNTL_INT_CLR,
              R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK);
    rtc_sleep(qts, TIMER_WAKEUP);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_STATE0), ==,
                    R_RTC_CNTL_STATE0_SLEEP_EN_MASK);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_SLP_WAKEUP_CAUSE), ==, 0);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 2);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_STATE0), ==,
                    R_RTC_CNTL_STATE0_SLEEP_EN_MASK);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 2 + 1);
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_STATE0), ==, 0);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_SLP_WAKEUP_CAUSE), ==,
                    TIMER_WAKEUP);
    g_assert_cmpuint(rtc_time(qts), ==, before + SLOW_HZ);
    qtest_quit(qts);
}

static void test_clock_change(void)
{
    QTestState *qts = rtc_init();
    uint64_t before;

    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    before = rtc_time(qts);
    rtc_alarm(qts, before + SLOW_HZ);
    /* Change to the faster RC slow clock halfway to the alarm. */
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 2);
    rtc_write(qts, A_RTC_CNTL_CLK_CONF, 0);
    g_assert_cmpuint(rtc_time(qts), ==, before + SLOW_HZ / 2);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 10);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_INT_RAW), ==, 0);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 20);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_INT_RAW), ==,
                    R_RTC_CNTL_INT_RAW_MAIN_TIMER_MASK);
    qtest_quit(qts);
}

static void test_timer_interrupt(void)
{
    QTestState *qts = rtc_init();

    qtest_irq_intercept_out_named(qts, RTC_PATH, SYSBUS_DEVICE_GPIO_IRQ);
    /* Cross the low timer word boundary before programming the alarm. */
    qtest_clock_step(qts, (UINT64_C(1) << 32) / SLOW_HZ *
                          NANOSECONDS_PER_SECOND);
    rtc_alarm(qts, rtc_time(qts) + SLOW_HZ);
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_SLP_TIMER1), ==, 1);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND + 1);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_INT_RAW), ==,
                    R_RTC_CNTL_INT_RAW_MAIN_TIMER_MASK);
    rtc_write(qts, A_RTC_CNTL_INT_ENA_W1TS,
              R_RTC_CNTL_INT_RAW_MAIN_TIMER_MASK);
    g_assert_true(qtest_get_irq(qts, 0));
    rtc_write(qts, A_RTC_CNTL_INT_ENA_W1TC,
              R_RTC_CNTL_INT_RAW_MAIN_TIMER_MASK);
    g_assert_false(qtest_get_irq(qts, 0));
    rtc_write(qts, A_RTC_CNTL_INT_ENA, R_RTC_CNTL_INT_RAW_MAIN_TIMER_MASK);
    g_assert_true(qtest_get_irq(qts, 0));
    rtc_write(qts, A_RTC_CNTL_INT_CLR, R_RTC_CNTL_INT_RAW_MAIN_TIMER_MASK);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_INT_ST), ==, 0);
    qtest_quit(qts);
}

static void test_guest_sleep(const void *with_timer)
{
    QTestState *qts;
    int64_t deadline;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is required to execute the sleep-entry guest");
        return;
    }
    qts = qtest_init("-machine esp32s3 -serial null -accel tcg -S");
    qtest_memwrite(qts, 0x50000000, sleep_guest, sizeof(sleep_guest));
    /* Boot CPU0 from RTC slow RAM and hold CPU1 at its ROM reset vector. */
    rtc_write(qts, A_RTC_CNTL_RESET_STATE,
              R_RTC_CNTL_RESET_STATE_APPCPU_STAT_VECTOR_SEL_MASK);
    rtc_write(qts, A_RTC_CNTL_SW_CPU_STALL, 0x21 << 20);
    rtc_write(qts, A_RTC_CNTL_OPTIONS0, 2);
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(qts, "RESET");
    rtc_write(qts, A_RTC_CNTL_CLK_CONF,
              FIELD_DP32(0, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL,
                         ESP32_SLOW_CLK_32KXTAL));
    rtc_write(qts, A_RTC_CNTL_WAKEUP_STATE,
              FIELD_DP32(0, RTC_CNTL_WAKEUP_STATE, WAKEUP_ENA,
                         with_timer ? TIMER_WAKEUP : 0));
    if (with_timer) {
        rtc_alarm(qts, rtc_time(qts) + SLOW_HZ / 4);
    }
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!rtc_read(qts, A_RTC_CNTL_STATE0)) {
        g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_STORE0), ==, 0);
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    if (with_timer) {
        qtest_qmp_eventwait(qts, "RESET");
        while (rtc_read(qts, A_RTC_CNTL_STORE1) != 2) {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
        }
        g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_SLP_WAKEUP_CAUSE), ==,
                        TIMER_WAKEUP);
    } else {
        g_usleep(100000);
        g_assert_cmpuint(rtc_read(qts, A_RTC_CNTL_STORE1), ==, 1);
    }
    g_assert_cmphex(rtc_read(qts, A_RTC_CNTL_STORE0), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/esp32s3/rtc/deep-sleep-timer", test_deep_sleep_timer);
    qtest_add_data_func("/esp32s3/rtc/deep-sleep-no-wakeup", NULL,
                        test_deep_sleep_no_wakeup);
    qtest_add_data_func("/esp32s3/rtc/deep-sleep-timer-not-selected",
                        GUINT_TO_POINTER(1), test_deep_sleep_no_wakeup);
    qtest_add_func("/esp32s3/rtc/timer-interrupt", test_timer_interrupt);
    qtest_add_func("/esp32s3/rtc/clock-change", test_clock_change);
    qtest_add_func("/esp32s3/rtc/clock-write-preserves-phase",
                   test_clock_write_preserves_phase);
    qtest_add_func("/esp32s3/rtc/rearm-pending-interrupt",
                   test_rearm_with_pending_interrupt);
    qtest_add_data_func("/esp32s3/rtc/guest-no-wakeup", NULL, test_guest_sleep);
    qtest_add_data_func("/esp32s3/rtc/guest-timer-wakeup", GUINT_TO_POINTER(1),
                        test_guest_sleep);
    return g_test_run();
}
