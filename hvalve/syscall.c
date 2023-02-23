// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/hvalve.c
 *
 * Copyright (c) 2022 Inhyuk Choi
 *
 * System-call function implementations for Hvalve.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <hvalve/hvalve.h>
#include <linux/types.h>
#include <linux/hashtable.h>

#include "../drivers/scsi/ufs/ufshcd.h"
#include "../drivers/scsi/ufs/ufshpb.h"

#ifdef ENABLE_HVALVE

#define NO_FOREGROUND_APP 987654321
atomic_t launch_state = ATOMIC_INIT(0);
atomic_t current_foreground_uid = ATOMIC_INIT(0);
atomic_t current_memory_pressure = ATOMIC_INIT(0);
int launcher_uid;

int mp_count[4] = { 0,};

struct ufs_hba* hvalve_dev;

void set_current_foreground_uid(int uid) {
	if (uid == launcher_uid) {
		atomic_set(&current_foreground_uid, NO_FOREGROUND_APP);
	} else {
		atomic_set(&current_foreground_uid, uid);
		atomic_set(&launch_state, 1);
	}
}
EXPORT_SYMBOL(set_current_foreground_uid);

int get_current_foreground_uid(void) {
	return current_foreground_uid.counter;
}
EXPORT_SYMBOL(get_current_foreground_uid);

int is_launch_state(void) {
	return launch_state.counter;
}
EXPORT_SYMBOL(is_launch_state);

void update_created_uid(int uid) {
	atomic_set(&current_foreground_uid, uid);
	atomic_set(&launch_state, 0);
}
EXPORT_SYMBOL(update_created_uid);

void update_memory_pressure(int size) {
	mp_count[size]++;
	atomic_set(&current_memory_pressure, size);
	printk("mp_event_signal: current memory pressure: %d, low: %d, midium: %d, critical:%d, super critical: %d\n",
			size, mp_count[0], mp_count[1], mp_count[2], mp_count[3]);
}
EXPORT_SYMBOL(update_memory_pressure);

int get_memory_pressure(void) {
	return current_memory_pressure.counter;
}
EXPORT_SYMBOL(update_memory_pressure);

void set_launcher_uid(int uid) {
	launcher_uid = uid;
}
EXPORT_SYMBOL(set_launcher_uid);

int get_launcher_uid() {
	return launcher_uid;
}
EXPORT_SYMBOL(get_launcher_uid);

void hvalve_init(struct ufs_hba* dev) {
	if (dev)
		hvalve_dev = dev;
}
EXPORT_SYMBOL(hvalve_init);

void hvalve_get_victim_data(int uid, int free_size, int priority) {
	if (hvalve_dev && !hvalve_dev->ufshpb_dev.hpb_disabled && hvalve_dev->hpb_func) {
		hvalve_dev->hpb_func(hvalve_dev, uid, free_size, priority);
	}
}
EXPORT_SYMBOL(hvalve_get_victim_data);

static int __init init_hvalve(void) {
	printk("init hvalve\n");
	return 0;
}

static void __exit exit_hvalve(void) {
	printk("exit hvalve\n");
}

module_init(init_hvalve);
module_exit(exit_hvalve);
#endif
