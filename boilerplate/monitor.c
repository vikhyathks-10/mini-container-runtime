/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Provided boilerplate:
 * - device registration and teardown
 * - timer setup
 * - RSS helper
 * - soft-limit and hard-limit event helpers
 * - ioctl dispatch shell
 *
 * Completed by student.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * TODO 1: Define your linked-list node struct.
 * ============================================================== */
struct monitor_node {
    pid_t pid;
    char container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int soft_warning_emitted; /* 0 = false, 1 = true */
    struct list_head list;    /* Kernel linked list linkage */
};

/* ==============================================================
 * TODO 2: Declare the global monitored list and a lock.
 * ============================================================== */
static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);

/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
 *
 * Returns the Resident Set Size in bytes for the given PID,
 * or -1 if the task no longer exists.
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit helper
 *
 * Log a warning when a process exceeds the soft limit.
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit helper
 *
 * Kill a process when it exceeds the hard limit.
 * --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Timer Callback - fires every CHECK_INTERVAL_SEC seconds.
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    /* ==============================================================
     * TODO 3: Implement periodic monitoring.
     * ============================================================== */
    struct monitor_node *entry, *tmp;
    long rss_bytes;

    /* Lock the list so nobody registers/unregisters while we are checking */
    mutex_lock(&monitor_lock);

    /* Safely loop through every container we are tracking */
    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        rss_bytes = get_rss_bytes(entry->pid);

        /* 1. If rss_bytes is -1, the container exited normally. Clean it up. */
        if (rss_bytes == -1) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* 2. HARD LIMIT: If memory > hard limit, kill it and clean it up. */
        if (rss_bytes > entry->hard_limit_bytes) {
            kill_process(entry->container_id, entry->pid, entry->hard_limit_bytes, rss_bytes);
            list_del(&entry->list);
            kfree(entry);
            continue; 
        }

        /* 3. SOFT LIMIT: If memory > soft limit, log a warning (but only once). */
        if (rss_bytes > entry->soft_limit_bytes && entry->soft_warning_emitted == 0) {
            log_soft_limit_event(entry->container_id, entry->pid, entry->soft_limit_bytes, rss_bytes);
            entry->soft_warning_emitted = 1; 
        }
    }

    mutex_unlock(&monitor_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 *
 * Supported operations:
 * - register a PID with soft + hard limits
 * - unregister a PID when the runtime no longer needs tracking
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        struct monitor_node *new_node;

        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid, req.soft_limit_bytes, req.hard_limit_bytes);

        /* ==============================================================
         * TODO 4: Add a monitored entry.
         * ============================================================== */
        new_node = kmalloc(sizeof(struct monitor_node), GFP_KERNEL);
        if (!new_node)
            return -ENOMEM;

        new_node->pid = req.pid;
        strncpy(new_node->container_id, req.container_id, MONITOR_NAME_LEN);
        new_node->soft_limit_bytes = req.soft_limit_bytes;
        new_node->hard_limit_bytes = req.hard_limit_bytes;
        new_node->soft_warning_emitted = 0;

        mutex_lock(&monitor_lock);
        list_add_tail(&new_node->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        return 0;
    }

    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /* ==============================================================
     * TODO 5: Remove a monitored entry on explicit unregister.
     * ============================================================== */
    {
        struct monitor_node *entry, *tmp;
        int found = 0;

        mutex_lock(&monitor_lock);
        list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
            if (entry->pid == req.pid && 
                strncmp(entry->container_id, req.container_id, MONITOR_NAME_LEN) == 0) {
                list_del(&entry->list);
                kfree(entry);
                found = 1;
                break;
            }
        }
        mutex_unlock(&monitor_lock);

        if (found)
            return 0;
    }

    return -ENOENT;
}

/* --- Provided: file operations --- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided: Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
    /* Use the newer timer_delete_sync for 6.15+ kernels, fallback for older ones */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
    timer_delete_sync(&monitor_timer);
#else
    del_timer_sync(&monitor_timer);
#endif

    /* ==============================================================
     * TODO 6: Free all remaining monitored entries.
     * ============================================================== */
    {
        struct monitor_node *entry, *tmp;

        mutex_lock(&monitor_lock);
        list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
            list_del(&entry->list);
            kfree(entry);
        }
        mutex_unlock(&monitor_lock);
    }

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
