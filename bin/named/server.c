/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>

#include <isc/assertions.h>
#include <isc/error.h>
#include <isc/rwlock.h>
#include <isc/mem.h>
#include <isc/task.h>
#include <isc/thread.h>
#include <isc/result.h>
#include <isc/socket.h>
#include <isc/timer.h>
#include <isc/app.h>
#include <isc/dir.h>
#include <isc/util.h>

#include <dns/aclconf.h>
#include <dns/cache.h>
#include <dns/confparser.h>
#include <dns/types.h>
#include <dns/result.h>
#include <dns/master.h>
#include <dns/name.h>
#include <dns/fixedname.h>
#include <dns/rdata.h>
#include <dns/rdatalist.h>
#include <dns/rdataset.h>
#include <dns/rdatasetiter.h>
#include <dns/compress.h>
#include <dns/db.h>
#include <dns/dbtable.h>
#include <dns/message.h>
#include <dns/journal.h>
#include <dns/view.h>
#include <dns/zone.h>
#include <dns/zoneconf.h>
#include <dns/tsig.h>
#include <dns/tkey.h>

#include <named/client.h>
#include <named/globals.h>
#include <named/interfacemgr.h>
#include <named/listenlist.h>
#include <named/log.h>
#include <named/rootns.h>
#include <named/server.h>
#include <named/types.h>

typedef struct {
	isc_mem_t *		mctx;
	dns_viewlist_t		viewlist;
	dns_aclconfctx_t	*aclconf;
} ns_load_t;

/*
 * Configure 'view' according to 'cctx'.
 *
 * XXX reconfiguration should preserve cache contents.
 */
static isc_result_t
configure_view(dns_view_t *view, dns_c_ctx_t *cctx, isc_mem_t *mctx)
{
	dns_cache_t *cache;
	isc_result_t result;
	isc_int32_t cleaning_interval;
	dns_tsig_keyring_t *ring;

	REQUIRE(DNS_VIEW_VALID(view));

	/*
	 * Cache.
	 */
	cache = NULL;
	result = dns_cache_create(mctx, ns_g_taskmgr, ns_g_timermgr,
				  view->rdclass, "rbt", 0, NULL, &cache);
	if (result != ISC_R_SUCCESS)
		goto cleanup;
	dns_view_setcache(view, cache);
	cleaning_interval = 3600; /* Default is 1 hour. */
	(void) dns_c_ctx_getcleaninterval(cctx, &cleaning_interval);
	dns_cache_setcleaninginterval(cache, cleaning_interval);
	dns_cache_detach(&cache);

	/*
	 * XXXRTH  Temporary support for loading cache contents.
	 */
	if (ns_g_cachefile != NULL) {
		isc_log_write(ns_g_lctx, NS_LOGCATEGORY_GENERAL,
			      NS_LOGMODULE_SERVER,
			      ISC_LOG_DEBUG(1), "loading cache '%s'",
			      ns_g_cachefile);
		result = dns_db_load(view->cachedb, ns_g_cachefile);
		if (result != ISC_R_SUCCESS)
			goto cleanup;
	}

	/*
	 * Resolver.
	 *
	 * XXXRTH hardwired number of tasks.  Also, we'll need to
	 * see if we are dealing with a shared dispatcher in this view.
	 */
	result = dns_view_createresolver(view, ns_g_taskmgr, 31,
					 ns_g_socketmgr, ns_g_timermgr,
					 NULL);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	/*
	 * We have default hints for class IN.
	 */
	if (view->rdclass == dns_rdataclass_in)
		dns_view_sethints(view, ns_g_server->roothints);

	/*
	 * Load the TSIG keys
	 */
	ring = NULL;
	result = dns_tsig_init(cctx, view->mctx, &ring);
	if (result != ISC_R_SUCCESS)
		goto cleanup;
	dns_view_setkeyring(view, ring);

	return (ISC_R_SUCCESS);

 cleanup:
	return (result);
}

/*
 * Create the special view that handles queries for
 * "version.bind. CH".   The version string returned is that
 * configured in 'configctx', or a compiled-in default if
 * there is no "version" configuration option.
 */
static isc_result_t
create_version_view(dns_c_ctx_t *configctx, dns_view_t **viewp) {
	isc_result_t result;
	dns_db_t *db = NULL;
	dns_zone_t *zone = NULL;
	dns_dbversion_t *dbver = NULL;
	dns_difftuple_t *tuple = NULL;
	dns_diff_t diff;
	dns_view_t *view = NULL;
	char *versiontext;
	unsigned char buf[256];
	isc_region_t r;
	size_t len;
	dns_rdata_t rdata;
	static unsigned char origindata[] = "\007version\004bind";
	dns_name_t origin;

	REQUIRE(viewp != NULL && *viewp == NULL);

	dns_diff_init(ns_g_mctx, &diff);

	dns_name_init(&origin, NULL);
	r.base = origindata;
	r.length = sizeof(origindata);
	dns_name_fromregion(&origin, &r);

	(void) dns_c_ctx_getversion(configctx, &versiontext);
	if (versiontext == NULL)
		versiontext = ns_g_version;
	len = strlen(versiontext);
	if (len > 255)
		len = 255; /* Silently truncate. */
	buf[0] = len;
	memcpy(buf + 1, versiontext, len);

	r.base = buf;
	r.length = 1 + len;
	dns_rdata_fromregion(&rdata, dns_rdataclass_ch, dns_rdatatype_txt, &r);

	result = dns_zone_create(&zone, ns_g_mctx);
	if (result != ISC_R_SUCCESS)
		goto cleanup;
	result = dns_zone_setorigin(zone, &origin);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	result = dns_db_create(ns_g_mctx, "rbt", &origin, ISC_FALSE,
			       dns_rdataclass_ch, 0, NULL, &db);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	result = dns_db_newversion(db, &dbver);
	if (result != DNS_R_SUCCESS)
		goto cleanup;

	dns_difftuple_create(ns_g_mctx, DNS_DIFFOP_ADD, &origin,
			     0, &rdata, &tuple);
	dns_diff_append(&diff, &tuple);
	result = dns_diff_apply(&diff, db, dbver);
	if (result != DNS_R_SUCCESS)
		goto cleanup;

	dns_db_closeversion(db, &dbver, ISC_TRUE);

	result = dns_view_create(ns_g_mctx, dns_rdataclass_ch, "_version",
				 &view);
	if (result != ISC_R_SUCCESS)
		return (result);

	result = dns_zone_replacedb(zone, db, ISC_FALSE);
	if (result != DNS_R_SUCCESS)
		goto cleanup;

	result = dns_view_addzone(view, zone);
	if (result != DNS_R_SUCCESS)
		goto cleanup;

	dns_view_freeze(view);

	/* Transfer ownership. */
	*viewp = view;
	view = NULL;

	result = ISC_R_SUCCESS;

 cleanup:
	if (view != NULL)
		dns_view_detach(&view);
	if (zone != NULL)
		dns_zone_detach(&zone);
	if (dbver != NULL)
		dns_db_closeversion(db, &dbver, ISC_FALSE);
	if (db != NULL)
		dns_db_detach(&db);
	dns_diff_clear(&diff);

	return (result);
}

/*
 * Configure or reconfigure a zone.  This callback function
 * is called after parsing each "zone" statement in named.conf.
 */
static isc_result_t
load_zone(dns_c_ctx_t *cctx, dns_c_zone_t *czone, dns_c_view_t *cview,
	  void *uap)
{
	ns_load_t *lctx = (ns_load_t *) uap;
	dns_view_t *view = NULL;	/* New view */
	dns_view_t *pview = NULL;	/* Production view */
	dns_zone_t *zone = NULL;	/* New or reused zone */
	dns_zone_t *tzone = NULL;	/* Temporary zone */
	char *viewname;
	
	isc_result_t result;

	char *corigin;	
	isc_buffer_t buffer;
	dns_fixedname_t fixorigin;
	dns_name_t *origin;
	
	/*
	 * Get the zone origin as a dns_name_t.
	 */
	corigin = NULL;
	/* XXX casting away const */
	result = dns_c_zone_getname(czone, (const char **) &corigin);
	if (result != DNS_R_SUCCESS)
		goto cleanup;
	isc_buffer_init(&buffer, corigin, strlen(corigin), ISC_BUFFERTYPE_TEXT);
	isc_buffer_add(&buffer, strlen(corigin));
	dns_fixedname_init(&fixorigin);
	result = dns_name_fromtext(dns_fixedname_name(&fixorigin),
			  	   &buffer, dns_rootname, ISC_FALSE, NULL);
	if (result != DNS_R_SUCCESS)
		goto cleanup;
	origin = dns_fixedname_name(&fixorigin);
	
	/*
	 * Find or create the view in the new view list.
	 */
	view = NULL;
	if (cview != NULL)
		viewname = cview->name;
	else
		viewname = "_default";
	result = dns_viewlist_find(&lctx->viewlist, viewname,
				   czone->zclass, &view);
	if (result != ISC_R_NOTFOUND && result != ISC_R_SUCCESS)
		goto cleanup;
	if (view == NULL) {
		dns_view_t *tview = NULL;
		result = dns_view_create(ns_g_mctx, czone->zclass,
					 viewname, &view);
		if (result != ISC_R_SUCCESS)
			goto cleanup;
		dns_view_attach(view, &tview);
		ISC_LIST_APPEND(lctx->viewlist, tview, link);
	}

	/*
	 * Check for duplicates in the new zone table.
	 */
	result = dns_view_findzone(view, origin, &tzone);
	if (result == ISC_R_SUCCESS) {
		/*
		 * We already have this zone!
		 */
		result = ISC_R_EXISTS;
		goto cleanup;
	}

	/*
	 * See if we can reuse an existing zone.  This is
	 * only possible if all of these are true:
	 *   - The zone's view exists
	 *   - A zone with the right name exists in the view
	 *   - The zone is compatible with the config
	 *     options (e.g., an existing master zone cannot 
	 *     be reused if the options specify a slave zone)
	 */
	RWLOCK(&ns_g_server->viewlock, isc_rwlocktype_read);
	result = dns_viewlist_find(&ns_g_server->viewlist,
				   view->name, view->rdclass,
				   &pview);
     	RWUNLOCK(&ns_g_server->viewlock, isc_rwlocktype_read);
	if (result != ISC_R_NOTFOUND && result != ISC_R_SUCCESS)
		goto cleanup;
	if (pview != NULL)
		result = dns_view_findzone(pview, origin, &zone);
	if (result != ISC_R_NOTFOUND && result != ISC_R_SUCCESS)
		goto cleanup;
	if (zone != NULL) {
		if (! dns_zone_reusable(zone, czone))
			dns_zone_detach(&zone);
	}

	/*
	 * If we cannot reuse an existing zone, we will have to
	 * create a new one.
	 */
	if (zone == NULL) {
		result = dns_zone_create(&zone, lctx->mctx);
		if (result != ISC_R_SUCCESS)
			goto cleanup;
		result = dns_zone_setorigin(zone, origin);
		if (result != ISC_R_SUCCESS)
			goto cleanup;
		result = dns_zonemgr_managezone(ns_g_server->zonemgr,
						zone);
		if (result != ISC_R_SUCCESS)
			goto cleanup;
		/* XXX Unmanage? */
	}

	/*
	 * Configure the zone.
	 */
	result = dns_zone_configure(cctx, lctx->aclconf, czone, zone);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	/*
	 * XXX Why was this here?
	 *
	 * if (dns_zone_gettype(zone) == dns_zone_hint)
	 *      INSIST(0);
	 */

	/*
	 * Add the zone to its view in the new view list.
	 */
	result = dns_view_addzone(view, zone);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

 cleanup:
	if (tzone != NULL)
		dns_zone_detach(&tzone);
	if (zone != NULL)
		dns_zone_detach(&zone);
	if (pview != NULL)
		dns_view_detach(&pview);
	if (view != NULL)
		dns_view_detach(&view);

	return (result);
}

/* XXX will need error recovery for reconfig */
static void
configure_server_acl(dns_c_ctx_t *cctx, dns_aclconfctx_t *actx, isc_mem_t *mctx,
		     isc_result_t (*getcacl)(dns_c_ctx_t *, dns_c_ipmatchlist_t **),
			  dns_acl_t **aclp)
{
	isc_result_t result;
	dns_c_ipmatchlist_t *cacl = NULL;
	if (*aclp != NULL)
		dns_acl_detach(aclp);
	(void) (*getcacl)(cctx, &cacl);
	if (cacl != NULL) {
		result = dns_acl_fromconfig(cacl, cctx, actx, mctx, aclp);
		if (result != DNS_R_SUCCESS)
			ns_server_fatal(NS_LOGMODULE_SERVER, ISC_FALSE,
					"server ACL setup failed");
		dns_c_ipmatchlist_detach(&cacl);
	}
}

static void
configure_server_quota(dns_c_ctx_t *cctx,
		       isc_result_t (*getquota)(dns_c_ctx_t *, isc_int32_t *),
		       isc_quota_t *quota, int defaultvalue)
{
	isc_int32_t val = defaultvalue;
	(void)(*getquota)(cctx, &val);
	quota->max = val;
}

static void
load_configuration(const char *filename, ns_server_t *server) {
	isc_result_t result;
	ns_load_t lctx;
	dns_c_cbks_t callbacks;
	dns_c_ctx_t *configctx;
	dns_view_t *view, *view_next;
	dns_viewlist_t oviewlist;
	dns_aclconfctx_t aclconfctx;

	dns_aclconfctx_init(&aclconfctx);

	lctx.mctx = ns_g_mctx;
	lctx.aclconf = &aclconfctx;
	ISC_LIST_INIT(lctx.viewlist);

	callbacks.zonecbk = load_zone;
	callbacks.zonecbkuap = &lctx;
	callbacks.optscbk = NULL;
	callbacks.optscbkuap = NULL;

	isc_log_write(ns_g_lctx, NS_LOGCATEGORY_GENERAL, NS_LOGMODULE_SERVER,
		      ISC_LOG_INFO, "loading configuration from '%s'",
		      filename);

	/*
	 * Parse the configuration file creating a parse tree.  Any
	 * 'zone' statements are handled immediately by calling
	 * load_zone() through 'callbacks'.
	 */
	configctx = NULL;
	result = dns_c_parse_namedconf(filename, ns_g_mctx, &configctx,
				       &callbacks);
	if (result != ISC_R_SUCCESS) {
#ifdef notyet
		for (view = ISC_LIST_HEAD(lctx.viewlist);
		     view != NULL;
		     view = view_next) {
			view_next = ISC_LIST_NEXT(view, link);
			ISC_LIST_UNLINK(lctx.viewlist, view, link);
			dns_view_detach(&view);
		}
#endif
		ns_server_fatal(NS_LOGMODULE_SERVER, ISC_FALSE,
				"load of '%s' failed", filename);
	}

	/*
	 * Configure various server options.
	 */
	(void) dns_c_ctx_getrecursion(configctx, &server->recursion);	
	(void) dns_c_ctx_getauthnxdomain(configctx, &server->auth_nxdomain);
	(void) dns_c_ctx_gettransferformat(configctx, &server->transfer_format);
	
	configure_server_acl(configctx, &aclconfctx, ns_g_mctx,
			     dns_c_ctx_getqueryacl, &server->queryacl);

	configure_server_acl(configctx, &aclconfctx, ns_g_mctx,
			     dns_c_ctx_getrecursionacl, &server->recursionacl);

	configure_server_acl(configctx, &aclconfctx, ns_g_mctx,
			     dns_c_ctx_gettransferacl, &server->transferacl);
	
	configure_server_quota(configctx, dns_c_ctx_gettransfersout,
			       &server->xfroutquota, 10);
	configure_server_quota(configctx, dns_c_ctx_gettcpclients,
			       &server->tcpquota, 100);
	configure_server_quota(configctx, dns_c_ctx_getrecursiveclients,
			       &server->recursionquota, 100);

	/*
	 * Configure the interface manager according to the "listen-on"
	 * statement.
	 */
	{
		dns_c_lstnlist_t *clistenon = NULL;
		ns_listenlist_t *listenon = NULL;

		(void) dns_c_ctx_getlistenlist(configctx, &clistenon);
		if (clistenon != NULL) {
			result = ns_listenlist_fromconfig(clistenon,
							  configctx,
							  &aclconfctx,
							  ns_g_mctx, &listenon);
		} else {
			/* Not specified, use default. */
			result = ns_listenlist_default(ns_g_mctx, ns_g_port,
						       &listenon);
		}
		RUNTIME_CHECK(result == ISC_R_SUCCESS);
		ns_interfacemgr_setlistenon(server->interfacemgr, listenon);
		ns_listenlist_detach(&listenon);
	}

	/*
	 * If we haven't created any views, create a default view for class
	 * IN.  (We're a caching-only server.)
	 */
	if (ISC_LIST_EMPTY(lctx.viewlist)) {
		view = NULL;
		result = dns_view_create(ns_g_mctx, dns_rdataclass_in, 
					 "_default", &view);
		if (result != ISC_R_SUCCESS) {
			UNEXPECTED_ERROR(__FILE__, __LINE__, 
					"could not create default view");
			goto cleanup;
		}
		ISC_LIST_APPEND(lctx.viewlist, view, link);
	}

	/*
	 * Configure and freeze the views.  Their zone tables have
	 * already been filled in at parsing time, but other stuff
	 * like the resolvers are still unconfigured.
	 */
	for (view = ISC_LIST_HEAD(lctx.viewlist);
	     view != NULL;
	     view = ISC_LIST_NEXT(view, link))
	{
		result = configure_view(view, configctx, ns_g_mctx);
		if (result != ISC_R_SUCCESS)
			goto cleanup;
		dns_view_freeze(view);
	}
	
	/*
	 * Create (or recreate) the version view.
	 */
	view = NULL;
	result = create_version_view(configctx, &view);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				"could not create version view");
		goto cleanup;
	}
	ISC_LIST_APPEND(lctx.viewlist, view, link);
	view = NULL;

	/*
	 * Change directory.
	 */
	if (configctx->options != NULL &&
	    configctx->options->directory != NULL) {
		result = isc_dir_chdir(configctx->options->directory);
		if (result != ISC_R_SUCCESS)
			ns_server_fatal(NS_LOGMODULE_SERVER, ISC_FALSE,
					"change directory to '%s' failed: %s",
					configctx->options->directory,
					isc_result_totext(result));
	}

	/*
	 * Load zones.
	 */
	for (view = ISC_LIST_HEAD(lctx.viewlist);
	     view != NULL;
	     view = view_next) {
		view_next = ISC_LIST_NEXT(view, link);
		dns_view_load(view);
	}

	/*
	 * Force zone maintenance.  Do this after loading
	 * so that we know when we need to force AXFR of
	 * slave zones whose master files are missing.
	 */
	dns_zonemgr_forcemaint(server->zonemgr);
		
	/*
	 * Put the configuration into production.
	 */

	RWLOCK(&server->viewlock, isc_rwlocktype_write);

	oviewlist = server->viewlist;
	server->viewlist = lctx.viewlist;

	RWUNLOCK(&server->viewlock, isc_rwlocktype_write);

	/*
	 * Cleanup old configuration.
	 */

	for (view = ISC_LIST_HEAD(oviewlist);
	     view != NULL;
	     view = view_next) {
		view_next = ISC_LIST_NEXT(view, link);
		ISC_LIST_UNLINK(oviewlist, view, link);
		dns_view_detach(&view);
	}

	if (ns_g_tkeyctx != NULL)
		dns_tkey_destroy(&ns_g_tkeyctx);

	/*
	 * Load the TKEY information from the configuration
	 */
	result = dns_tkey_init(configctx, ns_g_mctx, &ns_g_tkeyctx);
	if (result != ISC_R_SUCCESS) {
		ns_server_fatal(NS_LOGMODULE_SERVER, ISC_FALSE,
				"dns_tkey_init() failed: %s",
				isc_result_totext(result));
	}

	/*
	 * Rescan the interface list to pick up changes in the
	 * listen-on option.
	 */
	ns_interfacemgr_scan(server->interfacemgr);
	
	dns_aclconfctx_destroy(&aclconfctx);	

	dns_c_ctx_delete(&configctx);
 cleanup:
	; /* XXX */
}

static void
run_server(isc_task_t *task, isc_event_t *event) {
	isc_result_t result;
	ns_server_t *server = (ns_server_t *) event->arg;
	(void)task;

	isc_event_free(&event);

	result = ns_clientmgr_create(ns_g_mctx, ns_g_taskmgr, ns_g_timermgr,
				     &server->clientmgr);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "ns_clientmgr_create() failed: %s",
				 isc_result_totext(result));
		/* XXX cleanup */
		return;
	}
	
	result = ns_interfacemgr_create(ns_g_mctx, ns_g_taskmgr,
					ns_g_socketmgr, server->clientmgr,
					&server->interfacemgr);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "ns_interfacemgr_create() failed: %s",
				 isc_result_totext(result));
		/* XXX cleanup */
		return;
	}

	load_configuration(ns_g_conffile, server);

	isc_log_write(ns_g_lctx, NS_LOGCATEGORY_GENERAL, NS_LOGMODULE_SERVER,
		      ISC_LOG_INFO, "running");
}

static void
shutdown_server(isc_task_t *task, isc_event_t *event) {
	dns_view_t *view, *view_next;
	ns_server_t *server = (ns_server_t *) event->arg;
		
	(void)task;

	isc_log_write(ns_g_lctx, NS_LOGCATEGORY_GENERAL, NS_LOGMODULE_SERVER,
		      ISC_LOG_INFO, "shutting down");

	RWLOCK(&server->viewlock, isc_rwlocktype_write);

	for (view = ISC_LIST_HEAD(server->viewlist);
	     view != NULL;
	     view = view_next) {
		view_next = ISC_LIST_NEXT(view, link);
		ISC_LIST_UNLINK(server->viewlist, view, link);
		dns_view_detach(&view);
	}

	RWUNLOCK(&server->viewlock, isc_rwlocktype_write);

	dns_tkey_destroy(&ns_g_tkeyctx);

	ns_clientmgr_destroy(&server->clientmgr);
	ns_interfacemgr_shutdown(server->interfacemgr);
	ns_interfacemgr_detach(&server->interfacemgr);	
	dns_zonemgr_shutdown(server->zonemgr);
	
	isc_task_detach(&server->task);
	
	isc_event_free(&event);
}

isc_result_t
ns_server_create(isc_mem_t *mctx, ns_server_t **serverp) {
	isc_result_t result;
	
	ns_server_t *server = isc_mem_get(mctx, sizeof(*server));
	if (server == NULL)
		return (ISC_R_NOMEMORY);
	server->mctx = mctx;
	server->task = NULL;
	
	/* Initialize configuration data with default values. */
	server->recursion = ISC_TRUE;
	server->auth_nxdomain = ISC_FALSE; /* Was true in BIND 8 */
	server->transfer_format = dns_one_answer;
		
	server->queryacl = NULL;
	server->recursionacl = NULL;
	server->transferacl = NULL;

	result = isc_quota_init(&server->xfroutquota, 10);
	RUNTIME_CHECK(result == ISC_R_SUCCESS); 
	result = isc_quota_init(&server->tcpquota, 10);
	RUNTIME_CHECK(result == ISC_R_SUCCESS); 
	result = isc_quota_init(&server->recursionquota, 100);
	RUNTIME_CHECK(result == ISC_R_SUCCESS); 
	
	/* Initialize server data structures. */
	server->zonemgr = NULL;
	server->clientmgr = NULL;
	server->interfacemgr = NULL;
	ISC_LIST_INIT(server->viewlist);
	result = isc_rwlock_init(&server->viewlock, 0, 0);
	RUNTIME_CHECK(result == ISC_R_SUCCESS); 	
	server->roothints = NULL;
		
	result = ns_rootns_create(mctx, &server->roothints);
	RUNTIME_CHECK(result == ISC_R_SUCCESS);
	
	/*
	 * Setup the server task, which is responsible for coordinating
	 * startup and shutdown of the server.
	 */
	result = isc_task_create(ns_g_taskmgr, ns_g_mctx, 0, &server->task);
	if (result != ISC_R_SUCCESS)
		goto cleanup;
	result = isc_task_onshutdown(server->task, shutdown_server, server);
	if (result != ISC_R_SUCCESS)
		goto cleanup_task;
	result = isc_app_onrun(ns_g_mctx, server->task, run_server, server);
	if (result != ISC_R_SUCCESS)
		goto cleanup_task;

	result = dns_zonemgr_create(ns_g_mctx, ns_g_taskmgr, ns_g_timermgr,
				    ns_g_socketmgr, &server->zonemgr);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "ns_zonemgr_create() failed: %s",
				 isc_result_totext(result));
		/* XXX cleanup */
		return (result);
	}

	server->magic = NS_SERVER_MAGIC;
	*serverp = server;
	return (ISC_R_SUCCESS);
	
 cleanup_task:
	isc_task_detach(&server->task);
 cleanup:
	/* XXX more cleanup */
	return (result);
}
	
void
ns_server_destroy(ns_server_t **serverp) {
	ns_server_t *server = *serverp;
	REQUIRE(NS_SERVER_VALID(server));

	INSIST(ISC_LIST_EMPTY(server->viewlist));

	dns_zonemgr_destroy(&server->zonemgr);
	server->zonemgr = NULL;
	
	dns_db_detach(&server->roothints);
	
	isc_rwlock_destroy(&server->viewlock);
	
	if (server->queryacl != NULL)
		dns_acl_detach(&server->queryacl);
	if (server->recursionacl != NULL)
		dns_acl_detach(&server->recursionacl);
	if (server->transferacl != NULL)
		dns_acl_detach(&server->transferacl);

	isc_quota_destroy(&server->recursionquota);
	isc_quota_destroy(&server->tcpquota);
	isc_quota_destroy(&server->xfroutquota);
	
	server->magic = 0;
	isc_mem_put(server->mctx, server, sizeof(*server));
}

void
ns_server_fatal(isc_logmodule_t *module, isc_boolean_t want_core,
		const char *format, ...)
{
	va_list args;

	va_start(args, format);
	isc_log_vwrite(ns_g_lctx, NS_LOGCATEGORY_GENERAL, module,
		       ISC_LOG_CRITICAL, format, args);
	va_end(args);
	isc_log_write(ns_g_lctx, NS_LOGCATEGORY_GENERAL, NS_LOGMODULE_SERVER,
		      ISC_LOG_CRITICAL, "exiting (due to fatal error)");

	if (want_core && ns_g_coreok)
		abort();
	exit(1);
}
