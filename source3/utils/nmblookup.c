/*
   Unix SMB/CIFS implementation.
   NBT client - used to lookup netbios names
   Copyright (C) Andrew Tridgell 1994-1998
   Copyright (C) Jelmer Vernooij 2003 (Conversion to popt)

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
#include "lib/cmdline/cmdline.h"
#include "libsmb/nmblib.h"
#include "libsmb/namequery.h"
#include "lib/util/string_wrappers.h"

static bool give_flags = false;
static bool use_bcast = true;
static bool got_bcast = false;
static struct sockaddr_storage bcast_addr;
static bool recursion_desired = false;
static bool translate_addresses = false;
static int ServerFD= -1;
static bool RootPort = false;
static bool find_status = false;

/****************************************************************************
 Open the socket communication.
**************************************************************************/

static bool open_sockets(void)
{
	struct sockaddr_storage ss;
	const char *sock_addr = lp_nbt_client_socket_address();

	if (!interpret_string_addr(&ss, sock_addr,
				AI_NUMERICHOST|AI_PASSIVE)) {
		DEBUG(0,("open_sockets: unable to get socket address "
					"from string %s\n", sock_addr));
		return false;
	}
	ServerFD = open_socket_in(
		SOCK_DGRAM, &ss, (RootPort ? 137 : 0), true);
	if (ServerFD < 0) {
		if (RootPort) {
			DBG_ERR("open_socket_in failed: %s\n",
				strerror(-ServerFD));
		} else {
			DBG_NOTICE("open_socket_in failed: %s\n",
				   strerror(-ServerFD));
		}
		return false;
	}

	set_socket_options( ServerFD, "SO_BROADCAST" );

	DEBUG(3, ("Socket opened.\n"));
	return true;
}

/****************************************************************************
turn a node status flags field into a string
****************************************************************************/
static char *node_status_flags(unsigned char flags)
{
	static fstring ret;
	fstrcpy(ret,"");

	fstrcat(ret, (flags & 0x80) ? "<GROUP> " : "        ");
	if ((flags & 0x60) == 0x00) fstrcat(ret,"B ");
	if ((flags & 0x60) == 0x20) fstrcat(ret,"P ");
	if ((flags & 0x60) == 0x40) fstrcat(ret,"M ");
	if ((flags & 0x60) == 0x60) fstrcat(ret,"H ");
	if (flags & 0x10) fstrcat(ret,"<DEREGISTERING> ");
	if (flags & 0x08) fstrcat(ret,"<CONFLICT> ");
	if (flags & 0x04) fstrcat(ret,"<ACTIVE> ");
	if (flags & 0x02) fstrcat(ret,"<PERMANENT> ");

	return ret;
}

/****************************************************************************
 Turn the NMB Query flags into a string.
****************************************************************************/

static char *query_flags(int flags)
{
	static fstring ret1;
	fstrcpy(ret1, "");

	if (flags & NM_FLAGS_RS) fstrcat(ret1, "Response ");
	if (flags & NM_FLAGS_AA) fstrcat(ret1, "Authoritative ");
	if (flags & NM_FLAGS_TC) fstrcat(ret1, "Truncated ");
	if (flags & NM_FLAGS_RD) fstrcat(ret1, "Recursion_Desired ");
	if (flags & NM_FLAGS_RA) fstrcat(ret1, "Recursion_Available ");
	if (flags & NM_FLAGS_B)  fstrcat(ret1, "Broadcast ");

	return ret1;
}

/****************************************************************************
 Do a node status query.
****************************************************************************/

static bool do_node_status(const char *name,
		int type,
		struct sockaddr_storage *pss)
{
	struct nmb_name nname;
	size_t count = 0;
	size_t i, j;
	struct node_status *addrs;
	struct node_status_extra extra;
	fstring cleanname;
	char addr[INET6_ADDRSTRLEN];
	NTSTATUS status;

	print_sockaddr(addr, sizeof(addr), pss);
	d_printf("Looking up status of %s\n",addr);
	make_nmb_name(&nname, name, type);
	status = node_status_query(talloc_tos(), &nname, pss,
				   &addrs, &count, &extra);
	if (NT_STATUS_IS_OK(status)) {
		for (i=0;i<count;i++) {
			pull_ascii_fstring(cleanname, addrs[i].name);
			for (j=0;cleanname[j];j++) {
				if (!isprint((int)cleanname[j])) {
					cleanname[j] = '.';
				}
			}
			d_printf("\t%-15s <%02x> - %s\n",
			       cleanname,addrs[i].type,
			       node_status_flags(addrs[i].flags));
		}
		d_printf("\n\tMAC Address = %02X-%02X-%02X-%02X-%02X-%02X\n",
				extra.mac_addr[0], extra.mac_addr[1],
				extra.mac_addr[2], extra.mac_addr[3],
				extra.mac_addr[4], extra.mac_addr[5]);
		d_printf("\n");
		TALLOC_FREE(addrs);
		return true;
	} else {
		d_printf("No reply from %s\n\n",addr);
		return false;
	}
}


/****************************************************************************
 Send out one query.
****************************************************************************/

static bool query_one(const char *lookup, unsigned int lookup_type)
{
	size_t j, count = 0;
	uint8_t flags;
	struct sockaddr_storage *ip_list=NULL;
	NTSTATUS status = NT_STATUS_NOT_FOUND;

	if (got_bcast) {
		char addr[INET6_ADDRSTRLEN];
		print_sockaddr(addr, sizeof(addr), &bcast_addr);
		d_printf("querying %s on %s\n", lookup, addr);
		status = name_query(lookup,lookup_type,use_bcast,
				    use_bcast?true:recursion_desired,
				    &bcast_addr, talloc_tos(),
				    &ip_list, &count, &flags);
	} else {
		status = name_resolve_bcast(talloc_tos(),
					    lookup,
					    lookup_type,
					    &ip_list,
					    &count);
	}

	if (!NT_STATUS_IS_OK(status)) {
		return false;
	}

	if (give_flags) {
		d_printf("Flags: %s\n", query_flags(flags));
	}

	for (j=0;j<count;j++) {
		char addr[INET6_ADDRSTRLEN];
		if (translate_addresses) {
			char h_name[MAX_DNS_NAME_LENGTH];
			h_name[0] = '\0';
			if (sys_getnameinfo((const struct sockaddr *)&ip_list[j],
					sizeof(struct sockaddr_storage),
					h_name, sizeof(h_name),
					NULL, 0,
					NI_NAMEREQD)) {
				continue;
			}
			d_printf("%s, ", h_name);
		}
		print_sockaddr(addr, sizeof(addr), &ip_list[j]);
		d_printf("%s %s<%02x>\n", addr,lookup, lookup_type);
		/* We can only do find_status if the ip address returned
		   was valid - ie. name_query returned true.
		 */
		if (find_status) {
			if (!do_node_status(lookup, lookup_type, &ip_list[j])) {
				status = NT_STATUS_UNSUCCESSFUL;
			}
		}
	}

	TALLOC_FREE(ip_list);

	return NT_STATUS_IS_OK(status);
}


/****************************************************************************
  main program
****************************************************************************/
enum nmblookup_cmdline_options {
	CMDLINE_RECURSIVE = 1,
};

int main(int argc, const char *argv[])
{
	int opt;
	unsigned int lookup_type = 0x0;
	fstring lookup;
	static bool find_master=False;
	static bool lookup_by_ip = False;
	poptContext pc = NULL;
	TALLOC_CTX *frame = talloc_stackframe();
	int rc = 0;
	bool ok;

	struct poptOption long_options[] = {
		POPT_AUTOHELP
		{
			.longName   = "broadcast",
			.shortName  = 'B',
			.argInfo    = POPT_ARG_STRING,
			.arg        = NULL,
			.val        = 'B',
			.descrip    = "Specify address to use for broadcasts",
			.argDescrip = "BROADCAST-ADDRESS",
		},
		{
			.longName   = "flags",
			.shortName  = 'f',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'f',
			.descrip    = "List the NMB flags returned",
		},
		{
			.longName   = "unicast",
			.shortName  = 'U',
			.argInfo    = POPT_ARG_STRING,
			.arg        = NULL,
			.val        = 'U',
			.descrip    = "Specify address to use for unicast",
		},
		{
			.longName   = "master-browser",
			.shortName  = 'M',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'M',
			.descrip    = "Search for a master browser",
		},
		{
			.longName   = "recursion",
			.shortName  = 0,
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = CMDLINE_RECURSIVE,
			.descrip    = "Set recursion desired in package",
		},
		{
			.longName   = "status",
			.shortName  = 'S',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'S',
			.descrip    = "Lookup node status as well",
		},
		{
			.longName   = "translate",
			.shortName  = 'T',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'T',
			.descrip    = "Translate IP addresses into names",
		},
		{
			.longName   = "root-port",
			.shortName  = 'r',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'r',
			.descrip    = "Use root port 137 (Win95 only replies to this)",
		},
		{
			.longName   = "lookup-by-ip",
			.shortName  = 'A',
			.argInfo    = POPT_ARG_NONE,
			.arg        = NULL,
			.val        = 'A',
			.descrip    = "Do a node status on <name> as an IP Address",
		},
		POPT_COMMON_SAMBA
		POPT_COMMON_CONNECTION
		POPT_COMMON_VERSION
		POPT_TABLEEND
	};

	*lookup = 0;

	smb_init_locale();

	ok = samba_cmdline_init(frame,
				SAMBA_CMDLINE_CONFIG_CLIENT,
				false /* require_smbconf */);
	if (!ok) {
		DBG_ERR("Failed to init cmdline parser!\n");
		TALLOC_FREE(frame);
		exit(1);
	}

	pc = samba_popt_get_context(getprogname(),
				    argc,
				    argv,
				    long_options,
				    POPT_CONTEXT_KEEP_FIRST);
	if (pc == NULL) {
		DBG_ERR("Failed to setup popt context!\n");
		TALLOC_FREE(frame);
		exit(1);
	}

	poptSetOtherOptionHelp(pc, "<NODE> ...");

	while ((opt = poptGetNextOpt(pc)) != -1) {
		switch (opt) {
		case 'f':
			give_flags = true;
			break;
		case 'M':
			find_master = true;
			break;
		case CMDLINE_RECURSIVE:
			recursion_desired = true;
			break;
		case 'S':
			find_status = true;
			break;
		case 'r':
			RootPort = true;
			break;
		case 'A':
			lookup_by_ip = true;
			break;
		case 'B':
			if (interpret_string_addr(&bcast_addr,
					poptGetOptArg(pc),
					NI_NUMERICHOST)) {
				got_bcast = True;
				use_bcast = True;
			}
			break;
		case 'U':
			if (interpret_string_addr(&bcast_addr,
					poptGetOptArg(pc),
					0)) {
				got_bcast = True;
				use_bcast = False;
			}
			break;
		case 'T':
			translate_addresses = !translate_addresses;
			break;
		case POPT_ERROR_BADOPT:
			fprintf(stderr, "\nInvalid option %s: %s\n\n",
				poptBadOption(pc, 0), poptStrerror(opt));
			poptPrintUsage(pc, stderr, 0);
			exit(1);
		}
	}

	poptGetArg(pc); /* Remove argv[0] */

	if(!poptPeekArg(pc)) {
		poptPrintUsage(pc, stderr, 0);
		rc = 1;
		goto out;
	}

	if (!open_sockets()) {
		rc = 1;
		goto out;
	}

	while(poptPeekArg(pc)) {
		char *p;
		struct in_addr ip;
		size_t nbt_len;

		fstrcpy(lookup,poptGetArg(pc));

		if(lookup_by_ip) {
			struct sockaddr_storage ss;
			ip = interpret_addr2(lookup);
			in_addr_to_sockaddr_storage(&ss, ip);
			fstrcpy(lookup,"*");
			if (!do_node_status(lookup, lookup_type, &ss)) {
				rc = 1;
			}
			continue;
		}

		if (find_master) {
			if (*lookup == '-') {
				fstrcpy(lookup,"\01\02__MSBROWSE__\02");
				lookup_type = 1;
			} else {
				lookup_type = 0x1d;
			}
		}

		p = strchr_m(lookup,'#');
		if (p) {
			*p = '\0';
			sscanf(++p,"%x",&lookup_type);
		}

		nbt_len = strlen(lookup);
		if (nbt_len > MAX_NETBIOSNAME_LEN - 1) {
			d_printf("The specified netbios name [%s] is too long!\n",
				 lookup);
			continue;
		}


		if (!query_one(lookup, lookup_type)) {
			rc = 1;
			d_printf( "name_query failed to find name %s", lookup );
			if( 0 != lookup_type ) {
				d_printf( "#%02x", lookup_type );
			}
			d_printf( "\n" );
		}
	}

out:
	poptFreeContext(pc);
	TALLOC_FREE(frame);
	return rc;
}
