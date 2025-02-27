/* 
   Unix SMB/CIFS implementation.

   interface functions for the sam database

   Copyright (C) Andrew Tridgell 2004
   Copyright (C) Volker Lendecke 2004
   Copyright (C) Andrew Bartlett <abartlet@samba.org> 2006

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "librpc/gen_ndr/ndr_netlogon.h"
#include "librpc/gen_ndr/ndr_security.h"
#include "lib/events/events.h"
#include "lib/ldb-samba/ldb_wrap.h"
#include <ldb.h>
#include <ldb_errors.h>
#include "libcli/security/security.h"
#include "libcli/auth/libcli_auth.h"
#include "libcli/ldap/ldap_ndr.h"
#include "system/time.h"
#include "system/filesys.h"
#include "ldb_wrap.h"
#include "../lib/util/util_ldb.h"
#include "dsdb/samdb/samdb.h"
#include "../libds/common/flags.h"
#include "param/param.h"
#include "lib/events/events.h"
#include "auth/credentials/credentials.h"
#include "param/secrets.h"
#include "auth/auth.h"
#include "lib/tsocket/tsocket.h"

/*
  connect to the SAM database specified by URL
  return an opaque context pointer on success, or NULL on failure
 */
int samdb_connect_url(TALLOC_CTX *mem_ctx,
		      struct tevent_context *ev_ctx,
		      struct loadparm_context *lp_ctx,
		      struct auth_session_info *session_info,
		      unsigned int flags,
		      const char *url,
		      const struct tsocket_address *remote_address,
		      struct ldb_context **ldb_ret,
		      char **errstring)
{
	struct ldb_context *ldb = NULL;
	int ret;
	*ldb_ret = NULL;
	*errstring = NULL;

	/* We create sam.ldb in provision, and never anywhere else */
	flags |= LDB_FLG_DONT_CREATE_DB;

	if (remote_address == NULL) {
		ldb = ldb_wrap_find(url, ev_ctx, lp_ctx,
				    session_info, NULL, flags);
		if (ldb != NULL) {
			*ldb_ret = talloc_reference(mem_ctx, ldb);
			if (*ldb_ret == NULL) {
				return LDB_ERR_OPERATIONS_ERROR;
			}
			return LDB_SUCCESS;
		}
	}

	ldb = samba_ldb_init(mem_ctx, ev_ctx, lp_ctx, session_info, NULL);

	if (ldb == NULL) {
		*errstring = talloc_asprintf(mem_ctx,
					     "Failed to set up Samba ldb "
					     "wrappers with samba_ldb_init() "
					     "to connect to %s",
					     url);
		return LDB_ERR_OPERATIONS_ERROR;
	}

	dsdb_set_global_schema(ldb);

	ret = samba_ldb_connect(ldb, lp_ctx, url, flags);
	if (ret != LDB_SUCCESS) {
		*errstring = talloc_asprintf(mem_ctx,
					     "Failed to connect to %s: %s",
					     url,
					     ldb_errstring(ldb));
		talloc_free(ldb);
		return LDB_ERR_OPERATIONS_ERROR;
	}

	/*
	 * If a remote_address was specified, then set it on the DB
	 * and do not add to the wrap list (as we need to keep the LDB
	 * pointer unique for the address).
	 *
	 * We use this for audit logging and for the "netlogon" attribute
	 */
	if (remote_address != NULL) {
		ldb_set_opaque(ldb, "remoteAddress",
			       discard_const(remote_address));
		*ldb_ret = ldb;
		return LDB_SUCCESS;
	}
		
	if (!ldb_wrap_add(url, ev_ctx, lp_ctx, session_info, NULL, flags, ldb)) {
		*errstring = talloc_asprintf(mem_ctx,
					     "Failed to add cached DB reference"
					     " to %s",
					     url);
		talloc_free(ldb);
		return LDB_ERR_OPERATIONS_ERROR;
	}

	*ldb_ret = ldb;
	return LDB_SUCCESS;
}


/*
  connect to the SAM database
  return an opaque context pointer on success, or NULL on failure
 */
struct ldb_context *samdb_connect(TALLOC_CTX *mem_ctx,
				  struct tevent_context *ev_ctx,
				  struct loadparm_context *lp_ctx,
				  struct auth_session_info *session_info,
				  const struct tsocket_address *remote_address,
				  unsigned int flags)
{
	char *errstring;
	struct ldb_context *ldb;
	int ret = samdb_connect_url(mem_ctx,
				    ev_ctx,
				    lp_ctx,
				    session_info,
				    flags,
				    "sam.ldb",
				    remote_address,
				    &ldb,
				    &errstring);
	if (ret == LDB_SUCCESS) {
		return ldb;
	}
	return NULL;
}

/****************************************************************************
 Create the SID list for this user.
****************************************************************************/
NTSTATUS security_token_create(TALLOC_CTX *mem_ctx, 
			       struct loadparm_context *lp_ctx,
			       uint32_t num_sids,
			       const struct auth_SidAttr *sids,
			       uint32_t session_info_flags,
			       struct security_token **token)
{
	struct security_token *ptoken;
	uint32_t i;
	NTSTATUS status;

	ptoken = security_token_initialise(mem_ctx);
	NT_STATUS_HAVE_NO_MEMORY(ptoken);

	if (num_sids > UINT32_MAX - 6) {
		talloc_free(ptoken);
		return NT_STATUS_INVALID_PARAMETER;
	}
	ptoken->sids = talloc_array(ptoken, struct dom_sid, num_sids + 6 /* over-allocate */);
	if (ptoken->sids == NULL) {
		talloc_free(ptoken);
		return NT_STATUS_NO_MEMORY;
	}

	ptoken->num_sids = 0;

	for (i = 0; i < num_sids; i++) {
		uint32_t check_sid_idx;
		for (check_sid_idx = 0;
		     check_sid_idx < ptoken->num_sids;
		     check_sid_idx++) {
			if (dom_sid_equal(&ptoken->sids[check_sid_idx], &sids[i].sid)) {
				break;
			}
		}

		if (check_sid_idx == ptoken->num_sids) {
			ptoken->sids = talloc_realloc(ptoken, ptoken->sids, struct dom_sid, ptoken->num_sids + 1);
			if (ptoken->sids == NULL) {
				talloc_free(ptoken);
				return NT_STATUS_NO_MEMORY;
			}

			ptoken->sids[ptoken->num_sids] = sids[i].sid;
			ptoken->num_sids++;
		}
	}

	/* The caller may have requested simple privileges, for example if there isn't a local DB */
	if (session_info_flags & AUTH_SESSION_INFO_SIMPLE_PRIVILEGES) {
		/* Shortcuts to prevent recursion and avoid lookups */
		if (ptoken->sids == NULL) {
			ptoken->privilege_mask = 0;
		} else if (security_token_is_system(ptoken)) {
			ptoken->privilege_mask = ~0;
		} else if (security_token_is_anonymous(ptoken)) {
			ptoken->privilege_mask = 0;
		} else if (security_token_has_builtin_administrators(ptoken)) {
			ptoken->privilege_mask = ~0;
		} else {
			/* All other 'users' get a empty priv set so far */
			ptoken->privilege_mask = 0;
		}
	} else {
		/* setup the privilege mask for this token */
		status = samdb_privilege_setup(lp_ctx, ptoken);
		if (!NT_STATUS_IS_OK(status)) {
			talloc_free(ptoken);
			DEBUG(1,("Unable to access privileges database\n"));
			return status;
		}
	}

	security_token_debug(0, 10, ptoken);

	*token = ptoken;

	return NT_STATUS_OK;
}
