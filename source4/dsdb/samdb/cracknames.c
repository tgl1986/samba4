/* 
   Unix SMB/CIFS implementation.

   crachnames implementation for the drsuapi pipe
   DsCrackNames()

   Copyright (C) Stefan Metzmacher 2004
   Copyright (C) Andrew Bartlett <abartlet@samba.org> 2004-2005
   Copyright (C) Matthieu Patou <mat@matws.net> 2012
   Copyright (C) Catalyst .Net Ltd 2017

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
#include "librpc/gen_ndr/drsuapi.h"
#include "lib/events/events.h"
#include <ldb.h>
#include <ldb_errors.h>
#include "auth/kerberos/kerberos.h"
#include "libcli/ldap/ldap_ndr.h"
#include "libcli/security/security.h"
#include "auth/auth.h"
#include "../lib/util/util_ldb.h"
#include "dsdb/samdb/samdb.h"
#include "dsdb/common/util.h"
#include "param/param.h"

#undef strcasecmp

static WERROR DsCrackNameOneFilter(struct ldb_context *sam_ctx, TALLOC_CTX *mem_ctx,
				   struct smb_krb5_context *smb_krb5_context,
				   uint32_t format_flags, enum drsuapi_DsNameFormat format_offered,
				   enum drsuapi_DsNameFormat format_desired,
				   struct ldb_dn *name_dn, const char *name, 
				   const char *domain_filter, const char *result_filter, 
				   struct drsuapi_DsNameInfo1 *info1, int scope, struct ldb_dn *search_dn);
static WERROR DsCrackNameOneSyntactical(TALLOC_CTX *mem_ctx,
					enum drsuapi_DsNameFormat format_offered,
					enum drsuapi_DsNameFormat format_desired,
					struct ldb_dn *name_dn, const char *name, 
					struct drsuapi_DsNameInfo1 *info1);

static WERROR dns_domain_from_principal(TALLOC_CTX *mem_ctx, struct smb_krb5_context *smb_krb5_context, 
					const char *name, 
					struct drsuapi_DsNameInfo1 *info1) 
{
	krb5_error_code ret;
	krb5_principal principal;
	/* perhaps it's a principal with a realm, so return the right 'domain only' response */
	ret = krb5_parse_name_flags(smb_krb5_context->krb5_context, name, 
				    KRB5_PRINCIPAL_PARSE_REQUIRE_REALM, &principal);
	if (ret) {
		info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
		return WERR_OK;
	}

	info1->dns_domain_name = smb_krb5_principal_get_realm(
		mem_ctx, smb_krb5_context->krb5_context, principal);
	krb5_free_principal(smb_krb5_context->krb5_context, principal);

	W_ERROR_HAVE_NO_MEMORY(info1->dns_domain_name);

	info1->status = DRSUAPI_DS_NAME_STATUS_DOMAIN_ONLY;
	return WERR_OK;
}

static enum drsuapi_DsNameStatus LDB_lookup_spn_alias(struct ldb_context *ldb_ctx,
						      TALLOC_CTX *mem_ctx,
						      const char *alias_from,
						      char **alias_to)
{
	/*
	 * Some of the logic of this function is mirrored in find_spn_alias()
	 * in source4/dsdb.samdb/ldb_modules/samldb.c. If you change this to
	 * not return the first matched alias, you will need to rethink that
	 * function too.
	 */
	unsigned int i;
	int ret;
	struct ldb_result *res;
	struct ldb_message_element *spnmappings;
	TALLOC_CTX *tmp_ctx;
	struct ldb_dn *service_dn;
	char *service_dn_str;

	const char *directory_attrs[] = {
		"sPNMappings", 
		NULL
	};

	tmp_ctx = talloc_new(mem_ctx);
	if (!tmp_ctx) {
		return DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
	}

	service_dn = ldb_dn_new(tmp_ctx, ldb_ctx, "CN=Directory Service,CN=Windows NT,CN=Services");
	if ( ! ldb_dn_add_base(service_dn, ldb_get_config_basedn(ldb_ctx))) {
		talloc_free(tmp_ctx);
		return DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
	}
	service_dn_str = ldb_dn_alloc_linearized(tmp_ctx, service_dn);
	if ( ! service_dn_str) {
		talloc_free(tmp_ctx);
		return DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
	}

	ret = ldb_search(ldb_ctx, tmp_ctx, &res, service_dn, LDB_SCOPE_BASE,
			 directory_attrs, "(objectClass=nTDSService)");

	if (ret != LDB_SUCCESS && ret != LDB_ERR_NO_SUCH_OBJECT) {
		DEBUG(1, ("ldb_search: dn: %s not found: %s\n", service_dn_str, ldb_errstring(ldb_ctx)));
		talloc_free(tmp_ctx);
		return DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
	} else if (ret == LDB_ERR_NO_SUCH_OBJECT) {
		DEBUG(1, ("ldb_search: dn: %s not found\n", service_dn_str));
		talloc_free(tmp_ctx);
		return DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
	} else if (res->count != 1) {
		DEBUG(1, ("ldb_search: dn: %s not found\n", service_dn_str));
		talloc_free(tmp_ctx);
		return DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
	}

	spnmappings = ldb_msg_find_element(res->msgs[0], "sPNMappings");
	if (!spnmappings || spnmappings->num_values == 0) {
		DEBUG(1, ("ldb_search: dn: %s no sPNMappings attribute\n", service_dn_str));
		talloc_free(tmp_ctx);
		return DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
	}

	for (i = 0; i < spnmappings->num_values; i++) {
		char *mapping, *p, *str;
		mapping = talloc_strdup(tmp_ctx, 
					(const char *)spnmappings->values[i].data);
		if (!mapping) {
			DEBUG(1, ("LDB_lookup_spn_alias: ldb_search: dn: %s did not have an sPNMapping\n", service_dn_str));
			talloc_free(tmp_ctx);
			return DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
		}

		/* C string manipulation sucks */

		p = strchr(mapping, '=');
		if (!p) {
			DEBUG(1, ("ldb_search: dn: %s sPNMapping malformed: %s\n", 
				  service_dn_str, mapping));
			talloc_free(tmp_ctx);
			return DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
		}
		p[0] = '\0';
		p++;
		do {
			str = p;
			p = strchr(p, ',');
			if (p) {
				p[0] = '\0';
				p++;
			}
			if (strcasecmp(str, alias_from) == 0) {
				*alias_to = mapping;
				talloc_steal(mem_ctx, mapping);
				talloc_free(tmp_ctx);
				return DRSUAPI_DS_NAME_STATUS_OK;
			}
		} while (p);
	}
	DEBUG(4, ("LDB_lookup_spn_alias: no alias for service %s applicable\n", alias_from));
	talloc_free(tmp_ctx);
	return DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
}

/* When cracking a ServicePrincipalName, many services may be served
 * by the host/ servicePrincipalName.  The incoming query is for cifs/
 * but we translate it here, and search on host/.  This is done after
 * the cifs/ entry has been searched for, making this a fallback */

static WERROR DsCrackNameSPNAlias(struct ldb_context *sam_ctx, TALLOC_CTX *mem_ctx,
				  struct smb_krb5_context *smb_krb5_context,
				  uint32_t format_flags, enum drsuapi_DsNameFormat format_offered,
				  enum drsuapi_DsNameFormat format_desired,
				  const char *name, struct drsuapi_DsNameInfo1 *info1)
{
	WERROR wret;
	krb5_error_code ret;
	krb5_principal principal;
	krb5_data component;
	const char *service, *dns_name;
	char *new_service;
	char *new_princ;
	enum drsuapi_DsNameStatus namestatus;

	/* parse principal */
	ret = krb5_parse_name_flags(smb_krb5_context->krb5_context, 
				    name, KRB5_PRINCIPAL_PARSE_NO_REALM, &principal);
	if (ret) {
		DEBUG(2, ("Could not parse principal: %s: %s\n",
			  name, smb_get_krb5_error_message(smb_krb5_context->krb5_context, 
							   ret, mem_ctx)));
		return WERR_NOT_ENOUGH_MEMORY;
	}

	/* grab cifs/, http/ etc */

	ret = smb_krb5_princ_component(smb_krb5_context->krb5_context,
				       principal, 0, &component);
	if (ret) {
		info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
		krb5_free_principal(smb_krb5_context->krb5_context, principal);
		return WERR_OK;
	}
	service = (const char *)component.data;
	ret = smb_krb5_princ_component(smb_krb5_context->krb5_context,
				       principal, 1, &component);
	if (ret) {
		info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
		krb5_free_principal(smb_krb5_context->krb5_context, principal);
		return WERR_OK;
	}
	dns_name = (const char *)component.data;

	/* MAP it */
	namestatus = LDB_lookup_spn_alias(sam_ctx, mem_ctx,
					  service, &new_service);

	if (namestatus == DRSUAPI_DS_NAME_STATUS_NOT_FOUND) {
		wret = WERR_OK;
		info1->status		= DRSUAPI_DS_NAME_STATUS_DOMAIN_ONLY;
		info1->dns_domain_name	= talloc_strdup(mem_ctx, dns_name);
		if (!info1->dns_domain_name) {
			wret = WERR_NOT_ENOUGH_MEMORY;
		}
		krb5_free_principal(smb_krb5_context->krb5_context, principal);
		return wret;
	} else if (namestatus != DRSUAPI_DS_NAME_STATUS_OK) {
		info1->status = namestatus;
		krb5_free_principal(smb_krb5_context->krb5_context, principal);
		return WERR_OK;
	}

	/* reform principal */
	new_princ = talloc_asprintf(mem_ctx, "%s/%s", new_service, dns_name);
	if (!new_princ) {
		krb5_free_principal(smb_krb5_context->krb5_context, principal);
		return WERR_NOT_ENOUGH_MEMORY;
	}

	wret = DsCrackNameOneName(sam_ctx, mem_ctx, format_flags, format_offered, format_desired,
				  new_princ, info1);
	talloc_free(new_princ);
	if (W_ERROR_IS_OK(wret) && (info1->status == DRSUAPI_DS_NAME_STATUS_NOT_FOUND)) {
		info1->status		= DRSUAPI_DS_NAME_STATUS_DOMAIN_ONLY;
		info1->dns_domain_name	= talloc_strdup(mem_ctx, dns_name);
		if (!info1->dns_domain_name) {
			wret = WERR_NOT_ENOUGH_MEMORY;
		}
	}
	krb5_free_principal(smb_krb5_context->krb5_context, principal);
	return wret;
}

/* Subcase of CrackNames, for the userPrincipalName */

static WERROR DsCrackNameUPN(struct ldb_context *sam_ctx, TALLOC_CTX *mem_ctx,
			     struct smb_krb5_context *smb_krb5_context,
			     uint32_t format_flags, enum drsuapi_DsNameFormat format_offered,
			     enum drsuapi_DsNameFormat format_desired,
			     const char *name, struct drsuapi_DsNameInfo1 *info1)
{
	int ldb_ret;
	WERROR status;
	const char *domain_filter = NULL;
	const char *result_filter = NULL;
	krb5_error_code ret;
	krb5_principal principal;
	char *realm;
	char *realm_encoded = NULL;
	char *unparsed_name_short;
	const char *unparsed_name_short_encoded = NULL;
	const char *domain_attrs[] = { NULL };
	struct ldb_result *domain_res = NULL;

	/* Prevent recursion */
	if (!name) {
		info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
		return WERR_OK;
	}

	ret = krb5_parse_name_flags(smb_krb5_context->krb5_context, name, 
				    KRB5_PRINCIPAL_PARSE_REQUIRE_REALM, &principal);
	if (ret) {
		info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
		return WERR_OK;
	}

	realm = smb_krb5_principal_get_realm(
		mem_ctx, smb_krb5_context->krb5_context, principal);
	if (realm == NULL) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	realm_encoded = ldb_binary_encode_string(mem_ctx, realm);
	if (realm_encoded == NULL) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	ldb_ret = ldb_search(sam_ctx, mem_ctx, &domain_res,
			     samdb_partitions_dn(sam_ctx, mem_ctx),
			     LDB_SCOPE_ONELEVEL,
			     domain_attrs,
			     "(&(objectClass=crossRef)(|(dnsRoot=%s)(netbiosName=%s))"
			     "(systemFlags:"LDB_OID_COMPARATOR_AND":=%u))",
			     realm_encoded,
			     realm_encoded,
			     SYSTEM_FLAG_CR_NTDS_DOMAIN);
	TALLOC_FREE(realm_encoded);
	TALLOC_FREE(realm);

	if (ldb_ret != LDB_SUCCESS) {
		DEBUG(2, ("DsCrackNameUPN domain ref search failed: %s\n", ldb_errstring(sam_ctx)));
		info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
		krb5_free_principal(smb_krb5_context->krb5_context, principal);
		return WERR_OK;
	}

	switch (domain_res->count) {
	case 1:
		break;
	case 0:
		krb5_free_principal(smb_krb5_context->krb5_context, principal);
		return dns_domain_from_principal(mem_ctx, smb_krb5_context, 
						 name, info1);
	default:
		info1->status = DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE;
		krb5_free_principal(smb_krb5_context->krb5_context, principal);
		return WERR_OK;
	}

	/*
	 * The important thing here is that a samAccountName may have
	 * a space in it, and this must not be kerberos escaped to
	 * match this filter, so we specify
	 * KRB5_PRINCIPAL_UNPARSE_DISPLAY
	 */
	ret = krb5_unparse_name_flags(smb_krb5_context->krb5_context, principal, 
				      KRB5_PRINCIPAL_UNPARSE_NO_REALM |
				      KRB5_PRINCIPAL_UNPARSE_DISPLAY,
				      &unparsed_name_short);
	krb5_free_principal(smb_krb5_context->krb5_context, principal);

	if (ret) {
		free(unparsed_name_short);
		return WERR_NOT_ENOUGH_MEMORY;
	}

	unparsed_name_short_encoded = ldb_binary_encode_string(mem_ctx, unparsed_name_short);
	if (unparsed_name_short_encoded == NULL) {
		free(unparsed_name_short);
		return WERR_NOT_ENOUGH_MEMORY;
	}

	/* This may need to be extended for more userPrincipalName variations */
	result_filter = talloc_asprintf(mem_ctx, "(&(samAccountName=%s)(objectClass=user))",
					unparsed_name_short_encoded);

	domain_filter = talloc_asprintf(mem_ctx, "(distinguishedName=%s)", ldb_dn_get_linearized(domain_res->msgs[0]->dn));

	if (!result_filter || !domain_filter) {
		free(unparsed_name_short);
		return WERR_NOT_ENOUGH_MEMORY;
	}
	status = DsCrackNameOneFilter(sam_ctx, mem_ctx, 
				      smb_krb5_context, 
				      format_flags, format_offered, format_desired, 
				      NULL, unparsed_name_short, domain_filter, result_filter, 
				      info1, LDB_SCOPE_SUBTREE, NULL);
	free(unparsed_name_short);

	return status;
}

/*
 * This function will workout the filtering parameter in order to be able to do
 * the adapted search when the incoming format is format_functional.
 * This boils down to defining the search_dn (passed as pointer to ldb_dn *) and the
 * ldap filter request.
 * Main input parameters are:
 * * name, which is the portion of the functional name after the
 * first '/'.
 * * domain_filter, which is a ldap search filter used to find the NC DN given the
 * function name to crack.
 */
static WERROR get_format_functional_filtering_param(struct ldb_context *sam_ctx, TALLOC_CTX *mem_ctx,
			char *name, struct drsuapi_DsNameInfo1 *info1,
			struct ldb_dn **psearch_dn, const char *domain_filter, const char **presult_filter)
{
	struct ldb_result *domain_res = NULL;
	const char * const domain_attrs[] = {"ncName", NULL};
	struct ldb_dn *partitions_basedn = samdb_partitions_dn(sam_ctx, mem_ctx);
	int ldb_ret;
	char *account,  *s, *result_filter = NULL;
	struct ldb_dn *search_dn = NULL;

	*psearch_dn = NULL;
	*presult_filter = NULL;

	ldb_ret = ldb_search(sam_ctx, mem_ctx, &domain_res,
				partitions_basedn,
				LDB_SCOPE_ONELEVEL,
				domain_attrs,
				"%s", domain_filter);

	if (ldb_ret != LDB_SUCCESS) {
		DEBUG(2, ("DsCrackNameOne domain ref search failed: %s\n", ldb_errstring(sam_ctx)));
		info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
		return WERR_FOOBAR;
	}

	if (domain_res->count == 1) {
		struct ldb_dn *tmp_dn = samdb_result_dn(sam_ctx, mem_ctx, domain_res->msgs[0], "ncName", NULL);
		const char * const name_attrs[] = {"name", NULL};

		account = name;
		s = strchr(account, '/');
		talloc_free(domain_res);
		while(s) {
			s[0] = '\0';
			s++;

			ldb_ret = ldb_search(sam_ctx, mem_ctx, &domain_res,
						tmp_dn,
						LDB_SCOPE_ONELEVEL,
						name_attrs,
						"name=%s", account);

			if (ldb_ret != LDB_SUCCESS) {
				DEBUG(2, ("DsCrackNameOne domain ref search failed: %s\n", ldb_errstring(sam_ctx)));
				info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
				return WERR_OK;
			}
			talloc_free(tmp_dn);
			switch (domain_res->count) {
			case 1:
				break;
			case 0:
				talloc_free(domain_res);
				info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
				return WERR_OK;
			default:
				talloc_free(domain_res);
				info1->status = DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE;
				return WERR_OK;
			}

			tmp_dn = talloc_steal(mem_ctx, domain_res->msgs[0]->dn);
			talloc_free(domain_res);
			search_dn = tmp_dn;
			account = s;
			s = strchr(account, '/');
		}
		account = ldb_binary_encode_string(mem_ctx, account);
		W_ERROR_HAVE_NO_MEMORY(account);
		result_filter = talloc_asprintf(mem_ctx, "(name=%s)",
						account);
		W_ERROR_HAVE_NO_MEMORY(result_filter);
	}
	*psearch_dn = search_dn;
	*presult_filter = result_filter;
	return WERR_OK;
}

/* Crack a single 'name', from format_offered into format_desired, returning the result in info1 */

WERROR DsCrackNameOneName(struct ldb_context *sam_ctx, TALLOC_CTX *mem_ctx,
			  uint32_t format_flags, enum drsuapi_DsNameFormat format_offered,
			  enum drsuapi_DsNameFormat format_desired,
			  const char *name, struct drsuapi_DsNameInfo1 *info1)
{
	krb5_error_code ret;
	const char *domain_filter = NULL;
	const char *result_filter = NULL;
	struct ldb_dn *name_dn = NULL;
	struct ldb_dn *search_dn = NULL;

	struct smb_krb5_context *smb_krb5_context = NULL;
	int scope = LDB_SCOPE_SUBTREE;

	info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
	info1->dns_domain_name = NULL;
	info1->result_name = NULL;

	if (!name) {
		return WERR_INVALID_PARAMETER;
	}

	/* TODO: - fill the correct names in all cases!
	 *       - handle format_flags
	 */
	if (format_desired == DRSUAPI_DS_NAME_FORMAT_UNKNOWN) {
		return WERR_OK;
	}
	/* here we need to set the domain_filter and/or the result_filter */
	switch (format_offered) {
	case DRSUAPI_DS_NAME_FORMAT_UNKNOWN:
	{
		unsigned int i;
		enum drsuapi_DsNameFormat formats[] = {
			DRSUAPI_DS_NAME_FORMAT_FQDN_1779, DRSUAPI_DS_NAME_FORMAT_USER_PRINCIPAL,
			DRSUAPI_DS_NAME_FORMAT_NT4_ACCOUNT, DRSUAPI_DS_NAME_FORMAT_CANONICAL,
			DRSUAPI_DS_NAME_FORMAT_GUID, DRSUAPI_DS_NAME_FORMAT_DISPLAY,
			DRSUAPI_DS_NAME_FORMAT_SERVICE_PRINCIPAL,
			DRSUAPI_DS_NAME_FORMAT_SID_OR_SID_HISTORY,
			DRSUAPI_DS_NAME_FORMAT_CANONICAL_EX
		};
		WERROR werr;
		for (i=0; i < ARRAY_SIZE(formats); i++) {
			werr = DsCrackNameOneName(sam_ctx, mem_ctx, format_flags, formats[i], format_desired, name, info1);
			if (!W_ERROR_IS_OK(werr)) {
				return werr;
			}
			if (info1->status != DRSUAPI_DS_NAME_STATUS_NOT_FOUND &&
			    (formats[i] != DRSUAPI_DS_NAME_FORMAT_CANONICAL ||
			     info1->status != DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR))
			{
				return werr;
			}
		}
		return werr;
	}

	case DRSUAPI_DS_NAME_FORMAT_CANONICAL:
	case DRSUAPI_DS_NAME_FORMAT_CANONICAL_EX:
	{
		char *str, *s, *account;
		const char *str_encoded = NULL;
		scope = LDB_SCOPE_ONELEVEL;

		if (strlen(name) == 0) {
			info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
			return WERR_OK;
		}

		str = talloc_strdup(mem_ctx, name);
		W_ERROR_HAVE_NO_MEMORY(str);

		if (format_offered == DRSUAPI_DS_NAME_FORMAT_CANONICAL_EX) {
			/* Look backwards for the \n, and replace it with / */
			s = strrchr(str, '\n');
			if (!s) {
				info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
				return WERR_OK;
			}
			s[0] = '/';
		}

		s = strchr(str, '/');
		if (!s) {
			/* there must be at least one / */
			info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
			return WERR_OK;
		}

		s[0] = '\0';
		s++;

		str_encoded = ldb_binary_encode_string(mem_ctx, str);
		if (str_encoded == NULL) {
			return WERR_NOT_ENOUGH_MEMORY;
		}

		domain_filter = talloc_asprintf(mem_ctx, "(&(objectClass=crossRef)(dnsRoot=%s)(systemFlags:%s:=%u))",
						str_encoded,
						LDB_OID_COMPARATOR_AND,
						SYSTEM_FLAG_CR_NTDS_DOMAIN);
		W_ERROR_HAVE_NO_MEMORY(domain_filter);

		/* There may not be anything after the domain component (search for the domain itself) */
		account = s;
		if (account && *account) {
			WERROR werr = get_format_functional_filtering_param(sam_ctx,
										mem_ctx,
										account,
										info1,
										&search_dn,
										domain_filter,
										&result_filter);
			if (!W_ERROR_IS_OK(werr)) {
				return werr;
			}
			if (info1->status != DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR)
				return WERR_OK;
		}
		break;
	}
	case DRSUAPI_DS_NAME_FORMAT_NT4_ACCOUNT: {
		char *p;
		char *domain;
		char *domain_encoded = NULL;
		const char *account = NULL;

		domain = talloc_strdup(mem_ctx, name);
		W_ERROR_HAVE_NO_MEMORY(domain);

		p = strchr(domain, '\\');
		if (!p) {
			/* invalid input format */
			info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
			return WERR_OK;
		}
		p[0] = '\0';

		if (p[1]) {
			account = &p[1];
		}

		domain_encoded = ldb_binary_encode_string(mem_ctx, domain);
		if (domain_encoded == NULL) {
			return WERR_NOT_ENOUGH_MEMORY;
		}

		domain_filter = talloc_asprintf(mem_ctx, 
						"(&(objectClass=crossRef)(netbiosName=%s)(systemFlags:%s:=%u))",
						domain_encoded,
						LDB_OID_COMPARATOR_AND,
						SYSTEM_FLAG_CR_NTDS_DOMAIN);
		W_ERROR_HAVE_NO_MEMORY(domain_filter);
		if (account) {
			const char *account_encoded = NULL;

			account_encoded = ldb_binary_encode_string(mem_ctx, account);
			if (account_encoded == NULL) {
				return WERR_NOT_ENOUGH_MEMORY;
			}

			result_filter = talloc_asprintf(mem_ctx, "(sAMAccountName=%s)",
							account_encoded);
			W_ERROR_HAVE_NO_MEMORY(result_filter);
		}

		talloc_free(domain);
		break;
	}

		/* A LDAP DN as a string */
	case DRSUAPI_DS_NAME_FORMAT_FQDN_1779: {
		domain_filter = NULL;
		name_dn = ldb_dn_new(mem_ctx, sam_ctx, name);
		if (! ldb_dn_validate(name_dn)) {
			info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
			return WERR_OK;
		}
		break;
	}

		/* A GUID as a string */
	case DRSUAPI_DS_NAME_FORMAT_GUID: {
		struct GUID guid;
		char *ldap_guid;
		NTSTATUS nt_status;
		domain_filter = NULL;

		nt_status = GUID_from_string(name, &guid);
		if (!NT_STATUS_IS_OK(nt_status)) {
			info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
			return WERR_OK;
		}

		ldap_guid = ldap_encode_ndr_GUID(mem_ctx, &guid);
		if (!ldap_guid) {
			return WERR_NOT_ENOUGH_MEMORY;
		}
		result_filter = talloc_asprintf(mem_ctx, "(objectGUID=%s)",
						ldap_guid);
		W_ERROR_HAVE_NO_MEMORY(result_filter);
		break;
	}
	case DRSUAPI_DS_NAME_FORMAT_DISPLAY: {
		const char *name_encoded = NULL;

		domain_filter = NULL;

		name_encoded = ldb_binary_encode_string(mem_ctx, name);
		if (name_encoded == NULL) {
			return WERR_NOT_ENOUGH_MEMORY;
		}

		result_filter = talloc_asprintf(mem_ctx, "(|(displayName=%s)(samAccountName=%s))",
						name_encoded,
						name_encoded);
		W_ERROR_HAVE_NO_MEMORY(result_filter);
		break;
	}

		/* A S-1234-5678 style string */
	case DRSUAPI_DS_NAME_FORMAT_SID_OR_SID_HISTORY: {
		struct dom_sid *sid = dom_sid_parse_talloc(mem_ctx, name);
		char *ldap_sid;

		domain_filter = NULL;
		if (!sid) {
			info1->dns_domain_name = NULL;
			info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
			return WERR_OK;
		}
		ldap_sid = ldap_encode_ndr_dom_sid(mem_ctx, 
						   sid);
		if (!ldap_sid) {
			return WERR_NOT_ENOUGH_MEMORY;
		}
		result_filter = talloc_asprintf(mem_ctx, "(objectSid=%s)",
						ldap_sid);
		W_ERROR_HAVE_NO_MEMORY(result_filter);
		break;
	}
	case DRSUAPI_DS_NAME_FORMAT_USER_PRINCIPAL: {
		krb5_principal principal;
		char *unparsed_name;
		const char *unparsed_name_encoded = NULL;

		ret = smb_krb5_init_context(mem_ctx, 
					    (struct loadparm_context *)ldb_get_opaque(sam_ctx, "loadparm"), 
					    &smb_krb5_context);

		if (ret) {
			return WERR_NOT_ENOUGH_MEMORY;
		}

		/* Ensure we reject complete junk first */
		ret = krb5_parse_name(smb_krb5_context->krb5_context, name, &principal);
		if (ret) {
			info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
			return WERR_OK;
		}

		domain_filter = NULL;

		/*
		 * By getting the unparsed name here, we ensure the
		 * escaping is removed correctly (and trust the client
		 * less).  The important thing here is that a
		 * userPrincipalName may have a space in it, and this
		 * must not be kerberos escaped to match this filter,
		 * so we specify KRB5_PRINCIPAL_UNPARSE_DISPLAY
		 */
		ret = krb5_unparse_name_flags(smb_krb5_context->krb5_context,
					      principal,
					      KRB5_PRINCIPAL_UNPARSE_DISPLAY,
					      &unparsed_name);
		if (ret) {
			krb5_free_principal(smb_krb5_context->krb5_context, principal);
			return WERR_NOT_ENOUGH_MEMORY;
		}

		krb5_free_principal(smb_krb5_context->krb5_context, principal);

		/* The ldb_binary_encode_string() here avoids LDAP filter injection attacks */
		unparsed_name_encoded = ldb_binary_encode_string(mem_ctx, unparsed_name);
		if (unparsed_name_encoded == NULL) {
			return WERR_NOT_ENOUGH_MEMORY;
		}

		result_filter = talloc_asprintf(mem_ctx, "(&(userPrincipalName=%s)(objectClass=user))",
						unparsed_name_encoded);

		free(unparsed_name);
		W_ERROR_HAVE_NO_MEMORY(result_filter);
		break;
	}
	case DRSUAPI_DS_NAME_FORMAT_SERVICE_PRINCIPAL: {
		krb5_principal principal;
		char *unparsed_name_short;
		const char *unparsed_name_short_encoded = NULL;
		bool principal_is_host = false;

		ret = smb_krb5_init_context(mem_ctx, 
					    (struct loadparm_context *)ldb_get_opaque(sam_ctx, "loadparm"), 
					    &smb_krb5_context);

		if (ret) {
			return WERR_NOT_ENOUGH_MEMORY;
		}

		ret = krb5_parse_name(smb_krb5_context->krb5_context, name, &principal);
		if (ret == 0 &&
		    krb5_princ_size(smb_krb5_context->krb5_context,
							principal) < 2) {
			info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
			krb5_free_principal(smb_krb5_context->krb5_context, principal);
			return WERR_OK;
		} else if (ret == 0) {
			krb5_free_principal(smb_krb5_context->krb5_context, principal);
		}
		ret = krb5_parse_name_flags(smb_krb5_context->krb5_context, name, 
					    KRB5_PRINCIPAL_PARSE_NO_REALM, &principal);
		if (ret) {
			return dns_domain_from_principal(mem_ctx, smb_krb5_context,
							 name, info1);
		}

		domain_filter = NULL;

		ret = krb5_unparse_name_flags(smb_krb5_context->krb5_context, principal, 
					      KRB5_PRINCIPAL_UNPARSE_NO_REALM, &unparsed_name_short);
		if (ret) {
			krb5_free_principal(smb_krb5_context->krb5_context, principal);
			return WERR_NOT_ENOUGH_MEMORY;
		}

		unparsed_name_short_encoded = ldb_binary_encode_string(mem_ctx, unparsed_name_short);
		if (unparsed_name_short_encoded == NULL) {
			krb5_free_principal(smb_krb5_context->krb5_context, principal);
			free(unparsed_name_short);
			return WERR_NOT_ENOUGH_MEMORY;
		}

		if ((krb5_princ_size(smb_krb5_context->krb5_context, principal) == 2)) {
			krb5_data component;

			ret = smb_krb5_princ_component(smb_krb5_context->krb5_context,
						       principal, 0, &component);
			if (ret) {
				krb5_free_principal(smb_krb5_context->krb5_context, principal);
				free(unparsed_name_short);
				return WERR_INTERNAL_ERROR;
			}

			principal_is_host = strcasecmp(component.data, "host") == 0;
		}

		if (principal_is_host) {
			/* the 'cn' attribute is just the leading part of the name */
			krb5_data component;
			char *computer_name;
			const char *computer_name_encoded = NULL;
			ret = smb_krb5_princ_component(
				smb_krb5_context->krb5_context,
				principal, 1, &component);
			if (ret) {
				krb5_free_principal(smb_krb5_context->krb5_context, principal);
				free(unparsed_name_short);
				return WERR_INTERNAL_ERROR;
			}
			computer_name = talloc_strndup(mem_ctx, (char *)component.data,
							strcspn((char *)component.data, "."));
			if (computer_name == NULL) {
				krb5_free_principal(smb_krb5_context->krb5_context, principal);
				free(unparsed_name_short);
				return WERR_NOT_ENOUGH_MEMORY;
			}

			computer_name_encoded = ldb_binary_encode_string(mem_ctx, computer_name);
			if (computer_name_encoded == NULL) {
				krb5_free_principal(smb_krb5_context->krb5_context, principal);
				free(unparsed_name_short);
				return WERR_NOT_ENOUGH_MEMORY;
			}

			result_filter = talloc_asprintf(mem_ctx, "(|(&(servicePrincipalName=%s)(objectClass=user))(&(cn=%s)(objectClass=computer)))", 
							unparsed_name_short_encoded,
							computer_name_encoded);
		} else {
			result_filter = talloc_asprintf(mem_ctx, "(&(servicePrincipalName=%s)(objectClass=user))",
							unparsed_name_short_encoded);
		}
		krb5_free_principal(smb_krb5_context->krb5_context, principal);
		free(unparsed_name_short);
		W_ERROR_HAVE_NO_MEMORY(result_filter);

		break;
	}
	default: {
		info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
		return WERR_OK;
	}
	}

	if (format_flags & DRSUAPI_DS_NAME_FLAG_SYNTACTICAL_ONLY) {
		return DsCrackNameOneSyntactical(mem_ctx, format_offered, format_desired,
						 name_dn, name, info1);
	}

	return DsCrackNameOneFilter(sam_ctx, mem_ctx, 
				    smb_krb5_context, 
				    format_flags, format_offered, format_desired, 
				    name_dn, name, 
				    domain_filter, result_filter, 
				    info1, scope, search_dn);
}

/* Subcase of CrackNames.  It is possible to translate a LDAP-style DN
 * (FQDN_1779) into a canonical name without actually searching the
 * database */

static WERROR DsCrackNameOneSyntactical(TALLOC_CTX *mem_ctx,
					enum drsuapi_DsNameFormat format_offered,
					enum drsuapi_DsNameFormat format_desired,
					struct ldb_dn *name_dn, const char *name, 
					struct drsuapi_DsNameInfo1 *info1)
{
	char *cracked;
	if (format_offered != DRSUAPI_DS_NAME_FORMAT_FQDN_1779) {
		info1->status = DRSUAPI_DS_NAME_STATUS_NO_SYNTACTICAL_MAPPING;
		return WERR_OK;
	}

	switch (format_desired) {
	case DRSUAPI_DS_NAME_FORMAT_CANONICAL: 
		cracked = ldb_dn_canonical_string(mem_ctx, name_dn);
		break;
	case DRSUAPI_DS_NAME_FORMAT_CANONICAL_EX:
		cracked = ldb_dn_canonical_ex_string(mem_ctx, name_dn);
		break;
	default:
		info1->status = DRSUAPI_DS_NAME_STATUS_NO_SYNTACTICAL_MAPPING;
		return WERR_OK;
	}
	info1->status = DRSUAPI_DS_NAME_STATUS_OK;
	info1->result_name	= cracked;
	if (!cracked) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	return WERR_OK;	
}

/* Given a filter for the domain, and one for the result, perform the
 * ldb search. The format offered and desired flags change the
 * behaviours, including what attributes to return.
 *
 * The smb_krb5_context is required because we use the krb5 libs for principal parsing
 */

static WERROR DsCrackNameOneFilter(struct ldb_context *sam_ctx, TALLOC_CTX *mem_ctx,
				   struct smb_krb5_context *smb_krb5_context,
				   uint32_t format_flags, enum drsuapi_DsNameFormat format_offered,
				   enum drsuapi_DsNameFormat format_desired,
				   struct ldb_dn *name_dn, const char *name, 
				   const char *domain_filter, const char *result_filter, 
				   struct drsuapi_DsNameInfo1 *info1,
				   int scope, struct ldb_dn *search_dn)
{
	int ldb_ret;
	struct ldb_result *domain_res = NULL;
	const char * const *domain_attrs;
	const char * const *result_attrs;
	struct ldb_message **result_res = NULL;
	struct ldb_message *result = NULL;
	int i;
	char *p;
	struct ldb_dn *partitions_basedn = samdb_partitions_dn(sam_ctx, mem_ctx);

	const char * const _domain_attrs_1779[] = { "ncName", "dnsRoot", NULL};
	const char * const _result_attrs_null[] = { NULL };

	const char * const _domain_attrs_canonical[] = { "ncName", "dnsRoot", NULL};
	const char * const _result_attrs_canonical[] = { "canonicalName", NULL };

	const char * const _domain_attrs_nt4[] = { "ncName", "dnsRoot", "nETBIOSName", NULL};
	const char * const _result_attrs_nt4[] = { "sAMAccountName", "objectSid", "objectClass", NULL};

	const char * const _domain_attrs_guid[] = { "ncName", "dnsRoot", NULL};
	const char * const _result_attrs_guid[] = { "objectGUID", NULL};

	const char * const _domain_attrs_upn[] = { "ncName", "dnsRoot", NULL};
	const char * const _result_attrs_upn[] = { "userPrincipalName", NULL};

	const char * const _domain_attrs_spn[] = { "ncName", "dnsRoot", NULL};
	const char * const _result_attrs_spn[] = { "servicePrincipalName", NULL};

	const char * const _domain_attrs_display[] = { "ncName", "dnsRoot", NULL};
	const char * const _result_attrs_display[] = { "displayName", "samAccountName", NULL};

	const char * const _domain_attrs_sid[] = { "ncName", "dnsRoot", NULL};
	const char * const _result_attrs_sid[] = { "objectSid", NULL};

	const char * const _domain_attrs_none[] = { "ncName", "dnsRoot" , NULL};
	const char * const _result_attrs_none[] = { NULL};

	/* here we need to set the attrs lists for domain and result lookups */
	switch (format_desired) {
	case DRSUAPI_DS_NAME_FORMAT_FQDN_1779:
	case DRSUAPI_DS_NAME_FORMAT_CANONICAL_EX:
		domain_attrs = _domain_attrs_1779;
		result_attrs = _result_attrs_null;
		break;
	case DRSUAPI_DS_NAME_FORMAT_CANONICAL:
		domain_attrs = _domain_attrs_canonical;
		result_attrs = _result_attrs_canonical;
		break;
	case DRSUAPI_DS_NAME_FORMAT_NT4_ACCOUNT:
		domain_attrs = _domain_attrs_nt4;
		result_attrs = _result_attrs_nt4;
		break;
	case DRSUAPI_DS_NAME_FORMAT_GUID:		
		domain_attrs = _domain_attrs_guid;
		result_attrs = _result_attrs_guid;
		break;
	case DRSUAPI_DS_NAME_FORMAT_DISPLAY:		
		domain_attrs = _domain_attrs_display;
		result_attrs = _result_attrs_display;
		break;
	case DRSUAPI_DS_NAME_FORMAT_USER_PRINCIPAL:
		domain_attrs = _domain_attrs_upn;
		result_attrs = _result_attrs_upn;
		break;
	case DRSUAPI_DS_NAME_FORMAT_SERVICE_PRINCIPAL:
		domain_attrs = _domain_attrs_spn;
		result_attrs = _result_attrs_spn;
		break;
	case DRSUAPI_DS_NAME_FORMAT_SID_OR_SID_HISTORY:
		domain_attrs = _domain_attrs_sid;
		result_attrs = _result_attrs_sid;
		break;
	default:
		domain_attrs = _domain_attrs_none;
		result_attrs = _result_attrs_none;
		break;
	}

	if (domain_filter) {
		/* if we have a domain_filter look it up and set the result_basedn and the dns_domain_name */
		ldb_ret = ldb_search(sam_ctx, mem_ctx, &domain_res,
					     partitions_basedn,
					     LDB_SCOPE_ONELEVEL,
					     domain_attrs,
					     "%s", domain_filter);

		if (ldb_ret != LDB_SUCCESS) {
			DEBUG(2, ("DsCrackNameOneFilter domain ref search failed: %s\n", ldb_errstring(sam_ctx)));
			info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
			return WERR_OK;
		}

		switch (domain_res->count) {
		case 1:
			break;
		case 0:
			info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
			return WERR_OK;
		default:
			info1->status = DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE;
			return WERR_OK;
		}

		info1->dns_domain_name	= ldb_msg_find_attr_as_string(domain_res->msgs[0], "dnsRoot", NULL);
		W_ERROR_HAVE_NO_MEMORY(info1->dns_domain_name);
		info1->status		= DRSUAPI_DS_NAME_STATUS_DOMAIN_ONLY;
	} else {
		info1->dns_domain_name	= NULL;
		info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
	}

	if (result_filter) {
		int ret;
		struct ldb_result *res;
		uint32_t dsdb_flags = 0;
		struct ldb_dn *real_search_dn = NULL;
		info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;

		/*
		 * From 4.1.4.2.11 of MS-DRSR
		 * if DS_NAME_FLAG_GCVERIFY in flags then
		 * rt := select all O from all
		 * where attrValue in GetAttrVals(O, att, false)
		 * else
		 * rt := select all O from subtree DefaultNC()
		 * where attrValue in GetAttrVals(O, att, false)
		 * endif
		 * return rt
		 */
		if (format_flags & DRSUAPI_DS_NAME_FLAG_GCVERIFY ||
		    format_offered == DRSUAPI_DS_NAME_FORMAT_GUID)
		{
			dsdb_flags = DSDB_SEARCH_SEARCH_ALL_PARTITIONS;
		} else if (domain_res) {
			if (!search_dn) {
				struct ldb_dn *tmp_dn = samdb_result_dn(sam_ctx, mem_ctx, domain_res->msgs[0], "ncName", NULL);
				real_search_dn = tmp_dn;
			} else {
				real_search_dn = search_dn;
			}
		} else {
			real_search_dn = ldb_get_default_basedn(sam_ctx);
		}
		if (format_offered == DRSUAPI_DS_NAME_FORMAT_GUID){
			 dsdb_flags |= DSDB_SEARCH_SHOW_RECYCLED;
		}
		/* search with the 'phantom root' flag */
		ret = dsdb_search(sam_ctx, mem_ctx, &res,
				  real_search_dn,
				  scope,
				  result_attrs,
				  dsdb_flags,
				  "%s", result_filter);
		if (ret != LDB_SUCCESS) {
			DEBUG(2, ("DsCrackNameOneFilter search from '%s' with flags 0x%08x failed: %s\n",
				  ldb_dn_get_linearized(real_search_dn),
				  dsdb_flags,
				  ldb_errstring(sam_ctx)));
			info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
			return WERR_OK;
		}

		ldb_ret = res->count;
		result_res = res->msgs;
	} else if (format_offered == DRSUAPI_DS_NAME_FORMAT_FQDN_1779) {
		ldb_ret = gendb_search_dn(sam_ctx, mem_ctx, name_dn, &result_res,
					  result_attrs);
	} else if (domain_res) {
		name_dn = samdb_result_dn(sam_ctx, mem_ctx, domain_res->msgs[0], "ncName", NULL);
		ldb_ret = gendb_search_dn(sam_ctx, mem_ctx, name_dn, &result_res,
					  result_attrs);
	} else {
		/* Can't happen */
		DEBUG(0, ("LOGIC ERROR: DsCrackNameOneFilter domain ref search not available: This can't happen...\n"));
		info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
		return WERR_OK;
	}

	switch (ldb_ret) {
	case 1:
		result = result_res[0];
		break;
	case 0:
		switch (format_offered) {
		case DRSUAPI_DS_NAME_FORMAT_SERVICE_PRINCIPAL: 
			return DsCrackNameSPNAlias(sam_ctx, mem_ctx, 
						   smb_krb5_context, 
						   format_flags, format_offered, format_desired,
						   name, info1);

		case DRSUAPI_DS_NAME_FORMAT_USER_PRINCIPAL:
			return DsCrackNameUPN(sam_ctx, mem_ctx, smb_krb5_context, 
					      format_flags, format_offered, format_desired,
					      name, info1);
		default:
			break;
		}
		info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
		return WERR_OK;
	case -1:
		DEBUG(2, ("DsCrackNameOneFilter result search failed: %s\n", ldb_errstring(sam_ctx)));
		info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
		return WERR_OK;
	default:
		switch (format_offered) {
		case DRSUAPI_DS_NAME_FORMAT_CANONICAL:
		case DRSUAPI_DS_NAME_FORMAT_CANONICAL_EX:
		{
			const char *canonical_name = NULL; /* Not required, but we get warnings... */
			/* We may need to manually filter further */
			for (i = 0; i < ldb_ret; i++) {
				switch (format_offered) {
				case DRSUAPI_DS_NAME_FORMAT_CANONICAL:
					canonical_name = ldb_dn_canonical_string(mem_ctx, result_res[i]->dn);
					break;
				case DRSUAPI_DS_NAME_FORMAT_CANONICAL_EX:
					canonical_name = ldb_dn_canonical_ex_string(mem_ctx, result_res[i]->dn);
					break;
				default:
					break;
				}
				if (strcasecmp_m(canonical_name, name) == 0) {
					result = result_res[i];
					break;
				}
			}
			if (!result) {
				info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
				return WERR_OK;
			}
		}
		FALL_THROUGH;
		default:
			info1->status = DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE;
			return WERR_OK;
		}
	}

	info1->dns_domain_name = ldb_dn_canonical_string(mem_ctx, result->dn);
	W_ERROR_HAVE_NO_MEMORY(info1->dns_domain_name);
	p = strchr(info1->dns_domain_name, '/');
	if (p) {
		p[0] = '\0';
	}

	/* here we can use result and domain_res[0] */
	switch (format_desired) {
	case DRSUAPI_DS_NAME_FORMAT_FQDN_1779: {
		info1->result_name	= ldb_dn_alloc_linearized(mem_ctx, result->dn);
		W_ERROR_HAVE_NO_MEMORY(info1->result_name);

		info1->status		= DRSUAPI_DS_NAME_STATUS_OK;
		return WERR_OK;
	}
	case DRSUAPI_DS_NAME_FORMAT_CANONICAL: {
		info1->result_name	= ldb_msg_find_attr_as_string(result, "canonicalName", NULL);
		info1->status		= DRSUAPI_DS_NAME_STATUS_OK;
		return WERR_OK;
	}
	case DRSUAPI_DS_NAME_FORMAT_CANONICAL_EX: {
		/* Not in the virtual ldb attribute */
		return DsCrackNameOneSyntactical(mem_ctx, 
						 DRSUAPI_DS_NAME_FORMAT_FQDN_1779, 
						 DRSUAPI_DS_NAME_FORMAT_CANONICAL_EX,
						 result->dn, name, info1);
	}
	case DRSUAPI_DS_NAME_FORMAT_NT4_ACCOUNT: {

		const struct dom_sid *sid = samdb_result_dom_sid(mem_ctx, result, "objectSid");
		const char *_acc = "", *_dom = "";
		if (sid == NULL) {
			info1->status = DRSUAPI_DS_NAME_STATUS_NO_MAPPING;
			return WERR_OK;
		}

		if (samdb_find_attribute(sam_ctx, result, "objectClass",
					 "domain")) {
			/* This can also find a DomainDNSZones entry,
			 * but it won't have the SID we just
			 * checked.  */
			ldb_ret = ldb_search(sam_ctx, mem_ctx, &domain_res,
						     partitions_basedn,
						     LDB_SCOPE_ONELEVEL,
						     domain_attrs,
						     "(ncName=%s)", ldb_dn_get_linearized(result->dn));

			if (ldb_ret != LDB_SUCCESS) {
				DEBUG(2, ("DsCrackNameOneFilter domain ref search failed: %s\n", ldb_errstring(sam_ctx)));
				info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
				return WERR_OK;
			}

			switch (domain_res->count) {
			case 1:
				break;
			case 0:
				info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
				return WERR_OK;
			default:
				info1->status = DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE;
				return WERR_OK;
			}
			_dom = ldb_msg_find_attr_as_string(domain_res->msgs[0], "nETBIOSName", NULL);
			W_ERROR_HAVE_NO_MEMORY(_dom);
		} else {
			_acc = ldb_msg_find_attr_as_string(result, "sAMAccountName", NULL);
			if (!_acc) {
				info1->status = DRSUAPI_DS_NAME_STATUS_NO_MAPPING;
				return WERR_OK;
			}
			if (dom_sid_in_domain(dom_sid_parse_talloc(mem_ctx, SID_BUILTIN), sid)) {
				_dom = "BUILTIN";
			} else {
				const char *attrs[] = { NULL };
				struct ldb_result *domain_res2;
				struct dom_sid *dom_sid = dom_sid_dup(mem_ctx, sid);
				if (!dom_sid) {
					return WERR_OK;
				}
				dom_sid->num_auths--;
				ldb_ret = ldb_search(sam_ctx, mem_ctx, &domain_res,
							     NULL,
							     LDB_SCOPE_BASE,
							     attrs,
							     "(&(objectSid=%s)(objectClass=domain))", 
							     ldap_encode_ndr_dom_sid(mem_ctx, dom_sid));

				if (ldb_ret != LDB_SUCCESS) {
					DEBUG(2, ("DsCrackNameOneFilter domain search failed: %s\n", ldb_errstring(sam_ctx)));
					info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
					return WERR_OK;
				}

				switch (domain_res->count) {
				case 1:
					break;
				case 0:
					info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
					return WERR_OK;
				default:
					info1->status = DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE;
					return WERR_OK;
				}

				ldb_ret = ldb_search(sam_ctx, mem_ctx, &domain_res2,
							     partitions_basedn,
							     LDB_SCOPE_ONELEVEL,
							     domain_attrs,
							     "(ncName=%s)", ldb_dn_get_linearized(domain_res->msgs[0]->dn));

				if (ldb_ret != LDB_SUCCESS) {
					DEBUG(2, ("DsCrackNameOneFilter domain ref search failed: %s\n", ldb_errstring(sam_ctx)));
					info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
					return WERR_OK;
				}

				switch (domain_res2->count) {
				case 1:
					break;
				case 0:
					info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
					return WERR_OK;
				default:
					info1->status = DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE;
					return WERR_OK;
				}
				_dom = ldb_msg_find_attr_as_string(domain_res2->msgs[0], "nETBIOSName", NULL);
				W_ERROR_HAVE_NO_MEMORY(_dom);
			}
		}

		info1->result_name	= talloc_asprintf(mem_ctx, "%s\\%s", _dom, _acc);
		W_ERROR_HAVE_NO_MEMORY(info1->result_name);

		info1->status		= DRSUAPI_DS_NAME_STATUS_OK;
		return WERR_OK;
	}
	case DRSUAPI_DS_NAME_FORMAT_GUID: {
		struct GUID guid;

		guid = samdb_result_guid(result, "objectGUID");

		info1->result_name	= GUID_string2(mem_ctx, &guid);
		W_ERROR_HAVE_NO_MEMORY(info1->result_name);

		info1->status		= DRSUAPI_DS_NAME_STATUS_OK;
		return WERR_OK;
	}
	case DRSUAPI_DS_NAME_FORMAT_DISPLAY: {
		info1->result_name	= ldb_msg_find_attr_as_string(result, "displayName", NULL);
		if (!info1->result_name) {
			info1->result_name	= ldb_msg_find_attr_as_string(result, "sAMAccountName", NULL);
		} 
		if (!info1->result_name) {
			info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
		} else {
			info1->status = DRSUAPI_DS_NAME_STATUS_OK;
		}
		return WERR_OK;
	}
	case DRSUAPI_DS_NAME_FORMAT_SERVICE_PRINCIPAL: {
		struct ldb_message_element *el
			= ldb_msg_find_element(result,
					       "servicePrincipalName");
		if (el == NULL) {
			info1->status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
			return WERR_OK;
		} else if (el->num_values > 1) {
			info1->status = DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE;
			return WERR_OK;
		}

		info1->result_name = ldb_msg_find_attr_as_string(result, "servicePrincipalName", NULL);
		if (!info1->result_name) {
			info1->status = DRSUAPI_DS_NAME_STATUS_NO_MAPPING;
		} else {
			info1->status = DRSUAPI_DS_NAME_STATUS_OK;
		}
		return WERR_OK;
	}
	case DRSUAPI_DS_NAME_FORMAT_DNS_DOMAIN:	{
		info1->dns_domain_name = NULL;
		info1->status = DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR;
		return WERR_OK;
	}
	case DRSUAPI_DS_NAME_FORMAT_SID_OR_SID_HISTORY: {
		const struct dom_sid *sid = samdb_result_dom_sid(mem_ctx, result, "objectSid");

		if (sid == NULL) {
			info1->status = DRSUAPI_DS_NAME_STATUS_NO_MAPPING;
			return WERR_OK;
		}

		info1->result_name = dom_sid_string(mem_ctx, sid);
		W_ERROR_HAVE_NO_MEMORY(info1->result_name);

		info1->status = DRSUAPI_DS_NAME_STATUS_OK;
		return WERR_OK;
	}
	case DRSUAPI_DS_NAME_FORMAT_USER_PRINCIPAL: {
		info1->result_name = ldb_msg_find_attr_as_string(result, "userPrincipalName", NULL);
		if (!info1->result_name) {
			info1->status = DRSUAPI_DS_NAME_STATUS_NO_MAPPING;
		} else {
			info1->status = DRSUAPI_DS_NAME_STATUS_OK;
		}
		return WERR_OK;
	}
	default:
		info1->status = DRSUAPI_DS_NAME_STATUS_NO_MAPPING;
		return WERR_OK;
	}
}

/* Given a user Principal Name (such as foo@bar.com),
 * return the user and domain DNs.  This is used in the KDC to then
 * return the Keys and evaluate policy */

NTSTATUS crack_user_principal_name(struct ldb_context *sam_ctx, 
				   TALLOC_CTX *mem_ctx, 
				   const char *user_principal_name, 
				   struct ldb_dn **user_dn,
				   struct ldb_dn **domain_dn) 
{
	WERROR werr;
	struct drsuapi_DsNameInfo1 info1;
	werr = DsCrackNameOneName(sam_ctx, mem_ctx, 0,
				  DRSUAPI_DS_NAME_FORMAT_USER_PRINCIPAL,
				  DRSUAPI_DS_NAME_FORMAT_FQDN_1779, 
				  user_principal_name,
				  &info1);
	if (!W_ERROR_IS_OK(werr)) {
		return werror_to_ntstatus(werr);
	}
	switch (info1.status) {
	case DRSUAPI_DS_NAME_STATUS_OK:
		break;
	case DRSUAPI_DS_NAME_STATUS_NOT_FOUND:
	case DRSUAPI_DS_NAME_STATUS_DOMAIN_ONLY:
	case DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE:
		return NT_STATUS_NO_SUCH_USER;
	case DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR:
	default:
		return NT_STATUS_UNSUCCESSFUL;
	}

	*user_dn = ldb_dn_new(mem_ctx, sam_ctx, info1.result_name);

	if (domain_dn) {
		werr = DsCrackNameOneName(sam_ctx, mem_ctx, 0,
					  DRSUAPI_DS_NAME_FORMAT_CANONICAL,
					  DRSUAPI_DS_NAME_FORMAT_FQDN_1779, 
					  talloc_asprintf(mem_ctx, "%s/", 
							  info1.dns_domain_name),
					  &info1);
		if (!W_ERROR_IS_OK(werr)) {
			return werror_to_ntstatus(werr);
		}
		switch (info1.status) {
		case DRSUAPI_DS_NAME_STATUS_OK:
			break;
		case DRSUAPI_DS_NAME_STATUS_NOT_FOUND:
		case DRSUAPI_DS_NAME_STATUS_DOMAIN_ONLY:
		case DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE:
			return NT_STATUS_NO_SUCH_USER;
		case DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR:
		default:
			return NT_STATUS_UNSUCCESSFUL;
		}

		*domain_dn = ldb_dn_new(mem_ctx, sam_ctx, info1.result_name);
	}

	return NT_STATUS_OK;
}

/* Given a Service Principal Name (such as host/foo.bar.com@BAR.COM),
 * return the user and domain DNs.  This is used in the KDC to then
 * return the Keys and evaluate policy */

NTSTATUS crack_service_principal_name(struct ldb_context *sam_ctx, 
				      TALLOC_CTX *mem_ctx, 
				      const char *service_principal_name, 
				      struct ldb_dn **user_dn,
				      struct ldb_dn **domain_dn) 
{
	WERROR werr;
	struct drsuapi_DsNameInfo1 info1;
	werr = DsCrackNameOneName(sam_ctx, mem_ctx, 0,
				  DRSUAPI_DS_NAME_FORMAT_SERVICE_PRINCIPAL,
				  DRSUAPI_DS_NAME_FORMAT_FQDN_1779, 
				  service_principal_name,
				  &info1);
	if (!W_ERROR_IS_OK(werr)) {
		return werror_to_ntstatus(werr);
	}
	switch (info1.status) {
	case DRSUAPI_DS_NAME_STATUS_OK:
		break;
	case DRSUAPI_DS_NAME_STATUS_NOT_FOUND:
	case DRSUAPI_DS_NAME_STATUS_DOMAIN_ONLY:
	case DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE:
		return NT_STATUS_NO_SUCH_USER;
	case DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR:
	default:
		return NT_STATUS_UNSUCCESSFUL;
	}

	*user_dn = ldb_dn_new(mem_ctx, sam_ctx, info1.result_name);

	if (domain_dn) {
		werr = DsCrackNameOneName(sam_ctx, mem_ctx, 0,
					  DRSUAPI_DS_NAME_FORMAT_CANONICAL,
					  DRSUAPI_DS_NAME_FORMAT_FQDN_1779, 
					  talloc_asprintf(mem_ctx, "%s/", 
							  info1.dns_domain_name),
					  &info1);
		if (!W_ERROR_IS_OK(werr)) {
			return werror_to_ntstatus(werr);
		}
		switch (info1.status) {
		case DRSUAPI_DS_NAME_STATUS_OK:
			break;
		case DRSUAPI_DS_NAME_STATUS_NOT_FOUND:
		case DRSUAPI_DS_NAME_STATUS_DOMAIN_ONLY:
		case DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE:
			return NT_STATUS_NO_SUCH_USER;
		case DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR:
		default:
			return NT_STATUS_UNSUCCESSFUL;
		}

		*domain_dn = ldb_dn_new(mem_ctx, sam_ctx, info1.result_name);
	}

	return NT_STATUS_OK;
}

NTSTATUS crack_name_to_nt4_name(TALLOC_CTX *mem_ctx, 
				struct ldb_context *ldb,
				enum drsuapi_DsNameFormat format_offered,
				const char *name, 
				const char **nt4_domain, const char **nt4_account)
{
	WERROR werr;
	struct drsuapi_DsNameInfo1 info1;
	char *p;

	/* Handle anonymous bind */
	if (!name || !*name) {
		*nt4_domain = "";
		*nt4_account = "";
		return NT_STATUS_OK;
	}

	werr = DsCrackNameOneName(ldb, mem_ctx, 0,
				  format_offered, 
				  DRSUAPI_DS_NAME_FORMAT_NT4_ACCOUNT,
				  name,
				  &info1);
	if (!W_ERROR_IS_OK(werr)) {
		return werror_to_ntstatus(werr);
	}
	switch (info1.status) {
	case DRSUAPI_DS_NAME_STATUS_OK:
		break;
	case DRSUAPI_DS_NAME_STATUS_NOT_FOUND:
	case DRSUAPI_DS_NAME_STATUS_DOMAIN_ONLY:
	case DRSUAPI_DS_NAME_STATUS_NOT_UNIQUE:
		return NT_STATUS_NO_SUCH_USER;
	case DRSUAPI_DS_NAME_STATUS_RESOLVE_ERROR:
	default:
		return NT_STATUS_UNSUCCESSFUL;
	}

	*nt4_domain = talloc_strdup(mem_ctx, info1.result_name);
	if (*nt4_domain == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	p = strchr(*nt4_domain, '\\');
	if (!p) {
		return NT_STATUS_INVALID_PARAMETER;
	}
	p[0] = '\0';

	*nt4_account = talloc_strdup(mem_ctx, &p[1]);
	if (*nt4_account == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	return NT_STATUS_OK;
}

NTSTATUS crack_auto_name_to_nt4_name(TALLOC_CTX *mem_ctx,
				     struct ldb_context *ldb,
				     const char *name,
				     const char **nt4_domain,
				     const char **nt4_account)
{
	enum drsuapi_DsNameFormat format_offered = DRSUAPI_DS_NAME_FORMAT_UNKNOWN;

	/* Handle anonymous bind */
	if (!name || !*name) {
		*nt4_domain = "";
		*nt4_account = "";
		return NT_STATUS_OK;
	}

	/*
	 * Here we only consider a subset of the possible name forms listed in
	 * [MS-ADTS] 5.1.1.1.1, and we don't retry with a different name form if
	 * the first attempt fails.
	 */

	if (strchr_m(name, '=')) {
		format_offered = DRSUAPI_DS_NAME_FORMAT_FQDN_1779;
	} else if (strchr_m(name, '@')) {
		format_offered = DRSUAPI_DS_NAME_FORMAT_USER_PRINCIPAL;
	} else if (strchr_m(name, '\\')) {
		format_offered = DRSUAPI_DS_NAME_FORMAT_NT4_ACCOUNT;
	} else if (strchr_m(name, '\n')) {
		format_offered = DRSUAPI_DS_NAME_FORMAT_CANONICAL_EX;
	} else if (strchr_m(name, '/')) {
		format_offered = DRSUAPI_DS_NAME_FORMAT_CANONICAL;
	} else if ((name[0] == 'S' || name[0] == 's') && name[1] == '-') {
		format_offered = DRSUAPI_DS_NAME_FORMAT_SID_OR_SID_HISTORY;
	} else {
		return NT_STATUS_NO_SUCH_USER;
	}

	return crack_name_to_nt4_name(mem_ctx, ldb, format_offered, name, nt4_domain, nt4_account);
}


WERROR dcesrv_drsuapi_ListRoles(struct ldb_context *sam_ctx, TALLOC_CTX *mem_ctx,
				const struct drsuapi_DsNameRequest1 *req1,
				struct drsuapi_DsNameCtr1 **ctr1)
{
	struct drsuapi_DsNameInfo1 *names;
	uint32_t i;
	uint32_t count = 5;/*number of fsmo role owners we are going to return*/

	*ctr1 = talloc(mem_ctx, struct drsuapi_DsNameCtr1);
	W_ERROR_HAVE_NO_MEMORY(*ctr1);
	names = talloc_array(mem_ctx, struct drsuapi_DsNameInfo1, count);
	W_ERROR_HAVE_NO_MEMORY(names);

	for (i = 0; i < count; i++) {
		WERROR werr;
		struct ldb_dn *role_owner_dn, *fsmo_role_dn, *server_dn;
		werr = dsdb_get_fsmo_role_info(mem_ctx, sam_ctx, i,
					       &fsmo_role_dn, &role_owner_dn);
		if(!W_ERROR_IS_OK(werr)) {
			return werr;
		}
		server_dn = ldb_dn_copy(mem_ctx, role_owner_dn);
		ldb_dn_remove_child_components(server_dn, 1);
		names[i].status = DRSUAPI_DS_NAME_STATUS_OK;
		names[i].dns_domain_name = samdb_dn_to_dnshostname(sam_ctx, mem_ctx,
								   server_dn);
		if(!names[i].dns_domain_name) {
			DEBUG(4, ("list_roles: Failed to find dNSHostName for server %s\n",
				  ldb_dn_get_linearized(server_dn)));
		}
		names[i].result_name = talloc_strdup(mem_ctx, ldb_dn_get_linearized(role_owner_dn));
	}

	(*ctr1)->count = count;
	(*ctr1)->array = names;

	return WERR_OK;
}

WERROR dcesrv_drsuapi_CrackNamesByNameFormat(struct ldb_context *sam_ctx, TALLOC_CTX *mem_ctx,
					     const struct drsuapi_DsNameRequest1 *req1,
					     struct drsuapi_DsNameCtr1 **ctr1)
{
	struct drsuapi_DsNameInfo1 *names;
	uint32_t i, count;
	WERROR status;

	*ctr1 = talloc_zero(mem_ctx, struct drsuapi_DsNameCtr1);
	W_ERROR_HAVE_NO_MEMORY(*ctr1);

	count = req1->count;
	names = talloc_array(mem_ctx, struct drsuapi_DsNameInfo1, count);
	W_ERROR_HAVE_NO_MEMORY(names);

	for (i=0; i < count; i++) {
		status = DsCrackNameOneName(sam_ctx, mem_ctx,
					    req1->format_flags,
					    req1->format_offered,
					    req1->format_desired,
					    req1->names[i].str,
					    &names[i]);
		if (!W_ERROR_IS_OK(status)) {
			return status;
		}
	}

	(*ctr1)->count = count;
	(*ctr1)->array = names;

	return WERR_OK;
}

WERROR dcesrv_drsuapi_ListInfoServer(struct ldb_context *sam_ctx, TALLOC_CTX *mem_ctx,
				     const struct drsuapi_DsNameRequest1 *req1,
				     struct drsuapi_DsNameCtr1 **_ctr1)
{
	struct drsuapi_DsNameInfo1 *names;
	struct ldb_result *res;
	struct ldb_dn *server_dn, *dn;
	struct drsuapi_DsNameCtr1 *ctr1;
	int ret, i;
	const char *str;
	const char *attrs[] = {
		"dNSHostName",
		"serverReference",
		NULL
	};

	*_ctr1 = NULL;

	ctr1 = talloc_zero(mem_ctx, struct drsuapi_DsNameCtr1);
	W_ERROR_HAVE_NO_MEMORY(ctr1);

	/*
	 * No magic value here, we have to return 3 entries according to the
	 * MS-DRSR.pdf
	 */
	ctr1->count = 3;
	names = talloc_zero_array(ctr1, struct drsuapi_DsNameInfo1,
				  ctr1->count);
	W_ERROR_HAVE_NO_MEMORY(names);
	ctr1->array = names;

	for (i=0; i < ctr1->count; i++) {
		names[i].status = DRSUAPI_DS_NAME_STATUS_NOT_FOUND;
	}
	*_ctr1 = ctr1;

	if (req1->count != 1) {
		DEBUG(1, ("Expected a count of 1 for the ListInfoServer crackname \n"));
		return WERR_OK;
	}

	if (req1->names[0].str == NULL) {
		return WERR_OK;
	}

	server_dn = ldb_dn_new(mem_ctx, sam_ctx, req1->names[0].str);
	W_ERROR_HAVE_NO_MEMORY(server_dn);

	ret = ldb_search(sam_ctx, mem_ctx, &res, server_dn, LDB_SCOPE_ONELEVEL,
			 NULL, "(objectClass=nTDSDSA)");

	if (ret != LDB_SUCCESS) {
		DEBUG(1, ("Search for objectClass=nTDSDSA "
			  "returned less than 1 objects\n"));
		return WERR_OK;
	}

	if (res->count != 1) {
		DEBUG(1, ("Search for objectClass=nTDSDSA "
			  "returned less than 1 objects\n"));
		return WERR_OK;
	}

	if (res->msgs[0]->dn) {
		names[0].result_name = ldb_dn_alloc_linearized(names, res->msgs[0]->dn);
		W_ERROR_HAVE_NO_MEMORY(names[0].result_name);
		names[0].status = DRSUAPI_DS_NAME_STATUS_OK;
	}

	talloc_free(res);

	ret = ldb_search(sam_ctx, mem_ctx, &res, server_dn, LDB_SCOPE_BASE,
			 attrs, "(objectClass=*)");
	if (ret != LDB_SUCCESS) {
		DEBUG(1, ("Search for objectClass=* on dn %s"
			  "returned %s\n", req1->names[0].str,
			  ldb_strerror(ret)));
		return WERR_OK;
	}

	if (res->count != 1) {
		DEBUG(1, ("Search for objectClass=* on dn %s"
			  "returned less than 1 objects\n", req1->names[0].str));
		return WERR_OK;
	}

	str = ldb_msg_find_attr_as_string(res->msgs[0], "dNSHostName", NULL);
	if (str != NULL) {
		names[1].result_name = talloc_strdup(names, str);
		W_ERROR_HAVE_NO_MEMORY(names[1].result_name);
		names[1].status = DRSUAPI_DS_NAME_STATUS_OK;
	}

	dn = ldb_msg_find_attr_as_dn(sam_ctx, mem_ctx, res->msgs[0], "serverReference");
	if (dn != NULL) {
		names[2].result_name = ldb_dn_alloc_linearized(names, dn);
		W_ERROR_HAVE_NO_MEMORY(names[2].result_name);
		names[2].status = DRSUAPI_DS_NAME_STATUS_OK;
	}

	talloc_free(dn);
	talloc_free(res);

	return WERR_OK;
}
