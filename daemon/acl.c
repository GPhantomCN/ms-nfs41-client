/* Copyright (c) 2010
 * The Regents of the University of Michigan
 * All Rights Reserved
 *
 * Permission is granted to use, copy and redistribute this software
 * for noncommercial education and research purposes, so long as no
 * fee is charged, and so long as the name of the University of Michigan
 * is not used in any advertising or publicity pertaining to the use
 * or distribution of this software without specific, written prior
 * authorization.  Permission to modify or otherwise create derivative
 * works of this software is not granted.
 *
 * This software is provided as is, without representation or warranty
 * of any kind either express or implied, including without limitation
 * the implied warranties of merchantability, fitness for a particular
 * purpose, or noninfringement.  The Regents of the University of
 * Michigan shall not be liable for any damages, including special,
 * indirect, incidental, or consequential damages, with respect to any
 * claim arising out of or in connection with the use of the software,
 * even if it has been or is hereafter advised of the possibility of
 * such damages.
 */
#include <Windows.h>
#include <strsafe.h>
#include <sddl.h>

#include "nfs41.h"
#include "nfs41_ops.h"
#include "daemon_debug.h"
#include "util.h"
#include "upcall.h"
#include "nfs41_xdr.h"

static int parse_getacl(unsigned char *buffer, uint32_t length, nfs41_upcall *upcall)
{
    int status;
    getacl_upcall_args *args = &upcall->args.getacl;

    status = safe_read(&buffer, &length, &args->root, sizeof(HANDLE));
    if (status) goto out;
    upcall_root_ref(upcall, args->root);
    status = safe_read(&buffer, &length, &args->state, sizeof(args->state));
    if (status) goto out;
    upcall_open_state_ref(upcall, args->state);
    status = safe_read(&buffer, &length, &args->query, sizeof(args->query));
    if (status) goto out;

    dprintf(1, "parsing NFS41_ACL_QUERY: info_class=%d root=0x%p open_state=0x%p\n",
        args->query, args->root, args->state);
out:
    return status;
}

static int create_unknownsid(WELL_KNOWN_SID_TYPE type, PSID *sid, DWORD *sid_len)
{
    int status;
    *sid_len = 0;
    *sid = NULL;
    if (!CreateWellKnownSid(type, NULL, *sid, sid_len)) {
        status = GetLastError();
        if (status == ERROR_INSUFFICIENT_BUFFER) {
            *sid = malloc(*sid_len);
            if (*sid == NULL) return ERROR_INSUFFICIENT_BUFFER;
            if (!CreateWellKnownSid(type, NULL, *sid, sid_len)) {
                free(*sid);
                status = GetLastError();
                dprintf(1, "CreateWellKnownSid failed with %d\n", status);
                return status;
            } else return 0;
        } else return status;
    } else return ERROR_INTERNAL_ERROR;
}

static void convert_nfs4name_2_user_domain(LPSTR nfs4name, 
    LPSTR *domain)
{
    LPSTR p = nfs4name;
    for(; p[0] != '\0'; p++) {
        if (p[0] == '@') { 
            p[0] = '\0';
            *domain = &p[1];
            break;
        }
    }
}

static int map_name_2_sid(DWORD *sid_len, PSID *sid, LPCSTR name)
{
    int status;
    SID_NAME_USE sid_type;
    LPSTR tmp_buf = NULL;
    DWORD tmp = 0;

    status = LookupAccountName(NULL, name, NULL, sid_len, NULL, &tmp, &sid_type);
    dprintf(1, "LookupAccountName returned %d GetLastError %d owner len %d "
        "domain len %d\n", status, GetLastError(), *sid_len, tmp); 
    if (!status) {
        status = GetLastError();
        switch(status) {
        case ERROR_INSUFFICIENT_BUFFER:
            *sid = malloc(*sid_len);
            if (*sid == NULL) {
                status = GetLastError();
                goto out;
            }
            tmp_buf = (LPSTR) malloc(tmp);
            if (tmp_buf == NULL) {
                status = GetLastError();
                free(*sid);
                goto out;
            }
            status = LookupAccountName(NULL, name, *sid, sid_len, tmp_buf, 
                                        &tmp, &sid_type);
            dprintf(1, "sid_type = %d\n", sid_type);
            free(tmp_buf);
            if (!status) {
                status = GetLastError();
                free(*sid);
                dprintf(1, "handle_getacl: LookupAccountName for owner failed "
                        "with %d\n", status);
                goto out;
            } else {
                LPSTR ssid = NULL;
                if (IsValidSid(*sid))
                    if (ConvertSidToStringSidA(*sid, &ssid))
                        printf("SID %s\n", ssid);
                    else
                        printf("ConvertSidToStringSidA failed with %d\n", GetLastError());
                else
                    printf("Invalid Sid\n");
                if (ssid) LocalFree(ssid);
            }
            status = 0;
            break;
        case ERROR_NONE_MAPPED:
            status = create_unknownsid(WinNullSid, sid, sid_len);
            break;
        }
    } else // This shouldn't happen
        status = ERROR_INTERNAL_ERROR;
out:
    return status;
}

static int handle_getacl(nfs41_upcall *upcall)
{
    int status = ERROR_NOT_SUPPORTED;
    getacl_upcall_args *args = &upcall->args.getacl;
    nfs41_open_state *state = args->state;
    nfs41_file_info info;
    bitmap4 attr_request;
    LPSTR domain = NULL;

    // need to cache owner/group information XX
    ZeroMemory(&info, sizeof(info));
    init_getattr_request(&attr_request);
    if (args->query & DACL_SECURITY_INFORMATION) {
        info.acl = calloc(1, sizeof(nfsacl41));
        if (info.acl == NULL) {
            status = GetLastError();
            goto out;
        }
        attr_request.arr[0] |= FATTR4_WORD0_ACL;
    }
    status = nfs41_getattr(state->session, &state->file, &attr_request, &info);
    if (status) {
        eprintf("nfs41_cached_getattr() failed with %d\n", status);
        goto out;
    }

    args->osid_len = args->gsid_len = 0;
    if (args->query & OWNER_SECURITY_INFORMATION) {
        // parse user@domain. currently ignoring domain part XX
        convert_nfs4name_2_user_domain((LPSTR)info.owner, &domain);
        dprintf(1, "handle_getacl: OWNER_SECURITY_INFORMATION: for user=%s domain=%s\n", 
                info.owner, domain?domain:"<null>");
        status = map_name_2_sid(&args->osid_len, &args->osid, (LPSTR)info.owner);
        if (status)
            goto out;
    }
    if (args->query & GROUP_SECURITY_INFORMATION) {
        convert_nfs4name_2_user_domain((LPSTR)info.owner_group, &domain);
        dprintf(1, "handle_getacl: GROUP_SECURITY_INFORMATION: for %s domain=%s\n", 
                info.owner_group, domain?domain:"<null>");
        status = map_name_2_sid(&args->gsid_len, &args->gsid, (LPSTR)info.owner_group);
        if (status)
            goto out;
    }
    if (args->query & DACL_SECURITY_INFORMATION)
        dprintf(1, "handle_getacl: DACL_SECURITY_INFORMATION\n");
    if (args->query & SACL_SECURITY_INFORMATION)
        dprintf(1, "handle_getacl: SACL_SECURITY_INFORMATION\n");

out:
    if (args->query & DACL_SECURITY_INFORMATION) {
        nfsacl41_free(info.acl);
        free(info.acl);
    }
    return status;
}

static int marshall_acl(unsigned char **buffer, uint32_t *remaining, uint32_t sid_len, PSID sid)
{
    int status;
    status = safe_write(buffer, remaining, &sid_len, sizeof(sid_len));
    if (status) goto out;
    if (*remaining < sid_len)
        return ERROR_BUFFER_OVERFLOW;
    status = CopySid(sid_len, *buffer, sid);
    free(sid);
    if (!status) {
        status = GetLastError();
        dprintf(1, "marshall_acl: CopySid failed %d\n", status);
        goto out;
    } else {
        status = 0;
        *buffer += sid_len;
        *remaining -= sid_len;
    }
out:
    return status;
}

static int marshall_getacl(unsigned char *buffer, uint32_t *length, nfs41_upcall *upcall)
{
    int status = ERROR_NOT_SUPPORTED;
    getacl_upcall_args *args = &upcall->args.getacl;

    if (args->query & OWNER_SECURITY_INFORMATION) {
        status = marshall_acl(&buffer, length, args->osid_len, args->osid);
        if (status) goto out;
    }
    if (args->query & GROUP_SECURITY_INFORMATION) {
        status = marshall_acl(&buffer, length, args->gsid_len, args->gsid);
        if (status) goto out;
    }
out:
    return status;
}

const nfs41_upcall_op nfs41_op_getacl = {
    parse_getacl,
    handle_getacl,
    marshall_getacl
};
