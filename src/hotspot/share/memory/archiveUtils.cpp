/*
 * Copyright (c) 2019, 2020, Oracle and/or its affiliates. All rights reserved.
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
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "memory/archiveUtils.hpp"
#include "memory/dynamicArchive.hpp"
#include "memory/filemap.hpp"
#include "memory/heapShared.inline.hpp"
#include "memory/metaspace.hpp"
#include "memory/metaspaceShared.hpp"
#include "oops/compressedOops.inline.hpp"
#include "utilities/bitMap.inline.hpp"

CHeapBitMap* ArchivePtrMarker::_ptrmap = NULL;
address* ArchivePtrMarker::_ptr_base;
address* ArchivePtrMarker::_ptr_end;
bool ArchivePtrMarker::_compacted;

// Metaspace::allocate() requires that all blocks must be aligned with KlassAlignmentInBytes.
// We enforce the same alignment rule in blocks allocated from the shared space.
const int SharedSpaceObjectAlignment = KlassAlignmentInBytes;

void ArchivePtrMarker::initialize(CHeapBitMap* ptrmap, address* ptr_base, address* ptr_end) {
  assert(_ptrmap == NULL, "initialize only once");
  _ptr_base = ptr_base;
  _ptr_end = ptr_end;
  _compacted = false;
  _ptrmap = ptrmap;

  // Use this as initial guesstimate. We should need less space in the
  // archive, but if we're wrong the bitmap will be expanded automatically.
  size_t estimated_archive_size = MetaspaceGC::capacity_until_GC();
  // But set it smaller in debug builds so we always test the expansion code.
  // (Default archive is about 12MB).
  DEBUG_ONLY(estimated_archive_size = 6 * M);

  // We need one bit per pointer in the archive.
  _ptrmap->initialize(estimated_archive_size / sizeof(intptr_t));
}

void ArchivePtrMarker::mark_pointer(address* ptr_loc) {
  assert(_ptrmap != NULL, "not initialized");
  assert(!_compacted, "cannot mark anymore");

  if (_ptr_base <= ptr_loc && ptr_loc < _ptr_end) {
    address value = *ptr_loc;
    // We don't want any pointer that points to very bottom of the archive, otherwise when
    // MetaspaceShared::default_base_address()==0, we can't distinguish between a pointer
    // to nothing (NULL) vs a pointer to an objects that happens to be at the very bottom
    // of the archive.
    assert(value != (address)_ptr_base, "don't point to the bottom of the archive");

    if (value != NULL) {
      assert(uintx(ptr_loc) % sizeof(intptr_t) == 0, "pointers must be stored in aligned addresses");
      size_t idx = ptr_loc - _ptr_base;
      if (_ptrmap->size() <= idx) {
        _ptrmap->resize((idx + 1) * 2);
      }
      assert(idx < _ptrmap->size(), "must be");
      _ptrmap->set_bit(idx);
      //tty->print_cr("Marking pointer [" PTR_FORMAT "] -> " PTR_FORMAT " @ " SIZE_FORMAT_W(5), p2i(ptr_loc), p2i(*ptr_loc), idx);
    }
  }
}

void ArchivePtrMarker::clear_pointer(address* ptr_loc) {
  assert(_ptrmap != NULL, "not initialized");
  assert(!_compacted, "cannot clear anymore");

  assert(_ptr_base <= ptr_loc && ptr_loc < _ptr_end, "must be");
  assert(uintx(ptr_loc) % sizeof(intptr_t) == 0, "pointers must be stored in aligned addresses");
  size_t idx = ptr_loc - _ptr_base;
  assert(idx < _ptrmap->size(), "cannot clear pointers that have not been marked");
  _ptrmap->clear_bit(idx);
  //tty->print_cr("Clearing pointer [" PTR_FORMAT "] -> " PTR_FORMAT " @ " SIZE_FORMAT_W(5), p2i(ptr_loc), p2i(*ptr_loc), idx);
}

class ArchivePtrBitmapCleaner: public BitMapClosure {
  CHeapBitMap* _ptrmap;
  address* _ptr_base;
  address  _relocatable_base;
  address  _relocatable_end;
  size_t   _max_non_null_offset;

public:
  ArchivePtrBitmapCleaner(CHeapBitMap* ptrmap, address* ptr_base, address relocatable_base, address relocatable_end) :
    _ptrmap(ptrmap), _ptr_base(ptr_base),
    _relocatable_base(relocatable_base), _relocatable_end(relocatable_end), _max_non_null_offset(0) {}

  bool do_bit(size_t offset) {
    address* ptr_loc = _ptr_base + offset;
    address  ptr_value = *ptr_loc;
    if (ptr_value != NULL) {
      assert(_relocatable_base <= ptr_value && ptr_value < _relocatable_end, "do not point to arbitrary locations!");
      if (_max_non_null_offset < offset) {
        _max_non_null_offset = offset;
      }
    } else {
      _ptrmap->clear_bit(offset);
      DEBUG_ONLY(log_trace(cds, reloc)("Clearing pointer [" PTR_FORMAT  "] -> NULL @ " SIZE_FORMAT_W(9), p2i(ptr_loc), offset));
    }

    return true;
  }

  size_t max_non_null_offset() const { return _max_non_null_offset; }
};

void ArchivePtrMarker::compact(address relocatable_base, address relocatable_end) {
  assert(!_compacted, "cannot compact again");
  ArchivePtrBitmapCleaner cleaner(_ptrmap, _ptr_base, relocatable_base, relocatable_end);
  _ptrmap->iterate(&cleaner);
  compact(cleaner.max_non_null_offset());
}

void ArchivePtrMarker::compact(size_t max_non_null_offset) {
  assert(!_compacted, "cannot compact again");
  _ptrmap->resize(max_non_null_offset + 1);
  _compacted = true;
}

char* DumpRegion::expand_top_to(char* newtop) {
  assert(is_allocatable(), "must be initialized and not packed");
  assert(newtop >= _top, "must not grow backwards");
  if (newtop > _end) {
    MetaspaceShared::report_out_of_space(_name, newtop - _top);
    ShouldNotReachHere();
  }

  if (_rs == MetaspaceShared::shared_rs()) {
    uintx delta;
    if (DynamicDumpSharedSpaces) {
      delta = DynamicArchive::object_delta_uintx(newtop);
    } else {
      delta = MetaspaceShared::object_delta_uintx(newtop);
    }
    if (delta > MAX_SHARED_DELTA) {
      // This is just a sanity check and should not appear in any real world usage. This
      // happens only if you allocate more than 2GB of shared objects and would require
      // millions of shared classes.
      vm_exit_during_initialization("Out of memory in the CDS archive",
                                    "Please reduce the number of shared classes.");
    }
  }

  MetaspaceShared::commit_to(_rs, _vs, newtop);
  _top = newtop;
  return _top;
}

char* DumpRegion::allocate(size_t num_bytes) {
  char* p = (char*)align_up(_top, (size_t)SharedSpaceObjectAlignment);
  char* newtop = p + align_up(num_bytes, (size_t)SharedSpaceObjectAlignment);
  expand_top_to(newtop);
  memset(p, 0, newtop - p);
  return p;
}

void DumpRegion::append_intptr_t(intptr_t n, bool need_to_mark) {
  assert(is_aligned(_top, sizeof(intptr_t)), "bad alignment");
  intptr_t *p = (intptr_t*)_top;
  char* newtop = _top + sizeof(intptr_t);
  expand_top_to(newtop);
  *p = n;
  if (need_to_mark) {
    ArchivePtrMarker::mark_pointer(p);
  }
}

void DumpRegion::print(size_t total_bytes) const {
  log_debug(cds)("%-3s space: " SIZE_FORMAT_W(9) " [ %4.1f%% of total] out of " SIZE_FORMAT_W(9) " bytes [%5.1f%% used] at " INTPTR_FORMAT,
                 _name, used(), percent_of(used(), total_bytes), reserved(), percent_of(used(), reserved()),
                 p2i(_base + MetaspaceShared::final_delta()));
}

void DumpRegion::print_out_of_space_msg(const char* failing_region, size_t needed_bytes) {
  log_error(cds)("[%-8s] " PTR_FORMAT " - " PTR_FORMAT " capacity =%9d, allocated =%9d",
                 _name, p2i(_base), p2i(_top), int(_end - _base), int(_top - _base));
  if (strcmp(_name, failing_region) == 0) {
    log_error(cds)(" required = %d", int(needed_bytes));
  }
}

void DumpRegion::init(ReservedSpace* rs, VirtualSpace* vs) {
  _rs = rs;
  _vs = vs;
  // Start with 0 committed bytes. The memory will be committed as needed by
  // MetaspaceShared::commit_to().
  if (!_vs->initialize(*_rs, 0)) {
    fatal("Unable to allocate memory for shared space");
  }
  _base = _top = _rs->base();
  _end = _rs->end();
}

void DumpRegion::pack(DumpRegion* next) {
  assert(!is_packed(), "sanity");
  _end = (char*)align_up(_top, MetaspaceShared::reserved_space_alignment());
  _is_packed = true;
  if (next != NULL) {
    next->_rs = _rs;
    next->_vs = _vs;
    next->_base = next->_top = this->_end;
    next->_end = _rs->end();
  }
}

void WriteClosure::do_oop(oop* o) {
  if (*o == NULL) {
    _dump_region->append_intptr_t(0);
  } else {
    assert(HeapShared::is_heap_object_archiving_allowed(),
           "Archiving heap object is not allowed");
    _dump_region->append_intptr_t(
      (intptr_t)CompressedOops::encode_not_null(*o));
  }
}

void WriteClosure::do_region(u_char* start, size_t size) {
  assert((intptr_t)start % sizeof(intptr_t) == 0, "bad alignment");
  assert(size % sizeof(intptr_t) == 0, "bad size");
  do_tag((int)size);
  while (size > 0) {
    _dump_region->append_intptr_t(*(intptr_t*)start, true);
    start += sizeof(intptr_t);
    size -= sizeof(intptr_t);
  }
}

void ReadClosure::do_ptr(void** p) {
  assert(*p == NULL, "initializing previous initialized pointer.");
  intptr_t obj = nextPtr();
  assert((intptr_t)obj >= 0 || (intptr_t)obj < -100,
         "hit tag while initializing ptrs.");
  *p = (void*)obj;
}

void ReadClosure::do_u4(u4* p) {
  intptr_t obj = nextPtr();
  *p = (u4)(uintx(obj));
}

void ReadClosure::do_bool(bool* p) {
  intptr_t obj = nextPtr();
  *p = (bool)(uintx(obj));
}

void ReadClosure::do_tag(int tag) {
  int old_tag;
  old_tag = (int)(intptr_t)nextPtr();
  // do_int(&old_tag);
  assert(tag == old_tag, "old tag doesn't match");
  FileMapInfo::assert_mark(tag == old_tag);
}

void ReadClosure::do_oop(oop *p) {
  narrowOop o = CompressedOops::narrow_oop_cast(nextPtr());
  if (CompressedOops::is_null(o) || !HeapShared::open_archive_heap_region_mapped()) {
    *p = NULL;
  } else {
    assert(HeapShared::is_heap_object_archiving_allowed(),
           "Archived heap object is not allowed");
    assert(HeapShared::open_archive_heap_region_mapped(),
           "Open archive heap region is not mapped");
    *p = HeapShared::decode_from_archive(o);
  }
}

void ReadClosure::do_region(u_char* start, size_t size) {
  assert((intptr_t)start % sizeof(intptr_t) == 0, "bad alignment");
  assert(size % sizeof(intptr_t) == 0, "bad size");
  do_tag((int)size);
  while (size > 0) {
    *(intptr_t*)start = nextPtr();
    start += sizeof(intptr_t);
    size -= sizeof(intptr_t);
  }
}
