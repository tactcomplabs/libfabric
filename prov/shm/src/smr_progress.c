/*
 * Copyright (c) 2013-2018 Intel Corporation. All rights reserved
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include "ofi_iov.h"
#include "smr.h"

static int smr_progress_resp_entry(struct smr_ep *ep, struct smr_tx_entry *pending,
				   uint64_t *ret)
{
	struct smr_region *peer_smr;
	size_t inj_offset, size;
	struct smr_inject_buf *tx_buf;
	uint8_t *src;

	peer_smr = smr_peer_region(ep->region, pending->addr);
	if (fastlock_tryacquire(&peer_smr->lock))
		return -FI_EAGAIN;

	switch (pending->cmd.msg.hdr.op_src) {
	case smr_src_iov:
		goto out;
	case smr_src_mmap:
		if (pending->cmd.msg.hdr.op == ofi_op_read_req) {
			if (!*ret) {
				size = ofi_copy_to_iov(pending->iov,
						pending->iov_count, 0,
						pending->map_ptr,
						pending->cmd.msg.hdr.size);
				if (size != pending->cmd.msg.hdr.size) {
					FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
						"Incomplete copy from mmapped file\n");
					*ret = -FI_EIO;
				}
			}
			munmap(pending->map_ptr, pending->cmd.msg.hdr.size);
		}
		shm_unlink(pending->map_name->name);
		dlist_remove(&pending->map_name->entry);
		free(pending->map_name);
		goto out;
	case smr_src_inject:
		inj_offset = (size_t) pending->cmd.msg.hdr.src_data;
		tx_buf = (struct smr_inject_buf *) smr_get_addr(peer_smr, inj_offset);

		if (*ret)
			goto push;

		src = pending->cmd.msg.hdr.op == ofi_op_atomic_compare ?
		      tx_buf->buf : tx_buf->data;
		size = ofi_copy_to_iov(pending->iov, pending->iov_count,
				       0, src, pending->cmd.msg.hdr.size);

		if (size != pending->cmd.msg.hdr.size) {
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"Incomplete rma read/fetch buffer copied\n");
			*ret = FI_EIO;
		}
		break;
	default:
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"unidentified operation type\n");
		goto out;
	}
push:
	smr_freestack_push(smr_inject_pool(peer_smr), tx_buf);
out:
	peer_smr->cmd_cnt++;
	fastlock_release(&peer_smr->lock);
	return 0;
}

static void smr_progress_resp(struct smr_ep *ep)
{
	struct smr_resp *resp;
	struct smr_tx_entry *pending;
	int ret;

	fastlock_acquire(&ep->region->lock);
	fastlock_acquire(&ep->util_ep.tx_cq->cq_lock);
	while (!ofi_cirque_isempty(smr_resp_queue(ep->region)) &&
	       !ofi_cirque_isfull(ep->util_ep.tx_cq->cirq)) {
		resp = ofi_cirque_head(smr_resp_queue(ep->region));
		if (resp->status == FI_EBUSY)
			break;

		pending = (struct smr_tx_entry *) resp->msg_id;
		if (smr_progress_resp_entry(ep, pending, &resp->status))
				break;

		ret = smr_complete_tx(ep, pending->context,
				  pending->cmd.msg.hdr.op, pending->cmd.msg.hdr.op_flags,
				  -(resp->status));
		if (ret) {
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"unable to process tx completion\n");
			break;
		}
		freestack_push(ep->pend_fs, pending);
		ofi_cirque_discard(smr_resp_queue(ep->region));
	}
	fastlock_release(&ep->util_ep.tx_cq->cq_lock);
	fastlock_release(&ep->region->lock);
}

static int smr_progress_inline(struct smr_cmd *cmd, struct iovec *iov,
			       size_t iov_count, size_t *total_len)
{
	*total_len = ofi_copy_to_iov(iov, iov_count, 0, cmd->msg.data.msg,
				     cmd->msg.hdr.size);
	if (*total_len != cmd->msg.hdr.size) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"recv truncated");
		return -FI_EIO;
	}
	return 0;
}

static int smr_progress_inject(struct smr_cmd *cmd, struct iovec *iov,
			       size_t iov_count, size_t *total_len,
			       struct smr_ep *ep, int err)
{
	struct smr_inject_buf *tx_buf;
	size_t inj_offset;

	inj_offset = (size_t) cmd->msg.hdr.src_data;
	tx_buf = (struct smr_inject_buf *) smr_get_addr(ep->region, inj_offset);

	if (err) {
		smr_freestack_push(smr_inject_pool(ep->region), tx_buf);
		return err;
	}

	if (cmd->msg.hdr.op == ofi_op_read_req) {
		*total_len = ofi_copy_from_iov(tx_buf->data, cmd->msg.hdr.size,
					       iov, iov_count, 0);
	} else {
		*total_len = ofi_copy_to_iov(iov, iov_count, 0, tx_buf->data,
					     cmd->msg.hdr.size);
		smr_freestack_push(smr_inject_pool(ep->region), tx_buf);
	}

	if (*total_len != cmd->msg.hdr.size) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"recv truncated");
		return -FI_EIO;
	}

	return FI_SUCCESS;
}

static int smr_progress_iov(struct smr_cmd *cmd, struct iovec *iov,
			    size_t iov_count, size_t *total_len,
			    struct smr_ep *ep, int err)
{
	struct smr_region *peer_smr;
	struct smr_resp *resp;
	int peer_id, ret;

	peer_id = (int) cmd->msg.hdr.addr;
	peer_smr = smr_peer_region(ep->region, peer_id);
	resp = (struct smr_resp *) smr_get_addr(peer_smr, cmd->msg.hdr.src_data);

	if (err) {
		ret = -err;
		goto out;
	}

	if (cmd->msg.hdr.op == ofi_op_read_req) {
		ret = ofi_process_vm_writev(peer_smr->pid, iov, iov_count,
					    cmd->msg.data.iov,
					    cmd->msg.data.iov_count, 0);
	} else {
		ret = ofi_process_vm_readv(peer_smr->pid, iov, iov_count,
					   cmd->msg.data.iov,
					   cmd->msg.data.iov_count, 0);
	}

	if (ret != cmd->msg.hdr.size) {
		if (ret < 0) {
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"CMA write error\n");
			ret = errno;
		} else { 
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"partial read occurred\n");
			ret = FI_EIO;
		}
	} else {
		*total_len = ret;
		ret = 0;
	}

out:
	//Status must be set last (signals peer: op done, valid resp entry)
	resp->status = ret;

	return -ret;
}

static int smr_mmap_peer_copy(struct smr_ep *ep, struct smr_cmd *cmd,
				 struct iovec *iov, size_t iov_count,
				 size_t *total_len)
{
	char shm_name[NAME_MAX];
	void *mapped_ptr;
	int peer_id, fd, num;
	int ret = 0;

	peer_id = (int) cmd->msg.hdr.addr;

	num = smr_mmap_name(shm_name, ep->region->map->peers[peer_id].peer.name,
			    cmd->msg.hdr.msg_id);
	if (num < 0) {
		FI_WARN(&smr_prov, FI_LOG_AV, "generating shm file name failed\n");
		return -errno;
	}

	fd = shm_open(shm_name, O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		FI_WARN(&smr_prov, FI_LOG_AV, "shm_open error\n");
		return -errno;
	}

	mapped_ptr = mmap(NULL, cmd->msg.hdr.size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd, 0);
	if (mapped_ptr == MAP_FAILED) {
		FI_WARN(&smr_prov, FI_LOG_AV, "mmap error %s\n", strerror(errno));
		ret = -errno;
		goto unlink_close;
	}

	if (cmd->msg.hdr.op == ofi_op_read_req) {
		*total_len = ofi_total_iov_len(iov, iov_count);
		if (ofi_copy_from_iov(mapped_ptr, *total_len, iov, iov_count, 0)
		    != *total_len) {
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"mmap iov copy in error\n");
			ret = -FI_EIO;
			goto munmap;
		}
	} else {
		*total_len = ofi_copy_to_iov(iov, iov_count, 0, mapped_ptr,
				      cmd->msg.hdr.size);
		if (*total_len != cmd->msg.hdr.size) {
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"mmap iov copy out error\n");
			ret = -FI_EIO;
			goto munmap;
		}
	}

munmap:
	munmap(mapped_ptr, cmd->msg.hdr.size);
unlink_close:
	shm_unlink(shm_name);
	close(fd);
	return ret;
}

static int smr_progress_mmap(struct smr_cmd *cmd, struct iovec *iov,
			     size_t iov_count, size_t *total_len,
			     struct smr_ep *ep)
{
	struct smr_region *peer_smr;
	struct smr_resp *resp;
	int peer_id, ret;

	peer_id = (int) cmd->msg.hdr.addr;
	peer_smr = smr_peer_region(ep->region, peer_id);
	resp = (struct smr_resp *) smr_get_addr(peer_smr, cmd->msg.hdr.src_data);

	ret = smr_mmap_peer_copy(ep, cmd, iov, iov_count, total_len);

	//Status must be set last (signals peer: op done, valid resp entry)
	resp->status = ret;

	return ret;
}

static bool smr_progress_multi_recv(struct smr_ep *ep,
				    struct smr_rx_entry *entry, size_t len)
{
	size_t left;
	void *new_base;

	left = entry->iov[0].iov_len - len;
	if (left < ep->min_multi_recv_size)
		return true;

	new_base = (void *) ((uintptr_t) entry->iov[0].iov_base + len);
	entry->iov[0].iov_len = left;
	entry->iov[0].iov_base = new_base;

	return false;
}

static void smr_do_atomic(void *src, void *dst, void *cmp, enum fi_datatype datatype,
			  enum fi_op op, size_t cnt, uint16_t flags)
{
	char tmp_result[SMR_INJECT_SIZE];

	if (op >= OFI_SWAP_OP_START) {
		ofi_atomic_swap_handlers[op - OFI_SWAP_OP_START][datatype](dst,
			src, cmp, tmp_result, cnt);
	} else if (flags & SMR_RMA_REQ) {
		ofi_atomic_readwrite_handlers[op][datatype](dst, src,
			tmp_result, cnt);
	} else if (op != FI_ATOMIC_READ) {
		ofi_atomic_write_handlers[op][datatype](dst, src, cnt);
	}

	if (flags & SMR_RMA_REQ)
		memcpy(src, op == FI_ATOMIC_READ ? dst : tmp_result,
		       cnt * ofi_datatype_size(datatype));
}

static int smr_progress_inline_atomic(struct smr_cmd *cmd, struct fi_ioc *ioc,
			       size_t ioc_count, size_t *len)
{
	int i;
	uint8_t *src, *comp;

	switch (cmd->msg.hdr.op) {
	case ofi_op_atomic_compare:
		src = cmd->msg.data.buf;
		comp = cmd->msg.data.comp;
		break;
	default:
		src = cmd->msg.data.msg;
		comp = NULL;
		break;
	}

	for (i = *len = 0; i < ioc_count && *len < cmd->msg.hdr.size; i++) {
		smr_do_atomic(&src[*len], ioc[i].addr, comp ? &comp[*len] : NULL,
			      cmd->msg.hdr.datatype, cmd->msg.hdr.atomic_op,
			      ioc[i].count, cmd->msg.hdr.op_flags);
		*len += ioc[i].count * ofi_datatype_size(cmd->msg.hdr.datatype);
	}

	if (*len != cmd->msg.hdr.size) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"recv truncated");
		return -FI_EIO;
	}
	return 0;
}

static int smr_progress_inject_atomic(struct smr_cmd *cmd, struct fi_ioc *ioc,
			       size_t ioc_count, size_t *len,
			       struct smr_ep *ep, int err)
{
	struct smr_inject_buf *tx_buf;
	size_t inj_offset;
	uint8_t *src, *comp;
	int i;

	inj_offset = (size_t) cmd->msg.hdr.src_data;
	tx_buf = (struct smr_inject_buf *) smr_get_addr(ep->region, inj_offset);
	if (err)
		goto out;

	switch (cmd->msg.hdr.op) {
	case ofi_op_atomic_compare:
		src = tx_buf->buf;
		comp = tx_buf->comp;
		break;
	default:
		src = tx_buf->data;
		comp = NULL;
		break;
	}

	for (i = *len = 0; i < ioc_count && *len < cmd->msg.hdr.size; i++) {
		smr_do_atomic(&src[*len], ioc[i].addr, comp ? &comp[*len] : NULL,
			      cmd->msg.hdr.datatype, cmd->msg.hdr.atomic_op,
			      ioc[i].count, cmd->msg.hdr.op_flags);
		*len += ioc[i].count * ofi_datatype_size(cmd->msg.hdr.datatype);
	}

	if (*len != cmd->msg.hdr.size) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"recv truncated");
		err = -FI_EIO;
	}

out:
	if (!(cmd->msg.hdr.op_flags & SMR_RMA_REQ))
		smr_freestack_push(smr_inject_pool(ep->region), tx_buf);

	return err;
}

static int smr_progress_msg_common(struct smr_ep *ep, struct smr_cmd *cmd,
				   struct smr_rx_entry *entry)
{
	size_t total_len = 0;
	uint16_t comp_flags;
	void *comp_buf;
	int ret;
	bool free_entry = true;

	switch (cmd->msg.hdr.op_src) {
	case smr_src_inline:
		entry->err = smr_progress_inline(cmd, entry->iov, entry->iov_count,
						 &total_len);
		ep->region->cmd_cnt++;
		break;
	case smr_src_inject:
		entry->err = smr_progress_inject(cmd, entry->iov, entry->iov_count,
						 &total_len, ep, 0);
		ep->region->cmd_cnt++;
		break;
	case smr_src_iov:
		entry->err = smr_progress_iov(cmd, entry->iov, entry->iov_count,
					      &total_len, ep, 0);
		break;
	case smr_src_mmap:
		entry->err = smr_progress_mmap(cmd, entry->iov, entry->iov_count,
					       &total_len, ep);
		break;
	default:
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"unidentified operation type\n");
		entry->err = -FI_EINVAL;
	}

	comp_buf = entry->iov[0].iov_base;
	comp_flags = (cmd->msg.hdr.op_flags | entry->flags) & ~SMR_MULTI_RECV;

	if (entry->flags & SMR_MULTI_RECV) {
		free_entry = smr_progress_multi_recv(ep, entry, total_len);
		if (free_entry)
			comp_flags |= SMR_MULTI_RECV;
	}

	ret = smr_complete_rx(ep, entry->context, cmd->msg.hdr.op,
			comp_flags, total_len, comp_buf, cmd->msg.hdr.addr,
			cmd->msg.hdr.tag, cmd->msg.hdr.data, entry->err);
	if (ret) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"unable to process rx completion\n");
	}

	if (free_entry) {
		dlist_remove(&entry->entry);
		freestack_push(ep->recv_fs, entry);
		return 1;
	}
	return 0;
}

static int smr_progress_cmd_msg(struct smr_ep *ep, struct smr_cmd *cmd)
{
	struct smr_queue *recv_queue;
	struct smr_match_attr match_attr;
	struct dlist_entry *dlist_entry;
	struct smr_unexp_msg *unexp;
	int ret;

	if (ofi_cirque_isfull(ep->util_ep.rx_cq->cirq)) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"rx cq full\n");
		return -FI_ENOSPC;
	}

	recv_queue = (cmd->msg.hdr.op == ofi_op_tagged) ?
		      &ep->trecv_queue : &ep->recv_queue;

	match_attr.addr = cmd->msg.hdr.addr;
	match_attr.tag = cmd->msg.hdr.tag;

	dlist_entry = dlist_find_first_match(&recv_queue->list,
					     recv_queue->match_func,
					     &match_attr);
	if (!dlist_entry) {
		if (freestack_isempty(ep->unexp_fs))
			return -FI_EAGAIN;
		unexp = freestack_pop(ep->unexp_fs);
		memcpy(&unexp->cmd, cmd, sizeof(*cmd));
		ofi_cirque_discard(smr_cmd_queue(ep->region));
		if (cmd->msg.hdr.op == ofi_op_msg) {
			dlist_insert_tail(&unexp->entry, &ep->unexp_msg_queue.list);
		} else {
			assert(cmd->msg.hdr.op == ofi_op_tagged);
			dlist_insert_tail(&unexp->entry, &ep->unexp_tagged_queue.list);
		}
		return 0;
	}
	ret = smr_progress_msg_common(ep, cmd,
			container_of(dlist_entry, struct smr_rx_entry, entry));
	ofi_cirque_discard(smr_cmd_queue(ep->region));
	return ret < 0 ? ret : 0;
}

static int smr_progress_cmd_rma(struct smr_ep *ep, struct smr_cmd *cmd)
{
	struct smr_region *peer_smr;
	struct smr_domain *domain;
	struct smr_cmd *rma_cmd;
	struct smr_resp *resp;
	struct iovec iov[SMR_IOV_LIMIT];
	size_t iov_count;
	size_t total_len = 0;
	int err, ret = 0;

	domain = container_of(ep->util_ep.domain, struct smr_domain,
			      util_domain);

	if (cmd->msg.hdr.op_flags & SMR_REMOTE_CQ_DATA &&
	    ofi_cirque_isfull(ep->util_ep.rx_cq->cirq)) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"rx cq full\n");
		return -FI_ENOSPC;
	}

	ofi_cirque_discard(smr_cmd_queue(ep->region));
	ep->region->cmd_cnt++;
	rma_cmd = ofi_cirque_head(smr_cmd_queue(ep->region));

	for (iov_count = 0; iov_count < rma_cmd->rma.rma_count; iov_count++) {
		ret = ofi_mr_verify(&domain->util_domain.mr_map,
				rma_cmd->rma.rma_iov[iov_count].len,
				(uintptr_t *) &(rma_cmd->rma.rma_iov[iov_count].addr),
				rma_cmd->rma.rma_iov[iov_count].key,
				ofi_rx_mr_reg_flags(cmd->msg.hdr.op, 0));
		if (ret)
			break;

		iov[iov_count].iov_base = (void *) rma_cmd->rma.rma_iov[iov_count].addr;
		iov[iov_count].iov_len = rma_cmd->rma.rma_iov[iov_count].len;
	}
	ofi_cirque_discard(smr_cmd_queue(ep->region));
	if (ret) {
		ep->region->cmd_cnt++;
		return ret;
	}

	switch (cmd->msg.hdr.op_src) {
	case smr_src_inline:
		err = smr_progress_inline(cmd, iov, iov_count, &total_len);
		break;
	case smr_src_inject:
		err = smr_progress_inject(cmd, iov, iov_count, &total_len, ep, ret);
		break;
	case smr_src_iov:
		err = smr_progress_iov(cmd, iov, iov_count, &total_len, ep, ret);
		break;
	case smr_src_mmap:
		err = smr_progress_mmap(cmd, iov, iov_count, &total_len, ep);
		break;
	default:
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"unidentified operation type\n");
		err = -FI_EINVAL;
	}

	if (cmd->msg.hdr.op == ofi_op_read_req && cmd->msg.hdr.data) {
		peer_smr = smr_peer_region(ep->region, cmd->msg.hdr.addr);
		resp = (struct smr_resp *) smr_get_addr(peer_smr, cmd->msg.hdr.data);
		resp->status = -err;
	} else {
		ep->region->cmd_cnt++;
	}

	ret = smr_complete_rx(ep, (void *) cmd->msg.hdr.msg_id,
			cmd->msg.hdr.op, cmd->msg.hdr.op_flags,
			total_len, iov_count ? iov[0].iov_base : NULL,
			cmd->msg.hdr.addr, 0, cmd->msg.hdr.data, err);
	if (ret) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"unable to process rx completion\n");
	}

	return ret;
}

static int smr_progress_cmd_atomic(struct smr_ep *ep, struct smr_cmd *cmd)
{
	struct smr_region *peer_smr;
	struct smr_domain *domain;
	struct smr_cmd *rma_cmd;
	struct smr_resp *resp;
	struct fi_ioc ioc[SMR_IOV_LIMIT];
	size_t ioc_count;
	size_t total_len = 0;
	int err, ret = 0;

	domain = container_of(ep->util_ep.domain, struct smr_domain,
			      util_domain);

	ofi_cirque_discard(smr_cmd_queue(ep->region));
	ep->region->cmd_cnt++;
	rma_cmd = ofi_cirque_head(smr_cmd_queue(ep->region));

	for (ioc_count = 0; ioc_count < rma_cmd->rma.rma_count; ioc_count++) {
		ret = ofi_mr_verify(&domain->util_domain.mr_map,
				rma_cmd->rma.rma_ioc[ioc_count].count *
				ofi_datatype_size(cmd->msg.hdr.datatype),
				(uintptr_t *) &(rma_cmd->rma.rma_ioc[ioc_count].addr),
				rma_cmd->rma.rma_ioc[ioc_count].key,
				ofi_rx_mr_reg_flags(cmd->msg.hdr.op,
				cmd->msg.hdr.atomic_op));
		if (ret)
			break;

		ioc[ioc_count].addr = (void *) rma_cmd->rma.rma_ioc[ioc_count].addr;
		ioc[ioc_count].count = rma_cmd->rma.rma_ioc[ioc_count].count;
	}
	ofi_cirque_discard(smr_cmd_queue(ep->region));
	if (ret) {
		ep->region->cmd_cnt++;
		return ret;
	}

	switch (cmd->msg.hdr.op_src) {
	case smr_src_inline:
		err = smr_progress_inline_atomic(cmd, ioc, ioc_count, &total_len);
		break;
	case smr_src_inject:
		err = smr_progress_inject_atomic(cmd, ioc, ioc_count, &total_len, ep, ret);
		break;
	default:
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"unidentified operation type\n");
		err = -FI_EINVAL;
	}
	if (cmd->msg.hdr.data) {
		peer_smr = smr_peer_region(ep->region, cmd->msg.hdr.addr);
		resp = (struct smr_resp *) smr_get_addr(peer_smr, cmd->msg.hdr.data);
		resp->status = -err;
	} else {
		ep->region->cmd_cnt++;
	}

	if (err)
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"error processing atomic op\n");

	ret = smr_complete_rx(ep, NULL, cmd->msg.hdr.op, cmd->msg.hdr.op_flags,
			      total_len, ioc_count ? ioc[0].addr : NULL,
			      cmd->msg.hdr.addr, 0, cmd->msg.hdr.data, err);
	if (ret)
		return ret;

	return err; 
}

static void smr_progress_cmd(struct smr_ep *ep)
{
	struct smr_cmd *cmd;
	int ret = 0;

	fastlock_acquire(&ep->region->lock);
	fastlock_acquire(&ep->util_ep.rx_cq->cq_lock);

	while (!ofi_cirque_isempty(smr_cmd_queue(ep->region))) {
		cmd = ofi_cirque_head(smr_cmd_queue(ep->region));

		switch (cmd->msg.hdr.op) {
		case ofi_op_msg:
		case ofi_op_tagged:
			ret = smr_progress_cmd_msg(ep, cmd);
			break;
		case ofi_op_write:
		case ofi_op_read_req:
			ret = smr_progress_cmd_rma(ep, cmd);
			break;
		case ofi_op_write_async:
		case ofi_op_read_async:
			ofi_ep_rx_cntr_inc_func(&ep->util_ep, cmd->msg.hdr.op);
			ofi_cirque_discard(smr_cmd_queue(ep->region));
			ep->region->cmd_cnt++;
			break;
		case ofi_op_atomic:
		case ofi_op_atomic_fetch:
		case ofi_op_atomic_compare:
			ret = smr_progress_cmd_atomic(ep, cmd);
			break;
		default:
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"unidentified operation type\n");
			ret = -FI_EINVAL;
		}

		if (ret) {
			if (ret != -FI_EAGAIN) {
				FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
					"error processing command\n");
			}
			break;
		}
	}
	fastlock_release(&ep->util_ep.rx_cq->cq_lock);
	fastlock_release(&ep->region->lock);
}

void smr_ep_progress(struct util_ep *util_ep)
{
	struct smr_ep *ep;

	ep = container_of(util_ep, struct smr_ep, util_ep);

	smr_progress_resp(ep);
	smr_progress_cmd(ep);
}

int smr_progress_unexp_queue(struct smr_ep *ep, struct smr_rx_entry *entry,
			     struct smr_queue *unexp_queue)
{
	struct smr_match_attr match_attr;
	struct smr_unexp_msg *unexp_msg;
	struct dlist_entry *dlist_entry;
	int multi_recv;
	int ret;

	match_attr.addr = entry->addr;
	match_attr.ignore = entry->ignore;
	match_attr.tag = entry->tag;

	dlist_entry = dlist_remove_first_match(&unexp_queue->list,
					       unexp_queue->match_func,
					       &match_attr);
	if (!dlist_entry)
		return 0;

	multi_recv = entry->flags & SMR_MULTI_RECV;
	while (dlist_entry) {
		unexp_msg = container_of(dlist_entry, struct smr_unexp_msg, entry);
		ret = smr_progress_msg_common(ep, &unexp_msg->cmd, entry);
		freestack_push(ep->unexp_fs, unexp_msg);
		if (!multi_recv || ret)
			break;

		dlist_entry = dlist_remove_first_match(&unexp_queue->list,
						       unexp_queue->match_func,
						       &match_attr);
	}

	return ret < 0 ? ret : 0;
}
