/*
   Unix SMB/CIFS implementation.

   DNS server handler for queries

   Copyright (C) 2010 Kai Blin  <kai@samba.org>

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
#include "libcli/util/werror.h"
#include "librpc/ndr/libndr.h"
#include "librpc/gen_ndr/ndr_dns.h"
#include "librpc/gen_ndr/ndr_dnsp.h"
#include <ldb.h>
#include "dsdb/samdb/samdb.h"
#include "dsdb/common/util.h"
#include "dns_server/dns_server.h"

static WERROR create_response_rr(const struct dns_name_question *question,
				 const struct dnsp_DnssrvRpcRecord *rec,
				 struct dns_res_rec **answers, uint16_t *ancount)
{
	struct dns_res_rec *ans = *answers;
	uint16_t ai = *ancount;

	ZERO_STRUCT(ans[ai]);

	switch (rec->wType) {
	case DNS_QTYPE_CNAME:
		ans[ai].rdata.cname_record = talloc_strdup(ans, rec->data.cname);
		break;
	case DNS_QTYPE_A:
		ans[ai].rdata.ipv4_record = talloc_strdup(ans, rec->data.ipv4);
		break;
	case DNS_QTYPE_AAAA:
		ans[ai].rdata.ipv6_record = rec->data.ipv6;
		break;
	case DNS_TYPE_NS:
		ans[ai].rdata.ns_record = rec->data.ns;
		break;
	case DNS_QTYPE_SRV:
		ans[ai].rdata.srv_record.priority = rec->data.srv.wPriority;
		ans[ai].rdata.srv_record.weight   = rec->data.srv.wWeight;
		ans[ai].rdata.srv_record.port     = rec->data.srv.wPort;
		ans[ai].rdata.srv_record.target   = rec->data.srv.nameTarget;
		break;
	case DNS_QTYPE_SOA:
		ans[ai].rdata.soa_record.mname	 = rec->data.soa.mname;
		ans[ai].rdata.soa_record.rname	 = rec->data.soa.rname;
		ans[ai].rdata.soa_record.serial	 = rec->data.soa.serial;
		ans[ai].rdata.soa_record.refresh = rec->data.soa.refresh;
		ans[ai].rdata.soa_record.retry	 = rec->data.soa.retry;
		ans[ai].rdata.soa_record.expire	 = rec->data.soa.expire;
		ans[ai].rdata.soa_record.minimum = rec->data.soa.minimum;
		break;
	default:
		return DNS_ERR(NOT_IMPLEMENTED);
	}

	ans[ai].name = talloc_strdup(ans, question->name);
	ans[ai].rr_type = rec->wType;
	ans[ai].rr_class = DNS_QCLASS_IN;
	ans[ai].ttl = rec->dwTtlSeconds;
	ans[ai].length = UINT16_MAX;
	ai++;

	*answers = ans;
	*ancount = ai;

	return WERR_OK;
}

static WERROR handle_question(struct dns_server *dns,
			      TALLOC_CTX *mem_ctx,
			      const struct dns_name_question *question,
			      struct dns_res_rec **answers, uint16_t *ancount)
{
	struct dns_res_rec *ans;
	struct ldb_dn *dn = NULL;
	WERROR werror;
	static const char * const attrs[] = { "dnsRecord", NULL};
	int ret;
	uint16_t ai = *ancount;
	unsigned int ri;
	struct ldb_message *msg = NULL;
	struct dnsp_DnssrvRpcRecord *recs;
	struct ldb_message_element *el;

	werror = dns_name2dn(dns, mem_ctx, question->name, &dn);
	W_ERROR_NOT_OK_RETURN(werror);

	ret = dsdb_search_one(dns->samdb, mem_ctx, &msg, dn,
			      LDB_SCOPE_BASE, attrs, 0, "%s", "(objectClass=dnsNode)");
	if (ret != LDB_SUCCESS) {
		return DNS_ERR(NAME_ERROR);
	}

	el = ldb_msg_find_element(msg, attrs[0]);
	if (el == NULL) {
		return DNS_ERR(NAME_ERROR);
	}

	recs = talloc_array(mem_ctx, struct dnsp_DnssrvRpcRecord, el->num_values);
	for (ri = 0; ri < el->num_values; ri++) {
		struct ldb_val *v = &el->values[ri];
		enum ndr_err_code ndr_err;

		ndr_err = ndr_pull_struct_blob(v, recs, &recs[ri],
				(ndr_pull_flags_fn_t)ndr_pull_dnsp_DnssrvRpcRecord);
		if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
			DEBUG(0, ("Failed to grab dnsp_DnssrvRpcRecord\n"));
			return DNS_ERR(SERVER_FAILURE);
		}
	}

	ans = talloc_realloc(mem_ctx, *answers, struct dns_res_rec,
			     ai + el->num_values);
	W_ERROR_HAVE_NO_MEMORY(ans);

	for (ri = 0; ri < el->num_values; ri++) {
		if ((question->question_type != DNS_QTYPE_ALL) &&
		    (recs[ri].wType != question->question_type)) {
			continue;
		}
		create_response_rr(question, &recs[ri], &ans, &ai);
	}

	if (*ancount == ai) {
		return DNS_ERR(NAME_ERROR);
	}

	*ancount = ai;
	*answers = ans;

	return WERR_OK;

}

WERROR dns_server_process_query(struct dns_server *dns,
				TALLOC_CTX *mem_ctx,
				struct dns_name_packet *in,
				struct dns_res_rec **answers,    uint16_t *ancount,
				struct dns_res_rec **nsrecs,     uint16_t *nscount,
				struct dns_res_rec **additional, uint16_t *arcount)
{
	uint16_t num_answers=0;
	struct dns_res_rec *ans=NULL;
	WERROR werror;

	if (in->qdcount != 1) {
		return DNS_ERR(FORMAT_ERROR);
	}

	/* Windows returns NOT_IMPLEMENTED on this as well */
	if (in->questions[0].question_class == DNS_QCLASS_NONE) {
		return DNS_ERR(NOT_IMPLEMENTED);
	}

	ans = talloc_array(mem_ctx, struct dns_res_rec, 0);
	W_ERROR_HAVE_NO_MEMORY(ans);

	werror = handle_question(dns, mem_ctx, &in->questions[0], &ans, &num_answers);
	W_ERROR_NOT_OK_GOTO(werror, query_failed);

	*answers = ans;
	*ancount = num_answers;

	/*FIXME: Do something for these */
	*nsrecs  = NULL;
	*nscount = 0;

	*additional = NULL;
	*arcount    = 0;

	return WERR_OK;

query_failed:
	/*FIXME: add our SOA record to nsrecs */
	return werror;
}
