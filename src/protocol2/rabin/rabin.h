#ifndef __RABIN_H
#define __RABIN_H

#include "include.h"

extern int blks_generate_init(void);
extern int blks_generate(struct asfd *asfd, struct conf **confs,
	struct sbuf *sb, struct blist *blist);

#endif
