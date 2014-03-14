/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Issues
 *
 * + Major limitations
 *   . need to allow on-line (possibly automated) configuration of trusted logs
 *   . need to ensure that audit data is written in a way that audit can be
 *     performed
 *
 * + Known low-level code kludges/problems
 *   . shouldn't have to read file of server SCTs on every handshake
 *   . split mod_ssl_ct.c into more pieces
 *   . support building with httpd 2.4.x
 */

#if !defined(WIN32)
#define HAVE_SCT_DAEMON
#else
/* SCTs from logs or from admin-created .sct files are only picked up
 * at server start/restart.
 */
#endif

#if !defined(WIN32) && defined(HAVE_SCT_DAEMON)
#include <unistd.h>
#endif

#include "apr_version.h"
#if !APR_VERSION_AT_LEAST(1,5,0)
#error mod_ssl_ct requires APR 1.5.0 or later! (for apr_escape.h stuff)
#endif

#include "apr_escape.h"
#include "apr_global_mutex.h"
#include "apr_signal.h"
#include "apr_strings.h"

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_main.h"
#include "http_protocol.h"
#include "util_mutex.h"
#include "ap_mpm.h"

#include "ssl_hooks.h"
#include "mod_proxy.h"

#include "ssl_ct_util.h"

#include "openssl/x509v3.h"
#include "openssl/ocsp.h"

#if OPENSSL_VERSION_NUMBER < 0x10002001L
#error "mod_ssl_ct requires OpenSSL 1.0.2-beta1 or later"
#endif

#ifdef WIN32
#define DOTEXE ".exe"
#else
#define DOTEXE ""
#endif

#define STATUS_VAR                "SSL_CT_PEER_STATUS"
#define STATUS_VAR_AWARE_VAL      "peer-aware"
#define STATUS_VAR_UNAWARE_VAL    "peer-unaware"

#define PROXY_SCT_SOURCES_VAR     "SSL_PROXY_SCT_SOURCES"

#define DAEMON_NAME         "SCT maintenance daemon"
#define SERVICE_THREAD_NAME "service thread"

/** Limit on size of stored SCTs for a certificate (individual SCTs as well
 * as size of all.
 */
#define MAX_SCTS_SIZE 10000

/** Limit on size of log URL list for a certificate
 */
#define MAX_LOGLIST_SIZE 1000

typedef struct ct_server_config {
    apr_array_header_t *log_urls;
    apr_array_header_t *log_url_strs;
    apr_array_header_t *log_public_keys;
    apr_array_header_t *log_ids;
    apr_array_header_t *cert_sct_dirs;
    const char *sct_storage;
    const char *audit_storage;
    const char *ct_tools_dir;
    const char *ct_exe;
    apr_time_t max_sct_age;
#define PROXY_AWARENESS_UNSET -1
#define PROXY_OBLIVIOUS        1
#define PROXY_AWARE            2 /* default */
#define PROXY_REQUIRE          3
    int proxy_awareness;
} ct_server_config;

typedef struct cert_chain {
    apr_pool_t *p;
    apr_array_header_t *cert_arr; /* array of X509 * */
    X509 *leaf;
} cert_chain;

typedef struct ct_conn_config {
    int peer_ct_aware;
    /* proxy mode only */
    cert_chain *certs;
    int server_cert_has_sct_list;
    void *cert_sct_list;
    apr_size_t cert_sct_list_size;
    int serverhello_has_sct_list;
    void *serverhello_sct_list;
    apr_size_t serverhello_sct_list_size;
    int ocsp_has_sct_list;
    void *ocsp_sct_list;
    apr_size_t ocsp_sct_list_size;
    apr_array_header_t *all_scts; /* array of ct_sct_data */
} ct_conn_config;

typedef struct ct_sct_data {
    const void *data;
    apr_size_t len;
} ct_sct_data;

typedef struct ct_callback_info {
    server_rec *s;
    conn_rec *c;
    ct_conn_config *conncfg;
} ct_callback_info;

typedef struct ct_cached_server_data {
    apr_status_t validation_result;
} ct_cached_server_data;

module AP_MODULE_DECLARE_DATA ssl_ct_module;

#define SSL_CT_MUTEX_TYPE "ssl-ct-sct-update"

static apr_global_mutex_t *ssl_ct_sct_update;

static int refresh_all_scts(server_rec *s_main, apr_pool_t *p);

static apr_thread_t *service_thread;

static apr_hash_t *cached_server_data;

static const char *audit_fn_perm, *audit_fn_active;
static apr_file_t *audit_file;
static int audit_file_nonempty;
static apr_thread_mutex_t *audit_file_mutex;
static apr_thread_mutex_t *cached_server_data_mutex;

#ifdef HAVE_SCT_DAEMON

/* The APR other-child API doesn't tell us how the daemon exited
 * (SIGSEGV vs. exit(1)).  The other-child maintenance function
 * needs to decide whether to restart the daemon after a failure
 * based on whether or not it exited due to a fatal startup error
 * or something that happened at steady-state.  This exit status
 * is unlikely to collide with exit signals.
 */
#define DAEMON_STARTUP_ERROR 254

static int daemon_start(apr_pool_t *p, server_rec *main_server, apr_proc_t *procnew);
static server_rec *root_server = NULL;
static apr_pool_t *root_pool = NULL;
static apr_pool_t *pdaemon = NULL;
static pid_t daemon_pid;
static int daemon_should_exit = 0;

#endif /* HAVE_SCT_DAEMON */

static const char *get_cert_fingerprint(apr_pool_t *p, const X509 *x)
{
    const EVP_MD *digest;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n;
    digest = EVP_get_digestbyname("sha256");
    X509_digest(x, digest, md, &n);

    return apr_pescape_hex(p, md, n, 0);
}

#define LOG_ID_SIZE 32

typedef struct {
    unsigned char version;
    unsigned char logid[LOG_ID_SIZE];
    apr_uint64_t timestamp;
    apr_time_t time;
    char timestr[APR_RFC822_DATE_LEN];
    const unsigned char *extensions;
    apr_uint16_t extlen;
    unsigned char hash_alg;
    unsigned char sig_alg;
    apr_uint16_t siglen;
    const unsigned char *sig;
    const unsigned char *signed_data;
    apr_size_t signed_data_len;
} sct_fields_t;

static apr_status_t verify_signature(sct_fields_t *sctf,
                                     EVP_PKEY *pkey)
{
    EVP_MD_CTX ctx;
    int rc;

    if (sctf->signed_data == NULL) {
        return APR_EINVAL;
    }

    EVP_MD_CTX_init(&ctx);
    ap_assert(1 == EVP_VerifyInit(&ctx, EVP_sha256()));
    ap_assert(1 == EVP_VerifyUpdate(&ctx, sctf->signed_data,
                                    sctf->signed_data_len));
    rc = EVP_VerifyFinal(&ctx, sctf->sig, sctf->siglen, pkey);
    EVP_MD_CTX_cleanup(&ctx);

    return rc == 1 ? APR_SUCCESS : APR_EINVAL;
}

static apr_status_t try_verify_signature(conn_rec *c, sct_fields_t *sctf,
                                         apr_array_header_t *log_public_keys,
                                         apr_array_header_t *log_ids)
{
    apr_status_t rv = APR_EINVAL;
    int i;
    EVP_PKEY **pubkey_elts;
    char **logid_elts;
    int nelts = log_public_keys->nelts;

    ap_assert(log_public_keys->nelts == log_ids->nelts);
    ap_assert(sctf->signed_data != NULL);

    pubkey_elts = (EVP_PKEY **)log_public_keys->elts;
    logid_elts = (char **)log_ids->elts;

    for (i = 0; i < nelts; i++) {
        EVP_PKEY *pubkey = pubkey_elts[i];
        char *logid = logid_elts[i];

        if (!memcmp(logid, sctf->logid, LOG_ID_SIZE)) {
            rv = verify_signature(sctf, pubkey);
            if (rv != APR_SUCCESS) {
                ap_log_cerror(APLOG_MARK, 
                              APLOG_ERR,
                              rv, c,
                              "verify_signature failed");
            }
            else {
                ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                              "verify_signature succeeded");
            }
            return rv;
        }
    }

    return APR_NOTFOUND;
}

static apr_status_t parse_sct(const char *source,
                              server_rec *s, const unsigned char *sct,
                              apr_size_t len, cert_chain *cc,
                              sct_fields_t *fields)
{
    const unsigned char *cur;
    apr_size_t orig_len = len;
    apr_status_t rv;

    memset(fields, 0, sizeof *fields);

    if (len < 1 + LOG_ID_SIZE + 8) {
        /* no room for header */
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "SCT size %" APR_SIZE_T_FMT " is too small",
                     len);
        return APR_EINVAL;
    }

    cur = sct;

    fields->version = *cur;
    cur++;
    len -= 1;
    memcpy(fields->logid, cur, LOG_ID_SIZE);
    cur += LOG_ID_SIZE;
    len -= LOG_ID_SIZE;
    rv = ctutil_deserialize_uint64(&cur, &len, &fields->timestamp);
    ap_assert(rv == APR_SUCCESS);

    fields->time = apr_time_from_msec(fields->timestamp);

    /* XXX maybe do this only if log level is such that we'll
     *     use it later?
     */
    apr_rfc822_date(fields->timestr, fields->time);


    if (len < 2) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "SCT size %" APR_SIZE_T_FMT " has no space for extension "
                     "len", orig_len);
        return APR_EINVAL;
    }

    rv = ctutil_deserialize_uint16(&cur, &len, &fields->extlen);
    ap_assert(rv == APR_SUCCESS);

    if (fields->extlen != 0) {
        if (fields->extlen < len) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                         "SCT size %" APR_SIZE_T_FMT " has no space for "
                         "%hu bytes of extensions",
                         orig_len, fields->extlen);
            return APR_EINVAL;
        }

        fields->extensions = cur;
        cur += fields->extlen;
        len -= fields->extlen;
    }
    else {
        fields->extensions = 0;
    }

    if (len < 4) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "SCT size %" APR_SIZE_T_FMT " has no space for "
                     "hash algorithm, signature algorithm, and signature len",
                     orig_len);
        return APR_EINVAL;
    }

    fields->hash_alg = *cur;
    cur += 1;
    len -= 1;
    fields->sig_alg = *cur;
    cur += 1;
    len -= 1;
    rv = ctutil_deserialize_uint16(&cur, &len, &fields->siglen);
    ap_assert(rv == APR_SUCCESS);

    if (fields->siglen < len) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "SCT has no space for signature");
        return APR_EINVAL;
    }

    fields->sig = cur;
    cur += fields->siglen;
    len -= fields->siglen;

    fields->signed_data = NULL;
    fields->signed_data_len = 0;

    if (cc) {
        /* If we have the server certificate, we can construct the
         * data over which the signature is computed.
         */

        /* XXX Which part is signed? */
        /* See certificate-transparency/src/proto/serializer.cc,
         * method Serializer::SerializeV1CertSCTSignatureInput()
         */

        apr_size_t orig_len = 1000000;
        apr_size_t avail = orig_len;
        unsigned char *mem = malloc(avail);
        unsigned char *orig_mem = mem;

        rv = ctutil_serialize_uint8(&mem, &avail, 0); /* version 1 */
        if (rv == APR_SUCCESS) {
            rv = ctutil_serialize_uint8(&mem, &avail, 0); /* CERTIFICATE_TIMESTAMP */
        }
        if (rv == APR_SUCCESS) {
            rv = ctutil_serialize_uint64(&mem, &avail, fields->timestamp);
        }
        if (rv == APR_SUCCESS) {
            rv = ctutil_serialize_uint16(&mem, &avail, 0); /* X509_ENTRY */
        }
        if (rv == APR_SUCCESS) {
            /* Get DER encoding of leaf certificate */
            unsigned char *der_buf
                /* get OpenSSL to allocate: */
                = NULL;
            int der_length;

            der_length = i2d_X509(cc->leaf, &der_buf);
            if (der_length < 0) {
                rv = APR_EINVAL;
            }
            else {
                rv = ctutil_write_var24_bytes(&mem, &avail,
                                              der_buf, der_length);
                OPENSSL_free(der_buf);
            }
        }
        if (rv == APR_SUCCESS) {
            rv = ctutil_write_var16_bytes(&mem, &avail, fields->extensions,
                                          fields->extlen);
                                          
        }

        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s,
                         "Failed to reconstruct signed data for SCT");
            free(orig_mem);
        }
        else {
            fields->signed_data_len = orig_len - avail;
            fields->signed_data = orig_mem;
            /* Force invalid signature error: orig_mem[0] = orig_mem[0] + 1; */
        }
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                 "SCT from %s: version %d timestamp %s hash alg %d sig alg %d",
                 source, fields->version, fields->timestr,
                 fields->hash_alg, fields->sig_alg);
#if AP_MODULE_MAGIC_AT_LEAST(20130702,2)
    ap_log_data(APLOG_MARK, APLOG_DEBUG, s, "Log Id",
                fields->logid, sizeof(fields->logid),
                AP_LOG_DATA_SHOW_OFFSET);
    ap_log_data(APLOG_MARK, APLOG_DEBUG, s, "Signature",
                fields->sig, fields->siglen,
                AP_LOG_DATA_SHOW_OFFSET);
#endif /* httpd has ap_log_*data() */

    return rv;
}

/* a server's SCT-related storage on disk:
 *
 *   <rootdir>/<fingerprint>/servercerts.pem
 *                  Concatenation of leaf certificate and any
 *                  configured intermediate certificates
 *
 *   <rootdir>/<fingerprint>/logs  
 *                  List of log URLs, one per line
 *
 *   <rootdir>/<fingerprint>/AUTO_hostname_port_uri.sct
 *                  SCT for cert with this fingerprint
 *                  from this log (could be any number
 *                  of these)
 *
 *   <rootdir>/<fingerprint>/<anything>.sct
 *                  SCT maintained by the administrator
 *                  (file is optional; could be any number
 *                  of these; should not start with "AUTO_")
 *
 *   <rootdir>/<fingerprint>/collated
 *                  one or more SCTs ready to send
 *                  (this is all that the web server
 *                  processes care about)
 */

#define SERVERCERTS_BASENAME   "servercerts.pem"
#define COLLATED_SCTS_BASENAME "collated"
#define LOGLIST_BASENAME       "logs"
#define LOG_SCT_PREFIX         "AUTO_" /* to distinguish from admin-created .sct
                                        * files
                                        */

static apr_status_t collate_scts(server_rec *s, apr_pool_t *p,
                                 const char *cert_sct_dir)
{
    /* Read the various .sct files and stick them together in a single file */
    apr_array_header_t *arr;
    apr_status_t rv, tmprv;
    apr_file_t *tmpfile;
    apr_size_t bytes_written;
    apr_uint16_t overall_len = 0;
    char *tmp_collated_fn, *collated_fn;
    const char *cur_sct_file;
    const char * const *elts;
    int i, scts_written = 0;

    rv = ctutil_path_join(&collated_fn, cert_sct_dir, COLLATED_SCTS_BASENAME, p, s);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    /* Note: We rebuild the file that combines the SCTs every time this
     *       code runs, even if no individual SCTs are new (or at least
     *       re-fetched).
     *       That allows the admin to see the last processing by looking
     *       at the timestamp.
     *       Rechecking even if no SCTs are new allows SCTs which were not
     *       yet valid originally (just submitted to a log) to be used as
     *       soon as practical.
     */
    tmp_collated_fn = apr_pstrcat(p, collated_fn, ".tmp", NULL);

    rv = apr_file_open(&tmpfile, tmp_collated_fn,
                       APR_FOPEN_WRITE|APR_FOPEN_CREATE|APR_FOPEN_TRUNCATE
                       |APR_FOPEN_BINARY|APR_FOPEN_BUFFERED,
                       APR_FPROT_OS_DEFAULT, p);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "can't create %s", tmp_collated_fn);
        return rv;
    }

    /* stick a 0 len (for the list) at the start of the file;
     * we'll have to patch that later
     */
    rv = ctutil_file_write_uint16(s, tmpfile, overall_len);
    if (rv != APR_SUCCESS) {
        apr_file_close(tmpfile);
        return rv;
    }

    rv = ctutil_read_dir(p, s, cert_sct_dir, "*.sct", &arr);
    if (rv != APR_SUCCESS) {
        apr_file_close(tmpfile);
        return rv;
    }

    elts = (const char * const *)arr->elts;

    for (i = 0; i < arr->nelts; i++) {
        char *scts;
        apr_size_t scts_size;
        sct_fields_t fields;

        cur_sct_file = elts[i];

        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                     "Adding SCT from file %s", cur_sct_file);

        rv = ctutil_read_file(p, s, cur_sct_file, MAX_SCTS_SIZE, &scts, &scts_size);
        if (rv != APR_SUCCESS) {
            break;
        }

        rv = parse_sct(cur_sct_file,
                       s, (const unsigned char *)scts, scts_size, NULL, &fields);
        if (rv != APR_SUCCESS) {
            break;
        }

        if (fields.time > apr_time_now()) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, s,
                         "SCT in file %s has timestamp in future (%s), skipping",
                         cur_sct_file, fields.timestr);
            continue;
        }

        overall_len += scts_size + 2; /* include size header */

        rv = ctutil_file_write_uint16(s, tmpfile, (apr_uint16_t)scts_size);
        if (rv != APR_SUCCESS) {
            break;
        }

        rv = apr_file_write_full(tmpfile, scts, scts_size, &bytes_written);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                         "can't write %" APR_SIZE_T_FMT " bytes to %s",
                         scts_size, tmp_collated_fn);
            break;
        }

        scts_written++;
    }

    if (rv == APR_SUCCESS) {
        apr_off_t offset = 0;

        rv = apr_file_seek(tmpfile, APR_SET, &offset);
        if (rv == APR_SUCCESS) {
            rv = ctutil_file_write_uint16(s, tmpfile, overall_len);
        }
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                         "could not write the SCT list length at the start of the file");
        }
    }

    tmprv = apr_file_close(tmpfile);
    if (tmprv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, tmprv, s,
                     "error flushing and closing %s", tmp_collated_fn);
        if (rv == APR_SUCCESS) {
            rv = tmprv;
        }
    }

    if (rv == APR_SUCCESS && scts_written) {
        int replacing = ctutil_file_exists(p, collated_fn);

        if (replacing) {
            if ((rv = apr_global_mutex_lock(ssl_ct_sct_update)) != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                             "global mutex lock failed");
                return rv;
            }
            apr_file_remove(collated_fn, p);
        }
        rv = apr_file_rename(tmp_collated_fn, collated_fn, p);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s,
                         "couldn't rename %s to %s, no SCTs to send for now",
                         tmp_collated_fn, collated_fn);
        }
        if (replacing) {
            if ((tmprv = apr_global_mutex_unlock(ssl_ct_sct_update)) != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_ERR, tmprv, s,
                             "global mutex unlock failed");
                if (rv == APR_SUCCESS) {
                    rv = tmprv;
                }
            }
        }
    }

    return rv;
}

static const char *url_to_fn(apr_pool_t *p, const apr_uri_t *log_url)
{
    char *fn = apr_psprintf(p, LOG_SCT_PREFIX "%s_%s_%s.sct",
                            log_url->hostname, log_url->port_str, log_url->path);
    char *ch;

    ch = fn;
    while (*ch) {
        switch(*ch) {
        /* chars that shouldn't be used in a filename */
        case ':':
        case '/':
        case '\\':
        case '*':
        case '?':
        case '<':
        case '>':
        case '"':
        case '|':
            *ch = '-';
        }
        ++ch;
    }
    return fn;
}

static apr_status_t submission(server_rec *s, apr_pool_t *p, const char *ct_exe,
                               const apr_uri_t *log_url, const char *cert_file,
                               const char *sct_fn)
{
    apr_status_t rv;
    const char *args[8];
    int i;

    i = 0;
    args[i++] = ct_exe;
    args[i++] = apr_pstrcat(p, "--ct_server=", log_url->hostinfo, NULL);
    args[i++] = "--http_log";
    args[i++] = "--logtostderr";
    args[i++] = apr_pstrcat(p, "--ct_server_submission=", cert_file, NULL);
    args[i++] = apr_pstrcat(p, "--ct_server_response_out=", sct_fn, NULL);
    args[i++] = "upload";
    args[i++] = NULL;
    ap_assert(i == sizeof args / sizeof args[0]);

    rv = ctutil_run_to_log(p, s, args, "log client");

    return rv;
}

static apr_status_t fetch_sct(server_rec *s, apr_pool_t *p,
                              const char *cert_file,
                              const char *cert_sct_dir,
                              const apr_uri_t *log_url,
                              const char *ct_exe, apr_time_t max_sct_age)
{
    apr_status_t rv;
    char *sct_fn;
    apr_finfo_t finfo;
    const char *log_url_basename;

    log_url_basename = url_to_fn(p, log_url);

    rv = ctutil_path_join(&sct_fn, cert_sct_dir, log_url_basename, p, s);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    rv = apr_stat(&finfo, sct_fn, APR_FINFO_MTIME, p);
    if (rv == APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                     "Found SCT for %s in %s",
                     cert_file, sct_fn);

        if (finfo.mtime + max_sct_age < apr_time_now()) {
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                         "SCT for %s is older than %d seconds, must refresh",
                         cert_file,
                         (int)(apr_time_sec(max_sct_age)));
        }
        else {
            return APR_SUCCESS;
        }
    }
    else {
        ap_log_error(APLOG_MARK, APLOG_INFO,
                     /* no need to print error string for file-not-found err */
                     APR_STATUS_IS_ENOENT(rv) ? 0 : rv,
                     s,
                     "Did not find SCT for %s in %s, must fetch",
                     cert_file, sct_fn);
    }

    rv = submission(s, p, ct_exe, log_url, cert_file, sct_fn);

    return rv;
}

static apr_status_t record_log_urls(server_rec *s, apr_pool_t *p,
                                    const char *listfile, apr_array_header_t *log_urls)
{
    apr_file_t *f;
    apr_status_t rv, tmprv;
    apr_uri_t *log_elts;
    int i;

    rv = apr_file_open(&f, listfile,
                       APR_FOPEN_WRITE|APR_FOPEN_CREATE|APR_FOPEN_TRUNCATE
                       |APR_FOPEN_BUFFERED,
                       APR_FPROT_OS_DEFAULT, p);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "can't create %s", listfile);
        return rv;
    }

    log_elts  = (apr_uri_t *)log_urls->elts;

    for (i = 0; i < log_urls->nelts; i++) {
        rv = apr_file_puts(apr_uri_unparse(p, &log_elts[i], 0), f);
        if (rv == APR_SUCCESS) {
            rv = apr_file_puts("\n", f);
        }
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                         "error writing to %s", listfile);
            break;
        }
    }

    tmprv = apr_file_close(f);
    if (tmprv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, tmprv, s,
                     "error flushing and closing %s", listfile);
        if (rv == APR_SUCCESS) {
            rv = tmprv;
        }
    }

    return rv;
}

static apr_status_t update_log_list_for_cert(server_rec *s, apr_pool_t *p,
                                             const char *cert_sct_dir,
                                             apr_array_header_t *log_urls,
                                             apr_array_header_t *log_url_strs)
{
    apr_array_header_t *old_urls;
    apr_size_t contents_size;
    apr_status_t rv;
    char *contents, *listfile;

    /* The set of logs can change, and we need to remove SCTs retrieved
     * from logs that we no longer trust.  To track changes we'll use a
     * file in the directory for the server certificate.
     *
     * (When can the set change?  Right now they can only change at [re]start,
     * but in the future we should be able to find the set of trusted logs
     * dynamically.)
     */

    rv = ctutil_path_join(&listfile, cert_sct_dir, LOGLIST_BASENAME, p, s);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    if (ctutil_file_exists(p, listfile)) {
        char **elts;
        int i;

        rv = ctutil_read_file(p, s, listfile, MAX_LOGLIST_SIZE, &contents, &contents_size);
        if (rv != APR_SUCCESS) {
            return rv;
        }

        ctutil_buffer_to_array(p, contents, contents_size, &old_urls);

        elts = (char **)old_urls->elts;
        for (i = 0; i < old_urls->nelts; i++) {
            if (!ctutil_in_array(elts[i], log_url_strs)) {
                char *sct_for_log;
                int exists;
                apr_uri_t uri;

                rv = apr_uri_parse(p, elts[i], &uri);
                if (rv != APR_SUCCESS) {
                    ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s,
                                 "unparseable log URL %s in file %s - ignoring",
                                 elts[i], listfile);
                    /* some garbage in the file? can't map to an auto-maintained SCT,
                     * so just skip it
                     */
                    continue;
                }

                rv = ctutil_path_join(&sct_for_log, cert_sct_dir, url_to_fn(p, &uri), p, s);
                ap_assert(rv == APR_SUCCESS);
                exists = ctutil_file_exists(p, sct_for_log);

                ap_log_error(APLOG_MARK, 
                             exists ? APLOG_NOTICE : APLOG_DEBUG, 0, s,
                             "Log %s is no longer enabled%s",
                             elts[i],
                             exists ? ", removing SCT" : ", no SCT was present");

                if (exists) {
                    rv = apr_file_remove(sct_for_log, p);
                    if (rv != APR_SUCCESS) {
                        ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s,
                                     "can't remove SCT %s from previously trusted log %s",
                                     sct_for_log, elts[i]);
                        return rv;
                    }
                }
            }
        }
    }
    else {
        /* can't tell what was trusted before; just remove everything
         * that was created automatically
         */
        apr_array_header_t *arr;
        const char * const *elts;
        int i;

        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, s,
                     "List of previous logs doesn't exist (%s), removing previously obtained SCTs",
                     listfile);

        rv = ctutil_read_dir(p, s, cert_sct_dir, LOG_SCT_PREFIX "*.sct", &arr);
        if (rv != APR_SUCCESS) {
            return rv;
        }

        elts = (const char * const *)arr->elts;
        for (i = 0; i < arr->nelts; i++) {
            const char *cur_sct_file = elts[i];

            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                         "Removing %s", cur_sct_file);

            rv = apr_file_remove(cur_sct_file, p);
            if (rv != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s,
                             "can't remove %s", cur_sct_file);
            }
        }
    }

    if (rv == APR_SUCCESS) {
        rv = record_log_urls(s, p, listfile, log_urls);
    }

    return rv;
}

static apr_status_t refresh_scts_for_cert(server_rec *s, apr_pool_t *p,
                                          const char *cert_sct_dir,
                                          apr_array_header_t *log_urls,
                                          apr_array_header_t *log_url_strs,
                                          const char *ct_exe,
                                          apr_time_t max_sct_age)
{
    apr_status_t rv;
    apr_uri_t *log_elts;
    char *cert_fn;
    int i;

    rv = ctutil_path_join(&cert_fn, cert_sct_dir, SERVERCERTS_BASENAME, p, s);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    log_elts  = (apr_uri_t *)log_urls->elts;

    rv = update_log_list_for_cert(s, p, cert_sct_dir, log_urls, log_url_strs);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    for (i = 0; i < log_urls->nelts; i++) {
        rv = fetch_sct(s, p, cert_fn,
                       cert_sct_dir,
                       &log_elts[i],
                       ct_exe,
                       max_sct_age);
        if (rv != APR_SUCCESS) {
            return rv;
        }
    }

    rv = collate_scts(s, p, cert_sct_dir);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    return rv;
}

static void *run_service_thread(apr_thread_t *me, void *data)
{
    server_rec *s = data;
    int mpmq_s;
    apr_status_t rv;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                 SERVICE_THREAD_NAME " started");

    while (1) {
        if ((rv = ap_mpm_query(AP_MPMQ_MPM_STATE, &mpmq_s)) != APR_SUCCESS) {
            break;
        }
        if (mpmq_s == AP_MPMQ_STOPPING) {
            break;
        }
        apr_sleep(apr_time_from_sec(1));
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, rv, s,
                 SERVICE_THREAD_NAME " exiting");

    return NULL;
}

static apr_status_t wait_for_service_thread(void *data)
{
    apr_thread_t *thd = data;
    apr_status_t retval;

    apr_thread_join(&retval, thd);
    return APR_SUCCESS;
}

#ifdef HAVE_SCT_DAEMON

static void daemon_signal_handler(int sig)
{
    if (sig == SIGHUP) {
        ++daemon_should_exit;
    }
}

#if APR_HAS_OTHER_CHILD
static void daemon_maint(int reason, void *data, apr_wait_t status)
{
    apr_proc_t *proc = data;
    int mpm_state;
    int stopping;

    switch (reason) {
        case APR_OC_REASON_DEATH:
            apr_proc_other_child_unregister(data);
            /* If apache is not terminating or restarting,
             * restart the daemon
             */
            stopping = 1; /* if MPM doesn't support query,
                           * assume we shouldn't restart daemon
                           */
            if (ap_mpm_query(AP_MPMQ_MPM_STATE, &mpm_state) == APR_SUCCESS &&
                mpm_state != AP_MPMQ_STOPPING) {
                stopping = 0;
            }
            if (!stopping) {
                if (status == DAEMON_STARTUP_ERROR) {
                    ap_log_error(APLOG_MARK, APLOG_CRIT, 0, ap_server_conf, APLOGNO(01238)
                                 DAEMON_NAME " failed to initialize");
                }
                else {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf, APLOGNO(01239)
                                 DAEMON_NAME " process died, restarting");
                    daemon_start(root_pool, root_server, proc);
                }
            }
            break;
        case APR_OC_REASON_RESTART:
            /* don't do anything; server is stopping or restarting */
            apr_proc_other_child_unregister(data);
            break;
        case APR_OC_REASON_LOST:
            /* Restart the child cgid daemon process */
            apr_proc_other_child_unregister(data);
            daemon_start(root_pool, root_server, proc);
            break;
        case APR_OC_REASON_UNREGISTER:
            /* we get here when pcgi is cleaned up; pcgi gets cleaned
             * up when pconf gets cleaned up
             */
            kill(proc->pid, SIGHUP); /* send signal to daemon telling it to die */
            break;
    }
}
#endif

static int sct_daemon(server_rec *s_main)
{
    apr_status_t rv;
    apr_pool_t *ptemp;

    /* Ignoring SIGCHLD results in errno ECHILD returned from apr_proc_wait().
     * apr_signal(SIGCHLD, SIG_IGN);
     */
    apr_signal(SIGHUP, daemon_signal_handler);

    rv = apr_global_mutex_child_init(&ssl_ct_sct_update,
                                     apr_global_mutex_lockfile(ssl_ct_sct_update), pdaemon);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, rv, root_server,
                     "could not initialize " SSL_CT_MUTEX_TYPE
                     " mutex in " DAEMON_NAME);
        return DAEMON_STARTUP_ERROR;
    }

    /* ptemp - temporary pool for refresh cycles */
    apr_pool_create(&ptemp, pdaemon);

    while (!daemon_should_exit) {
        apr_sleep(apr_time_from_sec(30)); /* SIGHUP at restart/stop will break out */

        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s_main,
                     DAEMON_NAME " - refreshing SCTs as needed");
        rv = refresh_all_scts(s_main, ptemp);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_ERR, rv, s_main,
                         DAEMON_NAME " - SCT refresh failed; will try again later");
        }
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s_main,
                 DAEMON_NAME " - exiting");

    return 0;
}

static int daemon_start(apr_pool_t *p, server_rec *main_server,
                        apr_proc_t *procnew)
{
    daemon_should_exit = 0; /* clear setting from previous generation */
    if ((daemon_pid = fork()) < 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, errno, main_server,
                     "Couldn't create " DAEMON_NAME " process");
        return DECLINED;
    }
    else if (daemon_pid == 0) {
        if (pdaemon == NULL) {
            apr_pool_create(&pdaemon, p);
        }
        exit(sct_daemon(main_server) > 0 ? DAEMON_STARTUP_ERROR : -1);
    }
    procnew->pid = daemon_pid;
    procnew->err = procnew->in = procnew->out = NULL;
    apr_pool_note_subprocess(p, procnew, APR_KILL_AFTER_TIMEOUT);
#if APR_HAS_OTHER_CHILD
    apr_proc_other_child_register(procnew, daemon_maint, procnew, NULL, p);
#endif
    return OK;
}

#endif /* HAVE_SCT_DAEMON */

static apr_status_t ssl_ct_mutex_remove(void *data)
{
    apr_global_mutex_destroy(ssl_ct_sct_update);
    ssl_ct_sct_update = NULL;
    return APR_SUCCESS;
}

static int refresh_all_scts(server_rec *s_main, apr_pool_t *p)
{
    apr_hash_t *already_processed;
    apr_status_t rv;
    server_rec *s;

    already_processed = apr_hash_make(p);

    s = s_main;
    while (s) {
        ct_server_config *sconf = ap_get_module_config(s->module_config,
                                                       &ssl_ct_module);
        int i;
        const char **cert_sct_dirs_elts;
 
        if (sconf && sconf->cert_sct_dirs) {
            cert_sct_dirs_elts = (const char **)sconf->cert_sct_dirs->elts;
            for (i = 0; i < sconf->cert_sct_dirs->nelts; i++) {
                /* we may have already processed this cert for another
                 * server_rec
                 */
                if (!apr_hash_get(already_processed, cert_sct_dirs_elts[i],
                                  APR_HASH_KEY_STRING)) {
                    apr_hash_set(already_processed, cert_sct_dirs_elts[i],
                                 APR_HASH_KEY_STRING, "done");
                    rv = refresh_scts_for_cert(s_main, p, cert_sct_dirs_elts[i],
                                               sconf->log_urls,
                                               sconf->log_url_strs,
                                               sconf->ct_exe,
                                               sconf->max_sct_age);
                    if (rv != APR_SUCCESS) {
                        return rv;
                    }
                }
            }
        }

        s = s->next;
    }

    return rv;
}

static int ssl_ct_post_config(apr_pool_t *pconf, apr_pool_t *plog,
                              apr_pool_t *ptemp, server_rec *s_main)
{
    apr_status_t rv;

#ifdef HAVE_SCT_DAEMON
    apr_proc_t *procnew = NULL;
    const char *userdata_key = "sct_daemon_init";

    root_server = s_main;
    root_pool = pconf;

    procnew = ap_retained_data_get(userdata_key);
    if (!procnew) {
        procnew = ap_retained_data_create(userdata_key, sizeof(*procnew));
        procnew->pid = -1;
        procnew->err = procnew->in = procnew->out = NULL;
    }
#endif /* HAVE_SCT_DAEMON */

    rv = ap_global_mutex_create(&ssl_ct_sct_update, NULL,
                                SSL_CT_MUTEX_TYPE, NULL, s_main, pconf, 0);
    if (rv != APR_SUCCESS) {
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    apr_pool_cleanup_register(pconf, (void *)s_main, ssl_ct_mutex_remove,
                              apr_pool_cleanup_null);

    /* Ensure that we already have, or can fetch, fresh SCTs for each 
     * certificate.  If so, start the daemon to maintain these and let
     * startup continue.  (Otherwise abort startup.)
     */

    rv = refresh_all_scts(s_main, pconf);
    if (rv != APR_SUCCESS) {
        return HTTP_INTERNAL_SERVER_ERROR;
    }

#ifdef HAVE_SCT_DAEMON
    if (ap_state_query(AP_SQ_MAIN_STATE) != AP_SQ_MS_CREATE_PRE_CONFIG) {
        int ret = daemon_start(pconf, s_main, procnew);
        if (ret != OK) {
            return ret;
        }
    }
#endif /* HAVE_SCT_DAEMON */

    return OK;
}

static int ssl_ct_check_config(apr_pool_t *pconf, apr_pool_t *plog,
                              apr_pool_t *ptemp, server_rec *s_main)
{
    ct_server_config *sconf = ap_get_module_config(s_main->module_config,
                                                   &ssl_ct_module);

    if (!sconf->sct_storage) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s_main,
                     "Directive CTSCTStorage is required");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if (!sconf->audit_storage) {
        /* umm, hard to tell if needed...  must have server with
         * SSL proxy enabled and server-specific-sconf->proxy_awareness
         * != PROXY_OBLIVIOUS...
         */
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s_main,
                     "Directive CTAuditStorage is required");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if (!sconf->ct_tools_dir) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s_main,
                     "Directive CTToolsDir is required");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    return OK;
}

static apr_status_t read_scts(apr_pool_t *p, const char *fingerprint,
                              const char *sct_dir,
                              server_rec *s,
                              char **scts, apr_size_t *scts_len)
{
    apr_status_t rv, tmprv;
    char *cert_dir, *sct_fn;

    rv = ctutil_path_join(&cert_dir, sct_dir, fingerprint, p, s);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    rv = ctutil_path_join(&sct_fn, cert_dir, COLLATED_SCTS_BASENAME, p, s);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    if ((rv = apr_global_mutex_lock(ssl_ct_sct_update)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "global mutex lock failed");
        return rv;
    }

    rv = ctutil_read_file(p, s, sct_fn, MAX_SCTS_SIZE, scts, scts_len);

    if ((tmprv = apr_global_mutex_unlock(ssl_ct_sct_update)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, tmprv, s,
                     "global mutex unlock failed");
    }

    return rv;
}

static void look_for_server_certs(server_rec *s, SSL_CTX *ctx, const char *sct_dir)
{
    ct_server_config *sconf = ap_get_module_config(s->module_config,
                                                   &ssl_ct_module);
    apr_pool_t *p = s->process->pool;
    apr_status_t rv;
    FILE *concat;
    X509 *x;
    STACK_OF(X509) *chain;
    int i, rc;
    char *cert_sct_dir, *servercerts_pem;
    const char *fingerprint;

    sconf->cert_sct_dirs = apr_array_make(p, 2, sizeof(char *));

    rc = SSL_CTX_set_current_cert(ctx, SSL_CERT_SET_FIRST);
    while (rc) {
        x = SSL_CTX_get0_certificate(ctx);
        if (x) {
            fingerprint = get_cert_fingerprint(s->process->pool, x);
            rv = ctutil_path_join(&cert_sct_dir, sct_dir, fingerprint, p, s);
            ap_assert(rv == APR_SUCCESS);

            if (!ctutil_dir_exists(p, cert_sct_dir)) {
                rv = apr_dir_make(cert_sct_dir, APR_FPROT_OS_DEFAULT, p);
                if (rv != APR_SUCCESS) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                                 "can't create directory %s",
                                 cert_sct_dir);
                    ap_assert(rv == APR_SUCCESS);
                }
            }

            rv = ctutil_path_join(&servercerts_pem, cert_sct_dir,
                                  SERVERCERTS_BASENAME, p, s);
            ap_assert(rv == APR_SUCCESS);

            concat = fopen(servercerts_pem, "wb");
            ap_assert(concat);

            ap_assert(1 == PEM_write_X509(concat, x)); /* leaf */

            chain = NULL;

            /* Not this: SSL_CTX_get0_chain_certs(ctx, &chain);
             *
             * See this thread:
             *   http://mail-archives.apache.org/mod_mbox/httpd-dev/
             *   201402.mbox/%3CCAKUrXK5-2_Sg8FokxBP8nW7tmSuTZZWL-%3
             *   DBDhNnwyK-Z4dmQiQ%40mail.gmail.com%3E
             */
            SSL_CTX_get_extra_chain_certs(ctx, &chain);

            if (chain) {
                for (i = 0; i < sk_X509_num(chain); i++) {
                    X509 *x = sk_X509_value(chain, i);
                    ap_assert(1 == PEM_write_X509(concat, x));
                }
            }
            ap_assert(0 == fclose(concat));

            ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                         "wrote server cert and chain to %s", servercerts_pem);

            *(char **)apr_array_push(sconf->cert_sct_dirs)
                = cert_sct_dir;
        }
        else {
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, s,
                         "could not find leaf certificate");
        }
        rc = SSL_CTX_set_current_cert(ctx, SSL_CERT_SET_NEXT);
    }
}

static int ssl_ct_ssl_server_init(server_rec *s, SSL_CTX *ctx)
{
    ct_server_config *sconf = ap_get_module_config(s->module_config,
                                                   &ssl_ct_module);

    look_for_server_certs(s, ctx, sconf->sct_storage);

    return OK;
}

static ct_conn_config *get_conn_config(conn_rec *c)
{
    ct_conn_config *conncfg =
      ap_get_module_config(c->conn_config, &ssl_ct_module);

    if (!conncfg) {
        conncfg = apr_pcalloc(c->pool, sizeof *conncfg);
        ap_set_module_config(c->conn_config, &ssl_ct_module, conncfg);
    }

    return conncfg;
}

static void client_is_ct_aware(conn_rec *c)
{
    ct_conn_config *conncfg = get_conn_config(c);
    conncfg->peer_ct_aware = 1;
}

static int is_client_ct_aware(conn_rec *c)
{
    ct_conn_config *conncfg = get_conn_config(c);

    return conncfg->peer_ct_aware;
}

static void server_cert_has_sct_list(conn_rec *c)
{
    ct_conn_config *conncfg = get_conn_config(c);
    conncfg->server_cert_has_sct_list = 1;
    conncfg->peer_ct_aware = 1;
}

/* Look at SSLClient::VerifyCallback() and WriteSSLClientCTData()
 * for validation and saving of data for auditing in a form that
 * the c-t tools can use.
 */

static cert_chain *cert_chain_init(apr_pool_t *p, STACK_OF(X509) *chain)
{
    cert_chain *cc = apr_pcalloc(p, sizeof(cert_chain));
    int i;

    cc->cert_arr = apr_array_make(p, 4, sizeof(X509 *));

    for (i = 0; i < sk_X509_num(chain); i++) {
        X509 **spot = apr_array_push(cc->cert_arr);
        *spot = X509_dup(sk_X509_value(chain, i));
        if (i == 0) {
            cc->leaf = *spot;
        }
    }

    return cc;
}

static void cert_chain_free(cert_chain *cc) {
    X509 **elts = (X509 **)cc->cert_arr->elts;
    int i;

    for (i = 0; i < cc->cert_arr->nelts; i++) {
        X509_free(elts[i]);
    }
}

/* Create hash of leaf certificate and any SCTs so that
 * we can determine whether or not we've seen this exact
 * info from the server before.
 */
static const char *gen_key(conn_rec *c, cert_chain *cc,
                           ct_conn_config *conncfg)
{
    const char *fp;
    SHA256_CTX sha256ctx;
    unsigned char digest[SHA256_DIGEST_LENGTH];

    fp = get_cert_fingerprint(c->pool, cc->leaf);

    SHA256_Init(&sha256ctx);
    SHA256_Update(&sha256ctx, (unsigned char *)fp, strlen(fp));
    if (conncfg->cert_sct_list) {
        SHA256_Update(&sha256ctx, conncfg->cert_sct_list, 
                      conncfg->cert_sct_list_size);
    }
    if (conncfg->serverhello_sct_list) {
        SHA256_Update(&sha256ctx, conncfg->serverhello_sct_list,
                      conncfg->serverhello_sct_list_size);
    }
    if (conncfg->ocsp_sct_list) {
        SHA256_Update(&sha256ctx, conncfg->ocsp_sct_list,
                      conncfg->ocsp_sct_list_size);
    }
    SHA256_Final(digest, &sha256ctx);
    return apr_pescape_hex(c->pool, digest, sizeof digest, 0);
}

static apr_status_t deserialize_SCTs(apr_pool_t *p,
                                     ct_conn_config *conncfg,
                                     void *sct_list,
                                     apr_size_t sct_list_size)
{
    apr_size_t avail, len_of_data;
    apr_status_t rv;
    const unsigned char *mem, *start_of_data;

    mem = sct_list;
    avail = sct_list_size;

    /* Make sure the overall length is correct */

    rv = ctutil_read_var_bytes((const unsigned char **)&mem,
                               &avail, &start_of_data, &len_of_data);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    if (len_of_data + sizeof(apr_uint16_t) != sct_list_size) {
        return APR_EINVAL;
    }

    /* add each SCT in the list to the all_scts array */

    mem = (unsigned char *)sct_list + sizeof(apr_uint16_t);
    avail = sct_list_size - sizeof(apr_uint16_t);

    while (rv == APR_SUCCESS && avail > 0) {
        rv = ctutil_read_var_bytes((const unsigned char **)&mem, &avail, 
                                   &start_of_data, &len_of_data);
        if (rv == APR_SUCCESS) {
            ct_sct_data *sct = (ct_sct_data *)apr_array_push(conncfg->all_scts);

            sct->data = start_of_data;
            sct->len = len_of_data;
        }
    }

    if (rv == APR_SUCCESS && avail != 0) {
        return APR_EINVAL;
    }

    return APR_SUCCESS;
}

/* perform quick sanity check of server SCT(s) during handshake;
 * errors should result in fatal alert
 */
static apr_status_t validate_server_data(apr_pool_t *p, conn_rec *c,
                                         cert_chain *cc, ct_conn_config *conncfg,
                                         ct_server_config *sconf)
{
    apr_status_t rv = APR_SUCCESS;

#if AP_MODULE_MAGIC_AT_LEAST(20130702,2)
    if (conncfg->serverhello_sct_list) {
        ap_log_cdata(APLOG_MARK, APLOG_TRACE6, c, "SCT(s) from ServerHello",
                     conncfg->serverhello_sct_list,
                     conncfg->serverhello_sct_list_size,
                     AP_LOG_DATA_SHOW_OFFSET);
    }

    if (conncfg->cert_sct_list) {
        ap_log_cdata(APLOG_MARK, APLOG_TRACE6, c, "SCT(s) from certificate",
                     conncfg->cert_sct_list,
                     conncfg->cert_sct_list_size,
                     AP_LOG_DATA_SHOW_OFFSET);
    }

    if (conncfg->ocsp_sct_list) {
        ap_log_cdata(APLOG_MARK, APLOG_TRACE6, c, "SCT(s) from stapled OCSP response",
                     conncfg->ocsp_sct_list,
                     conncfg->ocsp_sct_list_size,
                     AP_LOG_DATA_SHOW_OFFSET);
    }
#endif /* httpd has ap_log_*data() */

    if (!conncfg->all_scts) {
        conncfg->all_scts = apr_array_make(p, 4, sizeof(ct_sct_data));
    }

    /* deserialize all the SCTs */
    if (conncfg->cert_sct_list) {
        rv = deserialize_SCTs(p, conncfg, conncfg->cert_sct_list,
                              conncfg->cert_sct_list_size);
        if (rv != APR_SUCCESS) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, rv, c,
                          "couldn't deserialize SCT list from certificate");
        }
    }
    if (rv == APR_SUCCESS && conncfg->serverhello_sct_list) {
        rv = deserialize_SCTs(p, conncfg, conncfg->serverhello_sct_list,
                              conncfg->serverhello_sct_list_size);
        if (rv != APR_SUCCESS) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, rv, c,
                          "couldn't deserialize SCT list from ServerHello");
        }
    }
    if (rv == APR_SUCCESS && conncfg->ocsp_sct_list) {
        rv = deserialize_SCTs(p, conncfg, conncfg->ocsp_sct_list,
                              conncfg->ocsp_sct_list_size);
        if (rv != APR_SUCCESS) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, rv, c,
                          "couldn't deserialize SCT list from stapled OCSP response");
        }
    }

    if (rv == APR_SUCCESS) {
        if (conncfg->all_scts->nelts < 1) {
            /* How did we get here without at least one SCT? */
            ap_log_cerror(APLOG_MARK, APLOG_CRIT, 0, c,
                          "SNAFU: No deserialized SCTs found in validate_server_data()");
            rv = APR_EINVAL;
        }
        else {
            apr_status_t tmprv;
            int i, verification_failures, verification_successes, unknown_log_ids;
            ct_sct_data *sct_elts;
            ct_sct_data sct;
            sct_fields_t fields;

            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                          "%d SCTs received total", conncfg->all_scts->nelts);

            verification_failures = verification_successes = unknown_log_ids = 0;
            sct_elts = (ct_sct_data *)conncfg->all_scts->elts;
            for (i = 0; i < conncfg->all_scts->nelts; i++) {
                sct = sct_elts[i];
                tmprv = parse_sct("backend server", c->base_server, 
                                  sct.data, sct.len, cc,
                                  &fields);
                if (tmprv != APR_SUCCESS) {
                    rv = tmprv;
                }
                else {
                    if (fields.time > apr_time_now()) {
                        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, c,
                                      "Server sent SCT not yet valid (timestamp %s)",
                                      fields.timestr);
                        verification_failures++;
                    }

                    if (sconf->log_public_keys) {
                        tmprv = try_verify_signature(c, &fields,
                                                     sconf->log_public_keys,
                                                     sconf->log_ids);
                        if (tmprv == APR_NOTFOUND) {
                            ap_log_cerror(APLOG_MARK, APLOG_WARNING, 0, c,
                                          "Server sent SCT from unrecognized log");
                            unknown_log_ids++;
                        }
                        else if (tmprv != APR_SUCCESS) {
                            ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, c,
                                          "Server sent SCT with invalid signature");
                            tmprv = APR_EINVAL;
                            verification_failures++;
                        }
                        else {
                            verification_successes++;
                        }
                    }
                    else {
                        unknown_log_ids++;
                        ap_log_cerror(APLOG_MARK, APLOG_WARNING, 0, c,
                                      "Signature of SCT from server could not be "
                                      "verified (no configured log public keys)");
                    }
                }
            }
            if (verification_failures && !verification_successes) {
                /* If no SCTs are valid, don't communicate. */
                rv = APR_EINVAL;
            }
            ap_log_cerror(APLOG_MARK,
                          rv != APR_SUCCESS ? APLOG_ERR : APLOG_INFO, 0, c,
                          "Signature/timestamp validation for %d SCTs: %d successes, "
                          "%d failures, %d from unknown logs",
                          conncfg->all_scts->nelts, verification_successes,
                          verification_failures, unknown_log_ids);
        }
    }

    return rv;
}

/* Enqueue data from server for off-line audit (cert, SCT(s))
 * We already filtered out duplicate data being saved from this
 * process.  (With reverse proxy it will be the same data over
 * and over.)
 */
#define SERVER_START 0x0001
#define CERT_START   0x0002
#define SCT_START    0x0003

static void save_server_data(conn_rec *c, cert_chain *cc,
                             ct_conn_config *conncfg)
{
    if (audit_file_mutex && audit_file) { /* child init successful, no
                                           * subsequent error
                                           */
        apr_size_t bytes_written;
        apr_status_t rv;
        int i;
        ct_sct_data *sct_elts;
        X509 **x509elts;
        server_rec *s = c->base_server;

        /* Any error in this function is a file I/O error;
         * if such an error occurs, the audit file will be closed
         * and removed, and this child won't be able to queue
         * anything for audit.  (It is likely that other child
         * processes will have the same problem.)
         */

        ctutil_thread_mutex_lock(audit_file_mutex);

        if (audit_file) { /* no error just occurred... */
            audit_file_nonempty = 1;

            rv = ctutil_file_write_uint16(s, audit_file,
                                          SERVER_START);

            /* Write each certificate, starting with leaf */
            x509elts = (X509 **)cc->cert_arr->elts;
            for (i = 0; rv == APR_SUCCESS && i < cc->cert_arr->nelts; i++) {
                unsigned char *der_buf = NULL;
                int der_length;

                rv = ctutil_file_write_uint16(s, audit_file, CERT_START);

                /* now write the cert!!! */

                if (rv == APR_SUCCESS) {
                    der_length = i2d_X509(x509elts[i], &der_buf);
                    ap_assert(der_length > 0);

                    rv = ctutil_file_write_uint16(s, audit_file, der_length);
                }

                if (rv == APR_SUCCESS) {
                    rv = apr_file_write_full(audit_file, der_buf, der_length,
                                             &bytes_written);
                }

                OPENSSL_free(der_buf);
            }

            /* Write each SCT */
            sct_elts = (ct_sct_data *)conncfg->all_scts->elts;
            for (i = 0; rv == APR_SUCCESS && i < conncfg->all_scts->nelts; i++) {
                ct_sct_data sct;

                rv = ctutil_file_write_uint16(s, audit_file, SCT_START);

                /* now write the SCT!!! */
                sct = sct_elts[i];

                if (rv == APR_SUCCESS) {
                    rv = ctutil_file_write_uint16(s, audit_file, sct.len);
                }

                if (rv == APR_SUCCESS) {
                    rv = apr_file_write_full(audit_file, sct.data, sct.len,
                                             &bytes_written);
                }
            }

            if (rv != APR_SUCCESS) {
                /* an I/O error occurred; file is not usable */
                ap_log_error(APLOG_MARK, APLOG_CRIT, rv, ap_server_conf,
                             "Failed to write to %s, disabling audit for this "
                             "child", audit_fn_active);
                apr_file_close(audit_file);
                audit_file = NULL;
                apr_file_remove(audit_fn_active,
                                /* not used in current implementations */
                                c->pool);
            }
        }

        ctutil_thread_mutex_unlock(audit_file_mutex);
    }
}

/* signed_certificate_timestamp */
static const unsigned short CT_EXTENSION_TYPE = 18;

/* See function of this name in openssl/apps/s_client.c */
static int ocsp_resp_cb(SSL *ssl, void *arg)
{
    conn_rec *c = (conn_rec *)SSL_get_app_data(ssl);
    ct_conn_config *conncfg = get_conn_config(c);
    const unsigned char *p;
    int i, len;
    OCSP_RESPONSE *rsp;
    OCSP_BASICRESP *br;
    OCSP_RESPDATA *rd;
    OCSP_SINGLERESP *single;
    STACK_OF(X509_EXTENSION) *exts;

    len = SSL_get_tlsext_status_ocsp_resp(ssl, &p);
    if (!p) {
        /* normal case */
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                      "OCSP response callback called but no stapled response from server");
        return 1;
    }

    rsp = d2i_OCSP_RESPONSE(NULL, &p, len);
    if (!rsp) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, c,
                      "Error parsing OCSP response");
        return 0;
    }

    br = OCSP_response_get1_basic(rsp);
    if (!br) {
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                      "no OCSP basic response");
        return 0;
    }

    rd = br->tbsResponseData;

    for (i = 0; i < sk_OCSP_SINGLERESP_num(rd->responses); i++) {
        X509_EXTENSION *ext;
        int idx;
        ASN1_OCTET_STRING *oct;

        single = sk_OCSP_SINGLERESP_value(rd->responses, i);
        if (!single) {
            continue;
        }

        idx = OCSP_SINGLERESP_get_ext_by_NID(single,
                                             NID_ct_cert_scts, -1);

        if (idx == -1) {
            continue;
        }

        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                      "index of NID_ct_cert_scts: %d", idx);

        exts = single->singleExtensions;

        ext = sk_X509_EXTENSION_value(exts, idx);
        oct = X509_EXTENSION_get_data(ext);

        /* X509V3_EXT_print(_, ext, _, _); */

        /* we need to get to the ASN1_OCTET_STRING * to get
         * the data and len.
         */

        /* i2r_scts(method, ext_str, _, _); */

        conncfg->ocsp_has_sct_list = 1;
        conncfg->peer_ct_aware = 1;
        conncfg->ocsp_sct_list_size = oct->length - 2;
        conncfg->ocsp_sct_list = apr_pmemdup(c->pool, oct->data + 2,
                                             conncfg->ocsp_sct_list_size);
    }

    OCSP_RESPONSE_free(rsp);

    return 1;
}

/* Callbacks and structures for handling custom TLS Extensions:
 *   cli_ext_first_cb  - sends data for ClientHello TLS Extension
 *   cli_ext_second_cb - receives data from ServerHello TLS Extension
 */
static int client_extension_callback_1(SSL *ssl, unsigned short ext_type,
                                       const unsigned char **out,
                                       unsigned short *outlen, int *al,
                                       void *arg)
{
    conn_rec *c = (conn_rec *)SSL_get_app_data(ssl);

    /* nothing to send in ClientHello */

    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, c,
                  "client_extension_callback_1 called, "
                  "ext %hu will be in ClientHello",
                  ext_type);

    return 1;
}

static int client_extension_callback_2(SSL *ssl, unsigned short ext_type,
                                    const unsigned char *in, unsigned short inlen,
                                    int *al, void *arg)
{
    conn_rec *c = (conn_rec *)SSL_get_app_data(ssl);
    ct_conn_config *conncfg = get_conn_config(c);

    /* need to retrieve SCT(s) from ServerHello (or certificate or stapled response) */

    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, c,
                  "client_extension_callback_2 called, "
                  "ext %hu was in ServerHello (len %hu)",
                  ext_type, inlen);

    /* Note: Peer certificate is not available in this callback via
     *       SSL_get_peer_certificate(ssl)
     */

    conncfg->serverhello_has_sct_list = 1;
    conncfg->peer_ct_aware = 1;
    conncfg->serverhello_sct_list = apr_pmemdup(c->pool, in, inlen);
    conncfg->serverhello_sct_list_size = inlen;
    return 1;
}

/* See SSLClient::VerifyCallback() in c-t/src/client/ssl_client.cc
 * (That's a beast and hard to duplicate in depth when you consider
 * all the support classes it relies on; mod_ssl_ct needs to be a
 * C++ module so that the bugs are fixed in one place.)
 *
 * . This code should care about stapled SCTs but doesn't.
 * . This code, unlike SSLClient::VerifyCallback(), doesn't look
 *   at the OpenSSL "input" chain.
 */
static int ssl_ct_ssl_proxy_verify(server_rec *s, conn_rec *c,
                                   X509_STORE_CTX *ctx)
{
    apr_pool_t *p = c->pool;
    ct_conn_config *conncfg = get_conn_config(c);
    ct_server_config *sconf = ap_get_module_config(s->module_config,
                                                   &ssl_ct_module);
    int chain_size = sk_X509_num(ctx->chain);
    int extension_index;
    cert_chain *certs;

    if (sconf->proxy_awareness == PROXY_OBLIVIOUS) {
        return OK;
    }

    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                  "ssl_ct_ssl_proxy_verify() - get server certificate info");

    if (chain_size < 1) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, c,
                      "odd chain size %d -- cannot proceed", chain_size);
        return APR_EINVAL;
    }

    /* Note: SSLClient::Verify looks in both the input chain and the
     *       verified chain.
     */

    certs = cert_chain_init(p, ctx->chain);
    conncfg->certs = certs;

    extension_index = 
        X509_get_ext_by_NID(certs->leaf,
                            NID_ct_precert_scts,
                            -1);
    /* use X509_get_ext(certs->leaf, extension_index) to obtain X509_EXTENSION * */

    if (extension_index >= 0) {
        void *ext_struct;

        server_cert_has_sct_list(c);
        /* as in Cert::ExtensionStructure() */
        ext_struct = X509_get_ext_d2i(certs->leaf,
                                      NID_ct_precert_scts,
                                      NULL, /* ignore criticality of extension */
                                      NULL);

        if (ext_struct == NULL) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, c,
                          "Could not retrieve SCT list from certificate (unexpected)");
        }
        else {
            /* as in Cert::OctetStringExtensionData */
            ASN1_OCTET_STRING *octet = (ASN1_OCTET_STRING *)ext_struct;
            conncfg->cert_sct_list = apr_pmemdup(p,
                                                 octet->data,
                                                 octet->length);
            conncfg->cert_sct_list_size = octet->length;
            ASN1_OCTET_STRING_free(octet);
        }
    }

    return OK;
}

static int ssl_ct_ssl_proxy_post_handshake(server_rec *s, conn_rec *c)
{
    apr_pool_t *p = c->pool;
    apr_status_t rv = APR_SUCCESS;
    const char *key;
    ct_cached_server_data *cached;
    ct_conn_config *conncfg = get_conn_config(c);
    ct_server_config *sconf = ap_get_module_config(s->module_config,
                                                   &ssl_ct_module);
    int validation_error = 0, missing_sct_error = 0;

    if (sconf->proxy_awareness == PROXY_OBLIVIOUS) {
        return OK;
    }

    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                  "finally at the point where we can see where SCTs came from"
                  " %pp/%pp/%pp (c %pp)",
                  conncfg->cert_sct_list, conncfg->serverhello_sct_list,
                  conncfg->ocsp_sct_list, c);

    /* At this point we have the SCTs from the cert (if any) and the
     * SCTs from the TLS extension (if any) in ct_conn_config.
     */

    if (conncfg->cert_sct_list || conncfg->serverhello_sct_list
        || conncfg->ocsp_sct_list) {

        /* The key is critical to avoiding validating and queueing of
         * the same stuff over and over.
         *
         * Is there any cheaper check than server cert and SCTs all exactly
         * the same as before?
         */
        
        key = gen_key(c, conncfg->certs, conncfg);

        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                      "key for server data: %s", key);

        ctutil_thread_mutex_lock(cached_server_data_mutex);

        cached = apr_hash_get(cached_server_data, key, APR_HASH_KEY_STRING);

        ctutil_thread_mutex_unlock(cached_server_data_mutex);

        if (!cached) {
            ct_cached_server_data *new_server_data =
                (ct_cached_server_data *)calloc(1, sizeof(ct_cached_server_data));

            new_server_data->validation_result = 
                rv = validate_server_data(p, c, conncfg->certs, conncfg, sconf);

            if (rv != APR_SUCCESS) {
                validation_error = 1;
            }

            ctutil_thread_mutex_lock(cached_server_data_mutex);

            if ((cached = apr_hash_get(cached_server_data, key, APR_HASH_KEY_STRING))) {
                /* some other thread snuck in
                 * we assume that the other thread got the same validation
                 * result that we did
                 */
                free(new_server_data);
                new_server_data = NULL;
            }
            else {
                /* no other thread snuck in */
                apr_hash_set(cached_server_data, key, APR_HASH_KEY_STRING,
                             new_server_data);
                new_server_data = NULL;
            }

            ctutil_thread_mutex_unlock(cached_server_data_mutex);

            if (rv == APR_SUCCESS && !cached) {
                save_server_data(c, conncfg->certs, conncfg);
            }
        }
        else {
            /* cached */
            rv = cached->validation_result;
            if (rv != APR_SUCCESS) {
                validation_error = 1;
                ap_log_cerror(APLOG_MARK, APLOG_INFO, rv, c, "bad cached validation result");
            }
        }
    }
    else {
        /* No SCTs at all; consult configuration to know what to do. */
        missing_sct_error = 1;
    }

    if (conncfg->certs) {
        cert_chain_free(conncfg->certs);
        conncfg->certs = NULL;
    }

    ap_log_cerror(APLOG_MARK,
                  rv == APR_SUCCESS ? APLOG_DEBUG : APLOG_ERR, rv, c,
                  "SCT list received in: %s%s%s(%s) (c %pp)",
                  conncfg->serverhello_has_sct_list ? "ServerHello " : "",
                  conncfg->server_cert_has_sct_list ? "certificate-extension " : "",
                  conncfg->ocsp_has_sct_list ? "OCSP " : "",
                  cached ? "already saved" : "seen for the first time",
                  c);

    if (sconf->proxy_awareness == PROXY_REQUIRE) {
        if (missing_sct_error || validation_error) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, c,
                          "Forbidding access to backend server; no valid SCTs");
            return HTTP_FORBIDDEN;
        }
    }

    return OK;
}

static int server_extension_callback_1(SSL *ssl, unsigned short ext_type,
                                       const unsigned char *in,
                                       unsigned short inlen, int *al,
                                       void *arg)
{
    conn_rec *c = (conn_rec *)SSL_get_app_data(ssl);

    /* this callback tells us that client is CT-aware;
     * there's nothing of interest in the extension data
     */
    client_is_ct_aware(c);

    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, c,
                  "server_extension_callback_1 called, "
                  "ext %hu was in ClientHello (len %hu)",
                  ext_type, inlen);

    return 1;
}

static int server_extension_callback_2(SSL *ssl, unsigned short ext_type,
                                       const unsigned char **out,
                                       unsigned short *outlen, int *al,
                                       void *arg)
{
    conn_rec *c = (conn_rec *)SSL_get_app_data(ssl);
    ct_server_config *sconf = ap_get_module_config(c->base_server->module_config,
                                                   &ssl_ct_module);
    X509 *server_cert;
    const char *fingerprint;
    const unsigned char *scts;
    apr_size_t scts_len;
    apr_status_t rv;

    if (!is_client_ct_aware(c)) {
        /* Hmmm...  Is this actually called if the client doesn't include
         * the extension in the ClientHello?  I don't think so.
         */
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                      "server_extension_callback_2: client isn't CT-aware");
        /* Skip this extension for ServerHello */
        return -1;
    }

    /* need to reply with SCT */

    server_cert = SSL_get_certificate(ssl); /* no need to free! */
    fingerprint = get_cert_fingerprint(c->pool, server_cert);

    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, c,
                  "server_extension_callback_2 called, "
                  "ext %hu will be in ServerHello",
                  ext_type);

    rv = read_scts(c->pool, fingerprint,
                   sconf->sct_storage,
                   c->base_server, (char **)&scts, &scts_len);
    if (rv == APR_SUCCESS) {
        *out = scts;
        *outlen = scts_len;
    }
    else {
        /* Skip this extension for ServerHello */
        return -1;
    }

    return 1;
}

static void tlsext_cb(SSL *ssl, int client_server, int type,
                      unsigned char *data, int len,
                      void *arg)
{
    conn_rec *c = arg;

    ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, c, "tlsext_cb called (%d,%d,%d)",
                  client_server, type, len);

    if (type == CT_EXTENSION_TYPE) {
        ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, c, "Got CT TLS extension");

        client_is_ct_aware(c);
    }
}

static int ssl_ct_ssl_new_client_pre_handshake(conn_rec *c, SSL *ssl)
{
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c, "client connected (pre-handshake)");

    SSL_set_tlsext_status_type(ssl, TLSEXT_STATUSTYPE_ocsp);

    /* This callback is needed only to determine that the peer is CT-aware
     * when resuming a session.  For an initial handshake, the callbacks
     * registered via SSL_CTX_set_custom_srv_ext() are sufficient.
     */
    SSL_set_tlsext_debug_callback(ssl, tlsext_cb);
    SSL_set_tlsext_debug_arg(ssl, c);

    return OK;
}

static int ssl_ct_ssl_init_ctx(server_rec *s, apr_pool_t *p, int is_proxy, SSL_CTX *ssl_ctx)
{
    ct_callback_info *cbi = apr_pcalloc(p, sizeof *cbi);
    ct_server_config *sconf = ap_get_module_config(s->module_config,
                                                   &ssl_ct_module);

    cbi->s = s;

    if (is_proxy && sconf->proxy_awareness != PROXY_OBLIVIOUS) {
        /* _cli_ = "client" */
        if (!SSL_CTX_set_custom_cli_ext(ssl_ctx, CT_EXTENSION_TYPE,
                                        client_extension_callback_1,
                                        client_extension_callback_2, cbi)) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s,
                         "Unable to initalize Certificate Transparency client "
                         "extension callbacks (callback for %d already registered?)",
                         CT_EXTENSION_TYPE);
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        /* Uhh, hopefully this doesn't collide with anybody else.  mod_ssl
         * currently only sets this on the server SSL_CTX, when OCSP is
         * enabled.
         */
        SSL_CTX_set_tlsext_status_cb(ssl_ctx, ocsp_resp_cb);
        SSL_CTX_set_tlsext_status_arg(ssl_ctx, cbi);
    }
    else if (!is_proxy) {
        /* _srv_ = "server" */
        if (!SSL_CTX_set_custom_srv_ext(ssl_ctx, CT_EXTENSION_TYPE,
                                        server_extension_callback_1,
                                        server_extension_callback_2, cbi)) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s,
                         "Unable to initalize Certificate Transparency server "
                         "extension callback (callbacks for %d already registered?)",
                         CT_EXTENSION_TYPE);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    return OK;
}

static int ssl_ct_post_read_request(request_rec *r)
{
    ct_conn_config *conncfg =
      ap_get_module_config(r->connection->conn_config, &ssl_ct_module);

    if (conncfg && conncfg->peer_ct_aware) {
        apr_table_set(r->subprocess_env, STATUS_VAR, STATUS_VAR_AWARE_VAL);
    }
    else {
        apr_table_set(r->subprocess_env, STATUS_VAR, STATUS_VAR_UNAWARE_VAL);
    }

    return DECLINED;
}

static int ssl_ct_pre_config(apr_pool_t *pconf, apr_pool_t *plog,
                             apr_pool_t *ptemp)
{
    apr_status_t rv = ap_mutex_register(pconf, SSL_CT_MUTEX_TYPE, NULL,
                                        APR_LOCK_DEFAULT, 0);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    ctutil_run_internal_tests(ptemp);

    return OK;
}

static apr_status_t inactivate_audit_file(void *data)
{
    apr_status_t rv;
    server_rec *s = data;

    if (!audit_file) { /* something bad happened after child init */
        return APR_SUCCESS;
    }

    /* the normal cleanup was disabled in the call to apr_file_open */
    rv = apr_file_close(audit_file);
    audit_file = NULL;
    if (rv == APR_SUCCESS) {
        if (audit_file_nonempty) {
            rv = apr_file_rename(audit_fn_active, audit_fn_perm,
                                 /* not used in current implementations */
                                 s->process->pool);
        }
        else {
            /* No data written; just remove the file */
            apr_file_remove(audit_fn_active,
                            /* not used in current implementations */
                            s->process->pool);
        }
    }
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s,
                     "error flushing/closing %s or renaming it to %s",
                     audit_fn_active, audit_fn_perm);
    }

    return APR_SUCCESS; /* what, you think anybody cares? */
}

static void ssl_ct_child_init(apr_pool_t *p, server_rec *s)
{
    apr_status_t rv;
    const char *audit_basename;
    ct_server_config *sconf = ap_get_module_config(s->module_config,
                                                   &ssl_ct_module);

    cached_server_data = apr_hash_make(p);

    rv = apr_global_mutex_child_init(&ssl_ct_sct_update,
                                     apr_global_mutex_lockfile(ssl_ct_sct_update), p);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s,
                     "could not initialize " SSL_CT_MUTEX_TYPE
                     " mutex in child");
        return;
    }

    rv = apr_thread_create(&service_thread, NULL, run_service_thread, s, p);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s,
                     "could not create " SERVICE_THREAD_NAME " in child");
        return;
    }

    apr_pool_cleanup_register(p, service_thread, wait_for_service_thread,
                              apr_pool_cleanup_null);

    if (sconf->proxy_awareness != PROXY_OBLIVIOUS) {
        rv = apr_thread_mutex_create(&audit_file_mutex,
                                     APR_THREAD_MUTEX_DEFAULT, p);
        if (rv == APR_SUCCESS) {
            rv = apr_thread_mutex_create(&cached_server_data_mutex,
                                         APR_THREAD_MUTEX_DEFAULT,
                                         p);
        }
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s,
                         "could not allocate a thread mutex");
            /* might crash due to lack of checking for initialized data in all
             * the right places
             */
            return;
        }

        audit_basename = apr_psprintf(p, "audit_%" APR_PID_T_FMT,
                                      getpid());
        rv = ctutil_path_join((char **)&audit_fn_perm, sconf->audit_storage,
                              audit_basename, p, s);
        if (rv != APR_SUCCESS) {
            audit_fn_perm = NULL;
            audit_fn_active = NULL;
            return;
        }

        audit_fn_active = apr_pstrcat(p, audit_fn_perm, ".tmp", NULL);
        audit_fn_perm = apr_pstrcat(p, audit_fn_perm, ".out", NULL);

        if (ctutil_file_exists(p, audit_fn_active)) {
            ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s,
                         "ummm, pid-specific file %s was reused before audit grabbed it! (removing)",
                         audit_fn_active);
            apr_file_remove(audit_fn_active, p);
        }

        if (ctutil_file_exists(p, audit_fn_perm)) {
            ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s,
                         "ummm, pid-specific file %s was reused before audit grabbed it! (removing)",
                         audit_fn_perm);
            apr_file_remove(audit_fn_perm, p);
        }

        rv = apr_file_open(&audit_file, audit_fn_active,
                           APR_FOPEN_WRITE|APR_FOPEN_CREATE|APR_FOPEN_TRUNCATE
                           |APR_FOPEN_BINARY|APR_FOPEN_BUFFERED|APR_FOPEN_NOCLEANUP,
                           APR_FPROT_OS_DEFAULT, p);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                         "can't create %s", audit_fn_active);
            audit_file = NULL;
        }

        if (audit_file) {
            apr_pool_cleanup_register(p, s, inactivate_audit_file, apr_pool_cleanup_null);
        }
    } /* !PROXY_OBLIVIOUS */
}

static void *create_ct_server_config(apr_pool_t *p, server_rec *s)
{
    ct_server_config *conf =
        (ct_server_config *)apr_pcalloc(p, sizeof(ct_server_config));

    conf->max_sct_age = apr_time_from_sec(3600);
    conf->proxy_awareness = PROXY_AWARENESS_UNSET;
    
    return conf;
}

static void *merge_ct_server_config(apr_pool_t *p, void *basev, void *virtv)
{
    ct_server_config *base = (ct_server_config *)basev;
    ct_server_config *virt = (ct_server_config *)virtv;
    ct_server_config *conf;

    conf = (ct_server_config *)apr_pmemdup(p, virt, sizeof(ct_server_config));

    conf->log_urls = (virt->log_urls != NULL)
        ? virt->log_urls
        : base->log_urls;

    conf->log_public_keys = base->log_public_keys;
    conf->log_ids = base->log_ids;
    conf->sct_storage = base->sct_storage;
    conf->audit_storage = base->audit_storage;
    conf->ct_tools_dir = base->ct_tools_dir;
    conf->max_sct_age = base->max_sct_age;

    conf->proxy_awareness = (virt->proxy_awareness != PROXY_AWARENESS_UNSET)
        ? virt->proxy_awareness
        : base->proxy_awareness;

    return conf;
}

static int ssl_ct_proxy_http_cleanup(request_rec *r, conn_rec *origin,
                                     proxy_conn_rec *backend)
{
    if (!origin && backend) {
        origin = backend->connection;
    }

    if (origin) {
        ct_conn_config *conncfg = get_conn_config(origin);
        char *list, *last;

        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "ssl_ct_proxy_http_cleanup, %d%d%d",
                      conncfg->server_cert_has_sct_list,
                      conncfg->serverhello_has_sct_list,
                      conncfg->ocsp_has_sct_list);

        apr_table_set(r->subprocess_env, STATUS_VAR,
                      conncfg->peer_ct_aware ? STATUS_VAR_AWARE_VAL : STATUS_VAR_UNAWARE_VAL);

        list = apr_pstrcat(r->pool,
                           conncfg->server_cert_has_sct_list ? "certext," : "",
                           conncfg->serverhello_has_sct_list ? "tlsext," : "",
                           conncfg->ocsp_has_sct_list ? "ocsp" : "",
                           NULL);
        if (*list) {
            last = list + strlen(list) - 1;
            if (*last == ',') {
                *last = '\0';
            }
        }

        apr_table_set(r->subprocess_env, PROXY_SCT_SOURCES_VAR, list);
    }
    else {
        ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                      "No backend connection available in "
                      "ssl_ct_proxy_http_cleanup(); assuming peer unaware");
        apr_table_set(r->subprocess_env, STATUS_VAR,
                      STATUS_VAR_UNAWARE_VAL);
    }

    return OK;
}

static void ct_register_hooks(apr_pool_t *p)
{
    ap_hook_pre_config(ssl_ct_pre_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_check_config(ssl_ct_check_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(ssl_ct_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_read_request(ssl_ct_post_read_request, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(ssl_ct_child_init, NULL, NULL, APR_HOOK_MIDDLE);
    AP_OPTIONAL_HOOK(proxy_http_cleanup, ssl_ct_proxy_http_cleanup, NULL, NULL,
                     APR_HOOK_MIDDLE);
    AP_OPTIONAL_HOOK(ssl_server_init, ssl_ct_ssl_server_init, NULL, NULL, 
                     APR_HOOK_MIDDLE);
    AP_OPTIONAL_HOOK(ssl_init_ctx, ssl_ct_ssl_init_ctx, NULL, NULL,
                     APR_HOOK_MIDDLE);
    AP_OPTIONAL_HOOK(ssl_new_client_pre_handshake,
                     ssl_ct_ssl_new_client_pre_handshake,
                     NULL, NULL, APR_HOOK_MIDDLE);
    AP_OPTIONAL_HOOK(ssl_proxy_verify, ssl_ct_ssl_proxy_verify,
                     NULL, NULL, APR_HOOK_MIDDLE);
    AP_OPTIONAL_HOOK(ssl_proxy_post_handshake, ssl_ct_ssl_proxy_post_handshake,
                     NULL, NULL, APR_HOOK_MIDDLE);
}

static apr_status_t save_log_url(apr_pool_t *p, const char *lu, ct_server_config *sconf)
{
    apr_status_t rv;
    apr_uri_t uri, *puri;
    char **pstr;

    rv = apr_uri_parse(p, lu, &uri);
    if (rv == APR_SUCCESS) {
        if (!uri.scheme
            || !uri.hostname
            || !uri.path) {
            rv = APR_EINVAL;
        }
        if (strcmp(uri.scheme, "http")) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL,
                         "Scheme must be \"http\" instead of \"%s\"",
                         uri.scheme);
            rv = APR_EINVAL;
        }
        if (strcmp(uri.path, "/")) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL,
                         "No URI path other than \"/\" is currently accepted (you have \"%s\")",
                         uri.path);
            rv = APR_EINVAL;
        }
        if (!sconf->log_urls) {
            sconf->log_urls = apr_array_make(p, 2, sizeof(uri));
            sconf->log_url_strs = apr_array_make(p, 2, sizeof(char *));
        }
        puri = (apr_uri_t *)apr_array_push(sconf->log_urls);
        *puri = uri;
        pstr = (char **)apr_array_push(sconf->log_url_strs);
        *pstr = apr_uri_unparse(p, &uri, 0);
    }
    return rv;
}

static apr_status_t save_log_public_key(apr_pool_t *p, const char *const_lpk_arg,
                                        ct_server_config *sconf)
{
    apr_status_t rv = APR_SUCCESS;
    const char *logid, *pubkey_fname;
    EVP_PKEY *pubkey, **ppkey;
    FILE *pubkeyf;
    char *lpk_arg = apr_pstrdup(p, const_lpk_arg);
    char *colon = ap_strchr(lpk_arg, ':');
    char **plogid, *logid_binary;

    if (!colon) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf,
                     "expected <logid>:pubkey-file");
        return APR_EINVAL;
    }

    pubkey_fname = colon + 1;
    logid = lpk_arg;
    *colon = '\0';

    if (strlen(logid) != 2 * LOG_ID_SIZE) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf,
                     "expected 64-character hex log id");
    }

    logid_binary = apr_palloc(p, LOG_ID_SIZE);
    rv = apr_unescape_hex(logid_binary, logid, 2 * LOG_ID_SIZE, 0, NULL);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, ap_server_conf,
                     "could not unencode hex log id %s",
                     logid);
        return rv;
    }

    pubkeyf = fopen(pubkey_fname, "r");
    if (!pubkeyf) {
        rv = errno; /* Unix-ism! */
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, ap_server_conf,
                     "could not open log public key file %s",
                     pubkey_fname);
        return rv;
    }

    pubkey = PEM_read_PUBKEY(pubkeyf, NULL, NULL, NULL);
    if (!pubkey) {
        fclose(pubkeyf);
        rv = APR_EINVAL;
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf,
                     "PEM_read_PUBKEY() failed to process public key file %s",
                     pubkey_fname);
        return rv;
    }

    fclose(pubkeyf);

    if (!sconf->log_public_keys) {
        sconf->log_public_keys = apr_array_make(p, 2, sizeof(EVP_PKEY *));
        sconf->log_ids = apr_array_make(p, 2, sizeof(char *));
    }
    ppkey = (EVP_PKEY **)apr_array_push(sconf->log_public_keys);
    *ppkey = pubkey;

    plogid = (char **)apr_array_push(sconf->log_ids);
    *plogid = logid_binary;

    return rv;
}

static const char *ct_logs(cmd_parms *cmd, void *x, int argc, char *const argv[])
{
    int i;
    apr_status_t rv;
    ct_server_config *sconf = ap_get_module_config(cmd->server->module_config,
                                                   &ssl_ct_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);

    if (err) {
        return err;
    }

    if (argc < 1) {
        return "CTLogs: At least one log URL must be provided";
    }

    for (i = 0; i < argc; i++) {
        rv = save_log_url(cmd->pool, argv[i], sconf);
        if (rv) {
            return apr_psprintf(cmd->pool, "CTLogs: Error with log URL %s: (%d)%pm",
                                argv[i], rv, &rv);
        }
    }

    return NULL;
}

static const char *ct_log_pubkeys(cmd_parms *cmd, void *x, int argc,
                                  char *const argv[])
{
    int i;
    apr_status_t rv;
    ct_server_config *sconf = ap_get_module_config(cmd->server->module_config,
                                                   &ssl_ct_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);

    if (err) {
        return err;
    }

    if (argc < 1) {
        return "CTLogPublicKeys: At least one public key must be provided";
    }

    for (i = 0; i < argc; i++) {
        rv = save_log_public_key(cmd->pool, argv[i], sconf);
        if (rv) {
            return apr_psprintf(cmd->pool, "CTLogPublicKeys: Error with log id/URL "
                                "%s: (%d)%pm",
                                argv[i], rv, &rv);
        }
    }

    return NULL;
}

static const char *ct_sct_storage(cmd_parms *cmd, void *x, const char *arg)
{
    ct_server_config *sconf = ap_get_module_config(cmd->server->module_config,
                                                   &ssl_ct_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);

    if (err) {
        return err;
    }

    if (!ctutil_dir_exists(cmd->pool, arg)) {
        return apr_pstrcat(cmd->pool, "CTSCTStorage: Directory ", arg,
                           " does not exist", NULL);
    }

    sconf->sct_storage = arg;

    return NULL;
}

static const char *ct_audit_storage(cmd_parms *cmd, void *x, const char *arg)
{
    ct_server_config *sconf = ap_get_module_config(cmd->server->module_config,
                                                   &ssl_ct_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);

    if (err) {
        return err;
    }

    if (!ctutil_dir_exists(cmd->pool, arg)) {
        return apr_pstrcat(cmd->pool, "CTAuditStorage: Directory ", arg,
                           " does not exist", NULL);
    }

    sconf->audit_storage = arg;

    return NULL;
}

static const char *ct_tools_dir(cmd_parms *cmd, void *x, const char *arg)
{
    apr_status_t rv;
    ct_server_config *sconf = ap_get_module_config(cmd->server->module_config,
                                                   &ssl_ct_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);

    if (err) {
        return err;
    }

    if (!ctutil_dir_exists(cmd->pool, arg)) {
        return apr_pstrcat(cmd->pool, "CTToolsDir: Directory ", arg,
                           " does not exist", NULL);
    }

    rv = ctutil_path_join((char **)&sconf->ct_exe, arg,  "src/client/ct" DOTEXE,
                          cmd->pool, NULL);
    if (rv != APR_SUCCESS) {
        return apr_psprintf(cmd->pool,
                            "CTToolsDir: Couldn't build path to ct" DOTEXE
                            ": %pm", &rv);
    }

    if (!ctutil_file_exists(cmd->pool, sconf->ct_exe)) {
        return apr_pstrcat(cmd->pool, "CTToolsDir: File ", sconf->ct_exe,
                           " does not exist", NULL);
    }

    sconf->ct_tools_dir = arg;

    return NULL;
}

static const char *ct_max_sct_age(cmd_parms *cmd, void *x, const char *arg)
{
    ct_server_config *sconf = ap_get_module_config(cmd->server->module_config,
                                                   &ssl_ct_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    long val;
    char *endptr;

    if (err) {
        return err;
    }

    errno = 0;
    val = strtol(arg, &endptr, 10);
    if (errno != 0
        || *endptr != '\0'
        || val < 10
        || val > 3600 * 12) {
        return apr_psprintf(cmd->pool, "CTMaxSCTAge must be between 10 seconds "
                            "and 12 hours worth of seconds (%d)",
                            3600 * 12);
    }
    sconf->max_sct_age = apr_time_from_sec(val);
    return NULL;
}    

static const char *ct_proxy_awareness(cmd_parms *cmd, void *x, const char *arg)
{
    ct_server_config *sconf = ap_get_module_config(cmd->server->module_config,
                                                   &ssl_ct_module);

    if (!strcasecmp(arg, "oblivious")) {
        sconf->proxy_awareness = PROXY_OBLIVIOUS;
    }
    else if (!strcasecmp(arg, "aware")) {
        sconf->proxy_awareness = PROXY_AWARE;
    }
    else if (!strcasecmp(arg, "require")) {
        sconf->proxy_awareness = PROXY_REQUIRE;
    }
    else {
        return apr_pstrcat(cmd->pool, "CTProxyAwareness: Invalid argument \"",
                           arg, "\"", NULL);
    }

    return NULL;
}

static const command_rec ct_cmds[] =
{
    AP_INIT_TAKE1("CTAuditStorage", ct_audit_storage, NULL, RSRC_CONF,
                  "Location to store files of audit data"),
    AP_INIT_TAKE_ARGV("CTLogs", ct_logs, NULL, RSRC_CONF,
                      "List of Certificate Transparency Log URLs"),
    AP_INIT_TAKE_ARGV("CTLogPublicKeys", ct_log_pubkeys, NULL, RSRC_CONF,
                      "List of Certificate Transparency Log public keys"),
    AP_INIT_TAKE1("CTSCTStorage", ct_sct_storage, NULL, RSRC_CONF,
                  "Location to store SCTs obtained from logs"),
    AP_INIT_TAKE1("CTToolsDir", ct_tools_dir, NULL, RSRC_CONF,
                  "Location of certificate-transparency.org tools"),
    AP_INIT_TAKE1("CTMaxSCTAge", ct_max_sct_age, NULL, RSRC_CONF,
                  "Max age of SCT obtained from log before refresh"),
    AP_INIT_TAKE1("CTProxyAwareness", ct_proxy_awareness, NULL, RSRC_CONF,
                  "\"oblivious\" to neither ask for nor check SCTs, "
                  "\"aware\" to ask for and process SCTs but allow all connections, "
                  "or \"require\" to abort backend connections if an acceptable "
                  "SCT is not provided"),
    {NULL}
};

AP_DECLARE_MODULE(ssl_ct) =
{
    STANDARD20_MODULE_STUFF,
    NULL,
    NULL,
    create_ct_server_config,
    merge_ct_server_config,
    ct_cmds,
    ct_register_hooks,
};
