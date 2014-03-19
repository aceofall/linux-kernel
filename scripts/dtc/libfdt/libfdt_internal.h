#ifndef _LIBFDT_INTERNAL_H
#define _LIBFDT_INTERNAL_H
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
#include <fdt.h>

// KID 20140314
#define FDT_ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))
// KID 20140314
// FDT_TAGSIZE: 4
#define FDT_TAGALIGN(x)		(FDT_ALIGN((x), FDT_TAGSIZE))

// KID 20140314
// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임
#define FDT_CHECK_HEADER(fdt) \
	{ \
		int err; \
		if ((err = fdt_check_header(fdt)) != 0) \
			return err; \
	}

int _fdt_check_node_offset(const void *fdt, int offset);
int _fdt_check_prop_offset(const void *fdt, int offset);
const char *_fdt_find_string(const char *strtab, int tabsize, const char *s);
int _fdt_node_end_offset(void *fdt, int nodeoffset);

// KID 20140314
// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임, offset: 0
static inline const void *_fdt_offset_ptr(const void *fdt, int offset)
{
	// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임, 
	// fdt_off_dt_struct(fdt): 0x38, offset: 0
	return (const char *)fdt + fdt_off_dt_struct(fdt) + offset;
	// return fdt의 시작위치 + 0x38 + offset : dt_struct 시작 위치
}

static inline void *_fdt_offset_ptr_w(void *fdt, int offset)
{
	return (void *)(uintptr_t)_fdt_offset_ptr(fdt, offset);
}

// KID 20140314
// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임, n: 0
static inline const struct fdt_reserve_entry *_fdt_mem_rsv(const void *fdt, int n)
{
	// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임
	// fdt_off_mem_rsvmap(fdt): 0x28
	const struct fdt_reserve_entry *rsv_table =
		(const struct fdt_reserve_entry *)
		((const char *)fdt + fdt_off_mem_rsvmap(fdt));
	// rsv_table: memory reserve map 정보가 있는 위치

	return rsv_table + n;
	// return memory reserve map 정보가 있는 위치 + 0
}
static inline struct fdt_reserve_entry *_fdt_mem_rsv_w(void *fdt, int n)
{
	return (void *)(uintptr_t)_fdt_mem_rsv(fdt, n);
}

// KID 20140314
// FDT_SW_MAGIC: 0x2ff20112
#define FDT_SW_MAGIC		(~FDT_MAGIC)

#endif /* _LIBFDT_INTERNAL_H */
