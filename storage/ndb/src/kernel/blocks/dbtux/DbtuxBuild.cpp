/*
   Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "Dbtux.hpp"

struct mt_BuildIndxCtx
{
  Uint32 indexId;
  Uint32 tableId;
  Uint32 fragId;
  Dbtup* tup_ptr;
  Dbtux::TuxCtx * tux_ctx_ptr;
  NdbMutex * alloc_mutex_ptr;
};

Uint32
Dbtux_mt_buildIndexFragment_wrapper_C(void * obj)
{
  return Dbtux::mt_buildIndexFragment_wrapper(obj);
}

Uint32
Dbtux::mt_buildIndexFragment_wrapper(void * obj)
{
  mt_BuildIndxReq* req = reinterpret_cast<mt_BuildIndxReq*>(obj);
  TuxCtx * tux_ctx = reinterpret_cast<TuxCtx*>(req->mem_buffer);
  {
    /**
     * Setup ctx object...
     */
    Uint32 * ptr = reinterpret_cast<Uint32*>(req->mem_buffer);
    ptr += (sizeof(* tux_ctx) + 3) / 4;

    tux_ctx->jamBuffer = (EmulatedJamBuffer*)ptr;
    tux_ctx->jamBuffer->theEmulatedJamIndex = 0;
    ptr += (sizeof(EmulatedJamBuffer) + 3) / 4;
    tux_ctx->c_searchKey = ptr;
    ptr += MaxAttrDataSize;
    tux_ctx->c_entryKey = ptr;
    ptr += MaxAttrDataSize;
    tux_ctx->c_dataBuffer = ptr;
    ptr += MaxAttrDataSize;
    if (!(UintPtr(ptr) - UintPtr(req->mem_buffer) <= req->buffer_size))
      abort();
  }

  mt_BuildIndxCtx ctx;
  ctx.indexId = req->indexId;
  ctx.tableId = req->tableId;
  ctx.fragId = req->fragId;
  ctx.tux_ctx_ptr = tux_ctx;
  ctx.tup_ptr = reinterpret_cast<Dbtup*>(req->tup_ptr);

  Dbtux* tux = reinterpret_cast<Dbtux*>(req->tux_ptr);
  return tux->mt_buildIndexFragment(&ctx);
}

Uint32 // error code
Dbtux::mt_buildIndexFragment(mt_BuildIndxCtx* req)
{
  IndexPtr indexPtr;
  c_indexPool.getPtr(indexPtr, req->indexId);
  ndbrequire(indexPtr.p->m_tableId == req->tableId);
  // get base fragment id and extra bits
  const Uint32 fragId = req->fragId;
  // get the fragment
  FragPtr fragPtr;
  findFrag(*indexPtr.p, fragId, fragPtr);
  ndbrequire(fragPtr.i != RNIL);
  Frag& frag = *fragPtr.p;

  TuxCtx & ctx = * (TuxCtx*)req->tux_ctx_ptr;

  Local_key pos;
  Uint32 fragPtrI;
  int err = req->tup_ptr->mt_scan_init(req->tableId, req->fragId,
                                       &pos, &fragPtrI);
  bool moveNext = false;
  while (globalData.theRestartFlag != perform_stop &&
         err == 0 &&
         (err = req->tup_ptr->mt_scan_next(req->tableId,
                                           fragPtrI, &pos, moveNext)) == 0)
  {
    moveNext = true;

    // set up search entry
    TreeEnt ent;
    ent.m_tupLoc = TupLoc(pos.m_page_no, pos.m_page_idx);
    ent.m_tupVersion = pos.m_file_no; // used for version

    // set up and read search key
    KeyData searchKey(indexPtr.p->m_keySpec, false, 0);
    searchKey.set_buf(ctx.c_searchKey, MaxAttrDataSize << 2);
    readKeyAttrs(ctx, frag, ent, searchKey, indexPtr.p->m_numAttrs);

    if (unlikely(! indexPtr.p->m_storeNullKey) &&
        searchKey.get_null_cnt() == indexPtr.p->m_numAttrs) {
      jam();
      continue;
    }

    TreePos treePos;
    bool ok = searchToAdd(ctx, frag, searchKey, ent, treePos);
    ndbrequire(ok);

    /*
     * At most one new node is inserted in the operation.  Pre-allocate
     * it so that the operation cannot fail.
     */
    if (frag.m_freeLoc == NullTupLoc)
    {
      jam();
      NodeHandle node(frag);
      err = -(int)allocNode(ctx, node);

      if (err != 0)
      {
        break;
      }
      frag.m_freeLoc = node.m_loc;
      ndbrequire(frag.m_freeLoc != NullTupLoc);
    }
    treeAdd(ctx, frag, treePos, ent);
  }

  if (err < 0)
  {
    return -err;
  }

  return 0;
};
