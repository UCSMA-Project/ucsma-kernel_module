#include "htc.h"
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/timer.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>

struct gpio unlock_gpios[] = {
  {21, GPIOF_OUT_INIT_LOW, "UNLOCK_OUT"},
  {22, GPIOF_IN, "UNLOCK_IN" },
};

/* Entry point of driver */
static int __init unlock_init(void)
{
  int ret, i;
  unsigned long int sec, usec;
  struct timeval start, end;

  printk(KERN_INFO "[Interrupt test] Init\n");

  /* Get access to GPIOS for recieving and propogating unlock signal */
  if ((ret = gpio_request_array(unlock_gpios, ARRAY_SIZE(unlock_gpios)))) {
    printk(KERN_ERR "[Interrupt test] unable to get input gpio\n");
	goto fail;
  }

  gpio_set_value(unlock_gpios[0].gpio, 1);
  gpio_set_value(unlock_gpios[0].gpio, 0);
  
  do_gettimeofday(&start);
  for (i = 0; i < 1000000; i++) {
    if (gpio_get_value(unlock_gpios[1].gpio))
      break;
  }
  do_gettimeofday(&end);
  sec = (end.tv_sec - start.tv_sec) * 1000000 + end.tv_usec - start.tv_usec;
  usec = sec % 1000000;
  sec /= 1000000;
  printk(KERN_INFO "[Interrupt test] GPIO polling %d times in %lu.%06lus\n", i, sec, usec);

  return 0;

fail:
  gpio_free_array(unlock_gpios, ARRAY_SIZE(unlock_gpios));

  return ret;
}

/* Exit Point of driver */
static void __exit unlock_exit(void)
{
  gpio_free_array(unlock_gpios, ARRAY_SIZE(unlock_gpios));
  printk(KERN_INFO "[Interrupt test] Uninit");
  return;
}

module_init(unlock_init);
module_exit(unlock_exit);
MODULE_LICENSE("GPL");
