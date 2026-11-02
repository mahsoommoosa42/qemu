/*
 * ESP32-S3 UART receive fairness with instruction counting.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/sockets.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "hw/char/esp32_uart.h"

#define UART_BASE       0x60010000
#define RESULT_BASE     0x3fc80000
#define RTC_SLOW_HZ     150000
#define UART_BAUD       115200

static char *rom_dir;

static QTestState *start_machine(int *peer, bool execute)
{
    QTestState *qts;
    int pair[2];

    qts = qtest_initf("-machine esp32s3 -L %s "
        "-chardev socket,id=rx,host=127.0.0.1,port=0,server=on,wait=off "
        "-serial null -serial chardev:rx %s", rom_dir,
        execute ? "-S -accel tcg,thread=single "
                  "-icount shift=4,align=off,sleep=off" : "");
    g_assert_cmpint(qemu_socketpair(AF_UNIX, SOCK_STREAM, 0, pair), ==, 0);
    qtest_qmp_add_client(qts, "rx", pair[1]);
    close(pair[1]);
    *peer = pair[0];
    return qts;
}

static void wait_word(QTestState *qts, uint32_t address, uint32_t expected,
                      uint32_t mask)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    while ((qtest_readl(qts, address) & mask) != expected &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readl(qts, address) & mask, ==, expected);
}

static void send_bytes(int peer, unsigned int count)
{
    uint8_t bytes[UART_FIFO_LENGTH];

    for (unsigned int i = 0; i < count; i++) {
        bytes[i] = i;
    }
    g_assert_cmpint(qemu_send_full(peer, bytes, count), ==, count);
}

static void wait_count(QTestState *qts, unsigned int count)
{
    wait_word(qts, UART_BASE + A_UART_STATUS, count,
              R_UART_STATUS_RXFIFO_CNT_MASK);
}

static void drain_bytes(QTestState *qts, unsigned int count)
{
    for (unsigned int i = 0; i < count; i++) {
        g_assert_cmphex(qtest_readl(qts, UART_BASE + A_UART_FIFO), ==, i);
    }
}

static void test_unthrottled_refill(void)
{
    int peer;
    QTestState *qts = start_machine(&peer, false);

    for (unsigned int pass = 0; pass < 2; pass++) {
        send_bytes(peer, UART_FIFO_LENGTH / 2);
        wait_count(qts, UART_FIFO_LENGTH / 2);
        drain_bytes(qts, UART_FIFO_LENGTH / 2);
        wait_count(qts, 0);
    }
    close(peer);
    qtest_quit(qts);
}

static void test_throttle_expiry(void)
{
    int peer;
    QTestState *qts = start_machine(&peer, false);
    int64_t delay = (int64_t)UART_FIFO_LENGTH * 10 *
                    NANOSECONDS_PER_SECOND / UART_BAUD;

    send_bytes(peer, UART_FIFO_LENGTH);
    wait_count(qts, UART_FIFO_LENGTH);
    drain_bytes(qts, UART_FIFO_LENGTH);
    send_bytes(peer, UART_FIFO_LENGTH);
    qtest_clock_step(qts, delay - 1);
    wait_count(qts, 0);
    qtest_clock_step(qts, 1);
    wait_count(qts, UART_FIFO_LENGTH);
    drain_bytes(qts, UART_FIFO_LENGTH);
    close(peer);
    qtest_quit(qts);
}

static void test_icount_burst(void)
{
    int peer;
    QTestState *qts = start_machine(&peer, true);
    uint32_t elapsed;

    send_bytes(peer, UART_FIFO_LENGTH);
    wait_count(qts, UART_FIFO_LENGTH);
    /* Release CPU1 on versions that model SYSTEM CPU1 clock control. */
    qtest_writel(qts, 0x600c0000, 2);
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    wait_word(qts, RESULT_BASE + 8, 1, UINT32_MAX);
    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");
    elapsed = qtest_readl(qts, RESULT_BASE);
    g_test_message("128-byte burst consumed %u RTC ticks", elapsed);
    g_assert_cmpuint(qtest_readl(qts, RESULT_BASE + 4), ==,
                     UART_FIFO_LENGTH * (UART_FIFO_LENGTH - 1) / 2);
    wait_count(qts, 0);
    /* Draining buffered data must not wait for another serial interval. */
    g_assert_cmpuint(elapsed, <,
        (UART_FIFO_LENGTH * 10 * RTC_SLOW_HZ + UART_BAUD - 1) / UART_BAUD);
    close(peer);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    /*
     * Reset-vector ROM: CPU0 spins; CPU1 measures RTC time around 128 UART1
     * FIFO reads, sums the bytes, and publishes elapsed/sum/done in DRAM.
     * No firmware, ESP-IDF or external cross compiler is required.
     *
     * rsr a2,prid; bbsi a2,1,rx; busy: j busy
     * rx: a2=RESULT_BASE; a3=RTC_BASE; a4=BIT(31)
     * s32i a4,a3,12; l32i a5,a3,16; a3=UART_BASE; a4=128; a6=0
     * loop: l32i a8,a3,0; add a6,a6,a8; addi a4,a4,-1; bnez a4,loop
     * a3=RTC_BASE; a4=BIT(31); s32i a4,a3,12; l32i a8,a3,16
     * sub a5,a8,a5; s32i a5,a2,0; s32i a6,a2,4
     * movi a4,1; s32i a4,a2,8; finish: j finish
     */
    static const uint8_t code[] = {
        0x06, 0x04, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x3f,
        0x00, 0x80, 0x00, 0x60, 0x00, 0x00, 0x00, 0x80,
        0x00, 0x00, 0x01, 0x60, 0x20, 0xeb, 0x03, 0x17,
        0xe2, 0x02, 0x06, 0xff, 0xff, 0x21, 0xf9, 0xff,
        0x31, 0xfa, 0xff, 0x41, 0xfa, 0xff, 0x49, 0x33,
        0x58, 0x43, 0x31, 0xf9, 0xff, 0x42, 0xa0, 0x80,
        0x0c, 0x06, 0x88, 0x03, 0x8a, 0x66, 0x0b, 0x44,
        0x56, 0x64, 0xff, 0x31, 0xf3, 0xff, 0x41, 0xf3,
        0xff, 0x49, 0x33, 0x88, 0x43, 0x50, 0x58, 0xc0,
        0x59, 0x02, 0x69, 0x12, 0x0c, 0x14, 0x49, 0x22,
        0x06, 0xff, 0xff, 0x00,
    };
    uint8_t rom[0x400 + sizeof(code)] = { 0 };
    char *path;
    int ret;

    g_test_init(&argc, &argv, NULL);
    rom_dir = g_dir_make_tmp("esp32s3-uart-icount-XXXXXX", NULL);
    g_assert_nonnull(rom_dir);
    path = g_build_filename(rom_dir, "esp32s3_rev0_rom.bin", NULL);
    memcpy(rom + 0x400, code, sizeof(code));
    g_assert_true(g_file_set_contents(path, (char *)rom, sizeof(rom), NULL));
    qtest_add_func("/esp32s3/uart-rx/unthrottled-refill",
                   test_unthrottled_refill);
    qtest_add_func("/esp32s3/uart-rx/throttle-expiry", test_throttle_expiry);
    if (qtest_has_accel("tcg")) {
        qtest_add_func("/esp32s3/uart-rx/icount-burst", test_icount_burst);
    }
    ret = g_test_run();
    unlink(path);
    rmdir(rom_dir);
    g_free(path);
    g_free(rom_dir);
    return ret;
}
