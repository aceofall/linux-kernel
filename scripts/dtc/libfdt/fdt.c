/*
 * libfdt - Flat Device Tree manipulation
 * Copyright (C) 2006 David Gibson, IBM Corporation.
 *
 * libfdt is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 *
 *  a) This library is free software; you can redistribute it and/or
 *     modify it under the terms of the GNU General Public License as
 *     published by the Free Software Foundation; either version 2 of the
 *     License, or (at your option) any later version.
 *
 *     This library is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public
 *     License along with this library; if not, write to the Free
 *     Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
 *     MA 02110-1301 USA
 *
 * Alternatively,
 *
 *  b) Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *     1. Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *     2. Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *     CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *     INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *     MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *     DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 *     CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *     SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *     NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *     HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *     CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *     OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 *     EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "libfdt_env.h"

#include <fdt.h>
#include <libfdt.h>

#include "libfdt_internal.h"

// KID 20140314
// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임
int fdt_check_header(const void *fdt)
{
	// fdt_magic(fdt): 0xd00dfeed, FDT_MAGIC: 0xd00dfeed
	// FDT_SW_MAGIC: 0x2ff20112
	if (fdt_magic(fdt) == FDT_MAGIC) {
		/* Complete tree */
		// fdt_version(fdt): 0x11, FDT_FIRST_SUPPORTED_VERSION: 0x10
		// FDT_ERR_BADVERSION: 10
		if (fdt_version(fdt) < FDT_FIRST_SUPPORTED_VERSION)
			return -FDT_ERR_BADVERSION;
		// fdt_last_comp_version(fdt): 0x10, FDT_LAST_SUPPORTED_VERSION: 0x11
		if (fdt_last_comp_version(fdt) > FDT_LAST_SUPPORTED_VERSION)
			return -FDT_ERR_BADVERSION;
	} else if (fdt_magic(fdt) == FDT_SW_MAGIC) {
		/* Unfinished sequential-write blob */
		if (fdt_size_dt_struct(fdt) == 0)
			return -FDT_ERR_BADSTATE;
	} else {
		return -FDT_ERR_BADMAGIC;
	}

	return 0;
	// return 0
}

// KID 20140314
// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임, offset: 0, FDT_TAGSIZE: 4
// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임, offset: 4, 1
// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임, startoffset: 0, offset: 5
const void *fdt_offset_ptr(const void *fdt, int offset, unsigned int len)
{
	const char *p;

	// fdt_version(fdt): 0x11
	if (fdt_version(fdt) >= 0x11)
		// offset: 0, len: 4, fdt_size_dt_struct(fdt): 0x38
		if (((offset + len) < offset)
		    || ((offset + len) > fdt_size_dt_struct(fdt)))
			return NULL;

	// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임, offset: 0
	// _fdt_offset_ptr(fdt, 0): dt_struct 시작 위치
	p = _fdt_offset_ptr(fdt, offset);
	// p: dt_struct 시작 위치

	// len: 4
	if (p + len < p)
		return NULL;

	// p: dt_struct 시작 위치
	return p;
	// return dt_struct 시작 위치
}

// KID 20140314
// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임, 0, &struct_size
uint32_t fdt_next_tag(const void *fdt, int startoffset, int *nextoffset)
{
	const uint32_t *tagp, *lenp;
	uint32_t tag;
	// startoffset: 0
	int offset = startoffset;
	// offset: 0
	const char *p;

	// nextoffset: &struct_size, FDT_ERR_TRUNCATED: 8
	*nextoffset = -FDT_ERR_TRUNCATED;
	// *nextoffset: -8

	// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임, offset: 0, FDT_TAGSIZE: 4
	// fdt_offset_ptr(fdt, 0, 4): dt_struct 시작 위치
	tagp = fdt_offset_ptr(fdt, offset, FDT_TAGSIZE);
	// tagp: dt_struct 시작 위치

	if (!tagp)
		return FDT_END; /* premature end */

	// tagp: dt_struct 시작 위치
	tag = fdt32_to_cpu(*tagp);
	// tag: 0x00000001

	// offset: 0, FDT_TAGSIZE: 4
	offset += FDT_TAGSIZE;
	// offset: 4

	// *nextoffset: -8, FDT_ERR_BADSTRUCTURE: 11
	*nextoffset = -FDT_ERR_BADSTRUCTURE;
	// *nextoffset: -11

	// tag: 0x00000001
	switch (tag) {
	case FDT_BEGIN_NODE: // FDT_BEGIN_NODE: 0x1
		/* skip name */
		do {
			// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임, offset: 4
			p = fdt_offset_ptr(fdt, offset++, 1);
			// offset: 5
		} while (p && (*p != '\0'));
		// 현재 노드의 name string 부분 만큼 offset 이동

		if (!p)
			return FDT_END; /* premature end */
		break;

	case FDT_PROP: // FDT_PROP: 0x3
		lenp = fdt_offset_ptr(fdt, offset, sizeof(*lenp));
		if (!lenp)
			return FDT_END; /* premature end */
		/* skip-name offset, length and value */
		offset += sizeof(struct fdt_property) - FDT_TAGSIZE
			+ fdt32_to_cpu(*lenp);
		break;

	case FDT_END: // FDT_END: 0x9
	case FDT_END_NODE: // FDT_END_NODE: 0x2
	case FDT_NOP: // FDT_NOP: 0x4
		break;

	default:
		return FDT_END;
	}

	// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임, startoffset: 0, offset: 5
	// fdt_offset_ptr(fdt, 0, 5): NULL아닌 값
	if (!fdt_offset_ptr(fdt, startoffset, offset - startoffset))
		return FDT_END; /* premature end */

	// offset: 5, FDT_TAGALIGN(5): 8
	*nextoffset = FDT_TAGALIGN(offset);
	// *nextoffset: 8

	// tag: 0x00000001
	return tag;
	// return 0x00000001
}

int _fdt_check_node_offset(const void *fdt, int offset)
{
	if ((offset < 0) || (offset % FDT_TAGSIZE)
	    || (fdt_next_tag(fdt, offset, &offset) != FDT_BEGIN_NODE))
		return -FDT_ERR_BADOFFSET;

	return offset;
}

// KID 20140318
// fdt: _edata (data영역의 끝 위치), offset: 0
int _fdt_check_prop_offset(const void *fdt, int offset)
{
	// offset: 0, FDT_TAGSIZE: 4, FDT_PROP: 0x3
	if ((offset < 0) || (offset % FDT_TAGSIZE)
	    || (fdt_next_tag(fdt, offset, &offset) != FDT_PROP))
		return -FDT_ERR_BADOFFSET;

	return offset;
}

int fdt_next_node(const void *fdt, int offset, int *depth)
{
	int nextoffset = 0;
	uint32_t tag;

	if (offset >= 0)
		if ((nextoffset = _fdt_check_node_offset(fdt, offset)) < 0)
			return nextoffset;

	do {
		offset = nextoffset;
		tag = fdt_next_tag(fdt, offset, &nextoffset);

		switch (tag) {
		case FDT_PROP:
		case FDT_NOP:
			break;

		case FDT_BEGIN_NODE:
			if (depth)
				(*depth)++;
			break;

		case FDT_END_NODE:
			if (depth && ((--(*depth)) < 0))
				return nextoffset;
			break;

		case FDT_END:
			if ((nextoffset >= 0)
			    || ((nextoffset == -FDT_ERR_TRUNCATED) && !depth))
				return -FDT_ERR_NOTFOUND;
			else
				return nextoffset;
		}
	} while (tag != FDT_BEGIN_NODE);

	return offset;
}

const char *_fdt_find_string(const char *strtab, int tabsize, const char *s)
{
	int len = strlen(s) + 1;
	const char *last = strtab + tabsize - len;
	const char *p;

	for (p = strtab; p <= last; p++)
		if (memcmp(p, s, len) == 0)
			return p;
	return NULL;
}

// KID 20140318
// fdt: fdt의 시작위치, buf: _edata (data영역의 끝 위치),
// bufsize: sp - _edata (실제로 사용할 수 있는 memory 공간)
int fdt_move(const void *fdt, void *buf, int bufsize)
{
	// fdt: fdt의 시작위치
	FDT_CHECK_HEADER(fdt);

	// fdt_totalsize(fdt): 0x3236, bufsize: 64K - _edata
	if (fdt_totalsize(fdt) > bufsize)
		return -FDT_ERR_NOSPACE;

	// fdt: fdt의 시작위치, buf: _edata (data영역의 끝 위치), fdt_totalsize(fdt): 0x3236
	memmove(buf, fdt, fdt_totalsize(fdt));
	// _edata (data영역의 끝 위치)에 fdt를 이동시킴
	return 0;
}
