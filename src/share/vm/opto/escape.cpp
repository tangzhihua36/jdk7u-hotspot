/*
 * Copyright 2005-2006 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 */

#include "incls/_precompiled.incl"
#include "incls/_escape.cpp.incl"

uint PointsToNode::edge_target(uint e) const {
  assert(_edges != NULL && e < (uint)_edges->length(), "valid edge index");
  return (_edges->at(e) >> EdgeShift);
}

PointsToNode::EdgeType PointsToNode::edge_type(uint e) const {
  assert(_edges != NULL && e < (uint)_edges->length(), "valid edge index");
  return (EdgeType) (_edges->at(e) & EdgeMask);
}

void PointsToNode::add_edge(uint targIdx, PointsToNode::EdgeType et) {
  uint v = (targIdx << EdgeShift) + ((uint) et);
  if (_edges == NULL) {
     Arena *a = Compile::current()->comp_arena();
    _edges = new(a) GrowableArray<uint>(a, INITIAL_EDGE_COUNT, 0, 0);
  }
  _edges->append_if_missing(v);
}

void PointsToNode::remove_edge(uint targIdx, PointsToNode::EdgeType et) {
  uint v = (targIdx << EdgeShift) + ((uint) et);

  _edges->remove(v);
}

#ifndef PRODUCT
static const char *node_type_names[] = {
  "UnknownType",
  "JavaObject",
  "LocalVar",
  "Field"
};

static const char *esc_names[] = {
  "UnknownEscape",
  "NoEscape",
  "ArgEscape",
  "GlobalEscape"
};

static const char *edge_type_suffix[] = {
 "?", // UnknownEdge
 "P", // PointsToEdge
 "D", // DeferredEdge
 "F"  // FieldEdge
};

void PointsToNode::dump() const {
  NodeType nt = node_type();
  EscapeState es = escape_state();
  tty->print("%s %s %s [[", node_type_names[(int) nt], esc_names[(int) es], _scalar_replaceable ? "" : "NSR");
  for (uint i = 0; i < edge_count(); i++) {
    tty->print(" %d%s", edge_target(i), edge_type_suffix[(int) edge_type(i)]);
  }
  tty->print("]]  ");
  if (_node == NULL)
    tty->print_cr("<null>");
  else
    _node->dump();
}
#endif

ConnectionGraph::ConnectionGraph(Compile * C) : _processed(C->comp_arena()), _node_map(C->comp_arena()) {
  _collecting = true;
  this->_compile = C;
  const PointsToNode &dummy = PointsToNode();
  int sz = C->unique();
  _nodes = new(C->comp_arena()) GrowableArray<PointsToNode>(C->comp_arena(), sz, sz, dummy);
  _phantom_object = C->top()->_idx;
  PointsToNode *phn = ptnode_adr(_phantom_object);
  phn->_node = C->top();
  phn->set_node_type(PointsToNode::JavaObject);
  phn->set_escape_state(PointsToNode::GlobalEscape);
}

void ConnectionGraph::add_pointsto_edge(uint from_i, uint to_i) {
  PointsToNode *f = ptnode_adr(from_i);
  PointsToNode *t = ptnode_adr(to_i);

  assert(f->node_type() != PointsToNode::UnknownType && t->node_type() != PointsToNode::UnknownType, "node types must be set");
  assert(f->node_type() == PointsToNode::LocalVar || f->node_type() == PointsToNode::Field, "invalid source of PointsTo edge");
  assert(t->node_type() == PointsToNode::JavaObject, "invalid destination of PointsTo edge");
  f->add_edge(to_i, PointsToNode::PointsToEdge);
}

void ConnectionGraph::add_deferred_edge(uint from_i, uint to_i) {
  PointsToNode *f = ptnode_adr(from_i);
  PointsToNode *t = ptnode_adr(to_i);

  assert(f->node_type() != PointsToNode::UnknownType && t->node_type() != PointsToNode::UnknownType, "node types must be set");
  assert(f->node_type() == PointsToNode::LocalVar || f->node_type() == PointsToNode::Field, "invalid source of Deferred edge");
  assert(t->node_type() == PointsToNode::LocalVar || t->node_type() == PointsToNode::Field, "invalid destination of Deferred edge");
  // don't add a self-referential edge, this can occur during removal of
  // deferred edges
  if (from_i != to_i)
    f->add_edge(to_i, PointsToNode::DeferredEdge);
}

int ConnectionGraph::address_offset(Node* adr, PhaseTransform *phase) {
  const Type *adr_type = phase->type(adr);
  if (adr->is_AddP() && adr_type->isa_oopptr() == NULL &&
      adr->in(AddPNode::Address)->is_Proj() &&
      adr->in(AddPNode::Address)->in(0)->is_Allocate()) {
    // We are computing a raw address for a store captured by an Initialize
    // compute an appropriate address type. AddP cases #3 and #5 (see below).
    int offs = (int)phase->find_intptr_t_con(adr->in(AddPNode::Offset), Type::OffsetBot);
    assert(offs != Type::OffsetBot ||
           adr->in(AddPNode::Address)->in(0)->is_AllocateArray(),
           "offset must be a constant or it is initialization of array");
    return offs;
  }
  const TypePtr *t_ptr = adr_type->isa_ptr();
  assert(t_ptr != NULL, "must be a pointer type");
  return t_ptr->offset();
}

void ConnectionGraph::add_field_edge(uint from_i, uint to_i, int offset) {
  PointsToNode *f = ptnode_adr(from_i);
  PointsToNode *t = ptnode_adr(to_i);

  assert(f->node_type() != PointsToNode::UnknownType && t->node_type() != PointsToNode::UnknownType, "node types must be set");
  assert(f->node_type() == PointsToNode::JavaObject, "invalid destination of Field edge");
  assert(t->node_type() == PointsToNode::Field, "invalid destination of Field edge");
  assert (t->offset() == -1 || t->offset() == offset, "conflicting field offsets");
  t->set_offset(offset);

  f->add_edge(to_i, PointsToNode::FieldEdge);
}

void ConnectionGraph::set_escape_state(uint ni, PointsToNode::EscapeState es) {
  PointsToNode *npt = ptnode_adr(ni);
  PointsToNode::EscapeState old_es = npt->escape_state();
  if (es > old_es)
    npt->set_escape_state(es);
}

void ConnectionGraph::add_node(Node *n, PointsToNode::NodeType nt,
                               PointsToNode::EscapeState es, bool done) {
  PointsToNode* ptadr = ptnode_adr(n->_idx);
  ptadr->_node = n;
  ptadr->set_node_type(nt);

  // inline set_escape_state(idx, es);
  PointsToNode::EscapeState old_es = ptadr->escape_state();
  if (es > old_es)
    ptadr->set_escape_state(es);

  if (done)
    _processed.set(n->_idx);
}

PointsToNode::EscapeState ConnectionGraph::escape_state(Node *n, PhaseTransform *phase) {
  uint idx = n->_idx;
  PointsToNode::EscapeState es;

  // If we are still collecting or there were no non-escaping allocations
  // we don't know the answer yet
  if (_collecting || !_has_allocations)
    return PointsToNode::UnknownEscape;

  // if the node was created after the escape computation, return
  // UnknownEscape
  if (idx >= (uint)_nodes->length())
    return PointsToNode::UnknownEscape;

  es = _nodes->at_grow(idx).escape_state();

  // if we have already computed a value, return it
  if (es != PointsToNode::UnknownEscape)
    return es;

  // compute max escape state of anything this node could point to
  VectorSet ptset(Thread::current()->resource_area());
  PointsTo(ptset, n, phase);
  for(VectorSetI i(&ptset); i.test() && es != PointsToNode::GlobalEscape; ++i) {
    uint pt = i.elem;
    PointsToNode::EscapeState pes = _nodes->adr_at(pt)->escape_state();
    if (pes > es)
      es = pes;
  }
  // cache the computed escape state
  assert(es != PointsToNode::UnknownEscape, "should have computed an escape state");
  _nodes->adr_at(idx)->set_escape_state(es);
  return es;
}

void ConnectionGraph::PointsTo(VectorSet &ptset, Node * n, PhaseTransform *phase) {
  VectorSet visited(Thread::current()->resource_area());
  GrowableArray<uint>  worklist;

  n = n->uncast();
  PointsToNode  npt = _nodes->at_grow(n->_idx);

  // If we have a JavaObject, return just that object
  if (npt.node_type() == PointsToNode::JavaObject) {
    ptset.set(n->_idx);
    return;
  }
  assert(npt._node != NULL, "unregistered node");

  worklist.push(n->_idx);
  while(worklist.length() > 0) {
    int ni = worklist.pop();
    PointsToNode pn = _nodes->at_grow(ni);
    if (!visited.test_set(ni)) {
      // ensure that all inputs of a Phi have been processed
      assert(!_collecting || !pn._node->is_Phi() || _processed.test(ni),"");

      int edges_processed = 0;
      for (uint e = 0; e < pn.edge_count(); e++) {
        uint etgt = pn.edge_target(e);
        PointsToNode::EdgeType et = pn.edge_type(e);
        if (et == PointsToNode::PointsToEdge) {
          ptset.set(etgt);
          edges_processed++;
        } else if (et == PointsToNode::DeferredEdge) {
          worklist.push(etgt);
          edges_processed++;
        } else {
          assert(false,"neither PointsToEdge or DeferredEdge");
        }
      }
      if (edges_processed == 0) {
        // no deferred or pointsto edges found.  Assume the value was set
        // outside this method.  Add the phantom object to the pointsto set.
        ptset.set(_phantom_object);
      }
    }
  }
}

void ConnectionGraph::remove_deferred(uint ni) {
  VectorSet visited(Thread::current()->resource_area());

  uint i = 0;
  PointsToNode *ptn = ptnode_adr(ni);

  while(i < ptn->edge_count()) {
    uint t = ptn->edge_target(i);
    PointsToNode *ptt = ptnode_adr(t);
    if (ptn->edge_type(i) != PointsToNode::DeferredEdge) {
      i++;
    } else {
      ptn->remove_edge(t, PointsToNode::DeferredEdge);
      if(!visited.test_set(t)) {
        for (uint j = 0; j < ptt->edge_count(); j++) {
          uint n1 = ptt->edge_target(j);
          PointsToNode *pt1 = ptnode_adr(n1);
          switch(ptt->edge_type(j)) {
            case PointsToNode::PointsToEdge:
              add_pointsto_edge(ni, n1);
              if(n1 == _phantom_object) {
                // Special case - field set outside (globally escaping).
                ptn->set_escape_state(PointsToNode::GlobalEscape);
              }
              break;
            case PointsToNode::DeferredEdge:
              add_deferred_edge(ni, n1);
              break;
            case PointsToNode::FieldEdge:
              assert(false, "invalid connection graph");
              break;
          }
        }
      }
    }
  }
}


//  Add an edge to node given by "to_i" from any field of adr_i whose offset
//  matches "offset"  A deferred edge is added if to_i is a LocalVar, and
//  a pointsto edge is added if it is a JavaObject

void ConnectionGraph::add_edge_from_fields(uint adr_i, uint to_i, int offs) {
  PointsToNode an = _nodes->at_grow(adr_i);
  PointsToNode to = _nodes->at_grow(to_i);
  bool deferred = (to.node_type() == PointsToNode::LocalVar);

  for (uint fe = 0; fe < an.edge_count(); fe++) {
    assert(an.edge_type(fe) == PointsToNode::FieldEdge, "expecting a field edge");
    int fi = an.edge_target(fe);
    PointsToNode pf = _nodes->at_grow(fi);
    int po = pf.offset();
    if (po == offs || po == Type::OffsetBot || offs == Type::OffsetBot) {
      if (deferred)
        add_deferred_edge(fi, to_i);
      else
        add_pointsto_edge(fi, to_i);
    }
  }
}

// Add a deferred  edge from node given by "from_i" to any field of adr_i
// whose offset matches "offset".
void ConnectionGraph::add_deferred_edge_to_fields(uint from_i, uint adr_i, int offs) {
  PointsToNode an = _nodes->at_grow(adr_i);
  for (uint fe = 0; fe < an.edge_count(); fe++) {
    assert(an.edge_type(fe) == PointsToNode::FieldEdge, "expecting a field edge");
    int fi = an.edge_target(fe);
    PointsToNode pf = _nodes->at_grow(fi);
    int po = pf.offset();
    if (pf.edge_count() == 0) {
      // we have not seen any stores to this field, assume it was set outside this method
      add_pointsto_edge(fi, _phantom_object);
    }
    if (po == offs || po == Type::OffsetBot || offs == Type::OffsetBot) {
      add_deferred_edge(from_i, fi);
    }
  }
}

// Helper functions

static Node* get_addp_base(Node *addp) {
  assert(addp->is_AddP(), "must be AddP");
  //
  // AddP cases for Base and Address inputs:
  // case #1. Direct object's field reference:
  //     Allocate
  //       |
  //     Proj #5 ( oop result )
  //       |
  //     CheckCastPP (cast to instance type)
  //      | |
  //     AddP  ( base == address )
  //
  // case #2. Indirect object's field reference:
  //      Phi
  //       |
  //     CastPP (cast to instance type)
  //      | |
  //     AddP  ( base == address )
  //
  // case #3. Raw object's field reference for Initialize node:
  //      Allocate
  //        |
  //      Proj #5 ( oop result )
  //  top   |
  //     \  |
  //     AddP  ( base == top )
  //
  // case #4. Array's element reference:
  //   {CheckCastPP | CastPP}
  //     |  | |
  //     |  AddP ( array's element offset )
  //     |  |
  //     AddP ( array's offset )
  //
  // case #5. Raw object's field reference for arraycopy stub call:
  //          The inline_native_clone() case when the arraycopy stub is called
  //          after the allocation before Initialize and CheckCastPP nodes.
  //      Allocate
  //        |
  //      Proj #5 ( oop result )
  //       | |
  //       AddP  ( base == address )
  //
  // case #6. Constant Pool, ThreadLocal, CastX2P or
  //          Raw object's field reference:
  //      {ConP, ThreadLocal, CastX2P, raw Load}
  //  top   |
  //     \  |
  //     AddP  ( base == top )
  //
  // case #7. Klass's field reference.
  //      LoadKlass
  //       | |
  //       AddP  ( base == address )
  //
  Node *base = addp->in(AddPNode::Base)->uncast();
  if (base->is_top()) { // The AddP case #3 and #6.
    base = addp->in(AddPNode::Address)->uncast();
    assert(base->Opcode() == Op_ConP || base->Opcode() == Op_ThreadLocal ||
           base->Opcode() == Op_CastX2P ||
           (base->is_Mem() && base->bottom_type() == TypeRawPtr::NOTNULL) ||
           (base->is_Proj() && base->in(0)->is_Allocate()), "sanity");
  }
  return base;
}

static Node* find_second_addp(Node* addp, Node* n) {
  assert(addp->is_AddP() && addp->outcnt() > 0, "Don't process dead nodes");

  Node* addp2 = addp->raw_out(0);
  if (addp->outcnt() == 1 && addp2->is_AddP() &&
      addp2->in(AddPNode::Base) == n &&
      addp2->in(AddPNode::Address) == addp) {

    assert(addp->in(AddPNode::Base) == n, "expecting the same base");
    //
    // Find array's offset to push it on worklist first and
    // as result process an array's element offset first (pushed second)
    // to avoid CastPP for the array's offset.
    // Otherwise the inserted CastPP (LocalVar) will point to what
    // the AddP (Field) points to. Which would be wrong since
    // the algorithm expects the CastPP has the same point as
    // as AddP's base CheckCastPP (LocalVar).
    //
    //    ArrayAllocation
    //     |
    //    CheckCastPP
    //     |
    //    memProj (from ArrayAllocation CheckCastPP)
    //     |  ||
    //     |  ||   Int (element index)
    //     |  ||    |   ConI (log(element size))
    //     |  ||    |   /
    //     |  ||   LShift
    //     |  ||  /
    //     |  AddP (array's element offset)
    //     |  |
    //     |  | ConI (array's offset: #12(32-bits) or #24(64-bits))
    //     | / /
    //     AddP (array's offset)
    //      |
    //     Load/Store (memory operation on array's element)
    //
    return addp2;
  }
  return NULL;
}

//
// Adjust the type and inputs of an AddP which computes the
// address of a field of an instance
//
void ConnectionGraph::split_AddP(Node *addp, Node *base,  PhaseGVN  *igvn) {
  const TypeOopPtr *base_t = igvn->type(base)->isa_oopptr();
  assert(base_t != NULL && base_t->is_instance(), "expecting instance oopptr");
  const TypeOopPtr *t = igvn->type(addp)->isa_oopptr();
  if (t == NULL) {
    // We are computing a raw address for a store captured by an Initialize
    // compute an appropriate address type.
    assert(igvn->type(addp) == TypeRawPtr::NOTNULL, "must be raw pointer");
    assert(addp->in(AddPNode::Address)->is_Proj(), "base of raw address must be result projection from allocation");
    int offs = (int)igvn->find_intptr_t_con(addp->in(AddPNode::Offset), Type::OffsetBot);
    assert(offs != Type::OffsetBot, "offset must be a constant");
    t = base_t->add_offset(offs)->is_oopptr();
  }
  uint inst_id =  base_t->instance_id();
  assert(!t->is_instance() || t->instance_id() == inst_id,
                             "old type must be non-instance or match new type");
  const TypeOopPtr *tinst = base_t->add_offset(t->offset())->is_oopptr();
  // Do NOT remove the next call: ensure an new alias index is allocated
  // for the instance type
  int alias_idx = _compile->get_alias_index(tinst);
  igvn->set_type(addp, tinst);
  // record the allocation in the node map
  set_map(addp->_idx, get_map(base->_idx));
  // if the Address input is not the appropriate instance type
  // (due to intervening casts,) insert a cast
  Node *adr = addp->in(AddPNode::Address);
  const TypeOopPtr  *atype = igvn->type(adr)->isa_oopptr();
  if (atype != NULL && atype->instance_id() != inst_id) {
    assert(!atype->is_instance(), "no conflicting instances");
    const TypeOopPtr *new_atype = base_t->add_offset(atype->offset())->isa_oopptr();
    Node *acast = new (_compile, 2) CastPPNode(adr, new_atype);
    acast->set_req(0, adr->in(0));
    igvn->set_type(acast, new_atype);
    record_for_optimizer(acast);
    Node *bcast = acast;
    Node *abase = addp->in(AddPNode::Base);
    if (abase != adr) {
      bcast = new (_compile, 2) CastPPNode(abase, base_t);
      bcast->set_req(0, abase->in(0));
      igvn->set_type(bcast, base_t);
      record_for_optimizer(bcast);
    }
    igvn->hash_delete(addp);
    addp->set_req(AddPNode::Base, bcast);
    addp->set_req(AddPNode::Address, acast);
    igvn->hash_insert(addp);
  }
  // Put on IGVN worklist since at least addp's type was changed above.
  record_for_optimizer(addp);
}

//
// Create a new version of orig_phi if necessary. Returns either the newly
// created phi or an existing phi.  Sets create_new to indicate wheter  a new
// phi was created.  Cache the last newly created phi in the node map.
//
PhiNode *ConnectionGraph::create_split_phi(PhiNode *orig_phi, int alias_idx, GrowableArray<PhiNode *>  &orig_phi_worklist, PhaseGVN  *igvn, bool &new_created) {
  Compile *C = _compile;
  new_created = false;
  int phi_alias_idx = C->get_alias_index(orig_phi->adr_type());
  // nothing to do if orig_phi is bottom memory or matches alias_idx
  if (phi_alias_idx == alias_idx) {
    return orig_phi;
  }
  // have we already created a Phi for this alias index?
  PhiNode *result = get_map_phi(orig_phi->_idx);
  if (result != NULL && C->get_alias_index(result->adr_type()) == alias_idx) {
    return result;
  }
  if ((int)C->unique() + 2*NodeLimitFudgeFactor > MaxNodeLimit) {
    if (C->do_escape_analysis() == true && !C->failing()) {
      // Retry compilation without escape analysis.
      // If this is the first failure, the sentinel string will "stick"
      // to the Compile object, and the C2Compiler will see it and retry.
      C->record_failure(C2Compiler::retry_no_escape_analysis());
    }
    return NULL;
  }
  orig_phi_worklist.append_if_missing(orig_phi);
  const TypePtr *atype = C->get_adr_type(alias_idx);
  result = PhiNode::make(orig_phi->in(0), NULL, Type::MEMORY, atype);
  set_map_phi(orig_phi->_idx, result);
  igvn->set_type(result, result->bottom_type());
  record_for_optimizer(result);
  new_created = true;
  return result;
}

//
// Return a new version  of Memory Phi "orig_phi" with the inputs having the
// specified alias index.
//
PhiNode *ConnectionGraph::split_memory_phi(PhiNode *orig_phi, int alias_idx, GrowableArray<PhiNode *>  &orig_phi_worklist, PhaseGVN  *igvn) {

  assert(alias_idx != Compile::AliasIdxBot, "can't split out bottom memory");
  Compile *C = _compile;
  bool new_phi_created;
  PhiNode *result = create_split_phi(orig_phi, alias_idx, orig_phi_worklist, igvn, new_phi_created);
  if (!new_phi_created) {
    return result;
  }

  GrowableArray<PhiNode *>  phi_list;
  GrowableArray<uint>  cur_input;

  PhiNode *phi = orig_phi;
  uint idx = 1;
  bool finished = false;
  while(!finished) {
    while (idx < phi->req()) {
      Node *mem = find_inst_mem(phi->in(idx), alias_idx, orig_phi_worklist, igvn);
      if (mem != NULL && mem->is_Phi()) {
        PhiNode *newphi = create_split_phi(mem->as_Phi(), alias_idx, orig_phi_worklist, igvn, new_phi_created);
        if (new_phi_created) {
          // found an phi for which we created a new split, push current one on worklist and begin
          // processing new one
          phi_list.push(phi);
          cur_input.push(idx);
          phi = mem->as_Phi();
          result = newphi;
          idx = 1;
          continue;
        } else {
          mem = newphi;
        }
      }
      if (C->failing()) {
        return NULL;
      }
      result->set_req(idx++, mem);
    }
#ifdef ASSERT
    // verify that the new Phi has an input for each input of the original
    assert( phi->req() == result->req(), "must have same number of inputs.");
    assert( result->in(0) != NULL && result->in(0) == phi->in(0), "regions must match");
#endif
    // Check if all new phi's inputs have specified alias index.
    // Otherwise use old phi.
    for (uint i = 1; i < phi->req(); i++) {
      Node* in = result->in(i);
      assert((phi->in(i) == NULL) == (in == NULL), "inputs must correspond.");
    }
    // we have finished processing a Phi, see if there are any more to do
    finished = (phi_list.length() == 0 );
    if (!finished) {
      phi = phi_list.pop();
      idx = cur_input.pop();
      PhiNode *prev_result = get_map_phi(phi->_idx);
      prev_result->set_req(idx++, result);
      result = prev_result;
    }
  }
  return result;
}


//
// The next methods are derived from methods in MemNode.
//
static Node *step_through_mergemem(MergeMemNode *mmem, int alias_idx, const TypeOopPtr *tinst) {
  Node *mem = mmem;
  // TypeInstPtr::NOTNULL+any is an OOP with unknown offset - generally
  // means an array I have not precisely typed yet.  Do not do any
  // alias stuff with it any time soon.
  if( tinst->base() != Type::AnyPtr &&
      !(tinst->klass()->is_java_lang_Object() &&
        tinst->offset() == Type::OffsetBot) ) {
    mem = mmem->memory_at(alias_idx);
    // Update input if it is progress over what we have now
  }
  return mem;
}

//
// Search memory chain of "mem" to find a MemNode whose address
// is the specified alias index.
//
Node* ConnectionGraph::find_inst_mem(Node *orig_mem, int alias_idx, GrowableArray<PhiNode *>  &orig_phis, PhaseGVN *phase) {
  if (orig_mem == NULL)
    return orig_mem;
  Compile* C = phase->C;
  const TypeOopPtr *tinst = C->get_adr_type(alias_idx)->isa_oopptr();
  bool is_instance = (tinst != NULL) && tinst->is_instance();
  Node *prev = NULL;
  Node *result = orig_mem;
  while (prev != result) {
    prev = result;
    if (result->is_Mem()) {
      MemNode *mem = result->as_Mem();
      const Type *at = phase->type(mem->in(MemNode::Address));
      if (at != Type::TOP) {
        assert (at->isa_ptr() != NULL, "pointer type required.");
        int idx = C->get_alias_index(at->is_ptr());
        if (idx == alias_idx)
          break;
      }
      result = mem->in(MemNode::Memory);
    }
    if (!is_instance)
      continue;  // don't search further for non-instance types
    // skip over a call which does not affect this memory slice
    if (result->is_Proj() && result->as_Proj()->_con == TypeFunc::Memory) {
      Node *proj_in = result->in(0);
      if (proj_in->is_Call()) {
        CallNode *call = proj_in->as_Call();
        if (!call->may_modify(tinst, phase)) {
          result = call->in(TypeFunc::Memory);
        }
      } else if (proj_in->is_Initialize()) {
        AllocateNode* alloc = proj_in->as_Initialize()->allocation();
        // Stop if this is the initialization for the object instance which
        // which contains this memory slice, otherwise skip over it.
        if (alloc == NULL || alloc->_idx != tinst->instance_id()) {
          result = proj_in->in(TypeFunc::Memory);
        }
      } else if (proj_in->is_MemBar()) {
        result = proj_in->in(TypeFunc::Memory);
      }
    } else if (result->is_MergeMem()) {
      MergeMemNode *mmem = result->as_MergeMem();
      result = step_through_mergemem(mmem, alias_idx, tinst);
      if (result == mmem->base_memory()) {
        // Didn't find instance memory, search through general slice recursively.
        result = mmem->memory_at(C->get_general_index(alias_idx));
        result = find_inst_mem(result, alias_idx, orig_phis, phase);
        if (C->failing()) {
          return NULL;
        }
        mmem->set_memory_at(alias_idx, result);
      }
    } else if (result->is_Phi() &&
               C->get_alias_index(result->as_Phi()->adr_type()) != alias_idx) {
      Node *un = result->as_Phi()->unique_input(phase);
      if (un != NULL) {
        result = un;
      } else {
        break;
      }
    }
  }
  if (is_instance && result->is_Phi()) {
    PhiNode *mphi = result->as_Phi();
    assert(mphi->bottom_type() == Type::MEMORY, "memory phi required");
    const TypePtr *t = mphi->adr_type();
    if (C->get_alias_index(t) != alias_idx) {
      result = split_memory_phi(mphi, alias_idx, orig_phis, phase);
    }
  }
  // the result is either MemNode, PhiNode, InitializeNode.
  return result;
}


//
//  Convert the types of unescaped object to instance types where possible,
//  propagate the new type information through the graph, and update memory
//  edges and MergeMem inputs to reflect the new type.
//
//  We start with allocations (and calls which may be allocations)  on alloc_worklist.
//  The processing is done in 4 phases:
//
//  Phase 1:  Process possible allocations from alloc_worklist.  Create instance
//            types for the CheckCastPP for allocations where possible.
//            Propagate the the new types through users as follows:
//               casts and Phi:  push users on alloc_worklist
//               AddP:  cast Base and Address inputs to the instance type
//                      push any AddP users on alloc_worklist and push any memnode
//                      users onto memnode_worklist.
//  Phase 2:  Process MemNode's from memnode_worklist. compute new address type and
//            search the Memory chain for a store with the appropriate type
//            address type.  If a Phi is found, create a new version with
//            the approriate memory slices from each of the Phi inputs.
//            For stores, process the users as follows:
//               MemNode:  push on memnode_worklist
//               MergeMem: push on mergemem_worklist
//  Phase 3:  Process MergeMem nodes from mergemem_worklist.  Walk each memory slice
//            moving the first node encountered of each  instance type to the
//            the input corresponding to its alias index.
//            appropriate memory slice.
//  Phase 4:  Update the inputs of non-instance memory Phis and the Memory input of memnodes.
//
// In the following example, the CheckCastPP nodes are the cast of allocation
// results and the allocation of node 29 is unescaped and eligible to be an
// instance type.
//
// We start with:
//
//     7 Parm #memory
//    10  ConI  "12"
//    19  CheckCastPP   "Foo"
//    20  AddP  _ 19 19 10  Foo+12  alias_index=4
//    29  CheckCastPP   "Foo"
//    30  AddP  _ 29 29 10  Foo+12  alias_index=4
//
//    40  StoreP  25   7  20   ... alias_index=4
//    50  StoreP  35  40  30   ... alias_index=4
//    60  StoreP  45  50  20   ... alias_index=4
//    70  LoadP    _  60  30   ... alias_index=4
//    80  Phi     75  50  60   Memory alias_index=4
//    90  LoadP    _  80  30   ... alias_index=4
//   100  LoadP    _  80  20   ... alias_index=4
//
//
// Phase 1 creates an instance type for node 29 assigning it an instance id of 24
// and creating a new alias index for node 30.  This gives:
//
//     7 Parm #memory
//    10  ConI  "12"
//    19  CheckCastPP   "Foo"
//    20  AddP  _ 19 19 10  Foo+12  alias_index=4
//    29  CheckCastPP   "Foo"  iid=24
//    30  AddP  _ 29 29 10  Foo+12  alias_index=6  iid=24
//
//    40  StoreP  25   7  20   ... alias_index=4
//    50  StoreP  35  40  30   ... alias_index=6
//    60  StoreP  45  50  20   ... alias_index=4
//    70  LoadP    _  60  30   ... alias_index=6
//    80  Phi     75  50  60   Memory alias_index=4
//    90  LoadP    _  80  30   ... alias_index=6
//   100  LoadP    _  80  20   ... alias_index=4
//
// In phase 2, new memory inputs are computed for the loads and stores,
// And a new version of the phi is created.  In phase 4, the inputs to
// node 80 are updated and then the memory nodes are updated with the
// values computed in phase 2.  This results in:
//
//     7 Parm #memory
//    10  ConI  "12"
//    19  CheckCastPP   "Foo"
//    20  AddP  _ 19 19 10  Foo+12  alias_index=4
//    29  CheckCastPP   "Foo"  iid=24
//    30  AddP  _ 29 29 10  Foo+12  alias_index=6  iid=24
//
//    40  StoreP  25  7   20   ... alias_index=4
//    50  StoreP  35  7   30   ... alias_index=6
//    60  StoreP  45  40  20   ... alias_index=4
//    70  LoadP    _  50  30   ... alias_index=6
//    80  Phi     75  40  60   Memory alias_index=4
//   120  Phi     75  50  50   Memory alias_index=6
//    90  LoadP    _ 120  30   ... alias_index=6
//   100  LoadP    _  80  20   ... alias_index=4
//
void ConnectionGraph::split_unique_types(GrowableArray<Node *>  &alloc_worklist) {
  GrowableArray<Node *>  memnode_worklist;
  GrowableArray<Node *>  mergemem_worklist;
  GrowableArray<PhiNode *>  orig_phis;
  PhaseGVN  *igvn = _compile->initial_gvn();
  uint new_index_start = (uint) _compile->num_alias_types();
  VectorSet visited(Thread::current()->resource_area());
  VectorSet ptset(Thread::current()->resource_area());


  //  Phase 1:  Process possible allocations from alloc_worklist.
  //  Create instance types for the CheckCastPP for allocations where possible.
  while (alloc_worklist.length() != 0) {
    Node *n = alloc_worklist.pop();
    uint ni = n->_idx;
    const TypeOopPtr* tinst = NULL;
    if (n->is_Call()) {
      CallNode *alloc = n->as_Call();
      // copy escape information to call node
      PointsToNode* ptn = _nodes->adr_at(alloc->_idx);
      PointsToNode::EscapeState es = escape_state(alloc, igvn);
      // We have an allocation or call which returns a Java object,
      // see if it is unescaped.
      if (es != PointsToNode::NoEscape || !ptn->_scalar_replaceable)
        continue;
      if (alloc->is_Allocate()) {
        // Set the scalar_replaceable flag before the next check.
        alloc->as_Allocate()->_is_scalar_replaceable = true;
      }
      // find CheckCastPP of call return value
      n = alloc->result_cast();
      if (n == NULL ||          // No uses accept Initialize or
          !n->is_CheckCastPP()) // not unique CheckCastPP.
        continue;
      // The inline code for Object.clone() casts the allocation result to
      // java.lang.Object and then to the the actual type of the allocated
      // object. Detect this case and use the second cast.
      if (alloc->is_Allocate() && n->as_Type()->type() == TypeInstPtr::NOTNULL
          && igvn->type(alloc->in(AllocateNode::KlassNode)) != TypeKlassPtr::OBJECT) {
        Node *cast2 = NULL;
        for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
          Node *use = n->fast_out(i);
          if (use->is_CheckCastPP()) {
            cast2 = use;
            break;
          }
        }
        if (cast2 != NULL) {
          n = cast2;
        } else {
          continue;
        }
      }
      set_escape_state(n->_idx, es);
      // in order for an object to be stackallocatable, it must be:
      //   - a direct allocation (not a call returning an object)
      //   - non-escaping
      //   - eligible to be a unique type
      //   - not determined to be ineligible by escape analysis
      set_map(alloc->_idx, n);
      set_map(n->_idx, alloc);
      const TypeOopPtr *t = igvn->type(n)->isa_oopptr();
      if (t == NULL)
        continue;  // not a TypeInstPtr
      tinst = t->cast_to_instance(ni);
      igvn->hash_delete(n);
      igvn->set_type(n,  tinst);
      n->raise_bottom_type(tinst);
      igvn->hash_insert(n);
      record_for_optimizer(n);
      if (alloc->is_Allocate() && ptn->_scalar_replaceable &&
          (t->isa_instptr() || t->isa_aryptr())) {
        // An allocation may have an Initialize which has raw stores. Scan
        // the users of the raw allocation result and push AddP users
        // on alloc_worklist.
        Node *raw_result = alloc->proj_out(TypeFunc::Parms);
        assert (raw_result != NULL, "must have an allocation result");
        for (DUIterator_Fast imax, i = raw_result->fast_outs(imax); i < imax; i++) {
          Node *use = raw_result->fast_out(i);
          if (use->is_AddP() && use->outcnt() > 0) { // Don't process dead nodes
            Node* addp2 = find_second_addp(use, raw_result);
            if (addp2 != NULL) {
              assert(alloc->is_AllocateArray(),"array allocation was expected");
              alloc_worklist.append_if_missing(addp2);
            }
            alloc_worklist.append_if_missing(use);
          } else if (use->is_Initialize()) {
            memnode_worklist.append_if_missing(use);
          }
        }
      }
    } else if (n->is_AddP()) {
      ptset.Clear();
      PointsTo(ptset, get_addp_base(n), igvn);
      assert(ptset.Size() == 1, "AddP address is unique");
      uint elem = ptset.getelem(); // Allocation node's index
      if (elem == _phantom_object)
        continue; // Assume the value was set outside this method.
      Node *base = get_map(elem);  // CheckCastPP node
      split_AddP(n, base, igvn);
      tinst = igvn->type(base)->isa_oopptr();
    } else if (n->is_Phi() ||
               n->is_CheckCastPP() ||
               (n->is_ConstraintCast() && n->Opcode() == Op_CastPP)) {
      if (visited.test_set(n->_idx)) {
        assert(n->is_Phi(), "loops only through Phi's");
        continue;  // already processed
      }
      ptset.Clear();
      PointsTo(ptset, n, igvn);
      if (ptset.Size() == 1) {
        uint elem = ptset.getelem(); // Allocation node's index
        if (elem == _phantom_object)
          continue; // Assume the value was set outside this method.
        Node *val = get_map(elem);   // CheckCastPP node
        TypeNode *tn = n->as_Type();
        tinst = igvn->type(val)->isa_oopptr();
        assert(tinst != NULL && tinst->is_instance() &&
               tinst->instance_id() == elem , "instance type expected.");
        const TypeOopPtr *tn_t = igvn->type(tn)->isa_oopptr();

        if (tn_t != NULL &&
 tinst->cast_to_instance(TypeOopPtr::UNKNOWN_INSTANCE)->higher_equal(tn_t)) {
          igvn->hash_delete(tn);
          igvn->set_type(tn, tinst);
          tn->set_type(tinst);
          igvn->hash_insert(tn);
          record_for_optimizer(n);
        }
      }
    } else {
      continue;
    }
    // push users on appropriate worklist
    for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
      Node *use = n->fast_out(i);
      if(use->is_Mem() && use->in(MemNode::Address) == n) {
        memnode_worklist.append_if_missing(use);
      } else if (use->is_Initialize()) {
        memnode_worklist.append_if_missing(use);
      } else if (use->is_MergeMem()) {
        mergemem_worklist.append_if_missing(use);
      } else if (use->is_Call() && tinst != NULL) {
        // Look for MergeMem nodes for calls which reference unique allocation
        // (through CheckCastPP nodes) even for debug info.
        Node* m = use->in(TypeFunc::Memory);
        uint iid = tinst->instance_id();
        while (m->is_Proj() && m->in(0)->is_Call() &&
               m->in(0) != use && !m->in(0)->_idx != iid) {
          m = m->in(0)->in(TypeFunc::Memory);
        }
        if (m->is_MergeMem()) {
          mergemem_worklist.append_if_missing(m);
        }
      } else if (use->is_AddP() && use->outcnt() > 0) { // No dead nodes
        Node* addp2 = find_second_addp(use, n);
        if (addp2 != NULL) {
          alloc_worklist.append_if_missing(addp2);
        }
        alloc_worklist.append_if_missing(use);
      } else if (use->is_Phi() ||
                 use->is_CheckCastPP() ||
                 (use->is_ConstraintCast() && use->Opcode() == Op_CastPP)) {
        alloc_worklist.append_if_missing(use);
      }
    }

  }
  // New alias types were created in split_AddP().
  uint new_index_end = (uint) _compile->num_alias_types();

  //  Phase 2:  Process MemNode's from memnode_worklist. compute new address type and
  //            compute new values for Memory inputs  (the Memory inputs are not
  //            actually updated until phase 4.)
  if (memnode_worklist.length() == 0)
    return;  // nothing to do

  while (memnode_worklist.length() != 0) {
    Node *n = memnode_worklist.pop();
    if (visited.test_set(n->_idx))
      continue;
    if (n->is_Phi()) {
      assert(n->as_Phi()->adr_type() != TypePtr::BOTTOM, "narrow memory slice required");
      // we don't need to do anything, but the users must be pushed if we haven't processed
      // this Phi before
    } else if (n->is_Initialize()) {
      // we don't need to do anything, but the users of the memory projection must be pushed
      n = n->as_Initialize()->proj_out(TypeFunc::Memory);
      if (n == NULL)
        continue;
    } else {
      assert(n->is_Mem(), "memory node required.");
      Node *addr = n->in(MemNode::Address);
      assert(addr->is_AddP(), "AddP required");
      const Type *addr_t = igvn->type(addr);
      if (addr_t == Type::TOP)
        continue;
      assert (addr_t->isa_ptr() != NULL, "pointer type required.");
      int alias_idx = _compile->get_alias_index(addr_t->is_ptr());
      assert ((uint)alias_idx < new_index_end, "wrong alias index");
      Node *mem = find_inst_mem(n->in(MemNode::Memory), alias_idx, orig_phis, igvn);
      if (_compile->failing()) {
        return;
      }
      if (mem != n->in(MemNode::Memory)) {
        set_map(n->_idx, mem);
        _nodes->adr_at(n->_idx)->_node = n;
      }
      if (n->is_Load()) {
        continue;  // don't push users
      } else if (n->is_LoadStore()) {
        // get the memory projection
        for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
          Node *use = n->fast_out(i);
          if (use->Opcode() == Op_SCMemProj) {
            n = use;
            break;
          }
        }
        assert(n->Opcode() == Op_SCMemProj, "memory projection required");
      }
    }
    // push user on appropriate worklist
    for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
      Node *use = n->fast_out(i);
      if (use->is_Phi()) {
        memnode_worklist.append_if_missing(use);
      } else if(use->is_Mem() && use->in(MemNode::Memory) == n) {
        memnode_worklist.append_if_missing(use);
      } else if (use->is_Initialize()) {
        memnode_worklist.append_if_missing(use);
      } else if (use->is_MergeMem()) {
        mergemem_worklist.append_if_missing(use);
      }
    }
  }

  //  Phase 3:  Process MergeMem nodes from mergemem_worklist.
  //            Walk each memory moving the first node encountered of each
  //            instance type to the the input corresponding to its alias index.
  while (mergemem_worklist.length() != 0) {
    Node *n = mergemem_worklist.pop();
    assert(n->is_MergeMem(), "MergeMem node required.");
    if (visited.test_set(n->_idx))
      continue;
    MergeMemNode *nmm = n->as_MergeMem();
    // Note: we don't want to use MergeMemStream here because we only want to
    //  scan inputs which exist at the start, not ones we add during processing.
    uint nslices = nmm->req();
    igvn->hash_delete(nmm);
    for (uint i = Compile::AliasIdxRaw+1; i < nslices; i++) {
      Node* mem = nmm->in(i);
      Node* cur = NULL;
      if (mem == NULL || mem->is_top())
        continue;
      while (mem->is_Mem()) {
        const Type *at = igvn->type(mem->in(MemNode::Address));
        if (at != Type::TOP) {
          assert (at->isa_ptr() != NULL, "pointer type required.");
          uint idx = (uint)_compile->get_alias_index(at->is_ptr());
          if (idx == i) {
            if (cur == NULL)
              cur = mem;
          } else {
            if (idx >= nmm->req() || nmm->is_empty_memory(nmm->in(idx))) {
              nmm->set_memory_at(idx, mem);
            }
          }
        }
        mem = mem->in(MemNode::Memory);
      }
      nmm->set_memory_at(i, (cur != NULL) ? cur : mem);
      // Find any instance of the current type if we haven't encountered
      // a value of the instance along the chain.
      for (uint ni = new_index_start; ni < new_index_end; ni++) {
        if((uint)_compile->get_general_index(ni) == i) {
          Node *m = (ni >= nmm->req()) ? nmm->empty_memory() : nmm->in(ni);
          if (nmm->is_empty_memory(m)) {
            Node* result = find_inst_mem(mem, ni, orig_phis, igvn);
            if (_compile->failing()) {
              return;
            }
            nmm->set_memory_at(ni, result);
          }
        }
      }
    }
    // Find the rest of instances values
    for (uint ni = new_index_start; ni < new_index_end; ni++) {
      const TypeOopPtr *tinst = igvn->C->get_adr_type(ni)->isa_oopptr();
      Node* result = step_through_mergemem(nmm, ni, tinst);
      if (result == nmm->base_memory()) {
        // Didn't find instance memory, search through general slice recursively.
        result = nmm->memory_at(igvn->C->get_general_index(ni));
        result = find_inst_mem(result, ni, orig_phis, igvn);
        if (_compile->failing()) {
          return;
        }
        nmm->set_memory_at(ni, result);
      }
    }
    igvn->hash_insert(nmm);
    record_for_optimizer(nmm);

    // Propagate new memory slices to following MergeMem nodes.
    for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
      Node *use = n->fast_out(i);
      if (use->is_Call()) {
        CallNode* in = use->as_Call();
        if (in->proj_out(TypeFunc::Memory) != NULL) {
          Node* m = in->proj_out(TypeFunc::Memory);
          for (DUIterator_Fast jmax, j = m->fast_outs(jmax); j < jmax; j++) {
            Node* mm = m->fast_out(j);
            if (mm->is_MergeMem()) {
              mergemem_worklist.append_if_missing(mm);
            }
          }
        }
        if (use->is_Allocate()) {
          use = use->as_Allocate()->initialization();
          if (use == NULL) {
            continue;
          }
        }
      }
      if (use->is_Initialize()) {
        InitializeNode* in = use->as_Initialize();
        if (in->proj_out(TypeFunc::Memory) != NULL) {
          Node* m = in->proj_out(TypeFunc::Memory);
          for (DUIterator_Fast jmax, j = m->fast_outs(jmax); j < jmax; j++) {
            Node* mm = m->fast_out(j);
            if (mm->is_MergeMem()) {
              mergemem_worklist.append_if_missing(mm);
            }
          }
        }
      }
    }
  }

  //  Phase 4:  Update the inputs of non-instance memory Phis and
  //            the Memory input of memnodes
  // First update the inputs of any non-instance Phi's from
  // which we split out an instance Phi.  Note we don't have
  // to recursively process Phi's encounted on the input memory
  // chains as is done in split_memory_phi() since they  will
  // also be processed here.
  while (orig_phis.length() != 0) {
    PhiNode *phi = orig_phis.pop();
    int alias_idx = _compile->get_alias_index(phi->adr_type());
    igvn->hash_delete(phi);
    for (uint i = 1; i < phi->req(); i++) {
      Node *mem = phi->in(i);
      Node *new_mem = find_inst_mem(mem, alias_idx, orig_phis, igvn);
      if (_compile->failing()) {
        return;
      }
      if (mem != new_mem) {
        phi->set_req(i, new_mem);
      }
    }
    igvn->hash_insert(phi);
    record_for_optimizer(phi);
  }

  // Update the memory inputs of MemNodes with the value we computed
  // in Phase 2.
  for (int i = 0; i < _nodes->length(); i++) {
    Node *nmem = get_map(i);
    if (nmem != NULL) {
      Node *n = _nodes->adr_at(i)->_node;
      if (n != NULL && n->is_Mem()) {
        igvn->hash_delete(n);
        n->set_req(MemNode::Memory, nmem);
        igvn->hash_insert(n);
        record_for_optimizer(n);
      }
    }
  }
}

void ConnectionGraph::compute_escape() {

  // 1. Populate Connection Graph with Ideal nodes.

  Unique_Node_List worklist_init;
  worklist_init.map(_compile->unique(), NULL);  // preallocate space

  // Initialize worklist
  if (_compile->root() != NULL) {
    worklist_init.push(_compile->root());
  }

  GrowableArray<int> cg_worklist;
  PhaseGVN* igvn = _compile->initial_gvn();
  bool has_allocations = false;

  // Push all useful nodes onto CG list and set their type.
  for( uint next = 0; next < worklist_init.size(); ++next ) {
    Node* n = worklist_init.at(next);
    record_for_escape_analysis(n, igvn);
    if (n->is_Call() &&
        _nodes->adr_at(n->_idx)->node_type() == PointsToNode::JavaObject) {
      has_allocations = true;
    }
    if(n->is_AddP())
      cg_worklist.append(n->_idx);
    for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
      Node* m = n->fast_out(i);   // Get user
      worklist_init.push(m);
    }
  }

  if (has_allocations) {
    _has_allocations = true;
  } else {
    _has_allocations = false;
    _collecting = false;
    return; // Nothing to do.
  }

  // 2. First pass to create simple CG edges (doesn't require to walk CG).
  for( uint next = 0; next < _delayed_worklist.size(); ++next ) {
    Node* n = _delayed_worklist.at(next);
    build_connection_graph(n, igvn);
  }

  // 3. Pass to create fields edges (Allocate -F-> AddP).
  for( int next = 0; next < cg_worklist.length(); ++next ) {
    int ni = cg_worklist.at(next);
    build_connection_graph(_nodes->adr_at(ni)->_node, igvn);
  }

  cg_worklist.clear();
  cg_worklist.append(_phantom_object);

  // 4. Build Connection Graph which need
  //    to walk the connection graph.
  for (uint ni = 0; ni < (uint)_nodes->length(); ni++) {
    PointsToNode* ptn = _nodes->adr_at(ni);
    Node *n = ptn->_node;
    if (n != NULL) { // Call, AddP, LoadP, StoreP
      build_connection_graph(n, igvn);
      if (ptn->node_type() != PointsToNode::UnknownType)
        cg_worklist.append(n->_idx); // Collect CG nodes
    }
  }

  VectorSet ptset(Thread::current()->resource_area());
  GrowableArray<Node*>  alloc_worklist;
  GrowableArray<int>  worklist;

  // remove deferred edges from the graph and collect
  // information we will need for type splitting
  for( int next = 0; next < cg_worklist.length(); ++next ) {
    int ni = cg_worklist.at(next);
    PointsToNode* ptn = _nodes->adr_at(ni);
    PointsToNode::NodeType nt = ptn->node_type();
    Node *n = ptn->_node;
    if (nt == PointsToNode::LocalVar || nt == PointsToNode::Field) {
      remove_deferred(ni);
      if (n->is_AddP()) {
        // If this AddP computes an address which may point to more that one
        // object, nothing the address points to can be scalar replaceable.
        Node *base = get_addp_base(n);
        ptset.Clear();
        PointsTo(ptset, base, igvn);
        if (ptset.Size() > 1) {
          for( VectorSetI j(&ptset); j.test(); ++j ) {
            uint pt = j.elem;
            ptnode_adr(pt)->_scalar_replaceable = false;
          }
        }
      }
    } else if (nt == PointsToNode::JavaObject && n->is_Call()) {
      // Push call on alloc_worlist (alocations are calls)
      // for processing by split_unique_types().
      alloc_worklist.append(n);
    }
  }

  // push all GlobalEscape nodes on the worklist
  for( int next = 0; next < cg_worklist.length(); ++next ) {
    int nk = cg_worklist.at(next);
    if (_nodes->adr_at(nk)->escape_state() == PointsToNode::GlobalEscape)
      worklist.append(nk);
  }
  // mark all node reachable from GlobalEscape nodes
  while(worklist.length() > 0) {
    PointsToNode n = _nodes->at(worklist.pop());
    for (uint ei = 0; ei < n.edge_count(); ei++) {
      uint npi = n.edge_target(ei);
      PointsToNode *np = ptnode_adr(npi);
      if (np->escape_state() < PointsToNode::GlobalEscape) {
        np->set_escape_state(PointsToNode::GlobalEscape);
        worklist.append_if_missing(npi);
      }
    }
  }

  // push all ArgEscape nodes on the worklist
  for( int next = 0; next < cg_worklist.length(); ++next ) {
    int nk = cg_worklist.at(next);
    if (_nodes->adr_at(nk)->escape_state() == PointsToNode::ArgEscape)
      worklist.push(nk);
  }
  // mark all node reachable from ArgEscape nodes
  while(worklist.length() > 0) {
    PointsToNode n = _nodes->at(worklist.pop());
    for (uint ei = 0; ei < n.edge_count(); ei++) {
      uint npi = n.edge_target(ei);
      PointsToNode *np = ptnode_adr(npi);
      if (np->escape_state() < PointsToNode::ArgEscape) {
        np->set_escape_state(PointsToNode::ArgEscape);
        worklist.append_if_missing(npi);
      }
    }
  }

  // push all NoEscape nodes on the worklist
  for( int next = 0; next < cg_worklist.length(); ++next ) {
    int nk = cg_worklist.at(next);
    if (_nodes->adr_at(nk)->escape_state() == PointsToNode::NoEscape)
      worklist.push(nk);
  }
  // mark all node reachable from NoEscape nodes
  while(worklist.length() > 0) {
    PointsToNode n = _nodes->at(worklist.pop());
    for (uint ei = 0; ei < n.edge_count(); ei++) {
      uint npi = n.edge_target(ei);
      PointsToNode *np = ptnode_adr(npi);
      if (np->escape_state() < PointsToNode::NoEscape) {
        np->set_escape_state(PointsToNode::NoEscape);
        worklist.append_if_missing(npi);
      }
    }
  }

  _collecting = false;

  has_allocations = false; // Are there scalar replaceable allocations?

  for( int next = 0; next < alloc_worklist.length(); ++next ) {
    Node* n = alloc_worklist.at(next);
    uint ni = n->_idx;
    PointsToNode* ptn = _nodes->adr_at(ni);
    PointsToNode::EscapeState es = ptn->escape_state();
    if (ptn->escape_state() == PointsToNode::NoEscape &&
        ptn->_scalar_replaceable) {
      has_allocations = true;
      break;
    }
  }
  if (!has_allocations) {
    return; // Nothing to do.
  }

  if(_compile->AliasLevel() >= 3 && EliminateAllocations) {
    // Now use the escape information to create unique types for
    // unescaped objects
    split_unique_types(alloc_worklist);
    if (_compile->failing())  return;

    // Clean up after split unique types.
    ResourceMark rm;
    PhaseRemoveUseless pru(_compile->initial_gvn(), _compile->for_igvn());

#ifdef ASSERT
  } else if (PrintEscapeAnalysis || PrintEliminateAllocations) {
    tty->print("=== No allocations eliminated for ");
    C()->method()->print_short_name();
    if(!EliminateAllocations) {
      tty->print(" since EliminateAllocations is off ===");
    } else if(_compile->AliasLevel() < 3) {
      tty->print(" since AliasLevel < 3 ===");
    }
    tty->cr();
#endif
  }
}

void ConnectionGraph::process_call_arguments(CallNode *call, PhaseTransform *phase) {

    switch (call->Opcode()) {
#ifdef ASSERT
    case Op_Allocate:
    case Op_AllocateArray:
    case Op_Lock:
    case Op_Unlock:
      assert(false, "should be done already");
      break;
#endif
    case Op_CallLeafNoFP:
    {
      // Stub calls, objects do not escape but they are not scale replaceable.
      // Adjust escape state for outgoing arguments.
      const TypeTuple * d = call->tf()->domain();
      VectorSet ptset(Thread::current()->resource_area());
      for (uint i = TypeFunc::Parms; i < d->cnt(); i++) {
        const Type* at = d->field_at(i);
        Node *arg = call->in(i)->uncast();
        const Type *aat = phase->type(arg);
        if (!arg->is_top() && at->isa_ptr() && aat->isa_ptr()) {
          assert(aat == Type::TOP || aat == TypePtr::NULL_PTR ||
                 aat->isa_ptr() != NULL, "expecting an Ptr");
          set_escape_state(arg->_idx, PointsToNode::ArgEscape);
          if (arg->is_AddP()) {
            //
            // The inline_native_clone() case when the arraycopy stub is called
            // after the allocation before Initialize and CheckCastPP nodes.
            //
            // Set AddP's base (Allocate) as not scalar replaceable since
            // pointer to the base (with offset) is passed as argument.
            //
            arg = get_addp_base(arg);
          }
          ptset.Clear();
          PointsTo(ptset, arg, phase);
          for( VectorSetI j(&ptset); j.test(); ++j ) {
            uint pt = j.elem;
            set_escape_state(pt, PointsToNode::ArgEscape);
          }
        }
      }
      break;
    }

    case Op_CallStaticJava:
    // For a static call, we know exactly what method is being called.
    // Use bytecode estimator to record the call's escape affects
    {
      ciMethod *meth = call->as_CallJava()->method();
      BCEscapeAnalyzer *call_analyzer = (meth !=NULL) ? meth->get_bcea() : NULL;
      // fall-through if not a Java method or no analyzer information
      if (call_analyzer != NULL) {
        const TypeTuple * d = call->tf()->domain();
        VectorSet ptset(Thread::current()->resource_area());
        bool copy_dependencies = false;
        for (uint i = TypeFunc::Parms; i < d->cnt(); i++) {
          const Type* at = d->field_at(i);
          int k = i - TypeFunc::Parms;

          if (at->isa_oopptr() != NULL) {
            Node *arg = call->in(i)->uncast();

            bool global_escapes = false;
            bool fields_escapes = false;
            if (!call_analyzer->is_arg_stack(k)) {
              // The argument global escapes, mark everything it could point to
              set_escape_state(arg->_idx, PointsToNode::GlobalEscape);
              global_escapes = true;
            } else {
              if (!call_analyzer->is_arg_local(k)) {
                // The argument itself doesn't escape, but any fields might
                fields_escapes = true;
              }
              set_escape_state(arg->_idx, PointsToNode::ArgEscape);
              copy_dependencies = true;
            }

            ptset.Clear();
            PointsTo(ptset, arg, phase);
            for( VectorSetI j(&ptset); j.test(); ++j ) {
              uint pt = j.elem;
              if (global_escapes) {
                //The argument global escapes, mark everything it could point to
                set_escape_state(pt, PointsToNode::GlobalEscape);
              } else {
                if (fields_escapes) {
                  // The argument itself doesn't escape, but any fields might
                  add_edge_from_fields(pt, _phantom_object, Type::OffsetBot);
                }
                set_escape_state(pt, PointsToNode::ArgEscape);
              }
            }
          }
        }
        if (copy_dependencies)
          call_analyzer->copy_dependencies(C()->dependencies());
        break;
      }
    }

    default:
    // Fall-through here if not a Java method or no analyzer information
    // or some other type of call, assume the worst case: all arguments
    // globally escape.
    {
      // adjust escape state for  outgoing arguments
      const TypeTuple * d = call->tf()->domain();
      VectorSet ptset(Thread::current()->resource_area());
      for (uint i = TypeFunc::Parms; i < d->cnt(); i++) {
        const Type* at = d->field_at(i);
        if (at->isa_oopptr() != NULL) {
          Node *arg = call->in(i)->uncast();
          set_escape_state(arg->_idx, PointsToNode::GlobalEscape);
          ptset.Clear();
          PointsTo(ptset, arg, phase);
          for( VectorSetI j(&ptset); j.test(); ++j ) {
            uint pt = j.elem;
            set_escape_state(pt, PointsToNode::GlobalEscape);
            PointsToNode *ptadr = ptnode_adr(pt);
          }
        }
      }
    }
  }
}
void ConnectionGraph::process_call_result(ProjNode *resproj, PhaseTransform *phase) {
  PointsToNode *ptadr = ptnode_adr(resproj->_idx);

  CallNode *call = resproj->in(0)->as_Call();
  switch (call->Opcode()) {
    case Op_Allocate:
    {
      Node *k = call->in(AllocateNode::KlassNode);
      const TypeKlassPtr *kt;
      if (k->Opcode() == Op_LoadKlass) {
        kt = k->as_Load()->type()->isa_klassptr();
      } else {
        kt = k->as_Type()->type()->isa_klassptr();
      }
      assert(kt != NULL, "TypeKlassPtr  required.");
      ciKlass* cik = kt->klass();
      ciInstanceKlass* ciik = cik->as_instance_klass();

      PointsToNode *ptadr = ptnode_adr(call->_idx);
      PointsToNode::EscapeState es;
      uint edge_to;
      if (cik->is_subclass_of(_compile->env()->Thread_klass()) || ciik->has_finalizer()) {
        es = PointsToNode::GlobalEscape;
        edge_to = _phantom_object; // Could not be worse
      } else {
        es = PointsToNode::NoEscape;
        edge_to = call->_idx;
      }
      set_escape_state(call->_idx, es);
      add_pointsto_edge(resproj->_idx, edge_to);
      _processed.set(resproj->_idx);
      break;
    }

    case Op_AllocateArray:
    {
      PointsToNode *ptadr = ptnode_adr(call->_idx);
      int length = call->in(AllocateNode::ALength)->find_int_con(-1);
      if (length < 0 || length > EliminateAllocationArraySizeLimit) {
        // Not scalar replaceable if the length is not constant or too big.
        ptadr->_scalar_replaceable = false;
      }
      set_escape_state(call->_idx, PointsToNode::NoEscape);
      add_pointsto_edge(resproj->_idx, call->_idx);
      _processed.set(resproj->_idx);
      break;
    }

    case Op_CallStaticJava:
    // For a static call, we know exactly what method is being called.
    // Use bytecode estimator to record whether the call's return value escapes
    {
      bool done = true;
      const TypeTuple *r = call->tf()->range();
      const Type* ret_type = NULL;

      if (r->cnt() > TypeFunc::Parms)
        ret_type = r->field_at(TypeFunc::Parms);

      // Note:  we use isa_ptr() instead of isa_oopptr()  here because the
      //        _multianewarray functions return a TypeRawPtr.
      if (ret_type == NULL || ret_type->isa_ptr() == NULL) {
        _processed.set(resproj->_idx);
        break;  // doesn't return a pointer type
      }
      ciMethod *meth = call->as_CallJava()->method();
      const TypeTuple * d = call->tf()->domain();
      if (meth == NULL) {
        // not a Java method, assume global escape
        set_escape_state(call->_idx, PointsToNode::GlobalEscape);
        if (resproj != NULL)
          add_pointsto_edge(resproj->_idx, _phantom_object);
      } else {
        BCEscapeAnalyzer *call_analyzer = meth->get_bcea();
        VectorSet ptset(Thread::current()->resource_area());
        bool copy_dependencies = false;

        if (call_analyzer->is_return_allocated()) {
          // Returns a newly allocated unescaped object, simply
          // update dependency information.
          // Mark it as NoEscape so that objects referenced by
          // it's fields will be marked as NoEscape at least.
          set_escape_state(call->_idx, PointsToNode::NoEscape);
          if (resproj != NULL)
            add_pointsto_edge(resproj->_idx, call->_idx);
          copy_dependencies = true;
        } else if (call_analyzer->is_return_local() && resproj != NULL) {
          // determine whether any arguments are returned
          set_escape_state(call->_idx, PointsToNode::NoEscape);
          for (uint i = TypeFunc::Parms; i < d->cnt(); i++) {
            const Type* at = d->field_at(i);

            if (at->isa_oopptr() != NULL) {
              Node *arg = call->in(i)->uncast();

              if (call_analyzer->is_arg_returned(i - TypeFunc::Parms)) {
                PointsToNode *arg_esp = _nodes->adr_at(arg->_idx);
                if (arg_esp->node_type() == PointsToNode::UnknownType)
                  done = false;
                else if (arg_esp->node_type() == PointsToNode::JavaObject)
                  add_pointsto_edge(resproj->_idx, arg->_idx);
                else
                  add_deferred_edge(resproj->_idx, arg->_idx);
                arg_esp->_hidden_alias = true;
              }
            }
          }
          copy_dependencies = true;
        } else {
          set_escape_state(call->_idx, PointsToNode::GlobalEscape);
          if (resproj != NULL)
            add_pointsto_edge(resproj->_idx, _phantom_object);
          for (uint i = TypeFunc::Parms; i < d->cnt(); i++) {
            const Type* at = d->field_at(i);
            if (at->isa_oopptr() != NULL) {
              Node *arg = call->in(i)->uncast();
              PointsToNode *arg_esp = _nodes->adr_at(arg->_idx);
              arg_esp->_hidden_alias = true;
            }
          }
        }
        if (copy_dependencies)
          call_analyzer->copy_dependencies(C()->dependencies());
      }
      if (done)
        _processed.set(resproj->_idx);
      break;
    }

    default:
    // Some other type of call, assume the worst case that the
    // returned value, if any, globally escapes.
    {
      const TypeTuple *r = call->tf()->range();
      if (r->cnt() > TypeFunc::Parms) {
        const Type* ret_type = r->field_at(TypeFunc::Parms);

        // Note:  we use isa_ptr() instead of isa_oopptr()  here because the
        //        _multianewarray functions return a TypeRawPtr.
        if (ret_type->isa_ptr() != NULL) {
          PointsToNode *ptadr = ptnode_adr(call->_idx);
          set_escape_state(call->_idx, PointsToNode::GlobalEscape);
          if (resproj != NULL)
            add_pointsto_edge(resproj->_idx, _phantom_object);
        }
      }
      _processed.set(resproj->_idx);
    }
  }
}

// Populate Connection Graph with Ideal nodes and create simple
// connection graph edges (do not need to check the node_type of inputs
// or to call PointsTo() to walk the connection graph).
void ConnectionGraph::record_for_escape_analysis(Node *n, PhaseTransform *phase) {
  if (_processed.test(n->_idx))
    return; // No need to redefine node's state.

  if (n->is_Call()) {
    // Arguments to allocation and locking don't escape.
    if (n->is_Allocate()) {
      add_node(n, PointsToNode::JavaObject, PointsToNode::UnknownEscape, true);
      record_for_optimizer(n);
    } else if (n->is_Lock() || n->is_Unlock()) {
      // Put Lock and Unlock nodes on IGVN worklist to process them during
      // the first IGVN optimization when escape information is still available.
      record_for_optimizer(n);
      _processed.set(n->_idx);
    } else {
      // Have to process call's arguments first.
      PointsToNode::NodeType nt = PointsToNode::UnknownType;

      // Check if a call returns an object.
      const TypeTuple *r = n->as_Call()->tf()->range();
      if (r->cnt() > TypeFunc::Parms &&
          n->as_Call()->proj_out(TypeFunc::Parms) != NULL) {
        // Note:  use isa_ptr() instead of isa_oopptr() here because
        //        the _multianewarray functions return a TypeRawPtr.
        if (r->field_at(TypeFunc::Parms)->isa_ptr() != NULL) {
          nt = PointsToNode::JavaObject;
        }
      }
      add_node(n, nt, PointsToNode::UnknownEscape, false);
    }
    return;
  }

  // Using isa_ptr() instead of isa_oopptr() for LoadP and Phi because
  // ThreadLocal has RawPrt type.
  switch (n->Opcode()) {
    case Op_AddP:
    {
      add_node(n, PointsToNode::Field, PointsToNode::UnknownEscape, false);
      break;
    }
    case Op_CastX2P:
    { // "Unsafe" memory access.
      add_node(n, PointsToNode::JavaObject, PointsToNode::GlobalEscape, true);
      break;
    }
    case Op_CastPP:
    case Op_CheckCastPP:
    {
      add_node(n, PointsToNode::LocalVar, PointsToNode::UnknownEscape, false);
      int ti = n->in(1)->_idx;
      PointsToNode::NodeType nt = _nodes->adr_at(ti)->node_type();
      if (nt == PointsToNode::UnknownType) {
        _delayed_worklist.push(n); // Process it later.
        break;
      } else if (nt == PointsToNode::JavaObject) {
        add_pointsto_edge(n->_idx, ti);
      } else {
        add_deferred_edge(n->_idx, ti);
      }
      _processed.set(n->_idx);
      break;
    }
    case Op_ConP:
    {
      // assume all pointer constants globally escape except for null
      PointsToNode::EscapeState es;
      if (phase->type(n) == TypePtr::NULL_PTR)
        es = PointsToNode::NoEscape;
      else
        es = PointsToNode::GlobalEscape;

      add_node(n, PointsToNode::JavaObject, es, true);
      break;
    }
    case Op_CreateEx:
    {
      // assume that all exception objects globally escape
      add_node(n, PointsToNode::JavaObject, PointsToNode::GlobalEscape, true);
      break;
    }
    case Op_LoadKlass:
    {
      add_node(n, PointsToNode::JavaObject, PointsToNode::GlobalEscape, true);
      break;
    }
    case Op_LoadP:
    {
      const Type *t = phase->type(n);
      if (t->isa_ptr() == NULL) {
        _processed.set(n->_idx);
        return;
      }
      add_node(n, PointsToNode::LocalVar, PointsToNode::UnknownEscape, false);
      break;
    }
    case Op_Parm:
    {
      _processed.set(n->_idx); // No need to redefine it state.
      uint con = n->as_Proj()->_con;
      if (con < TypeFunc::Parms)
        return;
      const Type *t = n->in(0)->as_Start()->_domain->field_at(con);
      if (t->isa_ptr() == NULL)
        return;
      // We have to assume all input parameters globally escape
      // (Note: passing 'false' since _processed is already set).
      add_node(n, PointsToNode::JavaObject, PointsToNode::GlobalEscape, false);
      break;
    }
    case Op_Phi:
    {
      if (n->as_Phi()->type()->isa_ptr() == NULL) {
        // nothing to do if not an oop
        _processed.set(n->_idx);
        return;
      }
      add_node(n, PointsToNode::LocalVar, PointsToNode::UnknownEscape, false);
      uint i;
      for (i = 1; i < n->req() ; i++) {
        Node* in = n->in(i);
        if (in == NULL)
          continue;  // ignore NULL
        in = in->uncast();
        if (in->is_top() || in == n)
          continue;  // ignore top or inputs which go back this node
        int ti = in->_idx;
        PointsToNode::NodeType nt = _nodes->adr_at(ti)->node_type();
        if (nt == PointsToNode::UnknownType) {
          break;
        } else if (nt == PointsToNode::JavaObject) {
          add_pointsto_edge(n->_idx, ti);
        } else {
          add_deferred_edge(n->_idx, ti);
        }
      }
      if (i >= n->req())
        _processed.set(n->_idx);
      else
        _delayed_worklist.push(n);
      break;
    }
    case Op_Proj:
    {
      // we are only interested in the result projection from a call
      if (n->as_Proj()->_con == TypeFunc::Parms && n->in(0)->is_Call() ) {
        add_node(n, PointsToNode::LocalVar, PointsToNode::UnknownEscape, false);
        process_call_result(n->as_Proj(), phase);
        if (!_processed.test(n->_idx)) {
          // The call's result may need to be processed later if the call
          // returns it's argument and the argument is not processed yet.
          _delayed_worklist.push(n);
        }
      } else {
        _processed.set(n->_idx);
      }
      break;
    }
    case Op_Return:
    {
      if( n->req() > TypeFunc::Parms &&
          phase->type(n->in(TypeFunc::Parms))->isa_oopptr() ) {
        // Treat Return value as LocalVar with GlobalEscape escape state.
        add_node(n, PointsToNode::LocalVar, PointsToNode::GlobalEscape, false);
        int ti = n->in(TypeFunc::Parms)->_idx;
        PointsToNode::NodeType nt = _nodes->adr_at(ti)->node_type();
        if (nt == PointsToNode::UnknownType) {
          _delayed_worklist.push(n); // Process it later.
          break;
        } else if (nt == PointsToNode::JavaObject) {
          add_pointsto_edge(n->_idx, ti);
        } else {
          add_deferred_edge(n->_idx, ti);
        }
      }
      _processed.set(n->_idx);
      break;
    }
    case Op_StoreP:
    {
      const Type *adr_type = phase->type(n->in(MemNode::Address));
      if (adr_type->isa_oopptr()) {
        add_node(n, PointsToNode::UnknownType, PointsToNode::UnknownEscape, false);
      } else {
        Node* adr = n->in(MemNode::Address);
        if (adr->is_AddP() && phase->type(adr) == TypeRawPtr::NOTNULL &&
            adr->in(AddPNode::Address)->is_Proj() &&
            adr->in(AddPNode::Address)->in(0)->is_Allocate()) {
          add_node(n, PointsToNode::UnknownType, PointsToNode::UnknownEscape, false);
          // We are computing a raw address for a store captured
          // by an Initialize compute an appropriate address type.
          int offs = (int)phase->find_intptr_t_con(adr->in(AddPNode::Offset), Type::OffsetBot);
          assert(offs != Type::OffsetBot, "offset must be a constant");
        } else {
          _processed.set(n->_idx);
          return;
        }
      }
      break;
    }
    case Op_StorePConditional:
    case Op_CompareAndSwapP:
    {
      const Type *adr_type = phase->type(n->in(MemNode::Address));
      if (adr_type->isa_oopptr()) {
        add_node(n, PointsToNode::UnknownType, PointsToNode::UnknownEscape, false);
      } else {
        _processed.set(n->_idx);
        return;
      }
      break;
    }
    case Op_ThreadLocal:
    {
      add_node(n, PointsToNode::JavaObject, PointsToNode::ArgEscape, true);
      break;
    }
    default:
      ;
      // nothing to do
  }
  return;
}

void ConnectionGraph::build_connection_graph(Node *n, PhaseTransform *phase) {
  // Don't set processed bit for AddP, LoadP, StoreP since
  // they may need more then one pass to process.
  if (_processed.test(n->_idx))
    return; // No need to redefine node's state.

  PointsToNode *ptadr = ptnode_adr(n->_idx);

  if (n->is_Call()) {
    CallNode *call = n->as_Call();
    process_call_arguments(call, phase);
    _processed.set(n->_idx);
    return;
  }

  switch (n->Opcode()) {
    case Op_AddP:
    {
      Node *base = get_addp_base(n);
      // Create a field edge to this node from everything base could point to.
      VectorSet ptset(Thread::current()->resource_area());
      PointsTo(ptset, base, phase);
      for( VectorSetI i(&ptset); i.test(); ++i ) {
        uint pt = i.elem;
        add_field_edge(pt, n->_idx, address_offset(n, phase));
      }
      break;
    }
    case Op_CastX2P:
    {
      assert(false, "Op_CastX2P");
      break;
    }
    case Op_CastPP:
    case Op_CheckCastPP:
    {
      int ti = n->in(1)->_idx;
      if (_nodes->adr_at(ti)->node_type() == PointsToNode::JavaObject) {
        add_pointsto_edge(n->_idx, ti);
      } else {
        add_deferred_edge(n->_idx, ti);
      }
      _processed.set(n->_idx);
      break;
    }
    case Op_ConP:
    {
      assert(false, "Op_ConP");
      break;
    }
    case Op_CreateEx:
    {
      assert(false, "Op_CreateEx");
      break;
    }
    case Op_LoadKlass:
    {
      assert(false, "Op_LoadKlass");
      break;
    }
    case Op_LoadP:
    {
      const Type *t = phase->type(n);
#ifdef ASSERT
      if (t->isa_ptr() == NULL)
        assert(false, "Op_LoadP");
#endif

      Node* adr = n->in(MemNode::Address)->uncast();
      const Type *adr_type = phase->type(adr);
      Node* adr_base;
      if (adr->is_AddP()) {
        adr_base = get_addp_base(adr);
      } else {
        adr_base = adr;
      }

      // For everything "adr_base" could point to, create a deferred edge from
      // this node to each field with the same offset.
      VectorSet ptset(Thread::current()->resource_area());
      PointsTo(ptset, adr_base, phase);
      int offset = address_offset(adr, phase);
      for( VectorSetI i(&ptset); i.test(); ++i ) {
        uint pt = i.elem;
        add_deferred_edge_to_fields(n->_idx, pt, offset);
      }
      break;
    }
    case Op_Parm:
    {
      assert(false, "Op_Parm");
      break;
    }
    case Op_Phi:
    {
#ifdef ASSERT
      if (n->as_Phi()->type()->isa_ptr() == NULL)
        assert(false, "Op_Phi");
#endif
      for (uint i = 1; i < n->req() ; i++) {
        Node* in = n->in(i);
        if (in == NULL)
          continue;  // ignore NULL
        in = in->uncast();
        if (in->is_top() || in == n)
          continue;  // ignore top or inputs which go back this node
        int ti = in->_idx;
        if (_nodes->adr_at(in->_idx)->node_type() == PointsToNode::JavaObject) {
          add_pointsto_edge(n->_idx, ti);
        } else {
          add_deferred_edge(n->_idx, ti);
        }
      }
      _processed.set(n->_idx);
      break;
    }
    case Op_Proj:
    {
      // we are only interested in the result projection from a call
      if (n->as_Proj()->_con == TypeFunc::Parms && n->in(0)->is_Call() ) {
        process_call_result(n->as_Proj(), phase);
        assert(_processed.test(n->_idx), "all call results should be processed");
      } else {
        assert(false, "Op_Proj");
      }
      break;
    }
    case Op_Return:
    {
#ifdef ASSERT
      if( n->req() <= TypeFunc::Parms ||
          !phase->type(n->in(TypeFunc::Parms))->isa_oopptr() ) {
        assert(false, "Op_Return");
      }
#endif
      int ti = n->in(TypeFunc::Parms)->_idx;
      if (_nodes->adr_at(ti)->node_type() == PointsToNode::JavaObject) {
        add_pointsto_edge(n->_idx, ti);
      } else {
        add_deferred_edge(n->_idx, ti);
      }
      _processed.set(n->_idx);
      break;
    }
    case Op_StoreP:
    case Op_StorePConditional:
    case Op_CompareAndSwapP:
    {
      Node *adr = n->in(MemNode::Address);
      const Type *adr_type = phase->type(adr);
#ifdef ASSERT
      if (!adr_type->isa_oopptr())
        assert(phase->type(adr) == TypeRawPtr::NOTNULL, "Op_StoreP");
#endif

      assert(adr->is_AddP(), "expecting an AddP");
      Node *adr_base = get_addp_base(adr);
      Node *val = n->in(MemNode::ValueIn)->uncast();
      // For everything "adr_base" could point to, create a deferred edge
      // to "val" from each field with the same offset.
      VectorSet ptset(Thread::current()->resource_area());
      PointsTo(ptset, adr_base, phase);
      for( VectorSetI i(&ptset); i.test(); ++i ) {
        uint pt = i.elem;
        add_edge_from_fields(pt, val->_idx, address_offset(adr, phase));
      }
      break;
    }
    case Op_ThreadLocal:
    {
      assert(false, "Op_ThreadLocal");
      break;
    }
    default:
      ;
      // nothing to do
  }
}

#ifndef PRODUCT
void ConnectionGraph::dump() {
  PhaseGVN  *igvn = _compile->initial_gvn();
  bool first = true;

  uint size = (uint)_nodes->length();
  for (uint ni = 0; ni < size; ni++) {
    PointsToNode *ptn = _nodes->adr_at(ni);
    PointsToNode::NodeType ptn_type = ptn->node_type();

    if (ptn_type != PointsToNode::JavaObject || ptn->_node == NULL)
      continue;
    PointsToNode::EscapeState es = escape_state(ptn->_node, igvn);
    if (ptn->_node->is_Allocate() && (es == PointsToNode::NoEscape || Verbose)) {
      if (first) {
        tty->cr();
        tty->print("======== Connection graph for ");
        C()->method()->print_short_name();
        tty->cr();
        first = false;
      }
      tty->print("%6d ", ni);
      ptn->dump();
      // Print all locals which reference this allocation
      for (uint li = ni; li < size; li++) {
        PointsToNode *ptn_loc = _nodes->adr_at(li);
        PointsToNode::NodeType ptn_loc_type = ptn_loc->node_type();
        if ( ptn_loc_type == PointsToNode::LocalVar && ptn_loc->_node != NULL &&
             ptn_loc->edge_count() == 1 && ptn_loc->edge_target(0) == ni ) {
          tty->print("%6d  LocalVar [[%d]]", li, ni);
          _nodes->adr_at(li)->_node->dump();
        }
      }
      if (Verbose) {
        // Print all fields which reference this allocation
        for (uint i = 0; i < ptn->edge_count(); i++) {
          uint ei = ptn->edge_target(i);
          tty->print("%6d  Field [[%d]]", ei, ni);
          _nodes->adr_at(ei)->_node->dump();
        }
      }
      tty->cr();
    }
  }
}
#endif