// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/hvalve.c
 *
 * Copyright (c) 2022 Inhyuk Choi
 *
 * System-call handler for Hvalve.
 */

#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <asm/processor.h>
#include <asm/uaccess.h>

#include <hvalve/hvalve.h>

SYSCALL_DEFINE4(hvalve, int, comm, int, arg1, int, arg2, int, arg3)
{
	switch(comm) {
	case 1: //get launcher
		set_launcher_uid(arg1);
		break;
	case 2: //Not use
		break;
	case 3: //update foreground uid
		update_created_uid(arg1);
		break;
	case 4: //update memory pressure
		update_memory_pressure(arg1);
		break;
	case 5: //before kill send data
		hvalve_get_victim_data(arg1, arg2, arg3);
		break;
	default:
		return -1;
	}

	return 1;
}
