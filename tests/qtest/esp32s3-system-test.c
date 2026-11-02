/*
 * ESP32-S3 SYSTEM CPU1 control tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"

#define SYSTEM_CONTROL 0x600c0000
#define SYSTEM_MESSAGE 0x600c0004
#define RTC_OPTIONS0   0x60008000
#define RTC_CPU_STALL  0x600080bc
#define COUNTER0       0x3fc80000
#define COUNTER1       0x3fc80004
#define BOOT_COUNT1    0x3fc8000c

static char *rom_dir;

static QTestState *start_machine(bool execute)
{
    return qtest_initf("-machine esp32s3 -S -L %s %s", rom_dir,
                      execute ? "-accel tcg,thread=single" : "");
}

static void assert_defaults(QTestState *qts)
{
    g_assert_cmphex(qtest_readl(qts, SYSTEM_CONTROL), ==, 4);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_MESSAGE), ==, 0);
}

static void test_registers(void)
{
    QTestState *qts = start_machine(false);

    assert_defaults(qts);
    qtest_writel(qts, SYSTEM_CONTROL, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_CONTROL), ==, 7);
    qtest_writel(qts, SYSTEM_MESSAGE, 0x42001234);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_MESSAGE), ==, 0x42001234);

    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(qts, "RESET");
    assert_defaults(qts);

    qtest_writel(qts, SYSTEM_CONTROL, 2);
    qtest_writel(qts, SYSTEM_MESSAGE, 0x42005678);
    qtest_writel(qts, RTC_OPTIONS0, 1U << 4);
    /* A subsystem reset deliberately emits no QMP RESET event. */
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readl(qts, SYSTEM_CONTROL), ==, 2);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_MESSAGE), ==, 0x42005678);

    qtest_writel(qts, RTC_OPTIONS0, 1U << 31);
    qtest_qmp_eventwait(qts, "RESET");
    assert_defaults(qts);
    qtest_quit(qts);
}

static void wait_for_change(QTestState *qts, uint32_t address, uint32_t old)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    do {
        if (qtest_readl(qts, address) != old) {
            return;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_not_reached();
}

static void assert_cpu1_stalled(QTestState *qts)
{
    uint32_t value, core0;

    /* Allow the current translation block to exit before sampling. */
    g_usleep(20000);
    value = qtest_readl(qts, COUNTER1);
    core0 = qtest_readl(qts, COUNTER0);
    g_usleep(20000);
    g_assert_cmphex(qtest_readl(qts, COUNTER1), ==, value);
    wait_for_change(qts, COUNTER0, core0);
}

static void test_execution(void)
{
    QTestState *qts = start_machine(true);
    uint32_t boots;

    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    wait_for_change(qts, COUNTER0, 0);
    assert_cpu1_stalled(qts);
    g_assert_cmphex(qtest_readl(qts, BOOT_COUNT1), ==, 0);

    qtest_writel(qts, SYSTEM_CONTROL, 2);
    wait_for_change(qts, COUNTER1, 0);
    g_assert_cmphex(qtest_readl(qts, BOOT_COUNT1), ==, 1);

    /* RUNSTALL and clock gating each stop CPU1 without resetting it. */
    qtest_writel(qts, SYSTEM_CONTROL, 3);
    assert_cpu1_stalled(qts);
    qtest_writel(qts, SYSTEM_CONTROL, 2);
    wait_for_change(qts, COUNTER1, qtest_readl(qts, COUNTER1));
    qtest_writel(qts, SYSTEM_CONTROL, 0);
    assert_cpu1_stalled(qts);
    qtest_writel(qts, SYSTEM_CONTROL, 2);
    wait_for_change(qts, COUNTER1, qtest_readl(qts, COUNTER1));
    g_assert_cmphex(qtest_readl(qts, BOOT_COUNT1), ==, 1);

    /* Clearing one stall source must not override another. */
    qtest_writel(qts, RTC_OPTIONS0, 2);
    qtest_writel(qts, RTC_CPU_STALL, 0x21 << 20);
    qtest_writel(qts, SYSTEM_CONTROL, 3);
    qtest_writel(qts, SYSTEM_CONTROL, 2);
    assert_cpu1_stalled(qts);
    qtest_writel(qts, SYSTEM_CONTROL, 3);
    qtest_writel(qts, RTC_OPTIONS0, 0);
    qtest_writel(qts, RTC_CPU_STALL, 0);
    assert_cpu1_stalled(qts);
    qtest_writel(qts, SYSTEM_CONTROL, 2);
    wait_for_change(qts, COUNTER1, qtest_readl(qts, COUNTER1));

    /* Assert RESETING, hold it, then restart CPU1 at its reset vector. */
    boots = qtest_readl(qts, BOOT_COUNT1);
    qtest_writel(qts, SYSTEM_CONTROL, 6);
    assert_cpu1_stalled(qts);
    g_assert_cmphex(qtest_readl(qts, BOOT_COUNT1), ==, boots);
    qtest_writel(qts, SYSTEM_CONTROL, 6);
    qtest_writel(qts, SYSTEM_CONTROL, 2);
    wait_for_change(qts, BOOT_COUNT1, boots);
    g_assert_cmphex(qtest_readl(qts, BOOT_COUNT1), ==, boots + 1);
    g_assert_cmphex(qtest_readl(qts, COUNTER0 + 8), ==, 1);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    /*
     * Reset-vector ROM: select a counter using PRID bit 1, count boots at
     * counter+8, then increment the counter continuously. No ESP-IDF or
     * external cross compiler is needed to exercise both CPUs.
     *
     * j start; .align 4; .word 0x3fc80000
     * start: rsr a2,prid; extui a2,a2,1,1; slli a2,a2,2; l32r a3,literal
     * add.n a2,a2,a3; l32i.n a4,a2,8; addi.n a4,a4,1; s32i.n a4,a2,8
     * movi.n a4,0
     * loop: addi.n a4,a4,1; s32i.n a4,a2,0; j loop
     */
    static const uint8_t code[] = {
        0x06, 0x01, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x3f,
        0x20, 0xeb, 0x03, 0x20, 0x21, 0x04, 0xe0, 0x22,
        0x11, 0x31, 0xfc, 0xff, 0x3a, 0x22, 0x48, 0x22,
        0x1b, 0x44, 0x49, 0x22, 0x0c, 0x04, 0x1b, 0x44,
        0x49, 0x02, 0x06, 0xfe, 0xff,
    };
    uint8_t rom[0x400 + sizeof(code)] = { 0 };
    char *rom_path;
    int ret;

    g_test_init(&argc, &argv, NULL);
    rom_dir = g_dir_make_tmp("esp32s3-system-XXXXXX", NULL);
    g_assert_nonnull(rom_dir);
    rom_path = g_build_filename(rom_dir, "esp32s3_rev0_rom.bin", NULL);
    memcpy(rom + 0x400, code, sizeof(code));
    g_assert_true(g_file_set_contents(rom_path, (char *)rom,
                                     sizeof(rom), NULL));

    qtest_add_func("/esp32s3/system/registers", test_registers);
    if (qtest_has_accel("tcg")) {
        qtest_add_func("/esp32s3/system/execution", test_execution);
    }
    ret = g_test_run();
    unlink(rom_path);
    rmdir(rom_dir);
    g_free(rom_path);
    g_free(rom_dir);
    return ret;
}
