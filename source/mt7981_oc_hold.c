// SPDX-License-Identifier: GPL-2.0-only
/* EXPERIMENTAL MMIO WRITES, explicit user-triggered runtime OC only.
 * Fix candidate: CON1 writes were ignored at POSDIV=0 in the diagnostic.
 * Stage PCW at guarded stock POSDIV=1 with PLL disabled, then select the
 * desired divider with CHG. Record and verify every PCW readback. A test is not proof of safety.
 * Only BUS mux, ARMPLL_CON0 and ARMPLL_CON1 are writable. No voltage,
 * flash, PLL power, Device Tree, or Linux CCF-cache modifications.
 */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/stop_machine.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/thermal.h>
#include <linux/reboot.h>
#include <linux/cpumask.h>
#include <asm/barrier.h>
#include "mt7981_oc_freq.h"

#define MUX_BIT BIT(9)
#define EN BIT(0)
#define CHG BIT(2)
#define STOCK_BUS 0x000e0201U
#define STOCK_CON0 0x00670111U
#define STOCK_PCW 0x82000000U
#define OC_CON0 0x00670101U
/* Explicit manual selection only. Never enable autoload.
 * A successful short benchmark is not a long-term safety proof. */
#define TEMP_ENTER_MAX 70000
#define TEMP_RETURN 74000

static bool run_test, active, allow_unsafe;
static bool monitor_without_timer = true;
static unsigned int hold_seconds = 0; /* until manual return/reboot/guard */
static unsigned int target_mhz = 1300;
module_param(run_test, bool, 0);
module_param(allow_unsafe, bool, 0);
MODULE_PARM_DESC(allow_unsafe, "Explicit risk acknowledgement required above 1700 MHz; NOT a safety override");
module_param(monitor_without_timer, bool, 0);
module_param(hold_seconds, uint, 0);
module_param(target_mhz, uint, 0444);
module_param(active, bool, 0444);
static DEFINE_MUTEX(control_lock);
static struct delayed_work expiry;
static struct thermal_zone_device *thermal;
static unsigned long deadline;
static int temp_peak;
static const char *guard_reason = "stock";
static int operation; /* 0 enter, 1 return */


struct phase {
	u32 wanted_con0, wanted_pcw;
	u32 off, divider, pcw_written, pcw_trigger, pcw_enabled, pcw_final;
	u32 con0_final;
	bool delays_ok, pcw_ok;
};
struct context {
	void __iomem *bus, *aclken, *con0, *con1;
	struct phase target, restore, fallback;
	u32 bus_before, con0_before, pcw_before, aclken_before;
	u32 bus_after, con0_after, pcw_after, aclken_after;
	u64 hz, core, ticks_before, ticks_target, ticks_after;
	bool wrote, selected, fallback_used;
};
static struct context ctx;

static __always_inline u64 counter(void)
{
	u64 v;
	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v) : : "memory");
	return v;
}
static noinline bool wait_ticks(u64 ticks)
{
	u64 start = counter();
	u32 limit = 65536;
	do {
		if (counter() - start >= ticks) return true;
	} while (--limit);
	return false;
}
static noinline u64 loop_ticks(void)
{
	u64 start, end;
	u32 n;
	asm volatile("isb\n\tmrs %0, cntvct_el0\n\tmov %w2, #65536\n"
		     "1: subs %w2, %w2, #1\n\tb.ne 1b\n\tisb\n\tmrs %1, cntvct_el0"
		     : "=&r"(start), "=&r"(end), "=&r"(n) : : "cc", "memory");
	return end - start;
}
static noinline u32 write_checked(void __iomem *reg, u32 v)
{
	u32 r;
	writel(v, reg);
	r = readl(reg);
	dsb(sy); isb();
	return r;
}

/* Must be on alternate CPU parent; caller restores on any early failure.
 * No CCF, sleeping, allocation or printk in stop_machine callback.
 */
static noinline bool program(struct context *s, struct phase *p)
{
	bool d0, d1, d2, d3;
	u32 desired_off = p->wanted_con0 & ~(EN | CHG);
	u32 programming_off = (desired_off & ~GENMASK(6, 4)) | BIT(4);
	p->off = write_checked(s->con0, readl(s->con0) & ~(EN | CHG));
	p->divider = write_checked(s->con0, programming_off);
	d0 = wait_ticks(260);
	p->pcw_written = write_checked(s->con1, p->wanted_pcw);
	if (p->pcw_written != p->wanted_pcw) return false; /* caller restores */
	(void)write_checked(s->con0, desired_off | CHG);
	p->pcw_trigger = readl(s->con1);
	d1 = wait_ticks(260);
	(void)write_checked(s->con0, desired_off | CHG | EN);
	p->pcw_enabled = readl(s->con1);
	d2 = wait_ticks(1300);
	(void)write_checked(s->con0, p->wanted_con0);
	d3 = wait_ticks(260);
	p->con0_final = readl(s->con0);
	p->pcw_final = readl(s->con1);
	p->delays_ok = d0 && d1 && d2 && d3;
	p->pcw_ok = p->pcw_written == p->wanted_pcw &&
		p->pcw_trigger == p->wanted_pcw &&
		p->pcw_enabled == p->wanted_pcw && p->pcw_final == p->wanted_pcw;
	return !(p->off & EN) && p->divider == programming_off && p->delays_ok &&
		p->pcw_ok && p->con0_final == p->wanted_con0;
}


static void snapshot(struct context *s)
{
 s->bus_after=readl(s->bus); s->aclken_after=readl(s->aclken);
 s->con0_after=readl(s->con0); s->pcw_after=readl(s->con1);
}
static bool stock(const struct context *s)
{
 return s->bus_after==STOCK_BUS && s->aclken_after==0x12 &&
        s->con0_after==STOCK_CON0 && s->pcw_after==STOCK_PCW;
}
static bool overclock(const struct context *s)
{
 return s->bus_after==STOCK_BUS && s->aclken_after==0x12 &&
        s->con0_after==OC_CON0 && s->pcw_after==ctx.target.wanted_pcw;
}
/* Read-only live register reporting. clk_rate is cached after direct MMIO,
 * so it MUST NOT be presented as an actual OC frequency. */
static int state_get(char *buf, const struct kernel_param *kp)
{
 u64 mhz;
 unsigned long left;
 int n;
 bool valid;
 mutex_lock(&control_lock);
 if(!ctx.bus || !ctx.aclken || !ctx.con0 || !ctx.con1) {
  mutex_unlock(&control_lock);
  return -ENODEV;
 }
 snapshot(&ctx);
 valid=stock(&ctx) || overclock(&ctx);
 mhz=mt7981_oc_rate_mhz(ctx.pcw_after,ctx.con0_after);
 left=active && hold_seconds && time_before(jiffies,deadline) ? (deadline-jiffies)/HZ : 0;
 n=scnprintf(buf,PAGE_SIZE,"{\"active\":%s,\"valid\":%s,\"effective_mhz\":%llu,\"effective_hz\":%llu,\"requested_mhz\":%u,\"remaining_s\":%lu,\"timer_enabled\":%s,\"bus\":\"%08x\",\"aclken\":\"%08x\",\"con0\":\"%08x\",\"con1\":\"%08x\",\"reason\":\"%s\",\"temp_peak_mc\":%d}\n",
  active?"true":"false",valid?"true":"false",mhz,mt7981_oc_rate_hz(ctx.pcw_after,ctx.con0_after),target_mhz,left,hold_seconds?"true":"false",ctx.bus_after,
  ctx.aclken_after,ctx.con0_after,ctx.pcw_after,guard_reason,temp_peak);
 mutex_unlock(&control_lock);
 return n;
}
static const struct kernel_param_ops state_ops={.get=state_get};
module_param_cb(state,&state_ops,NULL,0444);
static int stopped(void *arg)
{
 struct context *s=arg;
 bool good;
 int rc=0;
 asm volatile("mrs %0, cntfrq_el0" : "=r"(s->hz));
 asm volatile("mrs %0, mpidr_el1" : "=r"(s->core));
 snapshot(s);
 if (s->hz!=13000000 || (s->core & 255)!=0) return -EINVAL;
 if (operation==0 && !stock(s)) return -EINVAL;
 if (operation!=0) {
  if(stock(s)) {active=false; return 0;}
  /* Recovery is allowed only on the known BUS/ACLKEN path and unchanged
   * CON0 non-divider fields. No blind writes to an unknown clock path. */
  if(s->bus_after!=STOCK_BUS || s->aclken_after!=0x12 ||
     (s->con0_after & ~(GENMASK(6,4)|EN|CHG)) !=
     (STOCK_CON0 & ~(GENMASK(6,4)|EN|CHG))) return -EINVAL;
 }
 (void)loop_ticks();
 s->ticks_before=loop_ticks();
 if(write_checked(s->bus, STOCK_BUS & ~MUX_BIT)!=(STOCK_BUS & ~MUX_BIT))
  return -EIO;
 s->wrote=true;
 if(operation==0) {
  good=program(s,&s->target);
  snapshot(s);
  if(good && s->con0_after==OC_CON0 && s->pcw_after==s->target.wanted_pcw &&
     write_checked(s->bus,STOCK_BUS)==STOCK_BUS) {
   s->ticks_target=loop_ticks();
   snapshot(s);
   if(overclock(s)) {active=true; return 0;}
  }
  rc=-EIO;
  if(write_checked(s->bus,STOCK_BUS & ~MUX_BIT)!=(STOCK_BUS & ~MUX_BIT))
   return -EIO;
 }
 good=program(s,&s->restore);
 if(!good) {
  s->fallback_used=true; good=program(s,&s->fallback); rc=-EIO;
 }
 if(good) (void)write_checked(s->bus,STOCK_BUS);
 snapshot(s); s->ticks_after=loop_ticks();
 if(!stock(s)) return -EIO;
 active=false;
 return rc;
}
static void report(const char *name, const struct phase *p)
{
 pr_info("mt7981-oc-hold: phase=%s want=%08x/%08x off=%08x div=%08x pcw=%08x/%08x/%08x/%08x final_con0=%08x delays=%u pcw_ok=%u\n",
  name,p->wanted_con0,p->wanted_pcw,p->off,p->divider,p->pcw_written,
  p->pcw_trigger,p->pcw_enabled,p->pcw_final,p->con0_final,p->delays_ok,p->pcw_ok);
}
static int transition(const char *reason, bool entering)
{
 int rc;
 operation=entering?0:1;
 rc=stop_machine(stopped,&ctx,cpumask_of(0));
 pr_info("mt7981-oc-hold: reason=%s result=%d active=%u fallback=%u ticks=%llu/%llu/%llu regs=%08x/%08x/%08x/%08x\n",
  reason,rc,active,ctx.fallback_used,ctx.ticks_before,ctx.ticks_target,
  ctx.ticks_after,ctx.bus_after,ctx.aclken_after,ctx.con0_after,ctx.pcw_after);
 if(entering) report("target",&ctx.target);
 else report("return",&ctx.restore);
 if(ctx.fallback_used) report("fallback",&ctx.fallback);
 return rc;
}
static void expire_work(struct work_struct *work)
{
 int temp=0, rc;
 bool restore=false;
 mutex_lock(&control_lock);
 if(active) {
  snapshot(&ctx);
  rc=thermal_zone_get_temp(thermal,&temp);
  if(!rc && temp>temp_peak) temp_peak=temp;
  if(!overclock(&ctx)) {guard_reason="register-mismatch";restore=true;}
  else if(rc || temp<=0) {guard_reason="temperature-read-failed";restore=true;}
  else if(temp>=TEMP_RETURN) {guard_reason="temperature-limit";restore=true;}
  else if(hold_seconds && time_after_eq(jiffies,deadline)) {guard_reason="timer-expired";restore=true;}
  if(restore) {
   pr_warn("mt7981-oc-hold: GUARD_RETURN reason=%s temperature_mc=%d\n",guard_reason,temp);
   rc=transition(guard_reason,false);
   if(rc) pr_warn("mt7981-oc-hold: recovery callback reported=%d; checking raw stock\n",rc);
   snapshot(&ctx);
   if(!stock(&ctx)) {
    pr_emerg("mt7981-oc-hold: RESTORE_FAILED_EMERGENCY_REBOOT\n");
    emergency_restart();
   }
  }
  if(active && (hold_seconds || monitor_without_timer)) schedule_delayed_work(&expiry,HZ);
 }
 mutex_unlock(&control_lock);
}
static void unmap(void)
{
 if(ctx.con1) {iounmap(ctx.con1);ctx.con1=NULL;}
 if(ctx.con0) {iounmap(ctx.con0);ctx.con0=NULL;}
 if(ctx.aclken) {iounmap(ctx.aclken);ctx.aclken=NULL;}
 if(ctx.bus) {iounmap(ctx.bus);ctx.bus=NULL;}
}
static int __init probe_init(void)
{
 int rc;
 int temp;
 if(!run_test || (hold_seconds && (hold_seconds<30 || hold_seconds>1800)) ||
    !mt7981_oc_request_allowed(target_mhz,allow_unsafe)) return -EPERM;
 if(target_mhz!=1300 && !mt7981_oc_calc_pcw(target_mhz,&ctx.target.wanted_pcw))
  return -ERANGE;
 if(!of_machine_is_compatible("mediatek,mt7981")) return -ENODEV;
 if(!cpu_online(0) || num_online_cpus()!=2) return -EINVAL;
 thermal=thermal_zone_get_zone_by_name("cpu-thermal");
 if(IS_ERR(thermal)) return PTR_ERR(thermal);
 rc=thermal_zone_get_temp(thermal,&temp);
 if(target_mhz!=1300 && (rc || temp<=0 || temp>=TEMP_ENTER_MAX))
  return rc ? rc : -ERANGE;
 temp_peak=temp;
 ctx.bus=ioremap(0x104007c0,4); ctx.aclken=ioremap(0x10400640,4);
 ctx.con0=ioremap(0x1001e200,4); ctx.con1=ioremap(0x1001e204,4);
 if(!ctx.bus || !ctx.aclken || !ctx.con0 || !ctx.con1) {unmap(); return -ENOMEM;}
 ctx.target.wanted_con0=OC_CON0;
 ctx.restore.wanted_con0=ctx.fallback.wanted_con0=STOCK_CON0;
 ctx.restore.wanted_pcw=ctx.fallback.wanted_pcw=STOCK_PCW;
 INIT_DELAYED_WORK(&expiry,expire_work);
 snapshot(&ctx);
 if(!stock(&ctx)) {unmap();return -EINVAL;}
 if(target_mhz==1300) {
  pr_info("mt7981-oc-hold: READ_ONLY_STOCK regs=%08x/%08x/%08x/%08x NO_WRITES\n",ctx.bus_after,ctx.aclken_after,ctx.con0_after,ctx.pcw_after);
  return 0;
 }
 rc=transition("enter",true);
 if(rc || !active) {
  snapshot(&ctx);
  if(!stock(&ctx)) {
   pr_emerg("mt7981-oc-hold: ENTER_FAILED_NOT_STOCK_EMERGENCY_REBOOT\n");
   emergency_restart();
  }
  unmap();return rc?rc:-EIO;
 }
 guard_reason="manual-overclock";
 deadline=jiffies+msecs_to_jiffies(hold_seconds*1000);
 if(hold_seconds || monitor_without_timer) schedule_delayed_work(&expiry,HZ);
 pr_info("mt7981-oc-hold: target_mhz=%u expiry_s=%u thermal_limit_mc=%u NO_AUTOLOAD NO_FLASH\n",target_mhz,hold_seconds,TEMP_RETURN);
 return 0;
}
static void __exit probe_exit(void)
{
 cancel_delayed_work_sync(&expiry);
 mutex_lock(&control_lock);
 if(active) {
  int rc=transition("unload",false);
  if(rc) pr_warn("mt7981-oc-hold: unload callback reported=%d; checking raw stock\n",rc);
  snapshot(&ctx);
  if(!stock(&ctx)) {
   pr_emerg("mt7981-oc-hold: UNLOAD_RESTORE_FAILED_EMERGENCY_REBOOT\n");
   emergency_restart();
  }
 }
 unmap();
 mutex_unlock(&control_lock);
}
module_init(probe_init);
module_exit(probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental manual MT7981 custom 1301..2000 MHz; above 1700 requires explicit risk; 74C guard; NEVER AUTOLOAD");
