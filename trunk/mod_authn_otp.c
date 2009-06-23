
/*
 * mod_authn_otp - Apache module for one-time password authentication
 *
 * Copyright 2009 Archie L. Cobbs <archie@dellroad.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * $Id$
 */

#include "apr_lib.h"
#include "ap_config.h"
#include "ap_provider.h"
#include "mod_auth.h"

#define APR_WANT_STRFUNC
#include "apr_want.h"
#include "apr_strings.h"
#include "apr_file_io.h"
#include "apr_time.h"

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_md5.h"

#include <time.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>

/* Module definition */
module AP_MODULE_DECLARE_DATA authn_otp_module;

/* Definitions related to users file */
#define WHITESPACE              " \t\r\n\v"
#define NEWFILE_SUFFIX          ".new"
#define LOCKFILE_SUFFIX         ".lock"
#define TIME_FORMAT             "%Y-%m-%dT%H:%M:%SL"

/* OTP counter algorithms */
#define OTP_ALGORITHM_HOTP      1
#define OTP_ALGORITHM_MOTP      2

/* Default configuration settings */
#define DEFAULT_NUM_DIGITS      6
#define DEFAULT_MAX_OFFSET      4
#define DEFAULT_MAX_LINGER      (10 * 60)   /* 10 minutes */

/* MobileOTP defaults */
#define MOTP_TIME_INTERVAL      10

/* Buffer size for OTPs */
#define OTP_BUF_SIZE            16

/* Per-directory configuration */
struct otp_config {
    char    *users_file;        /* Name of the users file */
    int     max_offset;         /* Maximum allowed counter offset from expected value */
    int     max_linger;         /* Maximum time for which the same OTP can be used repeatedly */
};

/* User info structure */
struct otp_user {
    int             algorithm;          /* one of OTP_ALGORITHM_* */
    int             time_interval;      /* in seconds, or zero for event-based tokens */
    int             num_digits;
    char            username[128];
    u_char          key[256];
    int             keylen;
    char            pin[128];
    long            offset;             /* if event: next expected count; if time: time slew */
    char            last_otp[128];
    time_t          last_auth;
};

/* Internal functions */
static authn_status find_update_user(request_rec *r, const char *usersfile, struct otp_user *const user, int update);
static void         hotp(const u_char *key, size_t keylen, u_long counter, int ndigits, char *buf10, char *buf16, size_t buflen);
static void         motp(const u_char *key, size_t keylen, const char *pin, u_long counter, int ndigits, char *buf, size_t buflen);
static int          parse_token_type(const char *type, struct otp_user *tokinfo);
static void         print_user(apr_file_t *file, const struct otp_user *user);
static void         printhex(char *buf, size_t buflen, const u_char *data, size_t dlen, int max_digits);
static authn_status authn_otp_check_password(request_rec *r, const char *username, const char *password);
static authn_status authn_otp_get_realm_hash(request_rec *r, const char *username, const char *realm, char **rethash);
static void         *create_authn_otp_dir_config(apr_pool_t *p, char *d);
static void         *merge_authn_otp_dir_config(apr_pool_t *p, void *base_conf, void *new_conf);
static struct       otp_config *get_config(request_rec *r);
static void         register_hooks(apr_pool_t *p);

/* Powers of ten */
static const int    powers10[] = { 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000, 1000000000 };

/*
 * Find/update a user in the users file.
 *
 * Note: if updating, the caller must ensure proper locking.
 */
static authn_status
find_update_user(request_rec *r, const char *usersfile, struct otp_user *const user, int update)
{
    char invalid_reason[128];
    char newusersfile[APR_PATH_MAX];
    char lockusersfile[APR_PATH_MAX];
    char linebuf[1024];
    char linecopy[1024];
    apr_file_t *file = NULL;
    apr_file_t *newfile = NULL;
    apr_file_t *lockfile = NULL;
    apr_status_t status;
    char errbuf[64];
    struct tm tm;
    int found = 0;
    int linenum;
    char *last;
    char *s;
    char *t;

    /* If updating, open and lock lockfile */
    if (update) {
        apr_snprintf(lockusersfile, sizeof(lockusersfile), "%s%s", usersfile, LOCKFILE_SUFFIX);
        if ((status = apr_file_open(&lockfile, lockusersfile,
          APR_WRITE|APR_CREATE|APR_TRUNCATE, APR_UREAD|APR_UWRITE, r->pool)) != 0) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "can't open OTP users lock file \"%s\": %s",
              lockusersfile, apr_strerror(status, errbuf, sizeof(errbuf)));
            goto fail;
        }
        if ((status = apr_file_lock(lockfile, APR_FLOCK_EXCLUSIVE)) != 0) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "can't lock OTP users lock file \"%s\": %s",
              lockusersfile, apr_strerror(status, errbuf, sizeof(errbuf)));
            goto fail;
        }
    }

    /* Open existing users file */
    if ((status = apr_file_open(&file, usersfile, APR_READ, 0, r->pool)) != 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "can't open OTP users file \"%s\": %s",
          usersfile, apr_strerror(status, errbuf, sizeof(errbuf)));
        goto fail;
    }

    /* Open new users file if updating */
    if (update) {
        apr_snprintf(newusersfile, sizeof(newusersfile), "%s%s", usersfile, NEWFILE_SUFFIX);
        if ((status = apr_file_open(&newfile, newusersfile,
          APR_WRITE|APR_CREATE|APR_TRUNCATE, APR_UREAD|APR_UWRITE, r->pool)) != 0) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "can't new open OTP users file \"%s\": %s", newusersfile,
              apr_strerror(status, errbuf, sizeof(errbuf)));
            goto fail;
        }
    }

    /* Scan entries */
    for (linenum = 1; apr_file_gets(linebuf, sizeof(linebuf), file) == 0; linenum++) {
        struct otp_user tokinfo;

        /* Save a copy of the line */
        apr_snprintf(linecopy, sizeof(linecopy), "%s", linebuf);

        /* Ignore lines starting with '#' and empty lines */
        if (*linebuf == '#')
            goto copy;
        if ((s = apr_strtok(linebuf, WHITESPACE, &last)) == NULL)
            goto copy;

        /* Parse token type */
        if (parse_token_type(s, &tokinfo) != 0) {
            apr_snprintf(invalid_reason, sizeof(invalid_reason), "invalid token type \"%s\"", s);
            goto invalid;
        }

        /* Get username */
        if ((s = apr_strtok(NULL, WHITESPACE, &last)) == NULL) {
            apr_snprintf(invalid_reason, sizeof(invalid_reason), "missing username field");
            goto invalid;
        }

        /* Is this the user we're interested in? */
        if (strcmp(s, user->username) != 0)
            goto copy;
        found = 1;

        /* If we're updating, print out updated user info to new file */
        if (update) {
            print_user(newfile, user);
            continue;
        }

        /* Initialize user record */
        memset(user, 0, sizeof(*user));
        apr_snprintf(user->username, sizeof(user->username), "%s", s);
        user->algorithm = tokinfo.algorithm;
        user->time_interval = tokinfo.time_interval;
        user->num_digits = tokinfo.num_digits;

        /* Read PIN */
        if ((s = apr_strtok(NULL, WHITESPACE, &last)) == NULL) {
            apr_snprintf(invalid_reason, sizeof(invalid_reason), "missing PIN field");
            goto invalid;
        }
        if (strcmp(s, "-") == 0)
            *s = '\0';
        apr_snprintf(user->pin, sizeof(user->pin), "%s", s);

        /* Read key */
        if ((s = apr_strtok(NULL, WHITESPACE, &last)) == NULL) {
            apr_snprintf(invalid_reason, sizeof(invalid_reason), "missing token key field");
            goto invalid;
        }
        for (user->keylen = 0; user->keylen < sizeof(user->key) && *s != '\0'; user->keylen++) {
            int nibs[2];
            int i;

            for (i = 0; i < 2; i++) {
                if (apr_isdigit(*s))
                    nibs[i] = *s - '0';
                else if (apr_isxdigit(*s))
                    nibs[i] = apr_tolower(*s) - 'a' + 10;
                else {
                    apr_snprintf(invalid_reason, sizeof(invalid_reason), "invalid key starting with \"%s\"", s);
                    goto invalid;
                }
                s++;
            }
            user->key[user->keylen] = (nibs[0] << 4) | nibs[1];
        }

        /* Read offset (optional) */
        if ((s = apr_strtok(NULL, WHITESPACE, &last)) == NULL)
            goto found;
        user->offset = atol(s);

        /* Read last used OTP (optional) */
        if ((s = apr_strtok(NULL, WHITESPACE, &last)) == NULL)
            goto found;
        apr_snprintf(user->last_otp, sizeof(user->last_otp), "%s", s);

        /* Read last successful authentication timestamp */
        if ((s = apr_strtok(NULL, WHITESPACE, &last)) == NULL) {
            apr_snprintf(invalid_reason, sizeof(invalid_reason), "missing last auth timestamp field");
            goto invalid;
        }
        if ((t = strptime(s, TIME_FORMAT, &tm)) == NULL || *t != '\0') {
            apr_snprintf(invalid_reason, sizeof(invalid_reason), "invalid auth timestamp \"%s\"", s);
            goto invalid;
        }
        tm.tm_isdst = -1;
        user->last_auth = mktime(&tm);

found:
        /* We are not updating; return the user we found */
        AP_DEBUG_ASSERT(!update);
        AP_DEBUG_ASSERT(newfile == NULL);
        AP_DEBUG_ASSERT(lockfile == NULL);
        apr_file_close(file);
        return AUTH_USER_FOUND;

invalid:
        /* Report invalid entry (but copy it anyway) */
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r, "ignoring invalid entry in OTP users file \"%s\" on line %d: %s",
          usersfile, linenum, invalid_reason);

copy:
        /* Copy line to new file */
        if (newfile != NULL)
            apr_file_puts(linecopy, newfile);
    }
    apr_file_close(file);
    file = NULL;

    /* If we're not updating and we get here, then the user was not found */
    if (!update) {
        ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, r, "user \"%s\" not found in OTP users file \"%s\"", user->username, usersfile);
        return AUTH_USER_NOT_FOUND;
    }

    /* Replace old file with new one */
    if ((status = apr_file_rename(newusersfile, usersfile, r->pool)) != 0) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "error renaming new OTP users file \"%s\" to \"%s\": %s",
          newusersfile, usersfile, apr_strerror(status, errbuf, sizeof(errbuf)));
        goto fail;
    }

    /* Close (and implicitly unlock) lock file */
    apr_file_close(lockfile);
    lockfile = NULL;

    /* Done updating */
    return found ? AUTH_USER_FOUND : AUTH_USER_NOT_FOUND;

fail:
    if (file != NULL)
        apr_file_close(file);
    if (newfile != NULL) {
        apr_file_close(newfile);
        (void)apr_file_remove(newusersfile, r->pool);
    }
    if (lockfile != NULL)
        apr_file_close(lockfile);
    return AUTH_GENERAL_ERROR;
}

/*
 * Parse a token type string such as "HOTP/T30/6".
 * Returns 0 if successful, else -1 on parse error.
 */
static int
parse_token_type(const char *type, struct otp_user *tokinfo)
{
    char tokbuf[128];
    char *last;
    char *eptr;
    char *t;

    /* Backwards compatibility hack */
    if (strcmp(type, "E") == 0)
        type = "HOTP/E";
    else if (strcmp(type, "T") == 0)
        type = "HOTP/T30";

    /* Initialize */
    memset(tokinfo, 0, sizeof(*tokinfo));
    apr_snprintf(tokbuf, sizeof(tokbuf), "%s", type);

    /* Parse algorithm */
    if ((t = apr_strtok(tokbuf, "/", &last)) == NULL)
        return -1;

    /* Apply per-algorithm defaults */
    if (strcasecmp(t, "HOTP") == 0) {
        tokinfo->algorithm = OTP_ALGORITHM_HOTP;
        tokinfo->time_interval = 0;
        tokinfo->num_digits = DEFAULT_NUM_DIGITS;
    } else if (strcasecmp(t, "MOTP") == 0) {
        tokinfo->algorithm = OTP_ALGORITHM_MOTP;
        tokinfo->time_interval = MOTP_TIME_INTERVAL;
        tokinfo->num_digits = DEFAULT_NUM_DIGITS;
    } else
        return -1;

    /* Parse token type: event or time-based */
    if ((t = apr_strtok(NULL, "/", &last)) == NULL)
        return 0;
    if (strcmp(t, "E") == 0)
        tokinfo->time_interval = 0;
    else if (*t == 'T') {
        if (!isdigit(*++t))
            return -1;
        tokinfo->time_interval = strtol(t, &eptr, 10);
        if (tokinfo->time_interval <= 0 || *eptr != '\0')
            return -1;
    } else
        return -1;

    /* Parse #digits */
    if ((t = apr_strtok(NULL, "/", &last)) == NULL)
        return 0;
    if (!isdigit(*t))
        return -1;
    tokinfo->num_digits = strtol(t, &eptr, 10);
    if (tokinfo->num_digits <= 0 || *eptr != '\0' || tokinfo->num_digits > sizeof(powers10) / sizeof(*powers10))
        return -1;

    /* Done */
    return 0;
}

static void
print_user(apr_file_t *file, const struct otp_user *user)
{
    const char *alg;
    char cbuf[64];
    char nbuf[64];
    char tbuf[128];
    int i;

    /* Format token type sub-fields */
    switch (user->algorithm) {
    case OTP_ALGORITHM_HOTP:
        alg = "HOTP";
        break;
    case OTP_ALGORITHM_MOTP:
        alg = "MOTP";
        break;
    default:
        alg = "???";
        break;
    }
    if (user->time_interval == 0)
        apr_snprintf(cbuf, sizeof(cbuf), "/E");
    else
        apr_snprintf(cbuf, sizeof(cbuf), "/T%d", user->time_interval);
    apr_snprintf(nbuf, sizeof(nbuf), "/%d", user->num_digits);

    /* Abbreviate when default values apply */
    if (user->num_digits == DEFAULT_NUM_DIGITS) {
        *nbuf = '\0';
        if (user->algorithm == OTP_ALGORITHM_HOTP && user->time_interval == 0)
            *cbuf = '\0';
        else if (user->algorithm == OTP_ALGORITHM_MOTP && user->time_interval == 10)
            *cbuf = '\0';
    }
    apr_snprintf(tbuf, sizeof(tbuf), "%s%s%s", alg, cbuf, nbuf);

    /* Print line in users file */
    apr_file_printf(file, "%-7s %-13s %-7s ", tbuf, user->username, *user->pin == '\0' ? "-" : user->pin);
    for (i = 0; i < user->keylen; i++)
        apr_file_printf(file, "%02x", user->key[i]);
    apr_file_printf(file, " %-7ld", user->offset);
    if (*user->last_otp != '\0') {
        strftime(tbuf, sizeof(tbuf), TIME_FORMAT, localtime(&user->last_auth));
        apr_file_printf(file, " %-7s %s", user->last_otp, tbuf);
    }
    apr_file_printf(file, "\n");
}

/*
 * Generate an OTP using the algorithm specified in RFC 4226,
 */
static void
hotp(const u_char *key, size_t keylen, u_long counter, int ndigits, char *buf10, char *buf16, size_t buflen)
{
    const int max10 = sizeof(powers10) / sizeof(*powers10);
    const int max16 = 8;
    const EVP_MD *sha1_md = EVP_sha1();
    u_char hash[EVP_MAX_MD_SIZE];
    u_int hash_len;
    u_char tosign[8];
    int offset;
    int value;
    int i;

    /* Encode counter */
    for (i = sizeof(tosign) - 1; i >= 0; i--) {
        tosign[i] = counter & 0xff;
        counter >>= 8;
    }

    /* Compute HMAC */
    HMAC(sha1_md, key, keylen, tosign, sizeof(tosign), hash, &hash_len);

    /* Extract selected bytes to get 32 bit integer value */
    offset = hash[hash_len - 1] & 0x0f;
    value = ((hash[offset] & 0x7f) << 24) | ((hash[offset + 1] & 0xff) << 16)
        | ((hash[offset + 2] & 0xff) << 8) | (hash[offset + 3] & 0xff);

    /* Sanity check max # digits */
    if (ndigits < 1)
        ndigits = 1;

    /* Generate decimal digits */
    if (buf10 != NULL) {
        apr_snprintf(buf10, buflen, "%0*d", ndigits < max10 ? ndigits : max10,
          ndigits < max10 ? value % powers10[ndigits - 1] : value);
    }

    /* Generate hexadecimal digits */
    if (buf16 != NULL) {
        apr_snprintf(buf16, buflen, "%0*x", ndigits < max16 ? ndigits : max16,
          ndigits < max16 ? (value & ((1 << (4 * ndigits)) - 1)) : value);
    }
}

/*
 * Generate an OTP using the mOTP algorithm defined by http://motp.sourceforge.net/
 */
static void
motp(const u_char *key, size_t keylen, const char *pin, u_long counter, int ndigits, char *buf, size_t buflen)
{
    u_char hash[MD5_DIGEST_LENGTH];
    char hashbuf[256];
    char keybuf[256];

    printhex(keybuf, sizeof(keybuf), key, keylen, keylen * 2);
    apr_snprintf(hashbuf, sizeof(hashbuf), "%lu%s%s", counter, keybuf, pin);
    MD5((u_char *)hashbuf, strlen(hashbuf), hash);
    printhex(buf, buflen, hash, sizeof(hash), ndigits);
}

/*
 * Print hex digits into a buffer.
 */
static void
printhex(char *buf, size_t buflen, const u_char *data, size_t dlen, int max_digits)
{
    const char *hexdig = "0123456789abcdef";
    int i;

    if (buflen > 0)
        *buf = '\0';
    for (i = 0; i / 2 < dlen && i < max_digits && i < buflen - 1; i++) {
        u_int val = data[i / 2];
        if ((i & 1) == 0)
            val >>= 4;
        val &= 0x0f;
        *buf++ = hexdig[val];
        *buf = '\0';
    }
}

/*
 * HTTP basic authentication
 */
static authn_status
authn_otp_check_password(request_rec *r, const char *username, const char *otp_given)
{
    struct otp_config *const conf = get_config(r);
    struct otp_user userbuf;
    struct otp_user *const user = &userbuf;
    authn_status status;
    int window_start;
    int window_stop;
    char otpbuf10[32];
    char otpbuf16[32];
    int counter;
    int offset;
    time_t now;

    /* Is the users file defined? */
    if (conf->users_file == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "No OTPAuthUsersFile has been configured");
        return AUTH_GENERAL_ERROR;
    }

    /* Lookup user in the users file */
    memset(user, 0, sizeof(*user));
    apr_snprintf(user->username, sizeof(user->username), "%s", username);
    if ((status = find_update_user(r, conf->users_file, user, 0)) != AUTH_USER_FOUND)
        return status;

    /* Check PIN prefix (if appropriate) */
    if (user->algorithm != OTP_ALGORITHM_MOTP) {
        if (strncmp(otp_given, user->pin, strlen(user->pin)) != 0) {
            ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, r, "user \"%s\" PIN does not match", user->username);
            return AUTH_DENIED;
        }
        otp_given += strlen(user->pin);
    }

    /* Check OTP length */
    if (strlen(otp_given) != user->num_digits) {
        ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, r, "user \"%s\" OTP has the wrong length %d != %d",
          user->username, (int)strlen(otp_given), user->num_digits);
        return AUTH_DENIED;
    }

    /* Check for reuse of previous OTP */
    now = time(NULL);
    if (strcmp(otp_given, user->last_otp) == 0) {

        /* Is it within the configured linger time? */
        if (now >= user->last_auth && now < user->last_auth + conf->max_linger) {
            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "accepting reuse of OTP for \"%s\" within %d sec. linger time",
              user->username, conf->max_linger);
            return AUTH_GRANTED;
        }

        /* Report failure to the log */
        ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, r, "user \"%s\" provided the previous OTP"
          " but it has expired (max linger is %d sec.)", user->username, conf->max_linger);
        return AUTH_DENIED;
    }

    /* Get expected counter value and offset window */
    if (user->time_interval == 0) {
        counter = user->offset;
        window_start = 1;
        window_stop = conf->max_offset;
    } else {
        counter = now / user->time_interval + user->offset;
        window_start = -conf->max_offset;
        window_stop = conf->max_offset;
    }

    /* Test OTP using expected counter first */
    *otpbuf10 = '\0';
    if (user->algorithm == OTP_ALGORITHM_MOTP)
        motp(user->key, user->keylen, user->pin, counter, user->num_digits, otpbuf16, OTP_BUF_SIZE);
    else
        hotp(user->key, user->keylen, counter, user->num_digits, otpbuf10, otpbuf16, OTP_BUF_SIZE);
    if (strcmp(otp_given, otpbuf10) == 0 || strcasecmp(otp_given, otpbuf16) == 0) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "accepting OTP for \"%s\" at counter %d", user->username, counter);
        offset = 0;
        goto success;
    }

    /* Try other OTP counter values within the maximum allowed offset */
    for (offset = window_start; offset <= window_stop; offset++) {
        if (offset == 0)    /* already tried it */
            continue;
        if (user->algorithm == OTP_ALGORITHM_MOTP)
            motp(user->key, user->keylen, user->pin, counter + offset, user->num_digits, otpbuf16, OTP_BUF_SIZE);
        else
            hotp(user->key, user->keylen, counter + offset, user->num_digits, otpbuf10, otpbuf16, OTP_BUF_SIZE);
        if (strcmp(otp_given, otpbuf10) == 0 || strcasecmp(otp_given, otpbuf16) == 0) {
            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "accepting OTP for \"%s\" at counter %d (offset adjust %d)",
              user->username, counter + offset, offset);
            goto success;
        }
    }

    /* Report failure to the log */
    ap_log_rerror(APLOG_MARK, APLOG_NOTICE, 0, r, "user \"%s\" provided the wrong OTP", user->username);
    return AUTH_DENIED;

success:
    /* Update user's last auth information and next expected offset */
    user->offset = user->time_interval == 0 ? counter + offset + 1 : user->offset + offset;
    apr_snprintf(user->last_otp, sizeof(user->last_otp), "%s", otp_given);
    user->last_auth = now;

    /* Update user's record */
    find_update_user(r, conf->users_file, user, 1);

    /* Done */
    return AUTH_GRANTED;
}

/*
 * HTTP digest authentication
 */
static authn_status
authn_otp_get_realm_hash(request_rec *r, const char *username, const char *realm, char **rethash)
{
    struct otp_config *const conf = get_config(r);
    struct otp_user userbuf;
    struct otp_user *const user = &userbuf;
    authn_status status;
    char hashbuf[256];
    char otpbuf[32];
    int counter = 0;
    int linger;
    time_t now;

    /* Is the users file configured? */
    if (conf->users_file == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "No OTPAuthUsersFile has been configured");
        return AUTH_GENERAL_ERROR;
    }

    /* Lookup the user in the users file */
    memset(user, 0, sizeof(*user));
    apr_snprintf(user->username, sizeof(user->username), "%s", username);
    if ((status = find_update_user(r, conf->users_file, user, 0)) != AUTH_USER_FOUND)
        return status;

    /* Determine the expected OTP, assuming OTP reuse if we are within the linger time */
    now = time(NULL);
    if (now >= user->last_auth && now < user->last_auth + conf->max_linger) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
          "generating digest hash for \"%s\" assuming reuse of OTP within %d sec. linger time",
          user->username, conf->max_linger);
        apr_snprintf(otpbuf, sizeof(otpbuf), "%s", user->last_otp);
        linger = 1;
    } else {

        /* Log note if previous OTP has expired */
        if (user->last_auth != 0 && *user->last_otp != '\0') {
            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "not using previous expired OTP for user \"%s\" (max linger is %d sec.)",
              user->username, conf->max_linger);
        }

        /* Get expected counter value */
        counter = user->time_interval == 0 ? user->offset : now / user->time_interval + user->offset;

        /* Generate OTP using expected counter */
        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "generating digest hash for \"%s\" assuming OTP counter %d",
          user->username, counter);
        if (user->algorithm == OTP_ALGORITHM_MOTP)
            motp(user->key, user->keylen, user->pin, counter, user->num_digits, otpbuf, OTP_BUF_SIZE);
        else
            hotp(user->key, user->keylen, counter, user->num_digits, otpbuf, NULL, OTP_BUF_SIZE);   /* assume decimal! */
        linger = 0;
    }

    /* Generate digest hash */
    apr_snprintf(hashbuf, sizeof(hashbuf), "%s:%s:%s%s", user->username, realm,
      user->algorithm == OTP_ALGORITHM_MOTP ? "" : user->pin, otpbuf);
    *rethash = ap_md5(r->pool, (void *)hashbuf);

#if 0
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "OTP=\"%s\" counter=%d user=\"%s\" realm=\"%s\" pin=\"%s\" digest=\"%s\"",
      otpbuf10, counter, user->username, realm, user->pin, *rethash);
#endif

    /* If we are past the previous linger time, assume counter advance and update user's info */
    if (!linger) {
        if (user->time_interval == 0)
            user->offset = counter + 1;
        apr_snprintf(user->last_otp, sizeof(user->last_otp), "%s", otpbuf);
        user->last_auth = now;
        find_update_user(r, conf->users_file, user, 1);
    }

    /* Done */
    return AUTH_USER_FOUND;
}

/*
 * Get configuration
 */
static struct otp_config *
get_config(request_rec *r)
{
    struct otp_config *dir_conf;
    struct otp_config *conf;

    /* I don't understand this bug: sometimes r->per_dir_config == NULL. Some weird linking problem. */
    if (r->per_dir_config == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Oops, bug detected in mod_authn_otp: r->per_dir_config == NULL?");
        dir_conf = create_authn_otp_dir_config(r->pool, NULL);
    } else
        dir_conf = ap_get_module_config(r->per_dir_config, &authn_otp_module);

    /* Make a copy of the current per-directory config */
    conf = apr_pcalloc(r->pool, sizeof(*conf));
    if (dir_conf->users_file != NULL)
        conf->users_file = apr_pstrdup(r->pool, dir_conf->users_file);
    conf->max_offset = dir_conf->max_offset;
    conf->max_linger = dir_conf->max_linger;

    /* Apply defaults for any unset values */
    if (conf->max_offset == -1)
        conf->max_offset = DEFAULT_MAX_OFFSET;
    if (conf->max_linger == -1)
        conf->max_linger = DEFAULT_MAX_LINGER;

    /* Done */
    return conf;
}

/*
 * Constructor for per-directory configuration
 */
static void *
create_authn_otp_dir_config(apr_pool_t *p, char *d)
{
    struct otp_config *conf = apr_pcalloc(p, sizeof(struct otp_config));

    conf->users_file = NULL;
    conf->max_offset = -1;
    conf->max_linger = -1;
    return conf;
}

static void *
merge_authn_otp_dir_config(apr_pool_t *p, void *base_conf, void *new_conf)
{
    struct otp_config *const conf1 = base_conf;
    struct otp_config *const conf2 = new_conf;
    struct otp_config *conf = apr_palloc(p, sizeof(struct otp_config));

    if (conf2->users_file != NULL)
        conf->users_file = apr_pstrdup(p, conf2->users_file);
    else if (conf1->users_file != NULL)
        conf->users_file = apr_pstrdup(p, conf1->users_file);
    conf->max_offset = conf2->max_offset != -1 ? conf2->max_offset : conf1->max_offset;
    conf->max_linger = conf2->max_linger != -1 ? conf2->max_linger : conf1->max_linger;
    return conf;
}

/* Authorization provider information */
static const authn_provider authn_otp_provider =
{
    &authn_otp_check_password,
    &authn_otp_get_realm_hash
};

static void
register_hooks(apr_pool_t *p)
{
    ap_register_provider(p, AUTHN_PROVIDER_GROUP, "OTP", "0", &authn_otp_provider);
}

/* Configuration directives */
static const command_rec authn_otp_cmds[] =
{
    AP_INIT_TAKE1("OTPAuthUsersFile",
        ap_set_file_slot,
        (void *)APR_OFFSETOF(struct otp_config, users_file),
        OR_AUTHCFG,
        "pathname of the one-time password users file"),
    AP_INIT_TAKE1("OTPAuthMaxOffset",
        ap_set_int_slot,
        (void *)APR_OFFSETOF(struct otp_config, max_offset),
        OR_AUTHCFG,
        "maximum allowed offset from expected event or time counter value"),
    AP_INIT_TAKE1("OTPAuthMaxLinger",
        ap_set_int_slot,
        (void *)APR_OFFSETOF(struct otp_config, max_linger),
        OR_AUTHCFG,
        "maximum time (in seconds) for which a one-time password can be repeatedly used"),
    { NULL }
};

/* Module declaration */
module AP_MODULE_DECLARE_DATA authn_otp_module = {
    STANDARD20_MODULE_STUFF,
    create_authn_otp_dir_config,        /* create per-dir config */
    merge_authn_otp_dir_config,         /* merge per-dir config */
    NULL,                               /* create per-server config */
    NULL,                               /* merge per-server config */
    authn_otp_cmds,                     /* command apr_table_t */
    register_hooks                      /* register hooks */
};

