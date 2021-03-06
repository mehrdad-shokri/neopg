/* server.c - Keyserver access server
 * Copyright (C) 2002 Klarälvdalens Datakonsult AB
 * Copyright (C) 2003, 2004, 2005, 2007, 2008, 2009, 2011, 2015 g10 Code GmbH
 * Copyright (C) 2014, 2015, 2016 Werner Koch
 * Copyright (C) 2016 Bundesamt für Sicherheit in der Informationstechnik
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <config.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <botan/hash.h>
#include <botan/hex.h>
#include <sstream>

#include <assuan.h>
#include "dirmngr.h"

#include "../common/mbox-util.h"
#include "../common/server-help.h"
#include "../common/zb32.h"
#include "certcache.h"
#include "crlcache.h"
#include "crlfetch.h"
#include "ks-action.h"
#include "misc.h"
#include "ocsp.h"
#include "validate.h"

/* To avoid DoS attacks we limit the size of a certificate to
   something reasonable.  The DoS was actually only an issue back when
   Dirmngr was a system service and not a user service. */
#define MAX_CERT_LENGTH (16 * 1024)

/* The limit for the CERTLIST inquiry.  We allow for up to 20
 * certificates but also take PEM encoding into account.  */
#define MAX_CERTLIST_LENGTH ((MAX_CERT_LENGTH * 20 * 4) / 3)

/* The same goes for OpenPGP keyblocks, but here we need to allow for
   much longer blocks; a 200k keyblock is not too unusual for keys
   with a lot of signatures (e.g. 0x5b0358a2).  9C31503C6D866396 even
   has 770 KiB as of 2015-08-23.  To avoid adding a runtime option we
   now use 20MiB which should really be enough.  Well, a key with
   several pictures could be larger (the parser as a 18MiB limit for
   attribute packets) but it won't be nice to the keyservers to send
   them such large blobs.  */
#define MAX_KEYBLOCK_LENGTH (20 * 1024 * 1024)

#define PARM_ERROR(t) assuan_set_error(ctx, GPG_ERR_ASS_PARAMETER, (t))
#define set_error(e, t) assuan_set_error(ctx, e, (t))

/* Control structure per connection. */
struct server_local_s {
  /* Data used to associate an Assuan context with local server data */
  assuan_context_t assuan_ctx;

  /* Per-session list of keyservers.  */
  uri_item_t keyservers;

  /* If this flag is set to true this dirmngr process will be
     terminated after the end of this session.  */
  int stopme;
};

/* Release an uri_item_t list.  */
static void release_uri_item_list(uri_item_t list) {
  while (list) {
    uri_item_t tmp = list->next;
    http_release_parsed_uri(list->parsed_uri);
    xfree(list);
    list = tmp;
  }
}

/* Release all configured keyserver info from CTRL.  */
void release_ctrl_keyservers(ctrl_t ctrl) {
  if (!ctrl->server_local) return;

  release_uri_item_list(ctrl->server_local->keyservers);
  ctrl->server_local->keyservers = NULL;
}

/* Helper to print a message while leaving a command.  */
static gpg_error_t leave_cmd(assuan_context_t ctx, gpg_error_t err) {
  if (err) {
    const char *name = assuan_get_command_name(ctx);
    if (!name) name = "?";
    log_error("command '%s' failed: %s\n", name, gpg_strerror(err));
  }
  return err;
}

/* Copy the % and + escaped string S into the buffer D and replace the
   escape sequences.  Note, that it is sufficient to allocate the
   target string D as long as the source string S, i.e.: strlen(s)+1.
   Note further that if S contains an escaped binary Nul the resulting
   string D will contain the 0 as well as all other characters but it
   will be impossible to know whether this is the original EOS or a
   copied Nul. */
static void strcpy_escaped_plus(char *d, const unsigned char *s) {
  while (*s) {
    if (*s == '%' && s[1] && s[2]) {
      s++;
      *d++ = xtoi_2(s);
      s += 2;
    } else if (*s == '+')
      *d++ = ' ', s++;
    else
      *d++ = *s++;
  }
  *d = 0;
}

/* Common code for get_cert_local and get_issuer_cert_local. */
static ksba_cert_t do_get_cert_local(ctrl_t ctrl, const char *name,
                                     const char *command) {
  unsigned char *value;
  size_t valuelen;
  int rc;
  char *buf;
  ksba_cert_t cert;

  buf = name ? strconcat(command, " ", name, NULL) : xtrystrdup(command);
  if (!buf)
    rc = gpg_error_from_syserror();
  else {
    rc = assuan_inquire(ctrl->server_local->assuan_ctx, buf, &value, &valuelen,
                        MAX_CERT_LENGTH);
    xfree(buf);
  }
  if (rc) {
    log_error(_("assuan_inquire(%s) failed: %s\n"), command, gpg_strerror(rc));
    return NULL;
  }

  if (!valuelen) {
    xfree(value);
    return NULL;
  }

  rc = ksba_cert_new(&cert);
  if (!rc) {
    rc = ksba_cert_init_from_mem(cert, value, valuelen);
    if (rc) {
      ksba_cert_release(cert);
      cert = NULL;
    }
  }
  xfree(value);
  return cert;
}

/* Ask back to return a certificate for NAME, given as a regular gpgsm
 * certificate identifier (e.g. fingerprint or one of the other
 * methods).  Alternatively, NULL may be used for NAME to return the
 * current target certificate.  Either return the certificate in a
 * KSBA object or NULL if it is not available.  */
ksba_cert_t get_cert_local(ctrl_t ctrl, const char *name) {
  if (!ctrl || !ctrl->server_local || !ctrl->server_local->assuan_ctx) {
    if (opt.debug) log_debug("get_cert_local called w/o context\n");
    return NULL;
  }
  return do_get_cert_local(ctrl, name, "SENDCERT");
}

/* Ask back to return the issuing certificate for NAME, given as a
 * regular gpgsm certificate identifier (e.g. fingerprint or one
 * of the other methods).  Alternatively, NULL may be used for NAME to
 * return the current target certificate. Either return the certificate
 * in a KSBA object or NULL if it is not available.  */
ksba_cert_t get_issuing_cert_local(ctrl_t ctrl, const char *name) {
  if (!ctrl || !ctrl->server_local || !ctrl->server_local->assuan_ctx) {
    if (opt.debug) log_debug("get_issuing_cert_local called w/o context\n");
    return NULL;
  }
  return do_get_cert_local(ctrl, name, "SENDISSUERCERT");
}

/* Ask back to return a certificate with subject NAME and a
 * subjectKeyIdentifier of KEYID. */
ksba_cert_t get_cert_local_ski(ctrl_t ctrl, const char *name,
                               ksba_sexp_t keyid) {
  unsigned char *value;
  size_t valuelen;
  int rc;
  char *buf;
  ksba_cert_t cert;
  char *hexkeyid;

  if (!ctrl || !ctrl->server_local || !ctrl->server_local->assuan_ctx) {
    if (opt.debug) log_debug("get_cert_local_ski called w/o context\n");
    return NULL;
  }
  if (!name || !keyid) {
    log_debug("get_cert_local_ski called with insufficient arguments\n");
    return NULL;
  }

  hexkeyid = serial_hex(keyid);
  if (!hexkeyid) {
    log_debug("serial_hex() failed\n");
    return NULL;
  }

  buf = strconcat("SENDCERT_SKI ", hexkeyid, " /", name, NULL);
  if (!buf) {
    log_error("can't allocate enough memory: %s\n", strerror(errno));
    xfree(hexkeyid);
    return NULL;
  }
  xfree(hexkeyid);

  rc = assuan_inquire(ctrl->server_local->assuan_ctx, buf, &value, &valuelen,
                      MAX_CERT_LENGTH);
  xfree(buf);
  if (rc) {
    log_error(_("assuan_inquire(%s) failed: %s\n"), "SENDCERT_SKI",
              gpg_strerror(rc));
    return NULL;
  }

  if (!valuelen) {
    xfree(value);
    return NULL;
  }

  rc = ksba_cert_new(&cert);
  if (!rc) {
    rc = ksba_cert_init_from_mem(cert, value, valuelen);
    if (rc) {
      ksba_cert_release(cert);
      cert = NULL;
    }
  }
  xfree(value);
  return cert;
}

/* Ask the client via an inquiry to check the istrusted status of the
   certificate specified by the hexified fingerprint HEXFPR.  Returns
   0 if the certificate is trusted by the client or an error code.  */
gpg_error_t get_istrusted_from_client(ctrl_t ctrl, const char *hexfpr) {
  unsigned char *value;
  size_t valuelen;
  int rc;
  char request[100];

  if (!ctrl || !ctrl->server_local || !ctrl->server_local->assuan_ctx ||
      !hexfpr)
    return GPG_ERR_INV_ARG;

  snprintf(request, sizeof request, "ISTRUSTED %s", hexfpr);
  rc = assuan_inquire(ctrl->server_local->assuan_ctx, request, &value,
                      &valuelen, 100);
  if (rc) {
    log_error(_("assuan_inquire(%s) failed: %s\n"), request, gpg_strerror(rc));
    return rc;
  }
  /* The expected data is: "1" or "1 cruft" (not a C-string).  */
  if (valuelen && *value == '1' && (valuelen == 1 || spacep(value + 1)))
    rc = 0;
  else
    rc = GPG_ERR_NOT_TRUSTED;
  xfree(value);
  return rc;
}

/* Ask the client to return the certificate associated with the
   current command. This is sometimes needed because the client usually
   sends us just the cert ID, assuming that the request can be
   satisfied from the cache, where the cert ID is used as key. */
static int inquire_cert_and_load_crl(assuan_context_t ctx) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err;
  unsigned char *value = NULL;
  size_t valuelen;
  ksba_cert_t cert = NULL;

  err = assuan_inquire(ctx, "SENDCERT", &value, &valuelen, 0);
  if (err) return err;

  /*   { */
  /*     FILE *fp = fopen ("foo.der", "r"); */
  /*     value = xmalloc (2000); */
  /*     valuelen = fread (value, 1, 2000, fp); */
  /*     fclose (fp); */
  /*   } */

  if (!valuelen) /* No data returned; return a comprehensible error. */
    return GPG_ERR_MISSING_CERT;

  err = ksba_cert_new(&cert);
  if (err) goto leave;
  err = ksba_cert_init_from_mem(cert, value, valuelen);
  if (err) goto leave;
  xfree(value);
  value = NULL;

  err = crl_cache_reload_crl(ctrl, cert);

leave:
  ksba_cert_release(cert);
  xfree(value);
  return err;
}

/* Handle OPTION commands. */
static gpg_error_t option_handler(assuan_context_t ctx, const char *key,
                                  const char *value) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err = 0;

  if (!strcmp(key, "force-crl-refresh")) {
    int i = *value ? atoi(value) : 0;
    ctrl->force_crl_refresh = i;
  } else if (!strcmp(key, "http-proxy")) {
    xfree(ctrl->http_proxy);
    if (!*value || !strcmp(value, "none"))
      ctrl->http_proxy = NULL;
    else if (!(ctrl->http_proxy = xtrystrdup(value)))
      err = gpg_error_from_syserror();
  } else if (!strcmp(key, "http-crl")) {
    int i = *value ? atoi(value) : 0;
    ctrl->http_no_crl = !i;
  } else
    err = GPG_ERR_UNKNOWN_OPTION;

  return err;
}

static const char hlp_isvalid[] =
    "ISVALID [--only-ocsp] [--force-default-responder]"
    " <certificate_id>|<certificate_fpr>\n"
    "\n"
    "This command checks whether the certificate identified by the\n"
    "certificate_id is valid.  This is done by consulting CRLs or\n"
    "whatever has been configured.  Note, that the returned error codes\n"
    "are from gpg-error.h.  The command may callback using the inquire\n"
    "function.  See the manual for details.\n"
    "\n"
    "The CERTIFICATE_ID is a hex encoded string consisting of two parts,\n"
    "delimited by a single dot.  The first part is the SHA-1 hash of the\n"
    "issuer name and the second part the serial number.\n"
    "\n"
    "Alternatively the certificate's fingerprint may be given in which\n"
    "case an OCSP request is done before consulting the CRL.\n"
    "\n"
    "If the option --only-ocsp is given, no fallback to a CRL check will\n"
    "be used.\n"
    "\n"
    "If the option --force-default-responder is given, only the default\n"
    "OCSP responder will be used and any other methods of obtaining an\n"
    "OCSP responder URL won't be used.";
static gpg_error_t cmd_isvalid(assuan_context_t ctx, char *line) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  char *issuerhash, *serialno;
  gpg_error_t err;
  int did_inquire = 0;
  int ocsp_mode = 0;
  int only_ocsp;
  int force_default_responder;

  only_ocsp = has_option(line, "--only-ocsp");
  force_default_responder = has_option(line, "--force-default-responder");
  line = skip_options(line);

  issuerhash = xstrdup(line); /* We need to work on a copy of the
                                 line because that same Assuan
                                 context may be used for an inquiry.
                                 That is because Assuan reuses its
                                 line buffer.
                                  */

  serialno = strchr(issuerhash, '.');
  if (serialno)
    *serialno++ = 0;
  else {
    char *endp = strchr(issuerhash, ' ');
    if (endp) *endp = 0;
    if (strlen(issuerhash) != 40) {
      xfree(issuerhash);
      return leave_cmd(ctx, PARM_ERROR(_("serialno missing in cert ID")));
    }
    ocsp_mode = 1;
  }

again:
  if (ocsp_mode) {
    /* Note, that we ignore the given issuer hash and instead rely
       on the current certificate semantics used with this
       command. */
    if (!opt.allow_ocsp)
      err = GPG_ERR_NOT_SUPPORTED;
    else
      err = ocsp_isvalid(ctrl, NULL, NULL, force_default_responder);
    /* Fixme: If we got no ocsp response and --only-ocsp is not used
       we should fall back to CRL mode.  Thus we need to clear
       OCSP_MODE, get the issuerhash and the serialno from the
       current certificate and jump to again. */
  } else if (only_ocsp)
    err = GPG_ERR_NO_CRL_KNOWN;
  else {
    switch (crl_cache_isvalid(ctrl, issuerhash, serialno,
                              ctrl->force_crl_refresh)) {
      case CRL_CACHE_VALID:
        err = 0;
        break;
      case CRL_CACHE_INVALID:
        err = GPG_ERR_CERT_REVOKED;
        break;
      case CRL_CACHE_DONTKNOW:
        if (did_inquire)
          err = GPG_ERR_NO_CRL_KNOWN;
        else if (!(err = inquire_cert_and_load_crl(ctx))) {
          did_inquire = 1;
          goto again;
        }
        break;
      case CRL_CACHE_CANTUSE:
        err = GPG_ERR_NO_CRL_KNOWN;
        break;
      default:
        log_fatal("crl_cache_isvalid returned invalid code\n");
    }
  }

  xfree(issuerhash);
  return leave_cmd(ctx, err);
}

/* If the line contains a SHA-1 fingerprint as the first argument,
   return the FPR vuffer on success.  The function checks that the
   fingerprint consists of valid characters and prints and error
   message if it does not and returns NULL.  Fingerprints are
   considered optional and thus no explicit error is returned. NULL is
   also returned if there is no fingerprint at all available.
   FPR must be a caller provided buffer of at least 20 bytes.

   Note that colons within the fingerprint are allowed to separate 2
   hex digits; this allows for easier cutting and pasting using the
   usual fingerprint rendering.
*/
static unsigned char *get_fingerprint_from_line(const char *line,
                                                unsigned char *fpr) {
  const char *s;
  int i;

  for (s = line, i = 0; *s && *s != ' '; s++) {
    if (hexdigitp(s) && hexdigitp(s + 1)) {
      if (i >= 20) return NULL; /* Fingerprint too long.  */
      fpr[i++] = xtoi_2(s);
      s++;
    } else if (*s != ':')
      return NULL; /* Invalid.  */
  }
  if (i != 20) return NULL; /* Fingerprint to short.  */
  return fpr;
}

static const char hlp_checkcrl[] =
    "CHECKCRL [<fingerprint>]\n"
    "\n"
    "Check whether the certificate with FINGERPRINT (SHA-1 hash of the\n"
    "entire X.509 certificate blob) is valid or not by consulting the\n"
    "CRL responsible for this certificate.  If the fingerprint has not\n"
    "been given or the certificate is not known, the function \n"
    "inquires the certificate using an\n"
    "\n"
    "  INQUIRE TARGETCERT\n"
    "\n"
    "and the caller is expected to return the certificate for the\n"
    "request (which should match FINGERPRINT) as a binary blob.\n"
    "Processing then takes place without further interaction; in\n"
    "particular dirmngr tries to locate other required certificate by\n"
    "its own mechanism which includes a local certificate store as well\n"
    "as a list of trusted root certificates.\n"
    "\n"
    "The return value is the usual gpg-error code or 0 for ducesss;\n"
    "i.e. the certificate validity has been confirmed by a valid CRL.";
static gpg_error_t cmd_checkcrl(assuan_context_t ctx, char *line) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err;
  unsigned char fprbuffer[20], *fpr;
  ksba_cert_t cert;

  fpr = get_fingerprint_from_line(line, fprbuffer);
  cert = fpr ? get_cert_byfpr(fpr) : NULL;

  if (!cert) {
    /* We do not have this certificate yet or the fingerprint has
       not been given.  Inquire it from the client.  */
    unsigned char *value = NULL;
    size_t valuelen;

    err = assuan_inquire(ctrl->server_local->assuan_ctx, "TARGETCERT", &value,
                         &valuelen, MAX_CERT_LENGTH);
    if (err) {
      log_error(_("assuan_inquire failed: %s\n"), gpg_strerror(err));
      goto leave;
    }

    if (!valuelen) /* No data returned; return a comprehensible error. */
      err = GPG_ERR_MISSING_CERT;
    else {
      err = ksba_cert_new(&cert);
      if (!err) err = ksba_cert_init_from_mem(cert, value, valuelen);
    }
    xfree(value);
    if (err) goto leave;
  }

  assert(cert);

  err = crl_cache_cert_isvalid(ctrl, cert, ctrl->force_crl_refresh);
  if (err == GPG_ERR_NO_CRL_KNOWN) {
    err = crl_cache_reload_crl(ctrl, cert);
    if (!err) err = crl_cache_cert_isvalid(ctrl, cert, 0);
  }

leave:
  ksba_cert_release(cert);
  return leave_cmd(ctx, err);
}

static const char hlp_checkocsp[] =
    "CHECKOCSP [--force-default-responder] [<fingerprint>]\n"
    "\n"
    "Check whether the certificate with FINGERPRINT (SHA-1 hash of the\n"
    "entire X.509 certificate blob) is valid or not by asking an OCSP\n"
    "responder responsible for this certificate.  The optional\n"
    "fingerprint may be used for a quick check in case an OCSP check has\n"
    "been done for this certificate recently (we always cache OCSP\n"
    "responses for a couple of minutes). If the fingerprint has not been\n"
    "given or there is no cached result, the function inquires the\n"
    "certificate using an\n"
    "\n"
    "   INQUIRE TARGETCERT\n"
    "\n"
    "and the caller is expected to return the certificate for the\n"
    "request (which should match FINGERPRINT) as a binary blob.\n"
    "Processing then takes place without further interaction; in\n"
    "particular dirmngr tries to locate other required certificates by\n"
    "its own mechanism which includes a local certificate store as well\n"
    "as a list of trusted root certificates.\n"
    "\n"
    "If the option --force-default-responder is given, only the default\n"
    "OCSP responder will be used and any other methods of obtaining an\n"
    "OCSP responder URL won't be used.\n"
    "\n"
    "The return value is the usual gpg-error code or 0 for ducesss;\n"
    "i.e. the certificate validity has been confirmed by a valid CRL.";
static gpg_error_t cmd_checkocsp(assuan_context_t ctx, char *line) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err;
  unsigned char fprbuffer[20], *fpr;
  ksba_cert_t cert;
  int force_default_responder;

  force_default_responder = has_option(line, "--force-default-responder");
  line = skip_options(line);

  fpr = get_fingerprint_from_line(line, fprbuffer);
  cert = fpr ? get_cert_byfpr(fpr) : NULL;

  if (!cert) {
    /* We do not have this certificate yet or the fingerprint has
       not been given.  Inquire it from the client.  */
    unsigned char *value = NULL;
    size_t valuelen;

    err = assuan_inquire(ctrl->server_local->assuan_ctx, "TARGETCERT", &value,
                         &valuelen, MAX_CERT_LENGTH);
    if (err) {
      log_error(_("assuan_inquire failed: %s\n"), gpg_strerror(err));
      goto leave;
    }

    if (!valuelen) /* No data returned; return a comprehensible error. */
      err = GPG_ERR_MISSING_CERT;
    else {
      err = ksba_cert_new(&cert);
      if (!err) err = ksba_cert_init_from_mem(cert, value, valuelen);
    }
    xfree(value);
    if (err) goto leave;
  }

  assert(cert);

  if (!opt.allow_ocsp)
    err = GPG_ERR_NOT_SUPPORTED;
  else
    err = ocsp_isvalid(ctrl, cert, NULL, force_default_responder);

leave:
  ksba_cert_release(cert);
  return leave_cmd(ctx, err);
}

static int lookup_cert_by_url(assuan_context_t ctx, const char *url) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err = 0;
  unsigned char *value = NULL;
  size_t valuelen;

  /* Fetch single certificate given it's URL.  */
  err = fetch_cert_by_url(ctrl, url, &value, &valuelen);
  if (err) {
    log_error(_("fetch_cert_by_url failed: %s\n"), gpg_strerror(err));
    goto leave;
  }

  /* Send the data, flush the buffer and then send an END. */
  err = assuan_send_data(ctx, value, valuelen);
  if (!err) err = assuan_send_data(ctx, NULL, 0);
  if (!err) err = assuan_write_line(ctx, "END");
  if (err) {
    log_error(_("error sending data: %s\n"), gpg_strerror(err));
    goto leave;
  }

leave:

  return err;
}

/* Send the certificate, flush the buffer and then send an END. */
static gpg_error_t return_one_cert(void *opaque, ksba_cert_t cert) {
  assuan_context_t ctx = (assuan_context_t)opaque;
  gpg_error_t err;
  const unsigned char *der;
  size_t derlen;

  der = ksba_cert_get_image(cert, &derlen);
  if (!der)
    err = GPG_ERR_INV_CERT_OBJ;
  else {
    err = assuan_send_data(ctx, der, derlen);
    if (!err) err = assuan_send_data(ctx, NULL, 0);
    if (!err) err = assuan_write_line(ctx, "END");
  }
  if (err) log_error(_("error sending data: %s\n"), gpg_strerror(err));
  return err;
}

/* Lookup certificates from the internal cache.  */
static int lookup_cert_by_pattern(assuan_context_t ctx, char *line, int single,
                                  int cache_only) {
  gpg_error_t err = 0;
  char *p;
  std::vector<std::string> list;
  int truncated = 0, truncation_forced = 0;
  int count = 0;
  int local_count = 0;
  int any_no_data = 0;

  /* Break the line down into an list. */
  for (p = line; *p; line = p) {
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;

    if (*line) {
      char *sl = (char *)xtrymalloc(strlen(line));
      if (!sl) {
        err = gpg_error_from_errno(errno);
        goto leave;
      }
      strcpy_escaped_plus(sl, (const unsigned char *)(line));
      list.emplace_back(sl);
    }
  }

  /* First look through the internal cache.  The certificates returned
     here are not counted towards the truncation limit.  */
  if (single && !cache_only)
    ; /* Do not read from the local cache in this case.  */
  else {
    for (auto &el : list) {
      err = get_certs_bypattern(el.c_str(), return_one_cert, ctx);
      if (!err) local_count++;
      if (!err && single) goto ready;

      if (err == GPG_ERR_NO_DATA) {
        err = 0;
        if (cache_only) any_no_data = 1;
      } else if (err == GPG_ERR_INV_NAME && !cache_only) {
        /* No real fault because the internal pattern lookup
           can't yet cope with all types of pattern.  */
        err = 0;
      }
      if (err) goto ready;
    }
  }

/* Loop over all configured servers unless we want only the
   certificates from the cache.  */

ready:
  if (truncated || truncation_forced) {
    char str[50];

    sprintf(str, "%d", count);
    assuan_write_status(ctx, "TRUNCATED", str);
  }

  if (!err && !count && !local_count && any_no_data) err = GPG_ERR_NO_DATA;

leave:
  return err;
}

static const char hlp_lookup[] =
    "LOOKUP [--url] [--single] [--cache-only] <pattern>\n"
    "\n"
    "Lookup certificates matching PATTERN. With --url the pattern is\n"
    "expected to be one URL.\n"
    "\n"
    "If --url is not given:  To allow for multiple patterns (which are ORed)\n"
    "quoting is required: Spaces are translated to \"+\" or \"%20\";\n"
    "obviously this requires that the usual escape quoting rules are applied.\n"
    "\n"
    "If --url is given no special escaping is required because URLs are\n"
    "already escaped this way.\n"
    "\n"
    "If --single is given the first and only the first match will be\n"
    "returned.  If --cache-only is _not_ given, no local query will be\n"
    "done.\n"
    "\n"
    "If --cache-only is given no external lookup is done so that only\n"
    "certificates from the cache may get returned.";
static gpg_error_t cmd_lookup(assuan_context_t ctx, char *line) {
  gpg_error_t err;
  int lookup_url, single, cache_only;

  lookup_url = has_leading_option(line, "--url");
  single = has_leading_option(line, "--single");
  cache_only = has_leading_option(line, "--cache-only");
  line = skip_options(line);

  if (lookup_url && cache_only)
    err = GPG_ERR_NOT_FOUND;
  else if (lookup_url && single)
    err = GPG_ERR_NOT_IMPLEMENTED;
  else if (lookup_url)
    err = lookup_cert_by_url(ctx, line);
  else
    err = lookup_cert_by_pattern(ctx, line, single, cache_only);

  return leave_cmd(ctx, err);
}

static const char hlp_loadcrl[] =
    "LOADCRL [--url] <filename|url>\n"
    "\n"
    "Load the CRL in the file with name FILENAME into our cache.  Note\n"
    "that FILENAME should be given with an absolute path because\n"
    "Dirmngrs cwd is not known.  With --url the CRL is directly loaded\n"
    "from the given URL.\n"
    "\n"
    "This command is usually used by gpgsm using the invocation \"gpgsm\n"
    "--call-dirmngr loadcrl <filename>\".  A direct invocation of Dirmngr\n"
    "is not useful because gpgsm might need to callback gpgsm to ask for\n"
    "the CA's certificate.";
static gpg_error_t cmd_loadcrl(assuan_context_t ctx, char *line) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err = 0;
  int use_url = has_leading_option(line, "--url");

  line = skip_options(line);

  if (use_url) {
    ksba_reader_t reader;

    err = crl_fetch(ctrl, line, &reader);
    if (err)
      log_error(_("fetching CRL from '%s' failed: %s\n"), line,
                gpg_strerror(err));
    else {
      err = crl_cache_insert(ctrl, line, reader);
      if (err)
        log_error(_("processing CRL from '%s' failed: %s\n"), line,
                  gpg_strerror(err));
      crl_close_reader(reader);
    }
  } else {
    char *buf;

    buf = (char *)xtrymalloc(strlen(line) + 1);
    if (!buf)
      err = gpg_error_from_syserror();
    else {
      strcpy_escaped_plus(buf, (const unsigned char *)(line));
      err = crl_cache_load(ctrl, buf);
      xfree(buf);
    }
  }

  return leave_cmd(ctx, err);
}

static const char hlp_listcrls[] =
    "LISTCRLS\n"
    "\n"
    "List the content of all CRLs in a readable format.  This command is\n"
    "usually used by gpgsm using the invocation \"gpgsm --call-dirmngr\n"
    "listcrls\".  It may also be used directly using \"dirmngr\n"
    "--list-crls\".";
static gpg_error_t cmd_listcrls(assuan_context_t ctx, char *line) {
  gpg_error_t err;
  std::stringstream list;

  (void)line;

  err = crl_cache_list(list);
  if (err) leave_cmd(ctx, err);
  std::string output = list.str();

  err = assuan_send_data(ctx, output.data(), output.size());
  if (!err) err = assuan_send_data(ctx, NULL, 0);

  return leave_cmd(ctx, err);
}

static const char hlp_cachecert[] =
    "CACHECERT\n"
    "\n"
    "Put a certificate into the internal cache.  This command might be\n"
    "useful if a client knows in advance certificates required for a\n"
    "test and wants to make sure they get added to the internal cache.\n"
    "It is also helpful for debugging.  To get the actual certificate,\n"
    "this command immediately inquires it using\n"
    "\n"
    "  INQUIRE TARGETCERT\n"
    "\n"
    "and the caller is expected to return the certificate for the\n"
    "request as a binary blob.";
static gpg_error_t cmd_cachecert(assuan_context_t ctx, char *line) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err;
  ksba_cert_t cert = NULL;
  unsigned char *value = NULL;
  size_t valuelen;

  (void)line;

  err = assuan_inquire(ctrl->server_local->assuan_ctx, "TARGETCERT", &value,
                       &valuelen, MAX_CERT_LENGTH);
  if (err) {
    log_error(_("assuan_inquire failed: %s\n"), gpg_strerror(err));
    goto leave;
  }

  if (!valuelen) /* No data returned; return a comprehensible error. */
    err = GPG_ERR_MISSING_CERT;
  else {
    err = ksba_cert_new(&cert);
    if (!err) err = ksba_cert_init_from_mem(cert, value, valuelen);
  }
  xfree(value);
  if (err) goto leave;

  err = cache_cert(cert);

leave:
  ksba_cert_release(cert);
  return leave_cmd(ctx, err);
}

static const char hlp_validate[] =
    "VALIDATE [--systrust] [--tls] [--no-crl]\n"
    "\n"
    "Validate a certificate using the certificate validation function\n"
    "used internally by dirmngr.  This command is only useful for\n"
    "debugging.  To get the actual certificate, this command immediately\n"
    "inquires it using\n"
    "\n"
    "  INQUIRE TARGETCERT\n"
    "\n"
    "and the caller is expected to return the certificate for the\n"
    "request as a binary blob.  The option --tls modifies this by asking\n"
    "for list of certificates with\n"
    "\n"
    "  INQUIRE CERTLIST\n"
    "\n"
    "Here the first certificate is the target certificate, the remaining\n"
    "certificates are suggested intermediary certificates.  All certifciates\n"
    "need to be PEM encoded.\n"
    "\n"
    "The option --systrust changes the behaviour to include the system\n"
    "provided root certificates as trust anchors.  The option --no-crl\n"
    "skips CRL checks";
static gpg_error_t cmd_validate(assuan_context_t ctx, char *line) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err;
  ksba_cert_t cert = NULL;
  certlist_t certlist = NULL;
  unsigned char *value = NULL;
  size_t valuelen;
  int systrust_mode, tls_mode, no_crl;

  systrust_mode = has_option(line, "--systrust");
  tls_mode = has_option(line, "--tls");
  no_crl = has_option(line, "--no-crl");
  line = skip_options(line);

  if (tls_mode)
    err = assuan_inquire(ctrl->server_local->assuan_ctx, "CERTLIST", &value,
                         &valuelen, MAX_CERTLIST_LENGTH);
  else
    err = assuan_inquire(ctrl->server_local->assuan_ctx, "TARGETCERT", &value,
                         &valuelen, MAX_CERT_LENGTH);
  if (err) {
    log_error(_("assuan_inquire failed: %s\n"), gpg_strerror(err));
    goto leave;
  }

  if (!valuelen) /* No data returned; return a comprehensible error. */
    err = GPG_ERR_MISSING_CERT;
  else if (tls_mode) {
    estream_t fp;

    fp = es_fopenmem_init(0, "rb", value, valuelen);
    if (!fp)
      err = gpg_error_from_syserror();
    else {
      err = read_certlist_from_stream(&certlist, fp);
      es_fclose(fp);
      if (!err && !certlist) err = GPG_ERR_MISSING_CERT;
      if (!err) {
        /* Extraxt the first certificate from the list.  */
        cert = certlist->cert;
        ksba_cert_ref(cert);
      }
    }
  } else {
    err = ksba_cert_new(&cert);
    if (!err) err = ksba_cert_init_from_mem(cert, value, valuelen);
  }
  xfree(value);
  if (err) goto leave;

  if (!tls_mode) {
    /* If we have this certificate already in our cache, use the
     * cached version for validation because this will take care of
     * any cached results.  We don't need to do this in tls mode
     * because this has already been done for certificate in a
     * certlist_t. */
    unsigned char fpr[20];
    ksba_cert_t tmpcert;

    cert_compute_fpr(cert, fpr);
    tmpcert = get_cert_byfpr(fpr);
    if (tmpcert) {
      ksba_cert_release(cert);
      cert = tmpcert;
    }
  }

  /* Quick hack to make verification work by inserting the supplied
   * certs into the cache.  */
  if (tls_mode && certlist) {
    certlist_t cl;

    for (cl = certlist->next; cl; cl = cl->next) cache_cert(cl->cert);
  }

  err = validate_cert_chain(
      ctrl, cert, NULL,
      (VALIDATE_FLAG_TRUST_CONFIG | (tls_mode ? VALIDATE_FLAG_TLS : 0) |
       (systrust_mode ? VALIDATE_FLAG_TRUST_SYSTEM : 0) |
       (no_crl ? VALIDATE_FLAG_NOCRLCHECK : 0)),
      NULL);

leave:
  ksba_cert_release(cert);
  release_certlist(certlist);
  return leave_cmd(ctx, err);
}

/* Parse an keyserver URI and store it in a new uri item which is
   returned at R_ITEM.  On error return an error code.  */
static gpg_error_t make_keyserver_item(const char *uri, uri_item_t *r_item) {
  gpg_error_t err;
  uri_item_t item;

  *r_item = NULL;
  item = (uri_item_t)xtrymalloc(sizeof *item + strlen(uri));
  if (!item) return gpg_error_from_syserror();

  item->next = NULL;
  item->parsed_uri = NULL;
  strcpy(item->uri, uri);

  { err = http_parse_uri(&item->parsed_uri, uri, 1); }

  if (err)
    xfree(item);
  else
    *r_item = item;
  return err;
}

/* If no keyserver is stored in CTRL but a global keyserver has been
   set, put that global keyserver into CTRL.  We need use this
   function to help migrate from the old gpg based keyserver
   configuration to the new dirmngr based configuration.  */
static gpg_error_t ensure_keyserver(ctrl_t ctrl) {
  gpg_error_t err;
  uri_item_t item;

  if (ctrl->server_local->keyservers)
    return 0; /* Already set for this session.  */
  if (opt.keyserver.empty()) {
    /* No global option set.  Fall back to default:  */
    return make_keyserver_item(DIRMNGR_DEFAULT_KEYSERVER,
                               &ctrl->server_local->keyservers);
  }

  for (auto &ks : opt.keyserver) {
    err = make_keyserver_item(ks.c_str(), &item);
    if (err) break;
    item->next = ctrl->server_local->keyservers;
    ctrl->server_local->keyservers = item;
  }

  return err;
}

static const char hlp_keyserver[] =
    "KEYSERVER [<options>] <uri>\n"
    "Options are:\n"
    "  --help\n"
    "  --clear      Remove all configured keyservers\n"
    "\n"
    "If called without arguments list all configured keyserver URLs.\n"
    "If called with an URI add this as keyserver.  Note that keyservers\n"
    "are configured on a per-session base.  A default keyserver may already "
    "be\n"
    "present, thus the \"--clear\" option must be used to get full control.\n"
    "If \"--clear\" and an URI are used together the clear command is\n"
    "obviously executed first.  A RESET command does not change the list\n"
    "of configured keyservers.";
static gpg_error_t cmd_keyserver(assuan_context_t ctx, char *line) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err = 0;
  int clear_flag, add_flag, help_flag;
  uri_item_t item = NULL; /* gcc 4.4.5 is not able to detect that it
                             is always initialized.  */

  clear_flag = has_option(line, "--clear");
  help_flag = has_option(line, "--help");
  line = skip_options(line);
  add_flag = !!*line;

  if (help_flag) {
    err = ks_action_help(ctrl, line);
    goto leave;
  }

  if (add_flag) {
    err = make_keyserver_item(line, &item);
    if (err) goto leave;
  }
  if (clear_flag) release_ctrl_keyservers(ctrl);
  if (add_flag) {
    item->next = ctrl->server_local->keyservers;
    ctrl->server_local->keyservers = item;
  }

  if (!add_flag && !clear_flag && !help_flag) {
    /* List configured keyservers.  However, we first add a global
       keyserver. */
    uri_item_t u;

    err = ensure_keyserver(ctrl);
    if (err) {
      assuan_set_error(ctx, err, "Bad keyserver configuration in dirmngr.conf");
      goto leave;
    }

    for (u = ctrl->server_local->keyservers; u; u = u->next)
      dirmngr_status(ctrl, "KEYSERVER", u->uri, NULL);
  }
  err = 0;

leave:
  return leave_cmd(ctx, err);
}

static const char hlp_ks_search[] =
    "KS_SEARCH {<pattern>}\n"
    "\n"
    "Search the configured OpenPGP keyservers (see command KEYSERVER)\n"
    "for keys matching PATTERN";
static gpg_error_t cmd_ks_search(assuan_context_t ctx, char *line) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err;
  char *p;
  std::vector<std::string> list;

  if (has_option(line, "--quick")) ctrl->timeout = opt.connect_quick_timeout;
  line = skip_options(line);

  /* Break the line down into a list.  Each pattern is percent-plus
     escaped. */
  for (p = line; *p; line = p) {
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    if (*line) {
      char *str = (char *)xtrymalloc(strlen(line));
      if (!str) {
        err = gpg_error_from_syserror();
        return leave_cmd(ctx, err);
      }
      strcpy_escaped_plus(str, (const unsigned char *)(line));
      list.emplace_back(str);
    }
  }

  err = ensure_keyserver(ctrl);
  if (err) return leave_cmd(ctx, err);

  std::string output;
  err = ks_action_search(ctrl, ctrl->server_local->keyservers, list, output);

  err = assuan_send_data(ctx, output.data(), output.size());
  if (!err) err = assuan_send_data(ctx, NULL, 0);

  return leave_cmd(ctx, err);
}

static const char hlp_ks_get[] =
    "KS_GET {<pattern>}\n"
    "\n"
    "Get the keys matching PATTERN from the configured OpenPGP keyservers\n"
    "(see command KEYSERVER).  Each pattern should be a keyid, a fingerprint,\n"
    "or an exact name indicated by the '=' prefix.";
static gpg_error_t cmd_ks_get(assuan_context_t ctx, char *line) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err;
  std::vector<std::string> list;
  char *p;
  std::string output;

  if (has_option(line, "--quick")) ctrl->timeout = opt.connect_quick_timeout;
  line = skip_options(line);

  /* Break the line into a strlist.  Each pattern is by
     definition percent-plus escaped.  However we only support keyids
     and fingerprints and thus the client has no need to apply the
     escaping.  */
  for (p = line; *p; line = p) {
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    if (*line) {
      char *sl = (char *)xtrymalloc(strlen(line));
      if (!sl) {
        err = gpg_error_from_syserror();
        return leave_cmd(ctx, err);
      }
      strcpy_escaped_plus(sl, (const unsigned char *)(line));
      list.emplace_back(sl);
    }
  }

  err = ensure_keyserver(ctrl);
  if (err) leave_cmd(ctx, err);

  err = ks_action_get(ctrl, ctrl->server_local->keyservers, list, output);
  if (err) return leave_cmd(ctx, err);

  err = assuan_send_data(ctx, output.data(), output.size());
  if (!err) err = assuan_send_data(ctx, NULL, 0);

  return leave_cmd(ctx, err);
}

static const char hlp_ks_fetch[] =
    "KS_FETCH <URL>\n"
    "\n"
    "Get the key(s) from URL.";
static gpg_error_t cmd_ks_fetch(assuan_context_t ctx, char *line) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err;

  if (has_option(line, "--quick")) ctrl->timeout = opt.connect_quick_timeout;
  line = skip_options(line);

  err = ensure_keyserver(ctrl); /* FIXME: Why do we needs this here?  */
  if (err) return leave_cmd(ctx, err);

  /* Setup an output stream and perform the get.  */
  std::string output;
  err = ks_action_fetch(ctrl, line, output);

  err = assuan_send_data(ctx, output.data(), output.size());
  if (!err) err = assuan_send_data(ctx, NULL, 0);

  return leave_cmd(ctx, err);
}

static const char hlp_ks_put[] =
    "KS_PUT\n"
    "\n"
    "Send a key to the configured OpenPGP keyservers.  The actual key "
    "material\n"
    "is then requested by Dirmngr using\n"
    "\n"
    "  INQUIRE KEYBLOCK\n"
    "\n"
    "The client shall respond with a binary version of the keyblock (e.g.,\n"
    "the output of `gpg --export KEYID').\n"
    "The client shall respond with a colon delimited info lines (the output\n"
    "of 'for x in keys sigs; do gpg --list-$x --with-colons KEYID; done').\n";
static gpg_error_t cmd_ks_put(assuan_context_t ctx, char *line) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err;
  unsigned char *value = NULL;
  size_t valuelen;
  unsigned char *info = NULL;
  size_t infolen;

  /* No options for now.  */
  line = skip_options(line);

  err = ensure_keyserver(ctrl);
  if (err) goto leave;

  /* Ask for the key material.  */
  err = assuan_inquire(ctx, "KEYBLOCK", &value, &valuelen, MAX_KEYBLOCK_LENGTH);
  if (err) {
    log_error(_("assuan_inquire failed: %s\n"), gpg_strerror(err));
    goto leave;
  }

  if (!valuelen) /* No data returned; return a comprehensible error. */
  {
    err = GPG_ERR_MISSING_CERT;
    goto leave;
  }

  /* Ask for the key meta data. Not actually needed for HKP servers
     but we do it anyway to test the client implementation.  */
  err = assuan_inquire(ctx, "KEYBLOCK_INFO", &info, &infolen,
                       MAX_KEYBLOCK_LENGTH);
  if (err) {
    log_error(_("assuan_inquire failed: %s\n"), gpg_strerror(err));
    goto leave;
  }

  /* Send the key.  */
  err = ks_action_put(ctrl, ctrl->server_local->keyservers, value, valuelen,
                      info, infolen);

leave:
  xfree(info);
  xfree(value);
  return leave_cmd(ctx, err);
}

static const char hlp_getinfo[] =
    "GETINFO <what>\n"
    "\n"
    "Multi purpose command to return certain information.  \n"
    "Supported values of WHAT are:\n"
    "\n"
    "version     - Return the version of the program.\n"
    "pid         - Return the process id of the server.\n"
    "tor         - Return OK if running in Tor mode\n";
static gpg_error_t cmd_getinfo(assuan_context_t ctx, char *line) {
  ctrl_t ctrl = (ctrl_t)assuan_get_pointer(ctx);
  gpg_error_t err;

  if (!strcmp(line, "version")) {
    const char *s = VERSION;
    err = assuan_send_data(ctx, s, strlen(s));
  } else if (!strcmp(line, "pid")) {
    char numbuf[50];

    snprintf(numbuf, sizeof numbuf, "%lu", (unsigned long)getpid());
    err = assuan_send_data(ctx, numbuf, strlen(numbuf));
  } else
    err = set_error(GPG_ERR_ASS_PARAMETER, "unknown value for WHAT");

  return leave_cmd(ctx, err);
}

/* Tell the assuan library about our commands. */
static int register_commands(assuan_context_t ctx) {
  static struct {
    const char *name;
    assuan_handler_t handler;
    const char *const help;
  } table[] = {{"ISVALID", cmd_isvalid, hlp_isvalid},
               {"CHECKCRL", cmd_checkcrl, hlp_checkcrl},
               {"CHECKOCSP", cmd_checkocsp, hlp_checkocsp},
               {"LOOKUP", cmd_lookup, hlp_lookup},
               {"LOADCRL", cmd_loadcrl, hlp_loadcrl},
               {"LISTCRLS", cmd_listcrls, hlp_listcrls},
               {"CACHECERT", cmd_cachecert, hlp_cachecert},
               {"VALIDATE", cmd_validate, hlp_validate},
               {"KEYSERVER", cmd_keyserver, hlp_keyserver},
               {"KS_SEARCH", cmd_ks_search, hlp_ks_search},
               {"KS_GET", cmd_ks_get, hlp_ks_get},
               {"KS_FETCH", cmd_ks_fetch, hlp_ks_fetch},
               {"KS_PUT", cmd_ks_put, hlp_ks_put},
               {"GETINFO", cmd_getinfo, hlp_getinfo},
               {NULL, NULL, NULL}};
  int i, j, rc;

  for (i = j = 0; table[i].name; i++) {
    rc = assuan_register_command(ctx, table[i].name, table[i].handler,
                                 table[i].help);
    if (rc) return rc;
  }
  return 0;
}

/* Startup the server and run the main command loop.  With FD = -1,
   use stdin/stdout. */
void start_command_handler() {
  static const char hello[] = "Dirmngr " VERSION " at your service";
  static char *hello_line;
  int rc;
  assuan_context_t ctx;
  ctrl_t ctrl;
  assuan_fd_t filedes[2];

  ctrl = (ctrl_t)xtrycalloc(1, sizeof *ctrl);
  if (ctrl)
    ctrl->server_local =
        (server_local_s *)xtrycalloc(1, sizeof *ctrl->server_local);
  if (!ctrl || !ctrl->server_local) {
    log_error(_("can't allocate control structure: %s\n"), strerror(errno));
    xfree(ctrl);
    return;
  }

  dirmngr_init_default_ctrl(ctrl);

  rc = assuan_new(&ctx);
  if (rc) {
    log_error(_("failed to allocate assuan context: %s\n"), gpg_strerror(rc));
    dirmngr_exit(2);
  }

  filedes[0] = assuan_fdopen(0);
  filedes[1] = assuan_fdopen(1);
  rc = assuan_init_pipe_server(ctx, filedes);
  if (rc) {
    assuan_release(ctx);
    log_error(_("failed to initialize the server: %s\n"), gpg_strerror(rc));
    dirmngr_exit(2);
  }

  rc = register_commands(ctx);
  if (rc) {
    log_error(_("failed to the register commands with Assuan: %s\n"),
              gpg_strerror(rc));
    dirmngr_exit(2);
  }

  if (!hello_line) {
    hello_line = xtryasprintf(
        "Home: %s\n"
        "Config: %s\n"
        "%s",
        gnupg_homedir(), opt.config_filename ? opt.config_filename : "[none]",
        hello);
  }

  ctrl->server_local->assuan_ctx = ctx;
  assuan_set_pointer(ctx, ctrl);

  assuan_set_hello_line(ctx, hello_line);
  assuan_register_option_handler(ctx, option_handler);

  for (;;) {
    rc = assuan_accept(ctx);
    if (rc == -1) break;
    if (rc) {
      log_info(_("Assuan accept problem: %s\n"), gpg_strerror(rc));
      break;
    }

    rc = assuan_process(ctx);
    if (rc) {
      log_info(_("Assuan processing failed: %s\n"), gpg_strerror(rc));
      continue;
    }
  }

  release_ctrl_keyservers(ctrl);

  ctrl->server_local->assuan_ctx = NULL;
  assuan_release(ctx);

  if (ctrl->server_local->stopme) dirmngr_exit(0);

  release_ctrl_ocsp_certs(ctrl);
  xfree(ctrl->server_local);
  dirmngr_deinit_default_ctrl(ctrl);
  xfree(ctrl);
}

/* Send a status line back to the client.  KEYWORD is the status
   keyword, the optional string arguments are blank separated added to
   the line, the last argument must be a NULL. */
gpg_error_t dirmngr_status(ctrl_t ctrl, const char *keyword, ...) {
  gpg_error_t err = 0;
  va_list arg_ptr;
  const char *text;

  va_start(arg_ptr, keyword);

  if (ctrl->server_local) {
    assuan_context_t ctx = ctrl->server_local->assuan_ctx;
    char buf[950], *p;
    size_t n;

    p = buf;
    n = 0;
    while ((text = va_arg(arg_ptr, const char *))) {
      if (n) {
        *p++ = ' ';
        n++;
      }
      for (; *text && n < DIM(buf) - 2; n++) *p++ = *text++;
    }
    *p = 0;
    err = assuan_write_status(ctx, keyword, buf);
  }

  va_end(arg_ptr);
  return err;
}

/* Print a help status line.  TEXTLEN gives the length of the text
   from TEXT to be printed.  The function splits text at LFs.  */
gpg_error_t dirmngr_status_help(ctrl_t ctrl, const char *text) {
  gpg_error_t err = 0;

  if (ctrl->server_local) {
    assuan_context_t ctx = ctrl->server_local->assuan_ctx;
    char buf[950], *p;
    size_t n;

    do {
      p = buf;
      n = 0;
      for (; *text && *text != '\n' && n < DIM(buf) - 2; n++) *p++ = *text++;
      if (*text == '\n') text++;
      *p = 0;
      err = assuan_write_status(ctx, "#", buf);
    } while (!err && *text);
  }

  return err;
}
