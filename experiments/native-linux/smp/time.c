/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/sched/clock.h>
#include <linux/smp.h>
#include <asm/host_ops.h>

static unsigned long long boot_time;
static struct cpu_clock {
    struct clock_event_device event;
    void *timer;
    int irq;
} clocks[NR_CPUS];

void __ndelay(unsigned long ns)
{
    unsigned long long end = lkl_ops->time() + ns;
    while (lkl_ops->time() < end)
        cpu_relax();
}

void __udelay(unsigned long us)
{
    __ndelay(us * NSEC_PER_USEC);
}

void __const_udelay(unsigned long loops)
{
    __udelay(loops / 0x10c7ul);
}

void calibrate_delay(void)
{
}

void read_persistent_clock64(struct timespec64 *ts)
{
    *ts = ns_to_timespec64(lkl_ops->time());
}

unsigned long long sched_clock(void)
{
    return boot_time ? lkl_ops->time() - boot_time : 0;
}

static u64 clock_read(struct clocksource *source)
{
    return lkl_ops->time();
}

static struct clocksource source = {
    .name = "lkl",
    .rating = 499,
    .read = clock_read,
    .flags = CLOCK_SOURCE_IS_CONTINUOUS,
    .mask = CLOCKSOURCE_MASK(64),
};

static void timer0(void)
{
    lkl_trigger_cpu_irq(0, clocks[0].irq);
}

static void timer1(void)
{
    lkl_trigger_cpu_irq(1, clocks[1].irq);
}

static int shutdown(struct clock_event_device *event)
{
    struct cpu_clock *clock = container_of(event, struct cpu_clock, event);
    if (clock->timer) {
        lkl_ops->timer_free(clock->timer);
        clock->timer = NULL;
    }
    return 0;
}

static int oneshot(struct clock_event_device *event)
{
    struct cpu_clock *clock = container_of(event, struct cpu_clock, event);
    clock->timer = lkl_ops->timer_alloc(clock == clocks ? timer0 : timer1);
    return clock->timer ? 0 : -ENOMEM;
}

static int next_event(unsigned long ns, struct clock_event_device *event)
{
    struct cpu_clock *clock = container_of(event, struct cpu_clock, event);
    return lkl_ops->timer_set_oneshot(clock->timer, ns);
}

static irqreturn_t timer_irq(int irq, void *opaque)
{
    struct clock_event_device *event = opaque;
    event->event_handler(event);
    return IRQ_HANDLED;
}

void lkl_smp_clock_init(unsigned int cpu)
{
    struct cpu_clock *clock = &clocks[cpu];
    clock->event = (struct clock_event_device){
        .name = "lkl",
        .features = CLOCK_EVT_FEAT_ONESHOT,
        .set_state_oneshot = oneshot,
        .set_state_shutdown = shutdown,
        .set_next_event = next_event,
        .cpumask = cpumask_of(cpu),
    };
    clock->irq = lkl_get_free_irq("cpu-timer");
    if (clock->irq < 0 || request_irq(clock->irq, timer_irq, IRQF_NOBALANCING | IRQF_TIMER,
                                      "cpu-timer", &clock->event))
        panic("cannot initialize LKL CPU clock");
    clockevents_config_and_register(&clock->event, NSEC_PER_SEC, 1, ULONG_MAX);
}

void __init time_init(void)
{
    boot_time = lkl_ops->time();
    if (clocksource_register_khz(&source, 1000000))
        panic("cannot initialize LKL clock source");
    lkl_smp_clock_init(0);
}
