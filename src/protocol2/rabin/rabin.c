#include "include.h"

static struct blk *blk=NULL;
static char *gcp=NULL;
static char *gbuf=NULL;
static char *gbuf_end=NULL;
static struct rconf rconf;
static struct win *win=NULL; // Rabin sliding window.
static int first=0;

int blks_generate_init(void)
{
	rconf_init(&rconf);
	if(!(win=win_alloc(&rconf))
	  || !(gbuf=(char *)malloc_w(rconf.blk_max, __func__)))
		return -1;
	gbuf_end=gbuf;
	gcp=gbuf;
	return 0;
}

// This is where the magic happens.
// Return 1 for got a block, 0 for no block got.
static int blk_read(void)
{
	char c;

	for(; gcp<gbuf_end; gcp++)
	{
		c=*gcp;

		blk->fingerprint = (blk->fingerprint * rconf.prime) + c;
		win->checksum    = (win->checksum    * rconf.prime) + c
				   - (win->data[win->pos] * rconf.multiplier);
		win->data[win->pos] = c;

		win->pos++;
		if(blk->data) blk->data[blk->length] = c;
		blk->length++;

		if(win->pos == rconf.win_size) win->pos=0;

		if( blk->length >= rconf.blk_min
		 && (blk->length == rconf.blk_max
		  || (win->checksum % rconf.blk_avg) == rconf.prime))
		{
			gcp++;
			return 1;
		}
	}
	return 0;
}

static int blk_read_to_list(struct sbuf *sb, struct blist *blist)
{
	if(!blk_read()) return 0;

	// Got something.
	if(first)
	{
		sb->protocol2->bstart=blk;
		first=0;
	}
	if(!sb->protocol2->bsighead)
	{
		sb->protocol2->bsighead=blk;
	}
	blist_add_blk(blist, blk);
	blk=NULL;
	return 1;
}

// The client uses this.
int blks_generate(struct asfd *asfd, struct conf **confs,
	struct sbuf *sb, struct blist *blist)
{
	static ssize_t bytes;

	if(sb->protocol2->bfd.mode==BF_CLOSED)
	{
		if(sbuf_open_file(sb, asfd, confs)) return -1;
		first=1;
	}

	if(!blk && !(blk=blk_alloc_with_data(rconf.blk_max)))
		return -1;

	if(gcp<gbuf_end)
	{
		// Could have got a fill before buf ran out -
		// need to resume from the same place in that case.
		if(blk_read_to_list(sb, blist))
			return 0; // Got a block.
		// Did not get a block. Carry on and read more.
	}
	while((bytes=sbuf_read(sb, gbuf, rconf.blk_max)))
	{
		gcp=gbuf;
		gbuf_end=gbuf+bytes;
		sb->protocol2->bytes_read+=bytes;
		if(blk_read_to_list(sb, blist))
			return 0; // Got a block
		// Did not get a block. Maybe should try again?
		// If there are async timeouts, look at this!
		return 0;
	}

	// Getting here means there is no more to read from the file.
	// Make sure to deal with anything left over.

	if(!sb->protocol2->bytes_read)
	{
		// Empty file, set up an empty block so that the server
		// can skip over it.
		if(!(sb->protocol2->bstart=blk_alloc())) return -1;
		sb->protocol2->bsighead=blk;
		blist_add_blk(blist, blk);
		blk=NULL;
	}
	else if(blk)
	{
		if(blk->length)
		{
			if(first)
			{
				sb->protocol2->bstart=blk;
				first=0;
			}
			if(!sb->protocol2->bsighead)
			{
				sb->protocol2->bsighead=blk;
			}
			blist_add_blk(blist, blk);
		}
		else blk_free(&blk);
		blk=NULL;
	}
	if(blist->tail) sb->protocol2->bend=blist->tail;
	sbuf_close_file(sb, asfd);
	return 0;
}

// The server uses this for verification.
int blk_read_verify(struct blk *blk_to_verify, struct conf **confs)
{
	if(!win)
	{
		rconf_init(&rconf);
		if(!(win=win_alloc(&rconf))) return -1;
	}

	gbuf=blk_to_verify->data;
	gbuf_end=gbuf+blk_to_verify->length;
	gcp=gbuf;
	if(!blk && !(blk=blk_alloc())) return -1;
	blk->length=0;
	blk->fingerprint=0;

	// FIX THIS: blk_read should return 1 when it has a block.
	// But, if the block is too small (because the end of the file
	// happened), it returns 0, and blks_generate treats it as having found
	// a final block.
	// So, here the return of blk_read is ignored and we look at the
	// position of gcp instead.
	blk_read();
	if(gcp==gbuf_end
	  && blk->fingerprint==blk_to_verify->fingerprint)
		return 1;
	return 0;
}
