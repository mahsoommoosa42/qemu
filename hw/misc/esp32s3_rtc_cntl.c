/*
 * ESP32 RTC_CNTL (RTC block controller) device
 *
 * Copyright (c) 2019-2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/misc/esp32s3_reg.h"
#include "hw/misc/esp32s3_rtc_cntl.h"

static void esp32s3_rtc_update_cpu_stall(Esp32s3RtcCntlState* s);
static void esp32s3_rtc_update_clk(Esp32s3RtcCntlState* s);

#define RTC_TIMER_MASK MAKE_64BIT_MASK(0, 48)
#define RTC_TIMER_WAKEUP BIT(3)
#define RTC_SLEEP_INTERRUPTS (R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK | \
                              R_RTC_CNTL_INT_RAW_MAIN_TIMER_MASK)

static uint64_t esp32s3_rtc_ticks(Esp32s3RtcCntlState *s)
{
    return (s->time_base_ticks + muldiv64(
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->time_base_ns,
                s->rtc_slowclk_freq, NANOSECONDS_PER_SECOND)) & RTC_TIMER_MASK;
}

static void esp32s3_rtc_update_irq(Esp32s3RtcCntlState *s)
{
    qemu_set_irq(s->irq, (s->int_raw & s->int_ena) != 0);
}

static void esp32s3_rtc_update_alarm(Esp32s3RtcCntlState *s)
{
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed = muldiv64(now_ns - s->time_base_ns,
                               s->rtc_slowclk_freq, NANOSECONDS_PER_SECOND);
    uint64_t now = (s->time_base_ticks + elapsed) & RTC_TIMER_MASK;
    int64_t deadline = now_ns + 1;

    if (!s->alarm_enabled) {
        timer_del(s->sleep_timer);
        return;
    }
    if (s->sleep_target > now) {
        uint64_t ticks = elapsed + (s->sleep_target - now);
        uint64_t ns = muldiv64(ticks, NANOSECONDS_PER_SECOND,
                              s->rtc_slowclk_freq);

        /* Keep the counter's phase, including a partially elapsed slow tick. */
        deadline = s->time_base_ns + ns;
        if (muldiv64(ns, s->rtc_slowclk_freq, NANOSECONDS_PER_SECOND) < ticks) {
            deadline++;
        }
    }
    timer_mod(s->sleep_timer, deadline);
}

void esp32s3_rtc_cntl_reset_sleep(Esp32s3RtcCntlState *s)
{
    if (!s->sleeping) {
        return;
    }
    timer_del(s->sleep_timer);
    s->alarm_enabled = false;
    s->sleeping = false;
    esp32s3_rtc_update_cpu_stall(s);
}

static void esp32s3_rtc_timer_expired(void *opaque)
{
    Esp32s3RtcCntlState *s = opaque;

    s->alarm_enabled = false;
    s->int_raw |= R_RTC_CNTL_INT_RAW_MAIN_TIMER_MASK;
    if (s->sleeping && (FIELD_EX32(s->wakeup_state_reg,
            RTC_CNTL_WAKEUP_STATE, WAKEUP_ENA) & RTC_TIMER_WAKEUP)) {
        s->wakeup_cause = RTC_TIMER_WAKEUP;
        s->int_raw |= R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK;
        for (int i = 0; i < ESP32S3_CPU_COUNT; i++) {
            s->reset_cause[i] = ESP32_DEEPSLEEP_RESET;
        }
        qemu_irq_pulse(s->dig_reset_req);
    }
    esp32s3_rtc_update_irq(s);
}

static uint64_t esp32s3_rtc_cntl_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(opaque);
    uint64_t r = 0;
    switch (addr) {
    case A_RTC_CNTL_OPTIONS0:
        r = s->options0_reg;
        break;
    case A_RTC_CNTL_SLP_TIMER0:
        r = s->sleep_target & UINT32_MAX;
        break;
    case A_RTC_CNTL_SLP_TIMER1:
        r = s->sleep_target >> 32;
        break;
    case A_RTC_CNTL_STATE0:
        r = s->sleeping ? R_RTC_CNTL_STATE0_SLEEP_EN_MASK : 0;
        break;
    case A_RTC_CNTL_TIME_UPDATE:
        r = R_RTC_CNTL_TIME_UPDATE_VALID_MASK;
        break;
    case A_RTC_CNTL_TIME0:
        r = s->time_reg & UINT32_MAX;
        break;
    case A_RTC_CNTL_TIME1:
        r = s->time_reg >> 32;
        break;

    case A_RTC_CNTL_RESET_STATE:
        r = FIELD_DP32(r, RTC_CNTL_RESET_STATE, RESET_CAUSE_PROCPU, s->reset_cause[0]);
        r = FIELD_DP32(r, RTC_CNTL_RESET_STATE, RESET_CAUSE_APPCPU, s->reset_cause[1]);
        r = FIELD_DP32(r, RTC_CNTL_RESET_STATE, PROCPU_STAT_VECTOR_SEL, s->stat_vector_sel[0]);
        r = FIELD_DP32(r, RTC_CNTL_RESET_STATE, APPCPU_STAT_VECTOR_SEL, s->stat_vector_sel[1]);
        break;

    case A_RTC_CNTL_WAKEUP_STATE:
        r = s->wakeup_state_reg;
        break;
    case A_RTC_CNTL_SLP_WAKEUP_CAUSE:
        r = s->wakeup_cause;
        break;
    case A_RTC_CNTL_DIG_PWC:
        r = s->dig_pwc_reg;
        break;
    case A_RTC_CNTL_INT_ENA:
        r = s->int_ena;
        break;
    case A_RTC_CNTL_INT_RAW:
        r = s->int_raw;
        break;
    case A_RTC_CNTL_INT_ST:
        r = s->int_raw & s->int_ena;
        break;

    case A_RTC_CNTL_STORE0:
    case A_RTC_CNTL_STORE1:
    case A_RTC_CNTL_STORE2:
    case A_RTC_CNTL_STORE3:
        r = s->scratch_reg[(addr - A_RTC_CNTL_STORE0) / 4];
        break;

    case A_RTC_CNTL_CLK_CONF:
        r = FIELD_DP32(r, RTC_CNTL_CLK_CONF, SOC_CLK_SEL, s->soc_clk);
        r = FIELD_DP32(r, RTC_CNTL_CLK_CONF, FAST_CLK_RTC_SEL, s->rtc_fastclk);
        r = FIELD_DP32(r, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL, s->rtc_slowclk);
        break;

    case A_RTC_CNTL_SW_CPU_STALL:
        r = s->sw_cpu_stall_reg;
        break;

    case A_RTC_CNTL_STORE4:
    case A_RTC_CNTL_STORE5:
    case A_RTC_CNTL_STORE6:
    case A_RTC_CNTL_STORE7:
        r = s->scratch_reg[(addr - A_RTC_CNTL_STORE4) / 4 + 4];
        break;
    }
    return r;
}

static void esp32s3_rtc_cntl_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned int size)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(opaque);
    switch (addr) {
    case A_RTC_CNTL_SLP_TIMER0:
        s->sleep_target = (s->sleep_target & ~UINT64_C(0xffffffff)) | value;
        esp32s3_rtc_update_alarm(s);
        break;
    case A_RTC_CNTL_SLP_TIMER1:
        s->sleep_target = (s->sleep_target & UINT32_MAX) |
                         ((value & UINT64_C(0xffff)) << 32);
        if (value & R_RTC_CNTL_SLP_TIMER1_MAIN_TIMER_ALARM_EN_MASK) {
            s->alarm_enabled = true;
        }
        esp32s3_rtc_update_alarm(s);
        break;
    case A_RTC_CNTL_STATE0:
        if (!(value & R_RTC_CNTL_STATE0_SLEEP_EN_MASK)) {
            break;
        }
        if (!(s->dig_pwc_reg & R_RTC_CNTL_DIG_PWC_DG_WRAP_PD_EN_MASK)) {
            qemu_log_mask(LOG_UNIMP,
                          "esp32s3: light sleep is not implemented\n");
            break;
        }
        if (FIELD_EX32(s->wakeup_state_reg, RTC_CNTL_WAKEUP_STATE,
                      WAKEUP_ENA) & ~RTC_TIMER_WAKEUP) {
            qemu_log_mask(LOG_UNIMP,
                          "esp32s3: only RTC timer wakeup is implemented\n");
        }
        s->sleeping = true;
        s->wakeup_cause = 0;
        esp32s3_rtc_update_cpu_stall(s);
        if (!s->alarm_enabled &&
            (s->int_raw & R_RTC_CNTL_INT_RAW_MAIN_TIMER_MASK)) {
            esp32s3_rtc_timer_expired(s);
        }
        break;
    case A_RTC_CNTL_WAKEUP_STATE:
        s->wakeup_state_reg = value & R_RTC_CNTL_WAKEUP_STATE_WAKEUP_ENA_MASK;
        break;
    case A_RTC_CNTL_DIG_PWC:
        s->dig_pwc_reg = value;
        break;
    case A_RTC_CNTL_INT_ENA:
        s->int_ena = value & RTC_SLEEP_INTERRUPTS;
        esp32s3_rtc_update_irq(s);
        break;
    case A_RTC_CNTL_INT_ENA_W1TS:
        s->int_ena |= value & RTC_SLEEP_INTERRUPTS;
        esp32s3_rtc_update_irq(s);
        break;
    case A_RTC_CNTL_INT_ENA_W1TC:
        s->int_ena &= ~value;
        esp32s3_rtc_update_irq(s);
        break;
    case A_RTC_CNTL_INT_CLR:
        s->int_raw &= ~value;
        esp32s3_rtc_update_irq(s);
        break;
    case A_RTC_CNTL_OPTIONS0:
        if (value & R_RTC_CNTL_OPTIONS0_SW_SYS_RESET_MASK) {
            s->reset_cause[0] = ESP32_SW_SYS_RESET;
            s->reset_cause[1] = ESP32_SW_SYS_RESET;
            qemu_irq_pulse(s->dig_reset_req);
            value &= ~(R_RTC_CNTL_OPTIONS0_SW_SYS_RESET_MASK);
        }
        if (value & R_RTC_CNTL_OPTIONS0_SW_APPCPU_RESET_MASK) {
            s->reset_cause[1] = ESP32_SW_CPU_RESET;
            qemu_irq_pulse(s->cpu_reset_req[1]);
            value &= ~(R_RTC_CNTL_OPTIONS0_SW_APPCPU_RESET_MASK);
        }
        if (value & R_RTC_CNTL_OPTIONS0_SW_PROCPU_RESET_MASK) {
            s->reset_cause[0] = ESP32_SW_CPU_RESET;
            qemu_irq_pulse(s->cpu_reset_req[0]);
            value &= ~(R_RTC_CNTL_OPTIONS0_SW_PROCPU_RESET_MASK);
        }
        s->options0_reg = value;
        esp32s3_rtc_update_cpu_stall(s);
        break;

    case A_RTC_CNTL_TIME_UPDATE:
        if (value & R_RTC_CNTL_TIME_UPDATE_UPDATE_MASK) {
            s->time_reg = esp32s3_rtc_ticks(s);
        }
        break;

    case A_RTC_CNTL_RESET_STATE:
        s->stat_vector_sel[0] = FIELD_EX32(value, RTC_CNTL_RESET_STATE,
                                           PROCPU_STAT_VECTOR_SEL);
        s->stat_vector_sel[1] = FIELD_EX32(value, RTC_CNTL_RESET_STATE,
                                           APPCPU_STAT_VECTOR_SEL);
        break;

    case A_RTC_CNTL_STORE0:
    case A_RTC_CNTL_STORE1:
    case A_RTC_CNTL_STORE2:
    case A_RTC_CNTL_STORE3:
        s->scratch_reg[(addr - A_RTC_CNTL_STORE0) / 4] = value;
        break;

    case A_RTC_CNTL_CLK_CONF: {
        uint32_t slowclk = FIELD_EX32(value, RTC_CNTL_CLK_CONF,
                                    ANA_CLK_RTC_SEL);

        if (slowclk <= ESP32_SLOW_CLK_8MD256 && slowclk != s->rtc_slowclk) {
            s->time_base_ticks = esp32s3_rtc_ticks(s);
            s->time_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        s->soc_clk = FIELD_EX32(value, RTC_CNTL_CLK_CONF, SOC_CLK_SEL);
        s->rtc_fastclk = FIELD_EX32(value, RTC_CNTL_CLK_CONF, FAST_CLK_RTC_SEL);
        s->rtc_slowclk = slowclk;
        esp32s3_rtc_update_clk(s);
        esp32s3_rtc_update_alarm(s);
        break;
    }

    case A_RTC_CNTL_SW_CPU_STALL:
        s->sw_cpu_stall_reg = value;
        esp32s3_rtc_update_cpu_stall(s);
        break;

    case A_RTC_CNTL_STORE4:
    case A_RTC_CNTL_STORE5:
    case A_RTC_CNTL_STORE6:
    case A_RTC_CNTL_STORE7:
        s->scratch_reg[(addr - A_RTC_CNTL_STORE4) / 4 + 4] = value;
        break;
    }
}

static void esp32s3_rtc_update_cpu_stall(Esp32s3RtcCntlState* s)
{
    uint32_t procpu_stall = (FIELD_EX32(s->sw_cpu_stall_reg, RTC_CNTL_SW_CPU_STALL, PROCPU_C1) << 2) |
                            (FIELD_EX32(s->options0_reg, RTC_CNTL_OPTIONS0, SW_STALL_PROCPU_C0));

    uint32_t appcpu_stall = (FIELD_EX32(s->sw_cpu_stall_reg, RTC_CNTL_SW_CPU_STALL, APPCPU_C1) << 2) |
                            (FIELD_EX32(s->options0_reg, RTC_CNTL_OPTIONS0, SW_STALL_APPCPU_C0));

    const uint32_t stall_magic_val = 0x86;

    s->cpu_stall_state[0] = s->sleeping || procpu_stall == stall_magic_val;
    s->cpu_stall_state[1] = s->sleeping || appcpu_stall == stall_magic_val;

    qemu_set_irq(s->cpu_stall_req[0], s->cpu_stall_state[0]);
    qemu_set_irq(s->cpu_stall_req[1], s->cpu_stall_state[1]);
}

static void esp32s3_rtc_update_clk(Esp32s3RtcCntlState* s)
{
    const uint32_t slowclk_freq[] = {150000, 32768, 8000000/256};
    const uint32_t fastclk_freq[] = {s->xtal_apb_freq / 4, 8000000};
    s->rtc_slowclk_freq = slowclk_freq[s->rtc_slowclk];
    s->rtc_fastclk_freq = fastclk_freq[s->rtc_fastclk];
    qemu_irq_pulse(s->clk_update);
}

static const MemoryRegionOps esp32s3_rtc_cntl_ops = {
    .read =  esp32s3_rtc_cntl_read,
    .write = esp32s3_rtc_cntl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_rtc_cntl_reset_hold(Object *obj, ResetType type)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(obj);

    s->time_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->time_base_ticks = 0;
    esp32s3_rtc_cntl_reset_sleep(s);
}

static void esp32s3_rtc_cntl_realize(DeviceState *dev, Error **errp)
{
}

static void esp32s3_rtc_cntl_init(Object *obj)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->sleep_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                esp32s3_rtc_timer_expired, s);

    memory_region_init_io(&s->iomem, obj, &esp32s3_rtc_cntl_ops, s,
                          TYPE_ESP32S3_RTC_CNTL, ESP32S3_RTC_CNTL_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->dig_reset_req, ESP32S3_RTC_DIG_RESET_GPIO, 1);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->cpu_reset_req[0], ESP32S3_RTC_CPU_RESET_GPIO, ESP32S3_CPU_COUNT);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->cpu_stall_req[0], ESP32S3_RTC_CPU_STALL_GPIO, ESP32S3_CPU_COUNT);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->clk_update, ESP32S3_RTC_CLK_UPDATE_GPIO, 1);

    for (int i = 0; i < ESP32S3_CPU_COUNT; ++i) {
        s->reset_cause[i] = ESP32_POWERON_RESET;
        s->stat_vector_sel[i] = true;
    }

    s->rtc_slowclk = ESP32_SLOW_CLK_RC;
    s->rtc_fastclk = ESP32_FAST_CLK_8M;
    s->soc_clk = ESP32_SOC_CLK_XTAL;
    s->xtal_apb_freq = 40000000;
    s->pll_apb_freq = 80000000;
    esp32s3_rtc_update_clk(s);
}

static Property esp32s3_rtc_cntl_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32s3_rtc_cntl_finalize(Object *obj)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(obj);

    timer_free(s->sleep_timer);
}

static void esp32s3_rtc_cntl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_rtc_cntl_reset_hold;
    dc->realize = esp32s3_rtc_cntl_realize;
    device_class_set_props(dc, esp32s3_rtc_cntl_properties);
}

static const TypeInfo esp32s3_rtc_cntl_info = {
    .name = TYPE_ESP32S3_RTC_CNTL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3RtcCntlState),
    .instance_init = esp32s3_rtc_cntl_init,
    .instance_finalize = esp32s3_rtc_cntl_finalize,
    .class_init = esp32s3_rtc_cntl_class_init
};

static void esp32s3_rtc_cntl_register_types(void)
{
    type_register_static(&esp32s3_rtc_cntl_info);
}

type_init(esp32s3_rtc_cntl_register_types)
