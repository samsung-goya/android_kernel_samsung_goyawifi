/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/reboot.h>
#define ARRAY_AND_SIZE(x)	(x), ARRAY_SIZE(x)

extern void timer_init(int irq);
void __init mmp_dt_init_timer(void);

extern void __init mmp_map_io(void);
extern void mmp_restart(enum reboot_mode, const char *);
void pxa988_secondary_startup(void);
void pxa988_cpu_reset_entry(void);
void pxa988_cpu_resume_handler(void);
extern unsigned long pxa988_cpu_reset_vector[];
void pxa988_set_reset_handler(unsigned long phys_addr, unsigned int cpu);
void pxa988_gic_raise_softirq(const struct cpumask *mask, unsigned int irq);
