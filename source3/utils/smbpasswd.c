/*
 * Unix SMB/CIFS implementation. 
 * Copyright (C) Jeremy Allison 1995-1998
 * Copyright (C) Tim Potter     2001
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 * 
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>.  */

#include "includes.h"
#include "system/passwd.h"
#include "secrets.h"
#include "../librpc/gen_ndr/samr.h"
#include "../lib/util/util_pw.h"
#include "libsmb/proto.h"
#include "passdb.h"
#include "cmdline_contexts.h"
#include "passwd_proto.h"
#include "lib/util/string_wrappers.h"
#include "lib/param/param.h"

/*
 * Next two lines needed for SunOS and don't
 * hurt anything else...
 */
extern char *optarg;
extern int optind;

/* forced running in root-mode */
static bool got_username = False;
static bool stdin_passwd_get = False;
static fstring user_name;
static char *new_passwd = NULL;
static const char *remote_machine = NULL;

static fstring ldap_secret;


/*********************************************************
 Print command usage on stderr and die.
**********************************************************/
static void usage(void)
{
	printf("When run by root:\n");
	printf("    smbpasswd [options] [username]\n");
	printf("otherwise:\n");
	printf("    smbpasswd [options]\n\n");

	printf("options:\n");
	printf("  -L                   local mode (must be first option)\n");
	printf("  -h                   print this usage message\n");
	printf("  -s                   use stdin for password prompt\n");
	printf("  -c smb.conf file     Use the given path to the smb.conf file\n");
	printf("  -D LEVEL             debug level\n");
	printf("  -r MACHINE           remote machine\n");
	printf("  -U USER              remote username (e.g. SAM/user)\n");

	printf("extra options when run by root or in local mode:\n");
	printf("  -a                   add user\n");
	printf("  -d                   disable user\n");
	printf("  -e                   enable user\n");
	printf("  -i                   interdomain trust account\n");
	printf("  -m                   machine trust account\n");
	printf("  -n                   set no password\n");
	printf("  -W                   use stdin ldap admin password\n");
	printf("  -w PASSWORD          ldap admin password\n");
	printf("  -x                   delete user\n");
	printf("  -R ORDER             name resolve order\n");

	exit(1);
}

static void set_line_buffering(FILE *f)
{
	setvbuf(f, NULL, _IOLBF, 0);
}

/*******************************************************************
 Process command line options
 ******************************************************************/

static int process_options(int argc, char **argv, int local_flags,
			   struct loadparm_context *lp_ctx)
{
	int ch;
	const char *configfile = get_dyn_CONFIGFILE();

	local_flags |= LOCAL_SET_PASSWORD;

	ZERO_STRUCT(user_name);

	user_name[0] = '\0';

	while ((ch = getopt(argc, argv, "c:axdehminjr:sw:R:D:U:LWS:")) != EOF) {
		switch(ch) {
		case 'L':
			if (getuid() != 0) {
				fprintf(stderr, "smbpasswd -L can only be used by root.\n");
				exit(1);
			}
			local_flags |= LOCAL_AM_ROOT;
			break;
		case 'c':
			configfile = optarg;
			set_dyn_CONFIGFILE(optarg);
			break;
		case 'a':
			local_flags |= LOCAL_ADD_USER;
			break;
		case 'x':
			local_flags |= LOCAL_DELETE_USER;
			local_flags &= ~LOCAL_SET_PASSWORD;
			break;
		case 'd':
			local_flags |= LOCAL_DISABLE_USER;
			local_flags &= ~LOCAL_SET_PASSWORD;
			break;
		case 'e':
			local_flags |= LOCAL_ENABLE_USER;
			local_flags &= ~LOCAL_SET_PASSWORD;
			break;
		case 'm':
			local_flags |= LOCAL_TRUST_ACCOUNT;
			break;
		case 'i':
			local_flags |= LOCAL_INTERDOM_ACCOUNT;
			break;
		case 'j':
			d_printf("See 'net join' for this functionality\n");
			exit(1);
			break;
		case 'n':
			local_flags |= LOCAL_SET_NO_PASSWORD;
			local_flags &= ~LOCAL_SET_PASSWORD;
			SAFE_FREE(new_passwd);
			new_passwd = smb_xstrdup("NO PASSWORD");
			break;
		case 'r':
			remote_machine = optarg;
			break;
		case 's':
			set_line_buffering(stdin);
			set_line_buffering(stdout);
			set_line_buffering(stderr);
			stdin_passwd_get = True;
			break;
		case 'w':
			local_flags |= LOCAL_SET_LDAP_ADMIN_PW;
			fstrcpy(ldap_secret, optarg);
			break;
		case 'R':
			lpcfg_set_cmdline(lp_ctx, "name resolve order", optarg);
			break;
		case 'D':
			lpcfg_set_cmdline(lp_ctx, "log level", optarg);
			break;
		case 'U': {
			got_username = True;
			fstrcpy(user_name, optarg);
			break;
		case 'W':
			local_flags |= LOCAL_SET_LDAP_ADMIN_PW;
			*ldap_secret = '\0';
			break;
		}
		case 'h':
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	switch(argc) {
	case 0:
		if (!got_username)
			fstrcpy(user_name, "");
		break;
	case 1:
		if (!(local_flags & LOCAL_AM_ROOT)) {
			usage();
		} else {
			if (got_username) {
				usage();
			} else {
				fstrcpy(user_name, argv[0]);
			}
		}
		break;
	default:
		usage();
	}

	if (!lp_load_global(configfile)) {
		fprintf(stderr, "Can't load %s - run testparm to debug it\n", 
			configfile);
		exit(1);
	}

	return local_flags;
}

/*************************************************************
 Utility function to prompt for new password.
*************************************************************/
static char *prompt_for_new_password(bool stdin_get)
{
	char *p;
	fstring new_pw;

	ZERO_ARRAY(new_pw);

	p = get_pass("New SMB password:", stdin_get);
	if (p == NULL) {
		return NULL;
	}

	fstrcpy(new_pw, p);
	SAFE_FREE(p);

	p = get_pass("Retype new SMB password:", stdin_get);
	if (p == NULL) {
		return NULL;
	}

	if (strcmp(p, new_pw)) {
		fprintf(stderr, "Mismatch - password unchanged.\n");
		ZERO_ARRAY(new_pw);
		SAFE_FREE(p);
		return NULL;
	}

	return p;
}


/*************************************************************
 Change a password either locally or remotely.
*************************************************************/

static NTSTATUS password_change(const char *remote_mach,
				const char *domain, const char *username,
				const char *old_passwd, const char *new_pw,
				int local_flags)
{
	NTSTATUS ret;
	char *err_str = NULL;
	char *msg_str = NULL;

	if (remote_mach != NULL) {
		if (local_flags & (LOCAL_ADD_USER|LOCAL_DELETE_USER|
				   LOCAL_DISABLE_USER|LOCAL_ENABLE_USER|
				   LOCAL_TRUST_ACCOUNT|LOCAL_SET_NO_PASSWORD)) {
			/* these things can't be done remotely yet */
			fprintf(stderr, "Invalid remote operation!\n");
			return NT_STATUS_UNSUCCESSFUL;
		}
		ret = remote_password_change(remote_mach,
					     domain, username,
					     old_passwd, new_pw, &err_str);
	} else {
		ret = local_password_change(username, local_flags, new_pw,
					    &err_str, &msg_str);
	}

	if (msg_str) {
		printf("%s", msg_str);
	}
	if (err_str) {
		fprintf(stderr, "%s", err_str);
	}
	if (!NT_STATUS_IS_OK(ret) && !err_str) {
		fprintf(stderr, "Failed to change password!\n");
	}

	SAFE_FREE(msg_str);
	SAFE_FREE(err_str);
	return ret;
}

/*******************************************************************
 Store the LDAP admin password in secrets.tdb
 ******************************************************************/
static bool store_ldap_admin_pw (char* pw)
{	
	if (!pw) 
		return False;

	if (!secrets_init())
		return False;

	return secrets_store_ldap_pw(lp_ldap_admin_dn(), pw);
}


/*************************************************************
 Handle password changing for root.
*************************************************************/

static int process_root(int local_flags)
{
	struct passwd  *pwd;
	int result = 0;
	char *old_passwd = NULL;

	if (local_flags & LOCAL_SET_LDAP_ADMIN_PW) {
		const char *ldap_admin_dn = lp_ldap_admin_dn();
		if ( ! *ldap_admin_dn ) {
			DEBUG(0,("ERROR: 'ldap admin dn' not defined! Please check your smb.conf\n"));
			goto done;
		}

		printf("Setting stored password for \"%s\" in secrets.tdb\n", ldap_admin_dn);
		if ( ! *ldap_secret ) {
			new_passwd = prompt_for_new_password(stdin_passwd_get);
			if (new_passwd == NULL) {
				fprintf(stderr, "Failed to read new password!\n");
				exit(1);
			}
			fstrcpy(ldap_secret, new_passwd);
		}
		if (!store_ldap_admin_pw(ldap_secret)) {
			DEBUG(0,("ERROR: Failed to store the ldap admin password!\n"));
		}
		goto done;
	}

	/* Ensure passdb startup(). */
	if(!initialize_password_db(False, NULL)) {
		DEBUG(0, ("Failed to open passdb!\n"));
		exit(1);
	}

	/* Ensure we have a SAM sid. */
	get_global_sam_sid();

	/*
	 * Ensure both add/delete user are not set
	 * Ensure add/delete user and either remote machine or join domain are
	 * not both set.
	 */	
	if(((local_flags & (LOCAL_ADD_USER|LOCAL_DELETE_USER)) == (LOCAL_ADD_USER|LOCAL_DELETE_USER)) || 
	   ((local_flags & (LOCAL_ADD_USER|LOCAL_DELETE_USER)) && 
		(remote_machine != NULL))) {
		usage();
	}

	/* Only load interfaces if we are doing network operations. */

	if (remote_machine) {
		load_interfaces();
	}

	if (!user_name[0] && (pwd = getpwuid_alloc(talloc_tos(), geteuid()))) {
		fstrcpy(user_name, pwd->pw_name);
		TALLOC_FREE(pwd);
	} 

	if (!user_name[0]) {
		fprintf(stderr,"You must specify a username\n");
		exit(1);
	}

	if (local_flags & LOCAL_TRUST_ACCOUNT) {
		/* add the $ automatically */
		size_t user_name_len = strlen(user_name);

		if (user_name[user_name_len - 1] == '$') {
			user_name_len--;
		} else {
			if (user_name_len + 2 > sizeof(user_name)) {
				fprintf(stderr, "machine name too long\n");
				exit(1);
			}
			user_name[user_name_len] = '$';
			user_name[user_name_len + 1] = '\0';
		}

		if (local_flags & LOCAL_ADD_USER) {
		        SAFE_FREE(new_passwd);

			/*
			 * Remove any trailing '$' before we
			 * generate the initial machine password.
			 */
			new_passwd = smb_xstrndup(user_name, user_name_len);
			if (!strlower_m(new_passwd)) {
				fprintf(stderr, "strlower_m %s failed\n",
					new_passwd);
				exit(1);
			}
		}
	} else if (local_flags & LOCAL_INTERDOM_ACCOUNT) {
		size_t user_name_len = strlen(user_name);

		if (user_name[user_name_len - 1] != '$') {
			if (user_name_len + 2 > sizeof(user_name)) {
				fprintf(stderr, "machine name too long\n");
				exit(1);
			}
			user_name[user_name_len] = '$';
			user_name[user_name_len + 1] = '\0';
		}

		if ((local_flags & LOCAL_ADD_USER) && (new_passwd == NULL)) {
			/*
			 * Prompt for trusting domain's account password
			 */
			new_passwd = prompt_for_new_password(stdin_passwd_get);
			if(!new_passwd) {
				fprintf(stderr, "Unable to get newpassword.\n");
				exit(1);
			}
		}
	} else {

		if (remote_machine != NULL) {
			old_passwd = get_pass("Old SMB password:",stdin_passwd_get);
			if(!old_passwd) {
				fprintf(stderr, "Unable to get old password.\n");
				exit(1);
			}
		}

		if (!(local_flags & LOCAL_SET_PASSWORD)) {

			/*
			 * If we are trying to enable a user, first we need to find out
			 * if they are using a modern version of the smbpasswd file that
			 * disables a user by just writing a flag into the file. If so
			 * then we can re-enable a user without prompting for a new
			 * password. If not (ie. they have a no stored password in the
			 * smbpasswd file) then we need to prompt for a new password.
			 */

			if(local_flags & LOCAL_ENABLE_USER) {
				struct samu *sampass = NULL;

				sampass = samu_new( NULL );
				if (!sampass) {
					fprintf(stderr, "talloc fail for struct samu.\n");
					exit(1);
				}
				if (!pdb_getsampwnam(sampass, user_name)) {
					fprintf(stderr, "Failed to find user %s in passdb backend.\n",
						user_name );
					exit(1);
				}

				if(pdb_get_nt_passwd(sampass) == NULL) {
					local_flags |= LOCAL_SET_PASSWORD;
				}
				TALLOC_FREE(sampass);
			}
		}

		if((local_flags & LOCAL_SET_PASSWORD) && (new_passwd == NULL)) {

			new_passwd = prompt_for_new_password(stdin_passwd_get);
			if(!new_passwd) {
				fprintf(stderr, "Unable to get new password.\n");
				exit(1);
			}
		}
	}

	if (!NT_STATUS_IS_OK(password_change(remote_machine,
					     NULL, user_name,
					     old_passwd, new_passwd,
					     local_flags))) {
		result = 1;
		goto done;
	} 

	if(remote_machine) {
		printf("Password changed for user %s on %s.\n", user_name, remote_machine );
	} else if(!(local_flags & (LOCAL_ADD_USER|LOCAL_DISABLE_USER|LOCAL_ENABLE_USER|LOCAL_DELETE_USER|LOCAL_SET_NO_PASSWORD|LOCAL_SET_PASSWORD))) {
		struct samu *sampass = NULL;

		sampass = samu_new( NULL );
		if (!sampass) {
			fprintf(stderr, "talloc fail for struct samu.\n");
			exit(1);
		}

		if (!pdb_getsampwnam(sampass, user_name)) {
			fprintf(stderr, "Failed to find user %s in passdb backend.\n",
				user_name );
			exit(1);
		}

		printf("Password changed for user %s.", user_name );
		if(pdb_get_acct_ctrl(sampass)&ACB_DISABLED) {
			printf(" User has disabled flag set.");
		}
		if(pdb_get_acct_ctrl(sampass) & ACB_PWNOTREQ) {
			printf(" User has no password flag set.");
		}
		printf("\n");
		TALLOC_FREE(sampass);
	}

 done:
	SAFE_FREE(old_passwd);
	SAFE_FREE(new_passwd);
	return result;
}


/*************************************************************
 Handle password changing for non-root.
*************************************************************/

static int process_nonroot(int local_flags)
{
	struct passwd  *pwd = NULL;
	int result = 0;
	char *old_pw = NULL;
	char *new_pw = NULL;
	const char *username = user_name;
	const char *domain = NULL;
	char *p = NULL;

	if (local_flags & ~(LOCAL_AM_ROOT | LOCAL_SET_PASSWORD)) {
		/* Extra flags that we can't honor non-root */
		usage();
	}

	if (!user_name[0]) {
		pwd = getpwuid_alloc(talloc_tos(), getuid());
		if (pwd) {
			fstrcpy(user_name,pwd->pw_name);
			TALLOC_FREE(pwd);
		} else {
			fprintf(stderr, "smbpasswd: cannot lookup user name for uid %u\n", (unsigned int)getuid());
			exit(1);
		}
	}

	/* Allow domain as part of the username */
	if ((p = strchr_m(user_name, '\\')) ||
	    (p = strchr_m(user_name, '/')) ||
	    (p = strchr_m(user_name, *lp_winbind_separator()))) {
		*p = '\0';
		username = p + 1;
		domain = user_name;
	}

	/*
	 * A non-root user is always setting a password
	 * via a remote machine (even if that machine is
	 * localhost).
	 */	

	load_interfaces(); /* Delayed from main() */

	if (remote_machine != NULL) {
		if (!is_ipaddress(remote_machine)) {
			domain = remote_machine;
		}
	} else {
		remote_machine = "127.0.0.1";

		/*
		 * If we deal with a local user, change the password for the
		 * user in our SAM.
		 */
		domain = get_global_sam_name();
	}

	old_pw = get_pass("Old SMB password:",stdin_passwd_get);
	if (old_pw == NULL) {
		fprintf(stderr, "Unable to get old password.\n");
		exit(1);
	}

	if (!new_passwd) {
		new_pw = prompt_for_new_password(stdin_passwd_get);
	}
	else
		new_pw = smb_xstrdup(new_passwd);

	if (!new_pw) {
		fprintf(stderr, "Unable to get new password.\n");
		exit(1);
	}

	if (!NT_STATUS_IS_OK(password_change(remote_machine,
					     domain, username,
					     old_pw, new_pw, 0))) {
		result = 1;
		goto done;
	}

	printf("Password changed for user %s\n", username);

 done:
	SAFE_FREE(old_pw);
	SAFE_FREE(new_pw);

	return result;
}



/*********************************************************
 Start here.
**********************************************************/
int main(int argc, char **argv)
{	
	TALLOC_CTX *frame = talloc_stackframe();
	struct loadparm_context *lp_ctx = NULL;
	int local_flags = 0;
	int ret;

#if defined(HAVE_SET_AUTH_PARAMETERS)
	set_auth_parameters(argc, argv);
#endif /* HAVE_SET_AUTH_PARAMETERS */

	if (getuid() == 0) {
		local_flags = LOCAL_AM_ROOT;
	}

	smb_init_locale();

	lp_ctx = loadparm_init_s3(frame, loadparm_s3_helpers());
	if (lp_ctx == NULL) {
		fprintf(stderr,
			"Failed to initialise the global parameter structure.\n");
		return 1;
	}

	local_flags = process_options(argc, argv, local_flags, lp_ctx);

	setup_logging("smbpasswd", DEBUG_STDERR);

	/* Check the effective uid - make sure we are not setuid */
	if (is_setuid_root()) {
		fprintf(stderr, "smbpasswd must *NOT* be setuid root.\n");
		exit(1);
	}

	if (local_flags & LOCAL_AM_ROOT) {
		bool ok;

		ok = secrets_init();
		if (!ok) {
			return 1;
		}
		ret = process_root(local_flags);
	} else {
		ret = process_nonroot(local_flags);
	}
	TALLOC_FREE(frame);
	return ret;
}
