/*
 * kdc/kdc_preauth.c
 *
 * Copyright 1995, 2003 by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 * 
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 * 
 * Preauthentication routines for the KDC.
 */

/*
 * Copyright (C) 1998 by the FundsXpress, INC.
 * 
 * All rights reserved.
 * 
 * Export of this software from the United States of America may require
 * a specific license from the United States Government.  It is the
 * responsibility of any person or organization contemplating export to
 * obtain such a license before exporting.
 * 
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of FundsXpress. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  FundsXpress makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 * 
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "k5-int.h"
#include "kdc_util.h"
#include "extern.h"
#include <stdio.h>
#include "adm_proto.h"
#include "pkinit_server.h"
#include <syslog.h>

#include <assert.h>

/* XXX This is ugly and should be in a header file somewhere */
#ifndef KRB5INT_DES_TYPES_DEFINED
#define KRB5INT_DES_TYPES_DEFINED
typedef unsigned char des_cblock[8];	/* crypto-block size */
#endif
typedef des_cblock mit_des_cblock;
extern void mit_des_fixup_key_parity (mit_des_cblock );
extern int mit_des_is_weak_key (mit_des_cblock );

typedef krb5_error_code (*verify_proc)
    (krb5_context, krb5_db_entry *client,
		    krb5_kdc_req *request,
		    krb5_enc_tkt_part * enc_tkt_reply, krb5_pa_data *data);

typedef krb5_error_code (*edata_proc)
    (krb5_context, krb5_kdc_req *request,
		    krb5_db_entry *client, krb5_db_entry *server,
		    krb5_pa_data *data);

typedef krb5_error_code (*return_proc)
    (krb5_context, krb5_pa_data * padata, 
		    krb5_db_entry *client,
		    krb5_kdc_req *request, krb5_kdc_rep *reply,
		    krb5_key_data *client_key,
		    krb5_keyblock *encrypting_key,
		    krb5_pa_data **send_pa);

typedef struct _krb5_preauth_systems {
    char *	name;
    int		type;
    int		flags;
    edata_proc	get_edata;
    verify_proc	verify_padata;
    return_proc return_padata;
} krb5_preauth_systems;

static krb5_error_code verify_enc_timestamp
    (krb5_context, krb5_db_entry *client,
		    krb5_kdc_req *request,
		    krb5_enc_tkt_part * enc_tkt_reply, krb5_pa_data *data);

static krb5_error_code get_etype_info
    (krb5_context, krb5_kdc_req *request,
		    krb5_db_entry *client, krb5_db_entry *server,
		    krb5_pa_data *data);
static krb5_error_code
get_etype_info2(krb5_context context, krb5_kdc_req *request,
	       krb5_db_entry *client, krb5_db_entry *server,
		  krb5_pa_data *pa_data);
static krb5_error_code
return_etype_info2(krb5_context, krb5_pa_data * padata, 
		   krb5_db_entry *client,
		   krb5_kdc_req *request, krb5_kdc_rep *reply,
		   krb5_key_data *client_key,
		   krb5_keyblock *encrypting_key,
		   krb5_pa_data **send_pa);

static krb5_error_code return_pw_salt
    (krb5_context, krb5_pa_data * padata, 
		    krb5_db_entry *client,
		    krb5_kdc_req *request, krb5_kdc_rep *reply,
		    krb5_key_data *client_key,
		    krb5_keyblock *encrypting_key,
		    krb5_pa_data **send_pa);

/* SAM preauth support */
static krb5_error_code verify_sam_response
    (krb5_context, krb5_db_entry *client,
		    krb5_kdc_req *request,
		    krb5_enc_tkt_part * enc_tkt_reply, krb5_pa_data *data);

static krb5_error_code get_sam_edata
    (krb5_context, krb5_kdc_req *request,
		    krb5_db_entry *client, krb5_db_entry *server,
		    krb5_pa_data *data);
static krb5_error_code return_sam_data
    (krb5_context, krb5_pa_data * padata, 
		    krb5_db_entry *client,
		    krb5_kdc_req *request, krb5_kdc_rep *reply,
		    krb5_key_data *client_key,
		    krb5_keyblock *encrypting_key,
		    krb5_pa_data **send_pa);
		    
/* PKINIT preauth support */
static krb5_error_code get_pkinit_edata(
    krb5_context context, 
    krb5_kdc_req *request,
    krb5_db_entry *client, 
    krb5_db_entry *server,
    krb5_pa_data *pa_data);
static krb5_error_code verify_pkinit_request(
    krb5_context context,
    krb5_db_entry *client,
    krb5_kdc_req *request,
    krb5_enc_tkt_part *enc_tkt_reply, 
    krb5_pa_data *data);
static krb5_error_code return_pkinit_response(
    krb5_context context, 
    krb5_pa_data * padata, 
    krb5_db_entry *client,
    krb5_kdc_req *request, 
    krb5_kdc_rep *reply,
    krb5_key_data *client_key,
    krb5_keyblock *encrypting_key,
    krb5_pa_data **send_pa);

/*
 * Preauth property flags
 */
#define PA_HARDWARE	0x00000001
#define PA_REQUIRED	0x00000002
#define PA_SUFFICIENT	0x00000004
	/* Not really a padata, so don't include it in the etype list*/
#define PA_PSEUDO	0x00000008 

static krb5_preauth_systems preauth_systems[] = {
    {
	"pkinit",
	KRB5_PADATA_PK_AS_REQ,
	PA_SUFFICIENT,	
	get_pkinit_edata,	
	verify_pkinit_request,
	return_pkinit_response
    },
    {
	"timestamp",
        KRB5_PADATA_ENC_TIMESTAMP,
        0,
        0,
	verify_enc_timestamp,
	0
    },
    {
	"etype-info",
	KRB5_PADATA_ETYPE_INFO,
	0,
	get_etype_info,
	0,
	0
    },
    {
	"etype-info2",
	KRB5_PADATA_ETYPE_INFO2,
	0,
	get_etype_info2,
	0,
	return_etype_info2
    },
    {
	"pw-salt",
	KRB5_PADATA_PW_SALT,
	PA_PSEUDO,		/* Don't include this in the error list */
	0, 
	0,
	return_pw_salt
    },
    {
	"sam-response",
	KRB5_PADATA_SAM_RESPONSE,
	0,
	0,
	verify_sam_response,
	return_sam_data
    },
    {
	"sam-challenge",
	KRB5_PADATA_SAM_CHALLENGE,
	PA_HARDWARE,		/* causes get_preauth_hint_list to use this */
	get_sam_edata,
	0,
	0
    },
    { "[end]", -1,}
};

#define MAX_PREAUTH_SYSTEMS (sizeof(preauth_systems)/sizeof(preauth_systems[0]))

static krb5_error_code
find_pa_system(int type, krb5_preauth_systems **preauth)
{
    krb5_preauth_systems 	*ap = preauth_systems;
    
    while ((ap->type != -1) && (ap->type != type))
	ap++;
    if (ap->type == -1)
	return(KRB5_PREAUTH_BAD_TYPE);
    *preauth = ap;
    return 0;
} 

const char *missing_required_preauth(krb5_db_entry *client,
				     krb5_db_entry *server,
				     krb5_enc_tkt_part *enc_tkt_reply)
{
#if 0
    /*
     * If this is the pwchange service, and the pre-auth bit is set,
     * allow it even if the HW preauth would normally be required.
     * 
     * Sandia national labs wanted this for some strange reason... we
     * leave it disabled normally.
     */
    if (isflagset(server->attributes, KRB5_KDB_PWCHANGE_SERVICE) &&
	isflagset(enc_tkt_reply->flags, TKT_FLG_PRE_AUTH))
	return 0;
#endif
    
#ifdef DEBUG
    krb5_klog_syslog (LOG_DEBUG,
		      "client needs %spreauth, %shw preauth; request has %spreauth, %shw preauth",
		      isflagset (client->attributes, KRB5_KDB_REQUIRES_PRE_AUTH) ? "" : "no ",
		      isflagset (client->attributes, KRB5_KDB_REQUIRES_HW_AUTH) ? "" : "no ",
		      isflagset (enc_tkt_reply->flags, TKT_FLG_PRE_AUTH) ? "" : "no ",
		      isflagset (enc_tkt_reply->flags, TKT_FLG_HW_AUTH) ? "" : "no ");
#endif

    if (isflagset(client->attributes, KRB5_KDB_REQUIRES_PRE_AUTH) &&
	 !isflagset(enc_tkt_reply->flags, TKT_FLG_PRE_AUTH))
	return "NEEDED_PREAUTH";

    if (isflagset(client->attributes, KRB5_KDB_REQUIRES_HW_AUTH) &&
	!isflagset(enc_tkt_reply->flags, TKT_FLG_HW_AUTH))
	return "NEEDED_HW_PREAUTH";

    return 0;
}

void get_preauth_hint_list(krb5_kdc_req *request, krb5_db_entry *client,
			   krb5_db_entry *server, krb5_data *e_data)
{
    int hw_only;
    krb5_preauth_systems *ap;
    krb5_pa_data **pa_data, **pa;
    krb5_data *edat;
    krb5_error_code retval;
    
    /* Zero these out in case we need to abort */
    e_data->length = 0;
    e_data->data = 0;
    
    hw_only = isflagset(client->attributes, KRB5_KDB_REQUIRES_HW_AUTH);
    pa_data = malloc(sizeof(krb5_pa_data *) * (MAX_PREAUTH_SYSTEMS+1));
    if (pa_data == 0)
	return;
    memset(pa_data, 0, sizeof(krb5_pa_data *) * (MAX_PREAUTH_SYSTEMS+1));
    pa = pa_data;

    for (ap = preauth_systems; ap->type != -1; ap++) {
	if (hw_only && !(ap->flags & PA_HARDWARE))
	    continue;
	if (ap->flags & PA_PSEUDO)
	    continue;
	*pa = malloc(sizeof(krb5_pa_data));
	if (*pa == 0)
	    goto errout;
	memset(*pa, 0, sizeof(krb5_pa_data));
	(*pa)->magic = KV5M_PA_DATA;
	(*pa)->pa_type = ap->type;
	if (ap->get_edata) {
	  retval = (ap->get_edata)(kdc_context, request, client, server, *pa);
	  if (retval) {
	    /* just failed on this type, continue */
	    free(*pa);
	    *pa = 0;
	    continue;
	  }
	}
	pa++;
    }
    if (pa_data[0] == 0) {
	krb5_klog_syslog (LOG_INFO,
			  "%spreauth required but hint list is empty",
			  hw_only ? "hw" : "");
    }
    retval = encode_krb5_padata_sequence((const krb5_pa_data **) pa_data,
					 &edat);
    if (retval)
	goto errout;
    *e_data = *edat;
    free(edat);

errout:
    krb5_free_pa_data(kdc_context, pa_data);
    return;
}

/*
 * This routine is called to verify the preauthentication information
 * for a V5 request.
 * 	
 * Returns 0 if the pre-authentication is valid, non-zero to indicate
 * an error code of some sort.
 */

krb5_error_code
check_padata (krb5_context context, krb5_db_entry *client,
	      krb5_kdc_req *request, krb5_enc_tkt_part *enc_tkt_reply)
{
    krb5_error_code retval = 0;
    krb5_pa_data **padata;
    krb5_preauth_systems *pa_sys;
    int			pa_ok = 0, pa_found = 0;

    if (request->padata == 0)
	return 0;

#ifdef DEBUG
    krb5_klog_syslog (LOG_DEBUG, "checking padata");
#endif
    for (padata = request->padata; *padata; padata++) {
#ifdef DEBUG
	krb5_klog_syslog (LOG_DEBUG, ".. pa_type 0x%x", (*padata)->pa_type);
#endif
	if (find_pa_system((*padata)->pa_type, &pa_sys))
	    continue;
#ifdef DEBUG
	krb5_klog_syslog (LOG_DEBUG, ".. pa_type %s", pa_sys->name);
#endif
	if (pa_sys->verify_padata == 0)
	    continue;
	pa_found++;
	retval = pa_sys->verify_padata(context, client, request,
				       enc_tkt_reply, *padata);
	if (retval) {
	    krb5_klog_syslog (LOG_INFO, "preauth (%s) verify failure: %s",
			      pa_sys->name, error_message (retval));
	    if (pa_sys->flags & PA_REQUIRED) {
		pa_ok = 0;
		break;
	    }
	} else {
#ifdef DEBUG
	    krb5_klog_syslog (LOG_DEBUG, ".. .. ok");
#endif
	    pa_ok = 1;
	    if (pa_sys->flags & PA_SUFFICIENT) 
		break;
	}
    }
    if (pa_ok)
	return 0;

    /* pa system was not found, but principal doesn't require preauth */
    if (!pa_found &&
        !isflagset(client->attributes, KRB5_KDB_REQUIRES_PRE_AUTH) &&
        !isflagset(client->attributes, KRB5_KDB_REQUIRES_HW_AUTH))
       return 0;

    if (!pa_found)
	krb5_klog_syslog (LOG_INFO, "no valid preauth type found: %s",
			  error_message (retval));
/* The following switch statement allows us
 * to return some preauth system errors back to the client.
 */
        switch(retval) {
	case KRB5KRB_AP_ERR_BAD_INTEGRITY:
    case KRB5KRB_AP_ERR_SKEW:
	return retval;
    default:
	return KRB5KDC_ERR_PREAUTH_FAILED;
    }
}

/*
 * return_padata creates any necessary preauthentication
 * structures which should be returned by the KDC to the client
 */
krb5_error_code
return_padata(krb5_context context, krb5_db_entry *client,
	      krb5_kdc_req *request, krb5_kdc_rep *reply,
	      krb5_key_data *client_key, krb5_keyblock *encrypting_key)
{
    krb5_error_code		retval;
    krb5_pa_data **		padata;
    krb5_pa_data **		send_pa_list;
    krb5_pa_data **		send_pa;
    krb5_pa_data *		pa = 0;
    krb5_preauth_systems *	ap;
    int 			size = 0;

    for (ap = preauth_systems; ap->type != -1; ap++) {
	if (ap->return_padata)
	    size++;
    }

    if ((send_pa_list = malloc((size+1) * sizeof(krb5_pa_data *))) == NULL)
	return ENOMEM;

    send_pa = send_pa_list;
    *send_pa = 0;
    
    for (ap = preauth_systems; ap->type != -1; ap++) {
	if (ap->return_padata == 0)
	    continue;
	pa = 0;
	if (request->padata) {
	    for (padata = request->padata; *padata; padata++) {
		if ((*padata)->pa_type == ap->type) {
		    pa = *padata;
		    break;
		}
	    }
	}
	if ((retval = ap->return_padata(context, pa, client, request, reply,
					client_key, encrypting_key, send_pa)))
	    goto cleanup;

	if (*send_pa)
	    send_pa++;
	*send_pa = 0;
    }
    
    retval = 0;

    if (send_pa_list[0]) {
	reply->padata = send_pa_list;
	send_pa_list = 0;
    }
    
cleanup:
    if (send_pa_list)
	krb5_free_pa_data(context, send_pa_list);
    return (retval);
}

static krb5_boolean
enctype_requires_etype_info_2(krb5_enctype enctype)
{
    switch(enctype) {
    case ENCTYPE_DES_CBC_CRC:
    case ENCTYPE_DES_CBC_MD4:
    case ENCTYPE_DES_CBC_MD5:
    case ENCTYPE_DES3_CBC_SHA1:
    case ENCTYPE_DES3_CBC_RAW:
    case ENCTYPE_ARCFOUR_HMAC:
    case ENCTYPE_ARCFOUR_HMAC_EXP :
	return 0;
    default:
	if (krb5_c_valid_enctype(enctype))
	    return 1;
	else return 0;
    }
}

static krb5_boolean
request_contains_enctype (krb5_context context,  const krb5_kdc_req *request,
			  krb5_enctype enctype)
{
    int i;
    for (i =0; i < request->nktypes; i++)
	if (request->ktype[i] == enctype)
	    return 1;
    return 0;
}


static krb5_error_code
verify_enc_timestamp(krb5_context context, krb5_db_entry *client,
		     krb5_kdc_req *request, krb5_enc_tkt_part *enc_tkt_reply,
		     krb5_pa_data *pa)
{
    krb5_pa_enc_ts *		pa_enc = 0;
    krb5_error_code		retval;
    krb5_data			scratch;
    krb5_data			enc_ts_data;
    krb5_enc_data 		*enc_data = 0;
    krb5_keyblock		key;
    krb5_key_data *		client_key;
    krb5_int32			start;
    krb5_timestamp		timenow;
    krb5_error_code		decrypt_err;

    scratch.data = pa->contents;
    scratch.length = pa->length;

    enc_ts_data.data = 0;
    
    if ((retval = decode_krb5_enc_data(&scratch, &enc_data)) != 0)
	goto cleanup;

    enc_ts_data.length = enc_data->ciphertext.length;
    if ((enc_ts_data.data = (char *) malloc(enc_ts_data.length)) == NULL)
	goto cleanup;

    start = 0;
    decrypt_err = 0;
    while (1) {
	if ((retval = krb5_dbe_search_enctype(context, client,
					      &start, enc_data->enctype,
					      -1, 0, &client_key)))
	    goto cleanup;

	if ((retval = krb5_dbekd_decrypt_key_data(context, &master_keyblock, 
						  client_key, &key, NULL)))
	    goto cleanup;

	key.enctype = enc_data->enctype;

	retval = krb5_c_decrypt(context, &key, KRB5_KEYUSAGE_AS_REQ_PA_ENC_TS,
				0, enc_data, &enc_ts_data);
	krb5_free_keyblock_contents(context, &key);
	if (retval == 0)
	    break;
	else
	    decrypt_err = retval;
    }

    if ((retval = decode_krb5_pa_enc_ts(&enc_ts_data, &pa_enc)) != 0)
	goto cleanup;

    if ((retval = krb5_timeofday(context, &timenow)) != 0)
	goto cleanup;
    
    if (labs(timenow - pa_enc->patimestamp) > context->clockskew) {
	retval = KRB5KRB_AP_ERR_SKEW;
	goto cleanup;
    }

    setflag(enc_tkt_reply->flags, TKT_FLG_PRE_AUTH);

    retval = 0;
    
cleanup:
    if (enc_data) {
	krb5_free_data_contents(context, &enc_data->ciphertext);
	free(enc_data);
    }
    krb5_free_data_contents(context, &enc_ts_data);
    if (pa_enc)
	free(pa_enc);
    /*
     * If we get NO_MATCHING_KEY and decryption previously failed, and
     * we failed to find any other keys of the correct enctype after
     * that failed decryption, it probably means that the password was
     * incorrect.
     */
    if (retval == KRB5_KDB_NO_MATCHING_KEY && decrypt_err != 0)
	retval = decrypt_err;
    return retval;
}

static krb5_error_code
_make_etype_info_entry(krb5_context context,
		       krb5_kdc_req *request, krb5_key_data *client_key,
		       krb5_enctype etype, krb5_etype_info_entry **entry,
		       int etype_info2)
{
    krb5_data			salt;
    krb5_etype_info_entry *	tmp_entry; 
    krb5_error_code		retval;

    if ((tmp_entry = malloc(sizeof(krb5_etype_info_entry))) == NULL)
       return ENOMEM;

    salt.data = 0;

    tmp_entry->magic = KV5M_ETYPE_INFO_ENTRY;
    tmp_entry->etype = etype;
    tmp_entry->length = KRB5_ETYPE_NO_SALT;
    tmp_entry->salt = 0;
    tmp_entry->s2kparams.data = NULL;
    tmp_entry->s2kparams.length = 0;
    retval = get_salt_from_key(context, request->client,
			       client_key, &salt);
    if (retval)
	goto fail;
    if (etype_info2 && client_key->key_data_ver > 1 &&
	client_key->key_data_type[1] == KRB5_KDB_SALTTYPE_AFS3) {
	switch (etype) {
	case ENCTYPE_DES_CBC_CRC:
	case ENCTYPE_DES_CBC_MD4:
	case ENCTYPE_DES_CBC_MD5:
	    tmp_entry->s2kparams.data = malloc(1);
	    if (tmp_entry->s2kparams.data == NULL) {
		retval = ENOMEM;
		goto fail;
	    }
	    tmp_entry->s2kparams.length = 1;
	    tmp_entry->s2kparams.data[0] = 1;
	    break;
	default:
	    break;
	}
    }

    if (salt.length >= 0) {
	tmp_entry->length = salt.length;
	tmp_entry->salt = (unsigned char *) salt.data;
	salt.data = 0;
    }
    *entry = tmp_entry;
    return 0;

fail:
    if (tmp_entry) {
	if (tmp_entry->s2kparams.data)
	    free(tmp_entry->s2kparams.data);
	free(tmp_entry);
    }
    if (salt.data)
	free(salt.data);
    return retval;
}
/*
 * This function returns the etype information for a particular
 * client, to be passed back in the preauth list in the KRB_ERROR
 * message.  It supports generating both etype_info  and etype_info2
 *  as most of the work is the same.   
 */
static krb5_error_code
etype_info_helper(krb5_context context, krb5_kdc_req *request,
	       krb5_db_entry *client, krb5_db_entry *server,
	       krb5_pa_data *pa_data, int etype_info2)
{
    krb5_etype_info_entry **	entry = 0;
    krb5_key_data		*client_key;
    krb5_error_code		retval;
    krb5_data *			scratch;
    krb5_enctype		db_etype;
    int 			i = 0;
    int 			start = 0;
    int				seen_des = 0;

    entry = malloc((client->n_key_data * 2 + 1) * sizeof(krb5_etype_info_entry *));
    if (entry == NULL)
	return ENOMEM;
    entry[0] = NULL;

    while (1) {
	retval = krb5_dbe_search_enctype(context, client, &start, -1,
					 -1, 0, &client_key);
	if (retval == KRB5_KDB_NO_MATCHING_KEY)
	    break;
	if (retval)
	    goto cleanup;
	db_etype = client_key->key_data_type[0];
	if (db_etype == ENCTYPE_DES_CBC_MD4)
	    db_etype = ENCTYPE_DES_CBC_MD5;
	
	if (request_contains_enctype(context, request, db_etype)) {
	    assert(etype_info2 ||
		   !enctype_requires_etype_info_2(db_etype));
	    if ((retval = _make_etype_info_entry(context, request, client_key,
			    db_etype, &entry[i], etype_info2)) != 0) {
		goto cleanup;
	    }
	    entry[i+1] = 0;
	    i++;
	}

	/* 
	 * If there is a des key in the kdb, try the "similar" enctypes,
	 * avoid duplicate entries. 
	 */
	if (!seen_des) {
	    switch (db_etype) {
	    case ENCTYPE_DES_CBC_MD5:
		db_etype = ENCTYPE_DES_CBC_CRC;
		break;
	    case ENCTYPE_DES_CBC_CRC:
		db_etype = ENCTYPE_DES_CBC_MD5;
		break;
	    default:
		continue;

	    }
	    if (request_contains_enctype(context, request, db_etype)) {
		if ((retval = _make_etype_info_entry(context, request,
				client_key, db_etype, &entry[i], etype_info2)) != 0) {
		    goto cleanup;
		}
		entry[i+1] = 0;
		i++;
	    }
	    seen_des++;
	}
    }
    if (etype_info2)
	retval = encode_krb5_etype_info2((const krb5_etype_info_entry **) entry,
				    &scratch);
    else 	retval = encode_krb5_etype_info((const krb5_etype_info_entry **) entry,
				    &scratch);
    if (retval)
	goto cleanup;
    pa_data->contents = (unsigned char *)scratch->data;
    pa_data->length = scratch->length;
    free(scratch);

    retval = 0;

cleanup:
    if (entry)
	krb5_free_etype_info(context, entry);
    return retval;
}

static krb5_error_code
get_etype_info(krb5_context context, krb5_kdc_req *request,
	       krb5_db_entry *client, krb5_db_entry *server,
	       krb5_pa_data *pa_data)
{
  int i;
    for (i=0;  i < request->nktypes; i++) {
	if (enctype_requires_etype_info_2(request->ktype[i])) 
	    return KRB5KDC_ERR_PADATA_TYPE_NOSUPP ;;;; /*Caller will
							* skip this
							* type*/
    }
    return etype_info_helper(context, request, client, server, pa_data, 0);
}

static krb5_error_code
get_etype_info2(krb5_context context, krb5_kdc_req *request,
	       krb5_db_entry *client, krb5_db_entry *server,
	       krb5_pa_data *pa_data)
{
    return etype_info_helper( context, request, client, server, pa_data, 1);
}

static krb5_error_code
return_etype_info2(krb5_context context, krb5_pa_data * padata, 
		   krb5_db_entry *client,
		   krb5_kdc_req *request, krb5_kdc_rep *reply,
		   krb5_key_data *client_key,
		   krb5_keyblock *encrypting_key,
		   krb5_pa_data **send_pa)
{
    krb5_error_code retval;
    krb5_pa_data *tmp_padata;
    krb5_etype_info_entry **entry = NULL;
    krb5_data *scratch = NULL;
    tmp_padata = malloc( sizeof(krb5_pa_data));
    if (tmp_padata == NULL)
	return ENOMEM;
    tmp_padata->pa_type = KRB5_PADATA_ETYPE_INFO2;
    entry = malloc(2 * sizeof(krb5_etype_info_entry *));
    if (entry == NULL) {
	retval = ENOMEM;
	goto cleanup;
    }
    entry[0] = NULL;
    entry[1] = NULL;
    retval = _make_etype_info_entry(context, request, client_key, client_key->key_data_type[0],
				    entry, 1);
    if (retval)
	goto cleanup;
    retval = encode_krb5_etype_info2((const krb5_etype_info_entry **) entry, &scratch);
    if (retval)
	goto cleanup;
    tmp_padata->contents = scratch->data;
    tmp_padata->length = scratch->length;
    *send_pa = tmp_padata;

    /* For cleanup - we no longer own the contents of the krb5_data 
     * only to pointer to the krb5_data
     */
    scratch->data = 0;

 cleanup:
    if (entry)
	krb5_free_etype_info(context, entry);
    if (retval) {
	if (tmp_padata)
	    free(tmp_padata);
    }
    if (scratch)
	    krb5_free_data(context, scratch);
    return retval;
}


static krb5_error_code
return_pw_salt(krb5_context context, krb5_pa_data *in_padata,
	       krb5_db_entry *client, krb5_kdc_req *request,
	       krb5_kdc_rep *reply, krb5_key_data *client_key,
	       krb5_keyblock *encrypting_key, krb5_pa_data **send_pa)
{
    krb5_error_code	retval;
    krb5_pa_data *	padata;
    krb5_data *		scratch;
    krb5_data		salt_data;
    int i;
    
    for (i = 0; i < request->nktypes; i++) {
	if (enctype_requires_etype_info_2(request->ktype[i]))
	    return 0;
    }
    if (client_key->key_data_ver == 1 ||
	client_key->key_data_type[1] == KRB5_KDB_SALTTYPE_NORMAL)
	return 0;

    if ((padata = malloc(sizeof(krb5_pa_data))) == NULL)
	return ENOMEM;
    padata->magic = KV5M_PA_DATA;
    padata->pa_type = KRB5_PADATA_PW_SALT;
    
    switch (client_key->key_data_type[1]) {
    case KRB5_KDB_SALTTYPE_V4:
	/* send an empty (V4) salt */
	padata->contents = 0;
	padata->length = 0;
	break;
    case KRB5_KDB_SALTTYPE_NOREALM:
	if ((retval = krb5_principal2salt_norealm(kdc_context, 
						   request->client,
						   &salt_data)))
	    goto cleanup;
	padata->contents = (krb5_octet *)salt_data.data;
	padata->length = salt_data.length;
	break;
    case KRB5_KDB_SALTTYPE_AFS3:
	/* send an AFS style realm-based salt */
	/* for now, just pass the realm back and let the client
	   do the work. In the future, add a kdc configuration
	   variable that specifies the old cell name. */
	padata->pa_type = KRB5_PADATA_AFS3_SALT;
	/* it would be just like ONLYREALM, but we need to pass the 0 */
	scratch = krb5_princ_realm(kdc_context, request->client);
	if ((padata->contents = malloc(scratch->length+1)) == NULL) {
	    retval = ENOMEM;
	    goto cleanup;
	}
	memcpy(padata->contents, scratch->data, scratch->length);
	padata->length = scratch->length+1;
	padata->contents[scratch->length] = 0;
	break;
    case KRB5_KDB_SALTTYPE_ONLYREALM:
	scratch = krb5_princ_realm(kdc_context, request->client);
	if ((padata->contents = malloc(scratch->length)) == NULL) {
	    retval = ENOMEM;
	    goto cleanup;
	}
	memcpy(padata->contents, scratch->data, scratch->length);
	padata->length = scratch->length;
	break;
    case KRB5_KDB_SALTTYPE_SPECIAL:
	if ((padata->contents = malloc(client_key->key_data_length[1]))
	    == NULL) {
	    retval = ENOMEM;
	    goto cleanup;
	}
	memcpy(padata->contents, client_key->key_data_contents[1],
	       client_key->key_data_length[1]);
	padata->length = client_key->key_data_length[1];
	break;
    default:
	free(padata);
	return 0;
    }

    *send_pa = padata;
    return 0;
    
cleanup:
    free(padata);
    return retval;
}

static krb5_error_code
return_sam_data(krb5_context context, krb5_pa_data *in_padata,
		krb5_db_entry *client, krb5_kdc_req *request,
		krb5_kdc_rep *reply, krb5_key_data *client_key,
		krb5_keyblock *encrypting_key, krb5_pa_data **send_pa)
{
    krb5_error_code	retval;
    krb5_data		scratch;
    int			i;

    krb5_sam_response		*sr = 0;
    krb5_predicted_sam_response	*psr = 0;

    if (in_padata == 0)
	return 0;

    /*
     * We start by doing the same thing verify_sam_response() does:
     * extract the psr from the padata (which is an sr). Nothing
     * here should generate errors! We've already successfully done
     * all this once.
     */

    scratch.data = in_padata->contents;
    scratch.length = in_padata->length;
    
    if ((retval = decode_krb5_sam_response(&scratch, &sr))) {
	com_err("krb5kdc", retval,
		"return_sam_data(): decode_krb5_sam_response failed");
	goto cleanup;
    }

    {
	krb5_enc_data tmpdata;

	tmpdata.enctype = ENCTYPE_UNKNOWN;
	tmpdata.ciphertext = sr->sam_track_id;

	scratch.length = tmpdata.ciphertext.length;
	if ((scratch.data = (char *) malloc(scratch.length)) == NULL) {
	    retval = ENOMEM;
	    goto cleanup;
	}

	if ((retval = krb5_c_decrypt(context, &psr_key, /* XXX */ 0, 0,
				     &tmpdata, &scratch))) {
	    com_err("krb5kdc", retval,
		    "return_sam_data(): decrypt track_id failed");
	    free(scratch.data);
	    goto cleanup;
	}
    }

    if ((retval = decode_krb5_predicted_sam_response(&scratch, &psr))) {
	com_err("krb5kdc", retval,
		"return_sam_data(): decode_krb5_predicted_sam_response failed");
	free(scratch.data);
	goto cleanup;
    }

    /* We could use sr->sam_flags, but it may be absent or altered. */
    if (psr->sam_flags & KRB5_SAM_MUST_PK_ENCRYPT_SAD) {
	com_err("krb5kdc", retval = KRB5KDC_ERR_PREAUTH_FAILED,
		"Unsupported SAM flag must-pk-encrypt-sad");
	goto cleanup;
    }
    if (psr->sam_flags & KRB5_SAM_SEND_ENCRYPTED_SAD) {
	/* No key munging */
	goto cleanup;
    }
    if (psr->sam_flags & KRB5_SAM_USE_SAD_AS_KEY) {
	/* Use sam_key instead of client key */
	krb5_free_keyblock_contents(context, encrypting_key);
	krb5_copy_keyblock_contents(context, &psr->sam_key, encrypting_key);
	/* XXX Attach a useful pa_data */
	goto cleanup;
    }

    /* Otherwise (no flags set), we XOR the keys */
    /* XXX The passwords-04 draft is underspecified here wrt different
	   key types. We will do what I hope to get into the -05 draft. */
    {
	krb5_octet *p = encrypting_key->contents;
	krb5_octet *q = psr->sam_key.contents;
	int length = ((encrypting_key->length < psr->sam_key.length)
		      ? encrypting_key->length
		      : psr->sam_key.length);

	for (i = 0; i < length; i++)
	    p[i] ^= q[i];
    }

    /* Post-mixing key correction */
    switch (encrypting_key->enctype) {
    case ENCTYPE_DES_CBC_CRC:
    case ENCTYPE_DES_CBC_MD4:
    case ENCTYPE_DES_CBC_MD5:
    case ENCTYPE_DES_CBC_RAW:
	mit_des_fixup_key_parity(encrypting_key->contents);
	if (mit_des_is_weak_key(encrypting_key->contents))
	    ((krb5_octet *) encrypting_key->contents)[7] ^= 0xf0;
	break;

    /* XXX case ENCTYPE_DES3_CBC_MD5: listed in 1510bis-04 draft */
    case ENCTYPE_DES3_CBC_SHA: /* XXX deprecated? */
    case ENCTYPE_DES3_CBC_RAW:
    case ENCTYPE_DES3_CBC_SHA1:
	for (i = 0; i < 3; i++) {
	    mit_des_fixup_key_parity(encrypting_key->contents + i * 8);
	    if (mit_des_is_weak_key(encrypting_key->contents + i * 8))
		((krb5_octet *) encrypting_key->contents)[7 + i * 8] ^= 0xf0;
	}
	break;

    default:
	com_err("krb5kdc", retval = KRB5KDC_ERR_PREAUTH_FAILED,
		"Unimplemented keytype for SAM key mixing");
	goto cleanup;
    }

    /* XXX Attach a useful pa_data */
cleanup:
    if (sr)
	krb5_free_sam_response(context, sr);
    if (psr)
	krb5_free_predicted_sam_response(context, psr);

    return retval;
}
    
static struct {
  char* name;
  int   sam_type;
} *sam_ptr, sam_inst_map[] = {
  { "SNK4", PA_SAM_TYPE_DIGI_PATH, },
  { "SECURID", PA_SAM_TYPE_SECURID, },
  { "GRAIL", PA_SAM_TYPE_GRAIL, },
  { 0, 0 },
};

static krb5_error_code
get_sam_edata(krb5_context context, krb5_kdc_req *request,
	      krb5_db_entry *client, krb5_db_entry *server,
	      krb5_pa_data *pa_data)
{
    krb5_error_code		retval;
    krb5_sam_challenge		sc;
    krb5_predicted_sam_response	psr;
    krb5_data *			scratch;
    krb5_keyblock encrypting_key;
    char response[9];
    char inputblock[8];
    krb5_data predict_response;

    memset(&sc, 0, sizeof(sc));
    memset(&psr, 0, sizeof(psr));

    /* Given the client name we can figure out what type of preauth
       they need. The spec is currently for querying the database for
       names that match the types of preauth used. Later we should
       make this mapping show up in kdc.conf. In the meantime, we
       hardcode the following:
		/SNK4 -- Digital Pathways SNK/4 preauth.
		/GRAIL -- experimental preauth
       The first one found is used. See sam_inst_map above.

       For SNK4 in particular, the key in the database is the key for
       the device; kadmin needs a special interface for it.
     */

    {
      int npr = 1;
      krb5_boolean more;
      krb5_db_entry assoc;
      krb5_key_data  *assoc_key;
      krb5_principal newp;
      int probeslot;

      sc.sam_type = 0;

      retval = krb5_copy_principal(kdc_context, request->client, &newp);
      if (retval) {
	com_err("krb5kdc", retval, "copying client name for preauth probe");
	return retval;
      }

      probeslot = krb5_princ_size(context, newp)++;
      krb5_princ_name(kdc_context, newp) = 
	realloc(krb5_princ_name(kdc_context, newp),
		krb5_princ_size(context, newp) * sizeof(krb5_data));

      for(sam_ptr = sam_inst_map; sam_ptr->name; sam_ptr++) {
	krb5_princ_component(kdc_context,newp,probeslot)->data = sam_ptr->name;
	krb5_princ_component(kdc_context,newp,probeslot)->length = 
	  strlen(sam_ptr->name);
	npr = 1;
	retval = krb5_db_get_principal(kdc_context, newp, &assoc, &npr, &more);
	if(!retval && npr) {
	  sc.sam_type = sam_ptr->sam_type;
	  break;
	}
      }

      krb5_princ_component(kdc_context,newp,probeslot)->data = 0;
      krb5_princ_component(kdc_context,newp,probeslot)->length = 0;
      krb5_princ_size(context, newp)--;

      krb5_free_principal(kdc_context, newp);

      /* if sc.sam_type is set, it worked */
      if (sc.sam_type) {
	/* so use assoc to get the key out! */
	{
	  /* here's what do_tgs_req does */
	  retval = krb5_dbe_find_enctype(kdc_context, &assoc,
					 ENCTYPE_DES_CBC_RAW,
					 KRB5_KDB_SALTTYPE_NORMAL,
					 0,		/* Get highest kvno */
					 &assoc_key);
	  if (retval) {
	    char *sname;
	    krb5_unparse_name(kdc_context, request->client, &sname);
	    com_err("krb5kdc", retval, 
		    "snk4 finding the enctype and key <%s>", sname);
	    free(sname);
	    return retval;
	  }
	  /* convert server.key into a real key */
	  retval = krb5_dbekd_decrypt_key_data(kdc_context,
					       &master_keyblock, 
					       assoc_key, &encrypting_key,
					       NULL);
	  if (retval) {
	    com_err("krb5kdc", retval, 
		    "snk4 pulling out key entry");
	    return retval;
	  }
	  /* now we can use encrypting_key... */
	}
      } else {
	/* SAM is not an option - so don't return as hint */
	return KRB5_PREAUTH_BAD_TYPE;
      }
    }
    sc.magic = KV5M_SAM_CHALLENGE;
    psr.sam_flags = sc.sam_flags = KRB5_SAM_USE_SAD_AS_KEY;

    /* Replay prevention */
    if ((retval = krb5_copy_principal(context, request->client, &psr.client)))
	return retval;
#ifdef USE_RCACHE
    if ((retval = krb5_us_timeofday(context, &psr.stime, &psr.susec)))
	return retval;
#endif /* USE_RCACHE */

    switch (sc.sam_type) {
    case PA_SAM_TYPE_GRAIL:
      sc.sam_type_name.data = "Experimental System";
      sc.sam_type_name.length = strlen(sc.sam_type_name.data);
      sc.sam_challenge_label.data = "experimental challenge label";
      sc.sam_challenge_label.length = strlen(sc.sam_challenge_label.data);
      sc.sam_challenge.data = "12345";
      sc.sam_challenge.length = strlen(sc.sam_challenge.data);

#if 0 /* Enable this to test "normal" (no flags set) mode.  */
      psr.sam_flags = sc.sam_flags = 0;
#endif

      psr.magic = KV5M_PREDICTED_SAM_RESPONSE;
      /* string2key on sc.sam_challenge goes in here */
      /* eblock is just to set the enctype */
      {
	const krb5_enctype type = ENCTYPE_DES_CBC_MD5;

	if ((retval = krb5_c_string_to_key(context, type, &sc.sam_challenge,
					   0 /* salt */, &psr.sam_key)))
	    goto cleanup;

	if ((retval = encode_krb5_predicted_sam_response(&psr, &scratch)))
	    goto cleanup;
	
	{
	    size_t enclen;
	    krb5_enc_data tmpdata;

	    if ((retval = krb5_c_encrypt_length(context,
						psr_key.enctype,
						scratch->length, &enclen)))
		goto cleanup;

	    if ((tmpdata.ciphertext.data = (char *) malloc(enclen)) == NULL) {
		retval = ENOMEM;
		goto cleanup;
	    }
	    tmpdata.ciphertext.length = enclen;

	    if ((retval = krb5_c_encrypt(context, &psr_key,
					 /* XXX */ 0, 0, scratch, &tmpdata)))
		goto cleanup;

	    sc.sam_track_id = tmpdata.ciphertext;
	}
      }

      sc.sam_response_prompt.data = "response prompt";
      sc.sam_response_prompt.length = strlen(sc.sam_response_prompt.data);
      sc.sam_pk_for_sad.length = 0;
      sc.sam_nonce = 0;
      /* Generate checksum */
      /*krb5_checksum_size(context, ctype)*/
      /*krb5_calculate_checksum(context,ctype,in,in_length,seed,
	seed_length,outcksum) */
      /*krb5_verify_checksum(context,ctype,cksum,in,in_length,seed,
	seed_length) */
#if 0 /* XXX a) glue appears broken; b) this gives up the SAD */
      sc.sam_cksum.contents = (krb5_octet *)
	malloc(krb5_checksum_size(context, CKSUMTYPE_RSA_MD5_DES));
      if (sc.sam_cksum.contents == NULL) return(ENOMEM);

      retval = krb5_calculate_checksum(context, CKSUMTYPE_RSA_MD5_DES,
				       sc.sam_challenge.data,
				       sc.sam_challenge.length,
				       psr.sam_key.contents, /* key */
				       psr.sam_key.length, /* key length */
				       &sc.sam_cksum);
      if (retval) { free(sc.sam_cksum.contents); return(retval); }
#endif /* 0 */
      
      retval = encode_krb5_sam_challenge(&sc, &scratch);
      if (retval) goto cleanup;
      pa_data->magic = KV5M_PA_DATA;
      pa_data->pa_type = KRB5_PADATA_SAM_CHALLENGE;
      pa_data->contents = scratch->data;
      pa_data->length = scratch->length;
      
      retval = 0;
      break;
    case PA_SAM_TYPE_DIGI_PATH:
      sc.sam_type_name.data = "Digital Pathways";
      sc.sam_type_name.length = strlen(sc.sam_type_name.data);
#if 1
      sc.sam_challenge_label.data = "Enter the following on your keypad";
      sc.sam_challenge_label.length = strlen(sc.sam_challenge_label.data);
#endif
      /* generate digit string, take it mod 1000000 (six digits.) */
      {
	int j;
	krb5_keyblock session_key;
	char outputblock[8];
	int i;

	session_key.contents = 0;

	memset(inputblock, 0, 8);

	retval = krb5_c_make_random_key(kdc_context, ENCTYPE_DES_CBC_CRC,
					&session_key);

	if (retval) {
	  /* random key failed */
	  com_err("krb5kdc", retval,"generating random challenge for preauth");
	  return retval;
	}
	/* now session_key has a key which we can pick bits out of */
	/* we need six decimal digits. Grab 6 bytes, div 2, mod 10 each. */
	if (session_key.length != 8) {
	  com_err("krb5kdc", retval = KRB5KDC_ERR_ETYPE_NOSUPP,
		  "keytype didn't match code expectations");
	  return retval;
	}
	for(i = 0; i<6; i++) {
	  inputblock[i] = '0' + ((session_key.contents[i]/2) % 10);
	}
	if (session_key.contents)
	  krb5_free_keyblock_contents(kdc_context, &session_key);

	/* retval = krb5_finish_key(kdc_context, &eblock); */
	/* now we have inputblock containing the 8 byte input to DES... */
	sc.sam_challenge.data = inputblock;
	sc.sam_challenge.length = 6;

	encrypting_key.enctype = ENCTYPE_DES_CBC_RAW;

	if (retval) {
	  com_err("krb5kdc", retval, "snk4 processing key");
	}

	{
	    krb5_data plain;
	    krb5_enc_data cipher;

	    plain.length = 8;
	    plain.data = inputblock;

	    /* XXX I know this is enough because of the fixed raw enctype.
	       if it's not, the underlying code will return a reasonable
	       error, which should never happen */
	    cipher.ciphertext.length = 8;
	    cipher.ciphertext.data = outputblock;

	    if ((retval = krb5_c_encrypt(kdc_context, &encrypting_key,
					 /* XXX */ 0, 0, &plain, &cipher))) {
		com_err("krb5kdc", retval, "snk4 response generation failed");
		return retval;
	    }
	}

	/* now output block is the raw bits of the response; convert it
	   to display form */
	for (j=0; j<4; j++) {
	  char n[2];
	  int k;
	  n[0] = outputblock[j] & 0xf;
	  n[1] = (outputblock[j]>>4) & 0xf;
	  for (k=0; k<2; k++) {
	    if(n[k] > 9) n[k] = ((n[k]-1)>>2);
	    /* This is equivalent to:
	       if(n[k]>=0xa && n[k]<=0xc) n[k] = 2;
	       if(n[k]>=0xd && n[k]<=0xf) n[k] = 3;
	       */
	  }
	  /* for v4, we keygen: *(j+(char*)&key1) = (n[1]<<4) | n[0]; */
	  /* for v5, we just generate a string */
	  response[2*j+0] = '0' + n[1];
	  response[2*j+1] = '0' + n[0];
	  /* and now, response has what we work with. */
	}
	response[8] = 0;
	predict_response.data = response;
	predict_response.length = 8;
#if 0				/* for debugging, hack the output too! */
sc.sam_challenge_label.data = response;
sc.sam_challenge_label.length = strlen(sc.sam_challenge_label.data);
#endif
      }

      psr.magic = KV5M_PREDICTED_SAM_RESPONSE;
      /* string2key on sc.sam_challenge goes in here */
      /* eblock is just to set the enctype */
      {
	retval = krb5_c_string_to_key(context, ENCTYPE_DES_CBC_MD5,
				      &predict_response, 0 /* salt */,
				      &psr.sam_key);
	if (retval) goto cleanup;

	retval = encode_krb5_predicted_sam_response(&psr, &scratch);
	if (retval) goto cleanup;
	
	{
	    size_t enclen;
	    krb5_enc_data tmpdata;

	    if ((retval = krb5_c_encrypt_length(context,
						psr_key.enctype,
						scratch->length, &enclen)))
		goto cleanup;

	    if ((tmpdata.ciphertext.data = (char *) malloc(enclen)) == NULL) {
		retval = ENOMEM;
		goto cleanup;
	    }
	    tmpdata.ciphertext.length = enclen;

	    if ((retval = krb5_c_encrypt(context, &psr_key,
					 /* XXX */ 0, 0, scratch, &tmpdata)))
		goto cleanup;

	    sc.sam_track_id = tmpdata.ciphertext;
	}
	if (retval) goto cleanup;
      }

      sc.sam_response_prompt.data = "Enter the displayed response";
      sc.sam_response_prompt.length = strlen(sc.sam_response_prompt.data);
      sc.sam_pk_for_sad.length = 0;
      sc.sam_nonce = 0;
      /* Generate checksum */
      /*krb5_checksum_size(context, ctype)*/
      /*krb5_calculate_checksum(context,ctype,in,in_length,seed,
	seed_length,outcksum) */
      /*krb5_verify_checksum(context,ctype,cksum,in,in_length,seed,
	seed_length) */
#if 0 /* XXX a) glue appears broken; b) this gives up the SAD */
      sc.sam_cksum.contents = (krb5_octet *)
	malloc(krb5_checksum_size(context, CKSUMTYPE_RSA_MD5_DES));
      if (sc.sam_cksum.contents == NULL) return(ENOMEM);

      retval = krb5_calculate_checksum(context, CKSUMTYPE_RSA_MD5_DES,
				       sc.sam_challenge.data,
				       sc.sam_challenge.length,
				       psr.sam_key.contents, /* key */
				       psr.sam_key.length, /* key length */
				       &sc.sam_cksum);
      if (retval) { free(sc.sam_cksum.contents); return(retval); }
#endif /* 0 */
      
      retval = encode_krb5_sam_challenge(&sc, &scratch);
      if (retval) goto cleanup;
      pa_data->magic = KV5M_PA_DATA;
      pa_data->pa_type = KRB5_PADATA_SAM_CHALLENGE;
      pa_data->contents = scratch->data;
      pa_data->length = scratch->length;
      
      retval = 0;
      break;
    }

cleanup:
    krb5_free_keyblock_contents(context, &encrypting_key);
    return retval;
}

static krb5_error_code
verify_sam_response(krb5_context context, krb5_db_entry *client,
		    krb5_kdc_req *request, krb5_enc_tkt_part *enc_tkt_reply,
		    krb5_pa_data *pa)
{
    krb5_error_code		retval;
    krb5_data			scratch;
    krb5_sam_response		*sr = 0;
    krb5_predicted_sam_response	*psr = 0;
    krb5_enc_sam_response_enc	*esre = 0;
    krb5_timestamp		timenow;
    char			*princ_req = 0, *princ_psr = 0;

    scratch.data = pa->contents;
    scratch.length = pa->length;
    
    if ((retval = decode_krb5_sam_response(&scratch, &sr))) {
	scratch.data = 0;
	com_err("krb5kdc", retval, "decode_krb5_sam_response failed");
	goto cleanup;
    }

    /* XXX We can only handle the challenge/response model of SAM.
	   See passwords-04, par 4.1, 4.2 */
    {
      krb5_enc_data tmpdata;

      tmpdata.enctype = ENCTYPE_UNKNOWN;
      tmpdata.ciphertext = sr->sam_track_id;

      scratch.length = tmpdata.ciphertext.length;
      if ((scratch.data = (char *) malloc(scratch.length)) == NULL) {
	  retval = ENOMEM;
	  goto cleanup;
      }

      if ((retval = krb5_c_decrypt(context, &psr_key, /* XXX */ 0, 0,
				   &tmpdata, &scratch))) {
	  com_err("krb5kdc", retval, "decrypt track_id failed");
	  goto cleanup;
      }
    }

    if ((retval = decode_krb5_predicted_sam_response(&scratch, &psr))) {
	com_err("krb5kdc", retval,
		"decode_krb5_predicted_sam_response failed -- replay attack?");
	goto cleanup;
    }

    /* Replay detection */
    if ((retval = krb5_unparse_name(context, request->client, &princ_req)))
	goto cleanup;
    if ((retval = krb5_unparse_name(context, psr->client, &princ_psr)))
	goto cleanup;
    if (strcmp(princ_req, princ_psr) != 0) {
	com_err("krb5kdc", retval = KRB5KDC_ERR_PREAUTH_FAILED,
		"Principal mismatch in SAM psr! -- replay attack?");
	goto cleanup;
    }

    if ((retval = krb5_timeofday(context, &timenow)))
	goto cleanup;

#ifdef USE_RCACHE
    {
	krb5_donot_replay rep;
	extern krb5_deltat rc_lifetime;
	/*
	 * Verify this response came back in a timely manner.
	 * We do this b/c otherwise very old (expunged from the rcache)
	 * psr's would be able to be replayed.
	 */
	if (timenow - psr->stime > rc_lifetime) {
	    com_err("krb5kdc", retval = KRB5KDC_ERR_PREAUTH_FAILED,
	    "SAM psr came back too late! -- replay attack?");
	    goto cleanup;
	}

	/* Now check the replay cache. */
	rep.client = princ_psr;
	rep.server = "SAM/rc";  /* Should not match any principal name. */
	rep.ctime = psr->stime;
	rep.cusec = psr->susec;
	retval = krb5_rc_store(kdc_context, kdc_rcache, &rep);
	if (retval) {
	    com_err("krb5kdc", retval, "SAM psr replay attack!");
	    goto cleanup;
	}
    }
#endif /* USE_RCACHE */


    {
	free(scratch.data);
	scratch.length = sr->sam_enc_nonce_or_ts.ciphertext.length;
	if ((scratch.data = (char *) malloc(scratch.length)) == NULL) {
	    retval = ENOMEM;
	    goto cleanup;
	}

	if ((retval = krb5_c_decrypt(context, &psr->sam_key, /* XXX */ 0,
				     0, &sr->sam_enc_nonce_or_ts, &scratch))) {
	    com_err("krb5kdc", retval, "decrypt nonce_or_ts failed");
	    goto cleanup;
	}
    }

    if ((retval = decode_krb5_enc_sam_response_enc(&scratch, &esre))) {
	com_err("krb5kdc", retval, "decode_krb5_enc_sam_response_enc failed");
	goto cleanup;
    }

    if (esre->sam_timestamp != sr->sam_patimestamp) {
      retval = KRB5KDC_ERR_PREAUTH_FAILED;
      goto cleanup;
    }
    
    if (labs(timenow - sr->sam_patimestamp) > context->clockskew) {
	retval = KRB5KRB_AP_ERR_SKEW;
	goto cleanup;
    }

    setflag(enc_tkt_reply->flags, TKT_FLG_HW_AUTH);

  cleanup:
    if (retval)
	com_err("krb5kdc", retval, "sam verify failure");
    if (scratch.data) free(scratch.data);
    if (sr) free(sr);
    if (psr) free(psr);
    if (esre) free(esre);
    if (princ_psr) free(princ_psr);
    if (princ_req) free(princ_req);

    return retval;
}

/* PKINIT preauth support */
#define  PKINIT_DEBUG    0
#if     PKINIT_DEBUG
#define kdcPkinitDebug(args...)       printf(args)
#else
#define kdcPkinitDebug(args...)
#endif

/*
 * get_edata() - our only job is to determine whether this KDC is capable of 
 * performing PKINIT. We infer that from the presence or absence of a 
 * KDC signing cert.
 */
static krb5_error_code get_pkinit_edata(
    krb5_context context, 
    krb5_kdc_req *request,
    krb5_db_entry *client, 
    krb5_db_entry *server,
    krb5_pa_data *pa_data)
{
    pkinit_signing_cert_t cert = NULL;
    krb5_error_code err = pkinit_get_kdc_cert(&cert);
    
    kdcPkinitDebug("get_pkinit_edata: kdc cert %s\n", err ? "NOT FOUND" : "FOUND");
    if(cert) {
	pkinit_release_cert(cert);
    }
    return err;
}

static krb5_error_code verify_pkinit_request(
    krb5_context context,
    krb5_db_entry *client,
    krb5_kdc_req *request,
    krb5_enc_tkt_part *enc_tkt_reply, 
    krb5_pa_data *data)
{
    krb5_error_code	    krtn;
    krb5_data		    pa_data;
    krb5_data		    *der_req = NULL;
    krb5_boolean	    valid_cksum;
    
    /* the data we get from the AS-REQ */
    krb5_timestamp	    client_ctime = 0;
    krb5_ui_4		    client_cusec = 0;
    krb5_timestamp	    kdc_ctime = 0;
    krb5_ui_4		    kdc_cusec = 0;
    krb5_ui_4		    nonce = 0;
    krb5_checksum	    client_cksum;
    pki_cert_sig_status     sig_status;
    krb5_boolean	    is_signed;
    krb5_boolean	    is_encrypted;
    krb5_data		    signer_cert = {0, 0, NULL};
    
    kdcPkinitDebug("verify_pkinit_request\n");

    pa_data.data = data->contents;
    pa_data.length = data->length;
    krtn = pkinit_as_req_parse(&pa_data, &client_ctime, &client_cusec, 
	&nonce, &client_cksum,
	&sig_status, &is_signed, &is_encrypted, &signer_cert,
	/* remaining fields unused (for now) */
	NULL, NULL, NULL, NULL, NULL, NULL);
    if(krtn) {
	kdcPkinitDebug("pa_pk_as_req_parse returned %d; PKINIT aborting.\n", (int)krtn);
	return krtn;
    }
    
    /* verify cert: both by apparent verify status, and its presence in the 
     * client KDB entry */
    switch(sig_status) {
	case pki_cs_good:
	    break;
	case pki_cs_unknown_root:   /* for now allow unknown roots */
	    kdcPkinitDebug("verify_pkinit_request: allowing unknown root\n");
	    break;
	default:
	    kdcPkinitDebug("verify_pkinit_request: bad cert/sig status %d\n", 
		(int)sig_status);
	    krtn = KRB5KDC_ERR_PREAUTH_FAILED;
	    goto cleanup;
    }
    
    if(!is_signed) {
	kdcPkinitDebug("verify_pkinit_request: AS-REQ not signed!\n");
	krtn = KRB5KDC_ERR_PREAUTH_FAILED;
	goto cleanup;
    }
    krtn = krb5_us_timeofday(context, &kdc_ctime, &kdc_cusec);
    if(krtn) {
	goto cleanup;
    }
    if (labs(kdc_ctime - client_ctime) > context->clockskew) {
	kdcPkinitDebug("verify_pkinit_request: clock skew violation client %d svr %d\n",
	    (int)client_ctime, (int)kdc_ctime);
	krtn = KRB5KRB_AP_ERR_SKEW;
	goto cleanup;
    }
    
    /* calculate and compare checksum */
    krtn = encode_krb5_kdc_req_body(request, &der_req);
    if(krtn) {
	kdcPkinitDebug("encode_krb5_kdc_req_body returned %d\n", (int)krtn);
	goto cleanup;
    }
    krtn = krb5_c_verify_checksum(context, NULL, 0, der_req, 
	&client_cksum, &valid_cksum);
    if(krtn) {
	kdcPkinitDebug("krb5_c_verify_checksum returned %d\n", (int)krtn);
	goto cleanup;
    }
    if(!valid_cksum) {
	kdcPkinitDebug("verify_pkinit_request: checksum error\n");
	krtn = KRB5KRB_AP_ERR_BAD_INTEGRITY;
	goto cleanup;
    }
    
    /* TBD look up in the KDB */
    krtn = 0;
    setflag(enc_tkt_reply->flags, TKT_FLG_PRE_AUTH);
cleanup:
    if(client_cksum.contents) {
	free(client_cksum.contents);
    }
    if(signer_cert.data) {
	free(signer_cert.data);
    }
    if (der_req) {
	krb5_free_data(context, der_req);
    }
    kdcPkinitDebug("verify_pkinit_request: returning %d\n", (int)krtn);
    return krtn;
}

static krb5_error_code return_pkinit_response(
    krb5_context context, 
    krb5_pa_data * padata, 
    krb5_db_entry *client,
    krb5_kdc_req *request, 
    krb5_kdc_rep *reply,
    krb5_key_data *client_key,
    krb5_keyblock *encrypting_key,
    krb5_pa_data **send_pa)
{
    krb5_error_code	    krtn;
    krb5_data		    pa_data;
    unsigned		    keylen;
    pkinit_signing_cert_t   signing_cert = NULL;
    
    /* the data we get from the AS-REQ */
    krb5_ui_4		    nonce = 0;
    krb5_checksum	    cksum;
    krb5_data		    client_cert;
    
    if (padata == NULL) {
	/* Client has to send us something */
	return 0;
    }
    
    kdcPkinitDebug("return_pkinit_response\n");
    pa_data.data = padata->contents;
    pa_data.length = padata->length;
    if(pkinit_get_kdc_cert(&signing_cert)) {
	/* should never happen, we were able to get in in get_edata()... */
	kdcPkinitDebug("return_pkinit_response: NO signing cert!\n");
	return KRB5KDC_ERR_PREAUTH_FAILED;
    }
    /* 
     * We've already verified; just obtain the fields we need to create a response 
     */
    krtn = pkinit_as_req_parse(&pa_data, NULL, NULL, &nonce, NULL, NULL,
	NULL, NULL, &client_cert,
	/* remaining fields unused (for now) */
	NULL, NULL, NULL, NULL, NULL, NULL);
    if(krtn) {
	kdcPkinitDebug("pa_pk_as_req_parse returned %d; PKINIT aborting.\n", (int)krtn);
	goto cleanup;
    }
    if(client_cert.data == NULL) {
	kdcPkinitDebug("pa_pk_as_req_parse failed to give a client_cert aborting.\n");
	krtn = KRB5KDC_ERR_PREAUTH_FAILED;
	goto cleanup;
    }

    /* 
     * Cook up keyblock for caller and for outgoing AS-REP.
     * FIXME how much is known to be valid about encrypting_key?
     */
    krb5_c_make_random_key(context, encrypting_key->enctype, encrypting_key);

    krtn = pkinit_as_rep_create(encrypting_key, nonce, signing_cert, TRUE,
	&client_cert, &pa_data);
    if(krtn) {
	kdcPkinitDebug("pa_pk_as_rep_create returned %d; PKINIT aborting.\n", (int)krtn);
	goto cleanup;
    }
    
    *send_pa = (krb5_pa_data *)malloc(sizeof(krb5_pa_data));
    if(*send_pa == NULL) {
	krtn = ENOMEM;
	free(pa_data.data);
	goto cleanup;
    }
    (*send_pa)->magic = KV5M_PA_DATA;
    (*send_pa)->pa_type = KRB5_PADATA_PK_AS_REP;
    (*send_pa)->length = pa_data.length;
    (*send_pa)->contents = pa_data.data;
    krtn = 0;

    #if PKINIT_DEBUG
    fprintf(stderr, "return_pkinit_response: SUCCESS\n");
    fprintf(stderr, "nonce 0x%x enctype %d keylen %d keydata %02x %02x %02x %02x...\n",
	(int)nonce, (int)encrypting_key->enctype, (int)encrypting_key->length,
	encrypting_key->contents[0], encrypting_key->contents[1], 
	encrypting_key->contents[2], encrypting_key->contents[3]);
    #endif

cleanup:
    if(signing_cert) {
	pkinit_release_cert(signing_cert);
    }
    return krtn;
}
