#include "include.h"

static int write_all(struct asfd *asfd)
{
	int ret=-1;
	size_t len;
	const unsigned char *buf;
	yajl_gen_get_buf(yajl, &buf, &len);
	ret=asfd->write_strn(asfd, CMD_GEN /* not used */,
		(const char *)buf, len);
	yajl_gen_clear(yajl);
	return ret;
}

static int json_start(struct asfd *asfd)
{
	if(!yajl)
	{
		if(!(yajl=yajl_gen_alloc(NULL)))
			return -1;
		yajl_gen_config(yajl, yajl_gen_beautify, 1);
	}
	if(yajl_map_open_w()
	  || yajl_gen_str_w("clients")
	  || yajl_array_open_w())
		return -1;
	return 0;
}

static int json_end(struct asfd *asfd)
{
	int ret=-1;
	if(yajl_array_close_w()
	  || yajl_map_close_w())
		goto end;
	ret=write_all(asfd);
end:
	yajl_gen_free(yajl);
	yajl=NULL;
	return ret;
}

static long timestamp_to_long(const char *buf)
{
	struct tm tm;
	const char *b=NULL;
	if(!(b=strchr(buf, ' '))) return 0;
	memset(&tm, 0, sizeof(struct tm));
	if(!strptime(b, " %Y-%m-%d %H:%M:%S", &tm)) return 0;
	// Tell mktime to use the daylight savings time setting
	// from the time zone of the system.
	tm.tm_isdst=-1;
	return (long)mktime(&tm);
}

static int flag_matches(struct bu *bu, uint16_t flag)
{
	return (bu && (bu->flags & flag));
}

static int flag_wrap_str(struct bu *bu, uint16_t flag, const char *field)
{
	if(!flag_matches(bu, flag)) return 0;
	return yajl_gen_str_w(field);
}

static int flag_wrap_str_zp(struct bu *bu, uint16_t flag, const char *field,
	gzFile zp, const char *logfile)
{
	if(!flag_matches(bu, flag)) return 0;
	if(yajl_gen_str_w(field)) return -1;
	if(yajl_array_open_w()) return -1;
	if(zp && logfile && !strcmp(logfile, field))
	{
		char *cp=NULL;
		char buf[1024]="";
		while(gzgets(zp, buf, sizeof(buf)))
		{
			if((cp=strrchr(buf, '\n'))) *cp='\0';
			if(yajl_gen_str_w(buf))
				return -1;
		}
	}
	if(yajl_array_close_w()) return -1;
	return 0;
}

static int json_send_backup(struct asfd *asfd, struct bu *bu,
	int print_flags, gzFile zp, const char *logfile)
{
	long long bno=0;
	long long timestamp=0;
	if(!bu) return 0;
	bno=(long long)bu->bno;
	timestamp=(long long)timestamp_to_long(bu->timestamp);

	if(yajl_map_open_w()
	  || yajl_gen_int_pair_w("number", bno)
	  || yajl_gen_int_pair_w("timestamp", timestamp)
	  || yajl_gen_str_w("flags")
	  || yajl_array_open_w()
	  || flag_wrap_str(bu, BU_HARDLINKED, "hardlinked")
	  || flag_wrap_str(bu, BU_DELETABLE, "deletable")
	  || flag_wrap_str(bu, BU_WORKING, "working")
	  || flag_wrap_str(bu, BU_FINISHING, "finishing")
	  || flag_wrap_str(bu, BU_CURRENT, "current")
	  || flag_wrap_str(bu, BU_MANIFEST, "manifest")
	  || yajl_array_close_w())
		return -1;
	if(print_flags
	  && (bu->flags & (BU_LOG_BACKUP|BU_LOG_RESTORE|BU_LOG_VERIFY)))
	{
		if(yajl_gen_str_w("logs")
		  || yajl_map_open_w()
		  || flag_wrap_str_zp(bu, BU_LOG_BACKUP, "backup", zp, logfile)
		  || flag_wrap_str_zp(bu, BU_LOG_RESTORE, "restore", zp, logfile)
		  || flag_wrap_str_zp(bu, BU_LOG_VERIFY, "verify", zp, logfile)
		  || yajl_map_close_w())
			return -1;
	}
	if(yajl_gen_map_close(yajl)!=yajl_gen_status_ok)
		return -1;

	return 0;
}

static int json_send_client_start(struct asfd *asfd, struct cstat *cstat)
{
	const char *status=cstat_status_to_str(cstat);

	if(yajl_map_open_w()
	  || yajl_gen_str_pair_w("name", cstat->name)
	  || yajl_gen_str_pair_w("status", status)
	  || yajl_gen_str_w("backups")
	  || yajl_array_open_w())
			return -1;
	return 0;
}

static int json_send_client_end(struct asfd *asfd)
{
	if(yajl_array_close_w()
	  || yajl_map_close_w())
		return -1;
	return 0;
}

static int json_send_client_backup(struct asfd *asfd,
	struct cstat *cstat, struct bu *bu, gzFile zp, const char *logfile)
{
	int ret=-1;
	if(json_send_client_start(asfd, cstat)) return -1;
	ret=json_send_backup(asfd, bu, 1 /* print flags */, zp, logfile);
	if(json_send_client_end(asfd)) ret=-1;
	return ret;
}

static int json_send_client_backup_list(struct asfd *asfd, struct cstat *cstat)
{
	int ret=-1;
	struct bu *bu;
	if(json_send_client_start(asfd, cstat)) return -1;
	for(bu=cstat->bu; bu; bu=bu->prev)
	{
		if(json_send_backup(asfd, bu, 1 /* print flags */, NULL, NULL))
			goto end;
	}
	ret=0;
end:
	if(json_send_client_end(asfd)) ret=-1;
	return ret;
}

int json_send_zp(struct asfd *asfd, gzFile zp,
	struct cstat *cstat, unsigned long bno, const char *logfile)
{
	int ret=-1;
	struct bu *bu=NULL;

	if(json_start(asfd)) goto end;

	if(bno) for(bu=cstat->bu; bu; bu=bu->prev) if(bu->bno==bno) break;

	if(json_send_client_backup(asfd, cstat, bu, zp, logfile)) goto end;

	ret=0;
end:
	if(json_end(asfd)) return -1;
	return ret;
}

int json_send(struct asfd *asfd, struct cstat *clist, struct cstat *cstat)
{
	int ret=-1;
	struct cstat *c;

	if(json_start(asfd)) goto end;

	if(cstat)
	{
		if(json_send_client_backup_list(asfd, cstat)) goto end;
	}
	else for(c=clist; c; c=c->next)
	{
		if(json_send_client_backup(asfd, c, c->bu, NULL, NULL))
			return -1;
	}

	ret=0;
end:
	if(json_end(asfd)) return -1;
	return ret;
}