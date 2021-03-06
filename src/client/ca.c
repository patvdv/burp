#include "include.h"
#include "../cmd.h"

static int generate_key_and_csr(struct asfd *asfd,
	struct conf **confs, const char *csr_path)
{
	int a=0;
	const char *args[12];
	const char *ca_burp_ca=get_string(confs[OPT_CA_BURP_CA]);
	const char *cname=get_string(confs[OPT_CNAME]);
	const char *ssl_key=get_string(confs[OPT_SSL_KEY]);

	logp("Generating SSL key and certificate signing request\n");
	logp("Running '%s --key --keypath %s --request --requestpath %s --name %s'\n", ca_burp_ca, ssl_key, csr_path, cname);
#ifdef HAVE_WIN32
	win32_enable_backup_privileges();
#endif
	args[a++]=ca_burp_ca;
	args[a++]="--key";
	args[a++]="--keypath";
	args[a++]=ssl_key;
	args[a++]="--request";
	args[a++]="--requestpath";
	args[a++]=csr_path;
	args[a++]="--name";
	args[a++]=cname;
	args[a++]=NULL;
	if(run_script(asfd, args, NULL, confs, 1 /* wait */,
		0, 0 /* do not use logp - stupid openssl prints lots of dots
		        one at a time with no way to turn it off */))
	{
		logp("error when running '%s --key --keypath %s --request --requestpath %s --name %s'\n",
			ca_burp_ca, ssl_key, csr_path, cname);
		return -1;
	}

	return 0;
}

/* Rewrite the conf file with the ssl_peer_cn value changed to what the
   server told us it should be. */
static int rewrite_client_conf(struct conf **confs)
{
	int ret=-1;
	char p[32]="";
	FILE *dp=NULL;
	FILE *sp=NULL;
	char *tmp=NULL;
	char buf[4096]="";
	const char *conffile=get_string(confs[OPT_CONFFILE]);
	const char *ssl_peer_cn=get_string(confs[OPT_SSL_PEER_CN]);

	logp("Rewriting conf file: %s\n", conffile);
	snprintf(p, sizeof(p), ".%d", getpid());
	if(!(tmp=prepend(conffile, p)))
		goto end;
	if(!(sp=open_file(conffile, "rb"))
	  || !(dp=open_file(tmp, "wb")))
		goto end;

	while(fgets(buf, sizeof(buf), sp))
	{
		char *copy=NULL;
		char *field=NULL;
		char *value=NULL;

		if(!(copy=strdup_w(buf, __func__)))
			goto end;
		if(conf_get_pair(buf, &field, &value)
		  || !field || !value
		  || strcmp(field, "ssl_peer_cn"))
		{
			fprintf(dp, "%s", copy);
			free_w(&copy);
			continue;
		}
		free_w(&copy);
#ifdef HAVE_WIN32
		fprintf(dp, "ssl_peer_cn = %s\r\n", ssl_peer_cn);
#else
		fprintf(dp, "ssl_peer_cn = %s\n", ssl_peer_cn);
#endif
	}
	close_fp(&sp);
	if(close_fp(&dp))
	{
		logp("error closing %s in %s\n", tmp, __func__);
		goto end;
	}
	// Nasty race conditions going on here. However, the new config
	// file will get left behind, so at worse you will have to move
	// the new file into the correct place by hand. Or delete everything
	// and start again.
#ifdef HAVE_WIN32
	// Need to delete the destination, or Windows gets upset.
	unlink(conffile);
#endif
	if(do_rename(tmp, conffile)) goto end;

	ret=0;
end:
	close_fp(&sp);
	close_fp(&dp);
	if(ret)
	{
		logp("Rewrite failed\n");
		unlink(tmp);
	}
	free_w(&tmp);
	return ret;
}

static enum asl_ret csr_client_func(struct asfd *asfd,
        struct conf **confs, void *param)
{
	if(strncmp_w(asfd->rbuf->buf, "csr ok:"))
	{
		iobuf_log_unexpected(asfd->rbuf, __func__);
		return ASL_END_ERROR;
	}
	// The server appends its name after 'csr ok:'
	if(set_string(confs[OPT_SSL_PEER_CN], 
		asfd->rbuf->buf+strlen("csr ok:")))
			return ASL_END_ERROR;
	return ASL_END_OK;
}

/* Return 1 for everything OK, signed and returned, -1 for error, 0 for
   nothing done. */
int ca_client_setup(struct asfd *asfd, struct conf **confs)
{
	int ret=-1;
	struct stat statp;
	char csr_path[256]="";
	char ssl_cert_tmp[512]="";
	char ssl_cert_ca_tmp[512]="";
	const char *ca_burp_ca=get_string(confs[OPT_CA_BURP_CA]);
	const char *ca_csr_dir=get_string(confs[OPT_CA_CSR_DIR]);
	const char *cname=get_string(confs[OPT_CNAME]);
	const char *ssl_key=get_string(confs[OPT_SSL_KEY]);
	const char *ssl_cert=get_string(confs[OPT_SSL_CERT]);
	const char *ssl_cert_ca=get_string(confs[OPT_SSL_CERT_CA]);

	// Do not continue if we have one of the following things not set.
	if(  !ca_burp_ca
	  || !ca_csr_dir
	  || !ssl_cert_ca
	  || !ssl_cert
	  || !ssl_key
	// Do not try to get a new certificate if we already have a key.
	  || !lstat(ssl_key, &statp))
	{
		if(asfd->write_str(asfd, CMD_GEN, "nocsr")
		  || asfd->read_expect(asfd, CMD_GEN, "nocsr ok"))
		{
			logp("problem reading from server nocsr\n");
			goto end;
		}
		logp("nocsr ok\n");
		ret=0;
		goto end;
	}

	// Tell the server we want to do a signing request.
	if(asfd->write_str(asfd, CMD_GEN, "csr")
	  || asfd->simple_loop(asfd, confs, NULL, __func__, csr_client_func))
		goto end;

	logp("Server will sign a certificate request\n");

	// First need to generate a client key and a certificate signing
	// request.
	snprintf(csr_path, sizeof(csr_path), "%s/%s.csr", ca_csr_dir, cname);
	if(generate_key_and_csr(asfd, confs, csr_path)) goto end_cleanup;

	// Then copy the csr to the server.
	if(send_a_file(asfd, csr_path, confs)) goto end_cleanup;

	snprintf(ssl_cert_tmp, sizeof(ssl_cert_tmp), "%s.%d",
		ssl_cert, getpid());
	snprintf(ssl_cert_ca_tmp, sizeof(ssl_cert_ca_tmp), "%s.%d",
		ssl_cert_ca, getpid());

	// The server will then sign it, and give it back.
	if(receive_a_file(asfd, ssl_cert_tmp, confs)) goto end_cleanup;

	// The server will also send the CA certificate.
	if(receive_a_file(asfd, ssl_cert_ca_tmp, confs)) goto end_cleanup;

	// Possible race condition - the rename can delete the destination
	// and then fail. Worse case, the user has to rename them by hand.
	if(do_rename(ssl_cert_tmp, ssl_cert)
	  || do_rename(ssl_cert_ca_tmp, ssl_cert_ca))
		goto end_cleanup;

	// Need to rewrite our configuration file to contain the server
	// name (ssl_peer_cn)
	if(rewrite_client_conf(confs)) goto end_cleanup;

	// My goodness, everything seems to have gone OK. Stand back!
	ret=1;
end_cleanup:
	if(ret<0)
	{
		// On error, remove any possibly newly created files, so that
		// this function might run again on another go.
		unlink(csr_path);
		unlink(ssl_key);
		unlink(ssl_cert);
		unlink(ssl_cert_ca);
		unlink(ssl_cert_tmp);
		unlink(ssl_cert_ca_tmp);
	}
end:
	return ret;
}
