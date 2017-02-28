#include "htc.h"
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/timer.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>

#define POLL_TIME 200
#define BACKOFF_TIME 100
#define MAX_COUNT 2

struct gpio unlock_gpios[] = {
  {21, GPIOF_OUT_INIT_LOW, "UNLOCK_OUT"},
  {22, GPIOF_IN, "UNLOCK_IN" },
};

/* 
 * Status struct from regular ath9k driver
 * Holds onto a large part of the state of the driver
 * and exposes the hooks required to read and write
 * registers
 */
extern struct ath_hw *ath9k_ah;

// sleep delay time parameter, defines how long (in us) to sleep after setting FORCE_QUIET_BIT
static u32 sleep_delay = 0;
module_param(sleep_delay, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

/* FD that we use to keep track of incoming unlock signals */
short int unlock_irq = 0;

/* 
 * Holds onto any pending interrupts while we handle the
 * the incoming unlock interrupt
 */
unsigned long flags;
u32 unlock_count = 0, gpio_irq_count = 0;

long last_interrupt_time = 0;

/* 
 * Timer used to periodically poll Carrier Sense Timeout
 * to see how long TX has been delayed beacuse of 
 * RX_CLEAR being low
 */
struct timer_list cts_poll, gpio_poll;

/* Work that performs cts poll and propogates unlock signal */
static struct work_struct cts_poll_work;

/* Work Queue where all pending unlock operations are inserted */
static struct workqueue_struct *unlock_queue;

/* 
 * Spin Lock used to guarantee that a single unlock
 * interrupt is handled at a time 
 */
DEFINE_SPINLOCK(driver_lock);

/* function that performs unlock mechanism and propogates unlock */
void unlock_work_direct(void) {
  // gpio_set_value(unlock_gpios[0].gpio, 1);
  unlock_count++;
  if (unlock_count % 10000 == 0) {
    printk(KERN_INFO "unlock done %d times\n", unlock_count);
  }

  REG_SET_BIT(ath9k_ah, AR_PCU_MISC, AR_PCU_FORCE_QUIET_COLL);
  // printk(KERN_INFO "U-CSMA set write ack %x\n", REG_READ(ath9k_ah, AR_PCU_MISC) & AR_PCU_FORCE_QUIET_COLL);
  udelay(sleep_delay);
  REG_CLR_BIT(ath9k_ah, AR_PCU_MISC, AR_PCU_FORCE_QUIET_COLL);
  // printk(KERN_INFO "U-CSMA reset write ack %x\n", REG_READ(ath9k_ah, AR_PCU_MISC) & AR_PCU_FORCE_QUIET_COLL);

  // gpio_set_value(unlock_gpios[0].gpio, 0);

  return;
}

/* Work that polls how long it has been since last transmission */
static void poll_work(struct work_struct *work)
{
  u32 val;

  val = REG_READ(ath9k_ah, AR_CST);
  // printk(KERN_INFO "U-CSMA Poll Work %d\n", val & AR_CST_TIMEOUT_COUNTER);

  if ((val & AR_CST_TIMEOUT_COUNTER) > MAX_COUNT){
    gpio_set_value(unlock_gpios[0].gpio, 1);
    udelay(1);
    gpio_set_value(unlock_gpios[0].gpio, 0);
  }

  mod_timer(&cts_poll, jiffies + usecs_to_jiffies(POLL_TIME));
  return;
}

/* Interrupt handler called on falling edfe of UNLOCK_IN GPIO */
static irqreturn_t unlock_r_irq_handler(int irq, void *dev_id) {
  long interrupt_time = jiffies;

  spin_lock_irqsave(&driver_lock, flags);
  gpio_irq_count++;
  if (gpio_irq_count % 10000 == 0) {
    printk(KERN_INFO "GPIO IRQ triggerd %d times\n", gpio_irq_count);
  }
  // printk(KERN_INFO "irq called\n");
  
  //if (interrupt_time - last_interrupt_time >= usecs_to_jiffies(BACKOFF_TIME)) {
    // printk(KERN_INFO "U-CSMA scheduled tasklet\n");
    unlock_work_direct();

    last_interrupt_time = interrupt_time;
  //}

  spin_unlock_irqrestore(&driver_lock, flags);

  return IRQ_HANDLED;
}

/* Timer handler called to poll CTS TIMEOUT */
void cts_handler(unsigned long data) {
  queue_work(unlock_queue, &cts_poll_work);
  return;
}

/* Entry point of driver */
static int __init unlock_init(void)
{
  int ret;

  printk(KERN_INFO "U-CSMA - unlock inserted");

  /* Create workqueue and work items */
  unlock_queue = create_workqueue("U-CSMA");
  INIT_WORK(&cts_poll_work, poll_work);

  /* Initialize CTS poll timer */
  setup_timer(&cts_poll, cts_handler, (unsigned long) ath9k_ah);

  /* Get access to GPIOS for recieving and propogating unlock signal */
  if ((ret = gpio_request_array(unlock_gpios, ARRAY_SIZE(unlock_gpios)))) {
    printk(KERN_ERR "U-CSMA - unable to get input gpio\n");
    return ret;
  }
  /* Get interrupt number for UNLOCK_IN GPIO */
  else if ((ret = unlock_irq = gpio_to_irq(unlock_gpios[1].gpio)) < 0) {
    printk(KERN_ERR "U-CSMA - IRQ mapping failure\n");
    goto fail;
  }
  /* Initialize interrupt on UNLOCK_IN GPIO to call unlock_r_irq_handler */
  else if ((ret = request_irq(unlock_irq,
                (irq_handler_t) unlock_r_irq_handler,
                IRQF_TRIGGER_FALLING,
                "U-CSMA",
                "felipe device"))) {
    printk(KERN_ERR "U-CSMA - unable to get IRQ\n");
    goto fail;
  }
  
  /* Set first call of CTS to be POLL_TIME microseconds from now */
  else if(mod_timer(&cts_poll, jiffies + usecs_to_jiffies(POLL_TIME))){
    printk(KERN_ERR "U-CSMA - timer error\n");
    goto fail;
  }
//  else if(mod_timer(&gpio_poll, jiffies + usecs_to_jiffies(10))){
//    printk(KERN_ERR "U-CSMA - gpio timer error\n");
//    goto fail;
//  }

  printk(KERN_INFO "U-CSMA INIT complete\n");

  return 0;

fail:
  gpio_free_array(unlock_gpios, ARRAY_SIZE(unlock_gpios));

  return ret;
}

/* Exit Point of driver */
static void __exit unlock_exit(void)
{
  del_timer(&cts_poll);
  del_timer(&gpio_poll);
  free_irq(unlock_irq, "felipe device");

  flush_workqueue(unlock_queue);
  destroy_workqueue(unlock_queue);

  gpio_free_array(unlock_gpios, ARRAY_SIZE(unlock_gpios));

  printk(KERN_INFO "U-CSMA - unlock module unloaded\n");
  return;
}

module_init(unlock_init);
module_exit(unlock_exit);
MODULE_LICENSE("GPL");
