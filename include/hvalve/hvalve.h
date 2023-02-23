#ifndef HVALVE_H
#define HVALVE_H

#define ENABLE_HVALVE

enum launch_state {
	RUNNING,
	LAUNCHED
};

enum uid_state {
	NO_UID,
	FOREGROUND,
	BACKGROUND
};

struct ufs_hba;

extern void set_current_foreground_uid(int uid);
extern int get_current_foreground_uid(void);
extern int is_launch_state(void);
extern void update_created_uid(int uid);
extern void start_launch(int uid);
extern void update_memory_pressure(int size);
extern int get_memory_pressure(void);
extern void set_launcher_uid(int uid);
extern int get_launcher_uid(void);
extern void hvalve_init(struct ufs_hba* dev);
extern void hvalve_get_victim_data(int uid, int free_size, int priority);
#endif
