#include "include.h"
#include "../../base64.h"
#include "../../cmd.h"
#include "../../protocol2/blk.h"
#include "../../protocol2/blist.h"
#include "../../protocol2/rabin/rabin.h"

#define END_SIGS                0x01
#define END_BACKUP              0x02
#define END_REQUESTS            0x04
#define END_BLK_REQUESTS        0x08

static uint64_t decode_req(const char *buf)
{
	int64_t val;
	const char *p=buf;
	p+=from_base64(&val, p);
	return (uint64_t)val;
}

static int add_to_file_requests(struct slist *slist, struct iobuf *rbuf,
	struct conf **confs)
{
	static uint64_t file_no=1;
	struct sbuf *sb;

	if(!(sb=sbuf_alloc(confs))) return -1;

	iobuf_move(&sb->path, rbuf);
	// Give it a number to simplify tracking.
	sb->protocol2->index=file_no++;
	slist_add_sbuf(slist, sb);

	return 0;
}

static int add_to_data_requests(struct blist *blist, struct iobuf *rbuf)
{
	uint64_t index;
	struct blk *blk;
	index=decode_req(rbuf->buf);

//printf("last_requested: %d\n", blist->last_requested->index);

	// Find the matching entry.
	for(blk=blist->last_requested; blk; blk=blk->next)
		if(index==blk->index) break;
	if(!blk)
	{
		logp("Could not find requested block %lu\n", index);
		return -1;
	}
	blk->requested=1;
	blist->last_requested=blk;
	//printf("Found %lu\n", index);
	return 0;
}

static int deal_with_read(struct iobuf *rbuf, struct slist *slist,
	struct conf **confs, uint8_t *end_flags)
{
	int ret=0;
	switch(rbuf->cmd)
	{
		/* Incoming file request. */
		case CMD_FILE:
			if(add_to_file_requests(slist, rbuf, confs)) goto error;
			return 0;

		/* Incoming data block request. */
		case CMD_DATA_REQ:
			if(add_to_data_requests(slist->blist, rbuf)) goto error;
			goto end;

		/* Incoming control/message stuff. */
		case CMD_WRAP_UP:
		{
			int64_t wrap_up;
			struct blk *blk;
			struct blist *blist=slist->blist;
			from_base64(&wrap_up, rbuf->buf);
			for(blk=blist->head; blk; blk=blk->next)
			{
				if(blk->index==(uint64_t)wrap_up)
				{
					blist->last_requested=blk;
					blist->last_sent=blk;
					break;
				}
			}
			if(!blk)
			{
				logp("Could not find wrap up index: %016"PRIX64 "\n",
					wrap_up);
//				goto error;
			}
			goto end;
		}
		case CMD_MESSAGE:
		case CMD_WARNING:
			log_recvd(rbuf, confs, 0);
			goto end;
		case CMD_GEN:
			if(!strcmp(rbuf->buf, "requests_end"))
			{
				(*end_flags)|=END_REQUESTS;
				goto end;
			}
			else if(!strcmp(rbuf->buf, "blk_requests_end"))
			{
				(*end_flags)|=END_BLK_REQUESTS;
				goto end;
			}
			else if(!strcmp(rbuf->buf, "backup_end"))
			{
				(*end_flags)|=END_BACKUP;
				goto end;
			}
			break;
		default:
			break;
	}

	iobuf_log_unexpected(rbuf, __func__);
error:
	ret=-1;
end:
	iobuf_free_content(rbuf);
	return ret;
}

static int add_to_blks_list(struct asfd *asfd, struct conf **confs,
	struct slist *slist)
{
	struct sbuf *sb=slist->last_requested;
	if(!sb) return 0;
	if(blks_generate(asfd, confs, sb, slist->blist)) return -1;

	// If it closed the file, move to the next one.
	if(sb->protocol2->bfd.mode==BF_CLOSED) slist->last_requested=sb->next;

	return 0;
}

static void free_stuff(struct slist *slist)
{
	struct blk *blk;
	struct blist *blist=slist->blist;
	blk=blist->head;
	while(blk && blk!=blist->last_sent)
	{
		if(blk==slist->head->protocol2->bstart)
			slist->head->protocol2->bstart=NULL;
		if(blk==slist->head->protocol2->bend)
		{
			struct sbuf *sb;
			sb=slist->head;
			sb->protocol2->bend=NULL;
			if(!(slist->head=slist->head->next))
				slist->tail=NULL;
			sbuf_free(&sb);
		}
		blk=blk->next;
		blk_free(&blist->head);
		blist->head=blk;
	}
}

static void get_wbuf_from_data(struct conf **confs,
	struct iobuf *wbuf, struct slist *slist, uint8_t end_flags)
{
	struct blk *blk;
	struct blist *blist=slist->blist;

	for(blk=blist->last_sent; blk; blk=blk->next)
	{
		if(blk->requested)
		{
			wbuf->cmd=CMD_DATA;
			wbuf->buf=blk->data;
			wbuf->len=blk->length;
			blk->requested=0;
			blist->last_sent=blk;
			cntr_add(get_cntr(confs), CMD_DATA, 1);
			cntr_add_sentbytes(get_cntr(confs), blk->length);
			break;
		}
		else
		{
			cntr_add_same(get_cntr(confs), CMD_DATA);
			if(end_flags&END_BLK_REQUESTS)
			{
				// Force onwards when the server has said that
				// there are no more blocks to request.
				blist->last_sent=blk;
				continue;
			}
		}
		if(blk==blist->last_requested) break;
	}
	// Need to free stuff that is no longer needed.
	free_stuff(slist);
}

static int iobuf_from_blk_data(struct iobuf *wbuf, struct blk *blk)
{
	static char buf[CHECKSUM_LEN];
	if(blk_md5_update(blk)) return -1;

	// FIX THIS: consider endian-ness.
	memcpy(buf, &blk->fingerprint, FINGERPRINT_LEN);
	memcpy(buf+FINGERPRINT_LEN, blk->md5sum, MD5_DIGEST_LENGTH);
	iobuf_set(wbuf, CMD_SIG, buf, CHECKSUM_LEN);
	return 0;
}

static int get_wbuf_from_blks(struct iobuf *wbuf,
	struct slist *slist, uint8_t *end_flags)
{
	struct sbuf *sb=slist->blks_to_send;

	if(!sb)
	{
		if((*end_flags)&END_REQUESTS && !((*end_flags)&END_SIGS))
		{
			iobuf_from_str(wbuf, CMD_GEN, (char *)"sigs_end");
			(*end_flags)|=END_SIGS;
		}
		return 0;
	}
	if(!sb->protocol2->bsighead) return 0;

	if(!(sb->flags & SBUF_SENT_STAT))
	{
		iobuf_copy(wbuf, &sb->attr);
		wbuf->cmd=CMD_ATTRIBS_SIGS; // hack
		sb->flags |= SBUF_SENT_STAT;
		return 0;
	}

	if(iobuf_from_blk_data(wbuf, sb->protocol2->bsighead)) return -1;

	// Move on.
	if(sb->protocol2->bsighead==sb->protocol2->bend)
	{
/*
		wbuf->cmd=CMD_END_FILE;
		wbuf->buf=(char *)"0:0";
		wbuf->len=strlen(wbuf->buf);
*/
		slist->blks_to_send=sb->next;
		sb->protocol2->bsighead=sb->protocol2->bstart;
	}
	else
	{
		sb->protocol2->bsighead=sb->protocol2->bsighead->next;
	}
	return 0;
}

int backup_phase2_client_protocol2(struct asfd *asfd,
	struct conf **confs, int resume)
{
	int ret=-1;
	uint8_t end_flags=0;
	struct slist *slist=NULL;
	struct iobuf *rbuf=NULL;
	struct iobuf *wbuf=NULL;
	struct cntr *cntr=get_cntr(confs);

	logp("Phase 2 begin (send backup data)\n");
	printf("\n");

	if(!(slist=slist_alloc())
	  || !(wbuf=iobuf_alloc())
	  || blks_generate_init())
		goto end;
	rbuf=asfd->rbuf;

	if(!resume)
	{
		// Only do this bit if the server did not tell us to resume.
		if(asfd->write_str(asfd, CMD_GEN, "backupphase2")
		  || asfd->read_expect(asfd, CMD_GEN, "ok"))
			goto end;
	}
	else if(get_int(confs[OPT_SEND_CLIENT_CNTR]))
	{
		// On resume, the server might update the client with the
		// counters.
		if(cntr_recv(asfd, confs))
			goto end;
        }

	while(!(end_flags&END_BACKUP))
	{
		if(!wbuf->len)
		{
			get_wbuf_from_data(confs, wbuf, slist,
				end_flags);
			if(!wbuf->len)
			{
				if(get_wbuf_from_blks(wbuf, slist,
					&end_flags)) goto end;
			}
		}

		if(wbuf->len)
		{
			if(asfd->append_all_to_write_buffer(asfd, wbuf)
				==APPEND_ERROR)
					goto end;
		}
		if(asfd->as->read_write(asfd->as))
		{
			logp("error in %s\n", __func__);
			goto end;
		}

		if(rbuf->buf && deal_with_read(rbuf, slist, confs, &end_flags))
			goto end;

		if(slist->head
		// Need to limit how many blocks are allocated at once.
		  && (!slist->blist->head
		   || slist->blist->tail->index
			- slist->blist->head->index<BLKS_MAX_IN_MEM)
		)
		{
			if(add_to_blks_list(asfd, confs, slist))
				goto end;
		}

		if(end_flags&END_BLK_REQUESTS)
		{
			// If got to the end of the file request list
			// and the last block of the last file, and
			// the write buffer is empty, we got to the end.
			if(slist->head==slist->tail)
			{
				if(!slist->tail
				  || slist->blist->last_sent==
					slist->tail->protocol2->bend)
				{
					if(!wbuf->len)
						break;
				}
			}

		}
	}

	if(asfd->write_str(asfd, CMD_GEN, "backup_end"))
		goto end;

	ret=0;
end:
	slist_free(&slist);
	// Write buffer did not allocate 'buf'.
	wbuf->buf=NULL;
	iobuf_free(&wbuf);

	cntr_print_end(cntr);
	cntr_print(cntr, ACTION_BACKUP);
	if(ret) logp("Error in backup\n");
	logp("End backup\n");

	return ret;
}
