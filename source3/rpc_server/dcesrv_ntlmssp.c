/*
 *  NTLMSSP Acceptor
 *  DCERPC Server functions
 *  Copyright (C) Simo Sorce 2010.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */


#include "includes.h"
#include "rpc_server/dcesrv_ntlmssp.h"
#include "../auth/ntlmssp/ntlmssp.h"
#include "ntlmssp_wrap.h"
#include "auth.h"
#include "auth/gensec/gensec.h"

NTSTATUS ntlmssp_server_auth_start(TALLOC_CTX *mem_ctx,
				   bool do_sign,
				   bool do_seal,
				   bool is_dcerpc,
				   DATA_BLOB *token_in,
				   DATA_BLOB *token_out,
				   const struct tsocket_address *remote_address,
				   struct gensec_security **ctx)
{
	struct auth_ntlmssp_state *a = NULL;
	NTSTATUS status;

	status = auth_ntlmssp_prepare(remote_address, &a);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0, (__location__ ": auth_ntlmssp_prepare failed: %s\n",
			  nt_errstr(status)));
		return status;
	}

	if (do_sign) {
		gensec_want_feature(a->gensec_security, GENSEC_FEATURE_SIGN);
	}
	if (do_seal) {
		/* Always implies both sign and seal for ntlmssp */
		gensec_want_feature(a->gensec_security, GENSEC_FEATURE_SEAL);
	}

	status = auth_ntlmssp_start(a);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0, (__location__ ": auth_ntlmssp_start failed: %s\n",
			  nt_errstr(status)));
		return status;
	}

	status = gensec_update(a->gensec_security, mem_ctx, NULL, *token_in, token_out);
	if (!NT_STATUS_EQUAL(status, NT_STATUS_MORE_PROCESSING_REQUIRED)) {
		DEBUG(0, (__location__ ": auth_ntlmssp_update failed: %s\n",
			  nt_errstr(status)));
		goto done;
	}

	/* steal ntlmssp context too */
	*ctx = talloc_move(mem_ctx, &a->gensec_security);

	status = NT_STATUS_OK;

done:
	TALLOC_FREE(a);

	return status;
}

NTSTATUS ntlmssp_server_step(struct gensec_security *gensec_security,
			     TALLOC_CTX *mem_ctx,
			     DATA_BLOB *token_in,
			     DATA_BLOB *token_out)
{
	NTSTATUS status;

	/* this has to be done as root in order to verify the password */
	become_root();
	status = gensec_update(gensec_security, mem_ctx, NULL, *token_in, token_out);
	unbecome_root();

	return status;
}

NTSTATUS ntlmssp_server_check_flags(struct gensec_security *gensec_security,
				    bool do_sign, bool do_seal)
{
	if (do_sign && !gensec_have_feature(gensec_security, GENSEC_FEATURE_SIGN)) {
		DEBUG(1, (__location__ "Integrity was requested but client "
			  "failed to negotiate signing.\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	if (do_seal && !gensec_have_feature(gensec_security, GENSEC_FEATURE_SEAL)) {
		DEBUG(1, (__location__ "Privacy was requested but client "
			  "failed to negotiate sealing.\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	return NT_STATUS_OK;
}

NTSTATUS ntlmssp_server_get_user_info(struct gensec_security *gensec_security,
				      TALLOC_CTX *mem_ctx,
				      struct auth_session_info **session_info)
{
	NTSTATUS status;

	status = gensec_session_info(gensec_security, mem_ctx, session_info);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(1, (__location__ ": Failed to get authenticated user "
			  "info: %s\n", nt_errstr(status)));
		return status;
	}

	DEBUG(5, (__location__ "OK: user: %s domain: %s\n",
		  (*session_info)->info->account_name,
		  (*session_info)->info->domain_name));

	return NT_STATUS_OK;
}
