/* 
   Unix SMB/CIFS implementation.
   SMB parameters and setup, plus a whole lot more.
   
   Copyright (C) Andrew Tridgell              1992-2000
   Copyright (C) John H Terpstra              1996-2002
   Copyright (C) Luke Kenneth Casson Leighton 1996-2000
   Copyright (C) Paul Ashton                  1998-2000
   Copyright (C) Simo Sorce                   2001-2002
   Copyright (C) Martin Pool		      2002
   
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

#ifndef _RAW_SMB_H
#define _RAW_SMB_H

/* deny modes */
#define DENY_DOS 0
#define DENY_ALL 1
#define DENY_WRITE 2
#define DENY_READ 3
#define DENY_NONE 4
#define DENY_FCB 7

/* open modes */
#define DOS_OPEN_RDONLY 0
#define DOS_OPEN_WRONLY 1
#define DOS_OPEN_RDWR 2
#define DOS_OPEN_FCB 0xF


/**********************************/
/* SMBopen field definitions      */
#define OPEN_FLAGS_DENY_MASK  0x70
#define OPEN_FLAGS_DENY_DOS   0x00
#define OPEN_FLAGS_DENY_ALL   0x10
#define OPEN_FLAGS_DENY_WRITE 0x20
#define OPEN_FLAGS_DENY_READ  0x30
#define OPEN_FLAGS_DENY_NONE  0x40

#define OPEN_FLAGS_MODE_MASK  0x0F
#define OPEN_FLAGS_OPEN_READ     0
#define OPEN_FLAGS_OPEN_WRITE    1
#define OPEN_FLAGS_OPEN_RDWR     2
#define OPEN_FLAGS_FCB        0xFF


/**********************************/
/* SMBopenX field definitions     */

/* OpenX Flags field. */
#define OPENX_FLAGS_ADDITIONAL_INFO      0x01
#define OPENX_FLAGS_REQUEST_OPLOCK       0x02
#define OPENX_FLAGS_REQUEST_BATCH_OPLOCK 0x04
#define OPENX_FLAGS_EA_LEN               0x08
#define OPENX_FLAGS_EXTENDED_RETURN      0x10

/* desired access (open_mode), split info 4 4-bit nibbles */
#define OPENX_MODE_ACCESS_MASK   0x000F
#define OPENX_MODE_ACCESS_READ   0x0000
#define OPENX_MODE_ACCESS_WRITE  0x0001
#define OPENX_MODE_ACCESS_RDWR   0x0002
#define OPENX_MODE_ACCESS_EXEC   0x0003
#define OPENX_MODE_ACCESS_FCB    0x000F

#define OPENX_MODE_DENY_SHIFT    4
#define OPENX_MODE_DENY_MASK     (0xF        << OPENX_MODE_DENY_SHIFT)
#define OPENX_MODE_DENY_DOS      (DENY_DOS   << OPENX_MODE_DENY_SHIFT)
#define OPENX_MODE_DENY_ALL      (DENY_ALL   << OPENX_MODE_DENY_SHIFT)
#define OPENX_MODE_DENY_WRITE    (DENY_WRITE << OPENX_MODE_DENY_SHIFT)
#define OPENX_MODE_DENY_READ     (DENY_READ  << OPENX_MODE_DENY_SHIFT)
#define OPENX_MODE_DENY_NONE     (DENY_NONE  << OPENX_MODE_DENY_SHIFT)
#define OPENX_MODE_DENY_FCB      (0xF        << OPENX_MODE_DENY_SHIFT)

#define OPENX_MODE_LOCALITY_MASK 0x0F00 /* what does this do? */

#define OPENX_MODE_NO_CACHE      0x1000
#define OPENX_MODE_WRITE_THRU    0x4000

/* open function values */
#define OPENX_OPEN_FUNC_MASK  0x3
#define OPENX_OPEN_FUNC_FAIL  0x0
#define OPENX_OPEN_FUNC_OPEN  0x1
#define OPENX_OPEN_FUNC_TRUNC 0x2

/* The above can be OR'ed with... */
#define OPENX_OPEN_FUNC_CREATE 0x10

/* openx action in reply */
#define OPENX_ACTION_EXISTED    1
#define OPENX_ACTION_CREATED    2
#define OPENX_ACTION_TRUNCATED  3


/**********************************/
/* SMBntcreateX field definitions */

/* ntcreatex flags field. */
#define NTCREATEX_FLAGS_REQUEST_OPLOCK       0x02
#define NTCREATEX_FLAGS_REQUEST_BATCH_OPLOCK 0x04
#define NTCREATEX_FLAGS_OPEN_DIRECTORY       0x08 /* TODO: opens parent? we need
						     a test suite for this */
#define NTCREATEX_FLAGS_EXTENDED             0x10

/* the ntcreatex access_mask field 
   this is split into 4 pieces
   AAAABBBBCCCCCCCCDDDDDDDDDDDDDDDD
   A -> GENERIC_RIGHT_*
   B -> SEC_RIGHT_*
   C -> STD_RIGHT_*
   D -> SA_RIGHT_*
   
   which set of SA_RIGHT_* bits is applicable depends on the type
   of object.
*/



/* ntcreatex share_access field */
#define NTCREATEX_SHARE_ACCESS_NONE   0
#define NTCREATEX_SHARE_ACCESS_READ   1
#define NTCREATEX_SHARE_ACCESS_WRITE  2
#define NTCREATEX_SHARE_ACCESS_DELETE 4
#define NTCREATEX_SHARE_ACCESS_MASK   7

/* ntcreatex open_disposition field */
#define NTCREATEX_DISP_SUPERSEDE 0     /* supersede existing file (if it exists) */
#define NTCREATEX_DISP_OPEN 1          /* if file exists open it, else fail */
#define NTCREATEX_DISP_CREATE 2        /* if file exists fail, else create it */
#define NTCREATEX_DISP_OPEN_IF 3       /* if file exists open it, else create it */
#define NTCREATEX_DISP_OVERWRITE 4     /* if exists overwrite, else fail */
#define NTCREATEX_DISP_OVERWRITE_IF 5  /* if exists overwrite, else create */

/* ntcreatex create_options field */
#define NTCREATEX_OPTIONS_DIRECTORY                 0x0001
#define NTCREATEX_OPTIONS_WRITE_THROUGH             0x0002
#define NTCREATEX_OPTIONS_SEQUENTIAL_ONLY           0x0004
#define NTCREATEX_OPTIONS_NO_INTERMEDIATE_BUFFERING 0x0008
#define NTCREATEX_OPTIONS_SYNC_ALERT	            0x0010
#define NTCREATEX_OPTIONS_ASYNC_ALERT	            0x0020
#define NTCREATEX_OPTIONS_NON_DIRECTORY_FILE        0x0040
#define NTCREATEX_OPTIONS_TREE_CONNECTION           0x0080
#define NTCREATEX_OPTIONS_COMPLETE_IF_OPLOCKED      0x0100
#define NTCREATEX_OPTIONS_NO_EA_KNOWLEDGE           0x0200
#define NTCREATEX_OPTIONS_OPEN_FOR_RECOVERY         0x0400
#define NTCREATEX_OPTIONS_RANDOM_ACCESS             0x0800
#define NTCREATEX_OPTIONS_DELETE_ON_CLOSE           0x1000
#define NTCREATEX_OPTIONS_OPEN_BY_FILE_ID           0x2000
#define NTCREATEX_OPTIONS_BACKUP_INTENT             0x4000
#define NTCREATEX_OPTIONS_NO_COMPRESSION            0x8000
/* Must be ignored by the server, per MS-SMB 2.2.8 */
#define NTCREATEX_OPTIONS_OPFILTER              0x00100000
#define NTCREATEX_OPTIONS_REPARSE_POINT         0x00200000
/* Don't pull this file off tape in a HSM system */
#define NTCREATEX_OPTIONS_NO_RECALL             0x00400000
/* Must be ignored by the server, per MS-SMB 2.2.8 */
#define NTCREATEX_OPTIONS_FREE_SPACE_QUERY      0x00800000

#define NTCREATEX_OPTIONS_MUST_IGNORE_MASK      (NTCREATEX_OPTIONS_TREE_CONNECTION | \
						 NTCREATEX_OPTIONS_OPEN_FOR_RECOVERY | \
						 NTCREATEX_OPTIONS_FREE_SPACE_QUERY | \
						 0x000F0000)

#define NTCREATEX_OPTIONS_NOT_SUPPORTED_MASK    (NTCREATEX_OPTIONS_OPEN_BY_FILE_ID)

#define NTCREATEX_OPTIONS_INVALID_PARAM_MASK    (NTCREATEX_OPTIONS_OPFILTER | \
						 NTCREATEX_OPTIONS_SYNC_ALERT | \
						 NTCREATEX_OPTIONS_ASYNC_ALERT | \
						 0xFF000000)

/*
 * private_flags field in ntcreatex
 * This values have different meaning for some ntvfs backends.
 */
#define NTCREATEX_FLAG_DENY_DOS      0x0001
#define NTCREATEX_FLAG_DENY_FCB      0x0002


/* ntcreatex impersonation field */
#define NTCREATEX_IMPERSONATION_ANONYMOUS      0
#define NTCREATEX_IMPERSONATION_IDENTIFICATION 1
#define NTCREATEX_IMPERSONATION_IMPERSONATION  2
#define NTCREATEX_IMPERSONATION_DELEGATION     3

/* ntcreatex security flags bit field */
#define NTCREATEX_SECURITY_DYNAMIC             1
#define NTCREATEX_SECURITY_ALL                 2

/* ntcreatex create_action in reply */
#define NTCREATEX_ACTION_EXISTED     1
#define NTCREATEX_ACTION_CREATED     2
#define NTCREATEX_ACTION_TRUNCATED   3
/* the value 5 can also be returned when you try to create a directory with
   incorrect parameters - what does it mean? maybe created temporary file? */
#define NTCREATEX_ACTION_UNKNOWN 5

/* Named pipe write mode flags. Used in writeX calls. */
#define PIPE_RAW_MODE 0x4
#define PIPE_START_MESSAGE 0x8

/* the desired access to use when opening a pipe */
#define DESIRED_ACCESS_PIPE 0x2019f
 
/* Flag for NT transact rename call. */
#define RENAME_REPLACE_IF_EXISTS 1

/* flags for SMBntrename call */
#define RENAME_FLAG_MOVE_CLUSTER_INFORMATION 0x102 /* ???? */
#define RENAME_FLAG_HARD_LINK                0x103
#define RENAME_FLAG_RENAME                   0x104
#define RENAME_FLAG_COPY                     0x105

/* ChangeNotify flags. */
#define FILE_NOTIFY_CHANGE_FILE_NAME	0x00000001
#define FILE_NOTIFY_CHANGE_DIR_NAME	0x00000002
#define FILE_NOTIFY_CHANGE_ATTRIBUTES	0x00000004
#define FILE_NOTIFY_CHANGE_SIZE		0x00000008
#define FILE_NOTIFY_CHANGE_LAST_WRITE	0x00000010
#define FILE_NOTIFY_CHANGE_LAST_ACCESS	0x00000020
#define FILE_NOTIFY_CHANGE_CREATION	0x00000040
#define FILE_NOTIFY_CHANGE_EA		0x00000080
#define FILE_NOTIFY_CHANGE_SECURITY	0x00000100
#define FILE_NOTIFY_CHANGE_STREAM_NAME	0x00000200
#define FILE_NOTIFY_CHANGE_STREAM_SIZE	0x00000400
#define FILE_NOTIFY_CHANGE_STREAM_WRITE	0x00000800

#define FILE_NOTIFY_CHANGE_NAME \
	(FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME)

#define FILE_NOTIFY_CHANGE_ALL \
	(FILE_NOTIFY_CHANGE_FILE_NAME   | FILE_NOTIFY_CHANGE_DIR_NAME | \
	 FILE_NOTIFY_CHANGE_ATTRIBUTES  | FILE_NOTIFY_CHANGE_SIZE | \
	 FILE_NOTIFY_CHANGE_LAST_WRITE  | FILE_NOTIFY_CHANGE_LAST_ACCESS | \
	 FILE_NOTIFY_CHANGE_CREATION    | FILE_NOTIFY_CHANGE_EA | \
	 FILE_NOTIFY_CHANGE_SECURITY	| FILE_NOTIFY_CHANGE_STREAM_NAME | \
	 FILE_NOTIFY_CHANGE_STREAM_SIZE | FILE_NOTIFY_CHANGE_STREAM_WRITE)

/* change notify action results */
#define NOTIFY_ACTION_ADDED 1
#define NOTIFY_ACTION_REMOVED 2
#define NOTIFY_ACTION_MODIFIED 3
#define NOTIFY_ACTION_OLD_NAME 4
#define NOTIFY_ACTION_NEW_NAME 5
#define NOTIFY_ACTION_ADDED_STREAM 6
#define NOTIFY_ACTION_REMOVED_STREAM 7
#define NOTIFY_ACTION_MODIFIED_STREAM 8

/* seek modes for smb_seek */
#define SEEK_MODE_START   0
#define SEEK_MODE_CURRENT 1
#define SEEK_MODE_END     2

/* where to find the base of the SMB packet proper */
/* REWRITE TODO: smb_base needs to be removed */
#define smb_base(buf) (((const char *)(buf))+4)

/* we don't allow server strings to be longer than 48 characters as
   otherwise NT will not honour the announce packets */
#define MAX_SERVER_STRING_LENGTH 48

/* This was set by JHT in liaison with Jeremy Allison early 1997
 * History:
 * Version 4.0 - never made public
 * Version 4.10 - New to 1.9.16p2, lost in space 1.9.16p3 to 1.9.16p9
 *              - Reappeared in 1.9.16p11 with fixed smbd services
 * Version 4.20 - To indicate that nmbd and browsing now works better
 * Version 4.50 - Set at release of samba-2.2.0 by JHT
 *
 *  Note: In the presence of NT4.X do not set above 4.9
 *        Setting this above 4.9 can have undesired side-effects.
 *        This may change again in Samba-3.0 after further testing. JHT
 */
 
#define DEFAULT_MAJOR_VERSION 0x04
#define DEFAULT_MINOR_VERSION 0x09

/* Browser Election Values */
#define BROWSER_ELECTION_VERSION	0x010f
#define BROWSER_CONSTANT	0xaa55

/*
 * Global value meaning that the smb_uid field should be
 * ignored (in share level security and protocol level == CORE)
 */

#define UID_FIELD_INVALID 0

/*
  filesystem attribute bits
*/
#define FS_ATTR_CASE_SENSITIVE_SEARCH             0x00000001
#define FS_ATTR_CASE_PRESERVED_NAMES              0x00000002
#define FS_ATTR_UNICODE_ON_DISK                   0x00000004
#define FS_ATTR_PERSISTANT_ACLS                   0x00000008
#define FS_ATTR_COMPRESSION                       0x00000010
#define FS_ATTR_QUOTAS                            0x00000020
#define FS_ATTR_SPARSE_FILES                      0x00000040
#define FS_ATTR_REPARSE_POINTS                    0x00000080
#define FS_ATTR_REMOTE_STORAGE                    0x00000100
#define FS_ATTR_LFN_SUPPORT                       0x00004000
#define FS_ATTR_IS_COMPRESSED                     0x00008000
#define FS_ATTR_OBJECT_IDS                        0x00010000
#define FS_ATTR_ENCRYPTION                        0x00020000
#define FS_ATTR_NAMED_STREAMS                     0x00040000

#include "source4/libcli/raw/trans2.h"
#include "libcli/raw/interfaces.h"
#include "libcli/smb/smb_common.h"

#endif /* _RAW_SMB_H */
