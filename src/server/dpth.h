#ifndef __DPTH_H
#define __DPTH_H

// ext3 maximum number of subdirs is 32000, so leave a little room.
#define MAX_STORAGE_SUBDIRS	30000

// Wrapper around the lock stuff, so that we can have a list of them and
// also keep the save_path without the leading directories.
struct dpth_lock
{
	char save_path[15];
	struct lock *lock;
	struct dpth_lock *next;
};

struct dpth
{
	// Protocol 1 only uses these.
	uint16_t prim;
	uint16_t seco;
	uint16_t tert;

	// Protocol 2 also uses these.
	uint16_t sig;
	char *base_path;
	// Whether we need to lock another data file.
	uint8_t need_data_lock;
	int max_storage_subdirs;
	// Currently open data file. Only one is open at a time, while many
	// may be locked.
	FILE *fp;
	// List of locked data files. 
	struct dpth_lock *head;
	struct dpth_lock *tail;
};

extern struct dpth *dpth_alloc(void);
extern void dpth_free(struct dpth **dpth);

extern int dpth_incr(struct dpth *dpth);
extern int dpth_release_and_move_to_next_in_list(struct dpth *dpth);
extern int dpth_release_all(struct dpth *dpth);

#endif
