/*
 * ESP32-S3 timer-group RTC calibration tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/timer/esp_timg.h"

#define TIMG0_BASE 0x6001f000
#define CAL_COUNT 1024

static uint32_t timg_read(QTestState *qts, unsigned int group, uint32_t addr)
{
    return qtest_readl(qts, TIMG0_BASE + group * 0x1000 + addr);
}

static void timg_calibrate(QTestState *qts, unsigned int group,
                           unsigned int source, bool cycling)
{
    uint32_t value = FIELD_DP32(0, TIMG_RTCCALICFG, CLK_SEL, source);

    value = FIELD_DP32(value, TIMG_RTCCALICFG, MAX, CAL_COUNT);
    value |= cycling ? R_TIMG_RTCCALICFG_START_CYCLING_MASK :
                       R_TIMG_RTCCALICFG_START_MASK;
    /* RDY is read-only, including when an invalid source is requested. */
    value |= R_TIMG_RTCCALICFG_RDY_MASK;
    qtest_writel(qts, TIMG0_BASE + group * 0x1000 + A_TIMG_RTCCALICFG,
                 value);
}

static uint32_t timg_result(QTestState *qts, unsigned int group)
{
    return FIELD_EX32(timg_read(qts, group, A_TIMG_RTCCALICFG1),
                      TIMG_RTCCALICFG1, VALUE);
}

static void test_calibration(const void *data)
{
    unsigned int selection = GPOINTER_TO_UINT(data);
    unsigned int group = selection / 6;
    unsigned int source = (selection / 2) % 3;
    bool cycling = selection % 2;
    static const uint32_t nominal_hz[] = {136000, 17500000 / 256, 32768};
    uint32_t expected = UINT64_C(40000000) * CAL_COUNT / nominal_hz[source];
    QTestState *qts = qtest_init("-machine esp32s3 -serial null");

    timg_calibrate(qts, group, source, cycling);
    g_assert_true(timg_read(qts, group, A_TIMG_RTCCALICFG) &
                  R_TIMG_RTCCALICFG_RDY_MASK);
    g_assert_cmpuint(timg_result(qts, group), ==, expected);
    qtest_quit(qts);
}

static void test_invalid_calibration(const void *data)
{
    unsigned int selection = GPOINTER_TO_UINT(data);
    unsigned int group = selection / 2;
    bool cycling = selection % 2;
    uint32_t before;
    QTestState *qts = qtest_init("-machine esp32s3 -serial null");

    timg_calibrate(qts, group, 0, false);
    before = timg_result(qts, group);
    timg_calibrate(qts, group, 3, cycling);
    g_assert_false(timg_read(qts, group, A_TIMG_RTCCALICFG) &
                   R_TIMG_RTCCALICFG_RDY_MASK);
    g_assert_cmpuint(timg_result(qts, group), ==, before);
    /* A valid request must still work after rejecting the invalid source. */
    timg_calibrate(qts, group, 1, cycling);
    g_assert_true(timg_read(qts, group, A_TIMG_RTCCALICFG) &
                  R_TIMG_RTCCALICFG_RDY_MASK);
    g_assert_cmpuint(timg_result(qts, group), ==,
                     UINT64_C(40000000) * CAL_COUNT / (17500000 / 256));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    unsigned int i;

    g_test_init(&argc, &argv, NULL);
    for (i = 0; i < 12; i++) {
        g_autofree char *name = g_strdup_printf(
            "/esp32s3/timg-calibration/group-%u/source-%u/cycling-%u",
            i / 6, (i / 2) % 3, i % 2);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_calibration);
    }
    for (i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf(
            "/esp32s3/timg-calibration/group-%u/invalid/cycling-%u",
            i / 2, i % 2);
        qtest_add_data_func(name, GUINT_TO_POINTER(i),
                            test_invalid_calibration);
    }
    return g_test_run();
}
