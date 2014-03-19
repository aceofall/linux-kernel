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

// KID 20140319
// fdt: fdt 시작위치, offset: 404, name: "memory", namelen: 6
static int _fdt_nodename_eq(const void *fdt, int offset,
			    const char *s, int len)
{
	// fdt: fdt 시작위치, offset: 404, len: 6, FDT_TAGSIZE: 4
	const char *p = fdt_offset_ptr(fdt, offset + FDT_TAGSIZE, len+1);
	// p: fdt 시작위치 + 0x1d0

	if (! p)
		/* short match */
		return 0;

	// p: "memory", s: "memory", len: 6
	if (memcmp(p, s, len) != 0)
		return 0;

	// p[6]: '\0'
	if (p[len] == '\0')
		return 1;
		// return 1
	else if (!memchr(s, '@', len) && (p[len] == '@'))
		return 1;
	else
		return 0;
}

// KID 20140319
// fdt: _edata (data영역의 끝 위치), stroffset: 0
const char *fdt_string(const void *fdt, int stroffset)
{
	// fdt: _edata (data영역의 끝 위치), fdt_off_dt_strings(fdt): 0x30ac, stroffset: 0
	return (const char *)fdt + fdt_off_dt_strings(fdt) + stroffset;
	// fdt: fdt시작위치 + 0x30ac
}

// KID 20140319
// fdt: _edata (data영역의 끝 위치), fdt32_to_cpu(prop->nameoff): 0
// name: "#size-cells", namelen: 11
// fdt: _edata (data영역의 끝 위치), fdt32_to_cpu(prop->nameoff): 0xf
// name: "#size-cells", namelen: 11
static int _fdt_string_eq(const void *fdt, int stroffset,
			  const char *s, int len)
{
	// fdt: _edata (data영역의 끝 위치), stroffset: 0
	// fdt: _edata (data영역의 끝 위치), stroffset: 0xf
	const char *p = fdt_string(fdt, stroffset);
	// p: fdt시작위치 + 0x30ac
	// p: fdt시작위치 + 0x30ac + 0xf

	// strlen(p): 14, len: 11, p: "#address-cells", s: "#size-cells"
	// strlen(p): 11, len: 11, p: "#size-cells", s: "#size-cells"
	return (strlen(p) == len) && (memcmp(p, s, len) == 0);
	// return 0
	// return 1
}

int fdt_get_mem_rsv(const void *fdt, int n, uint64_t *address, uint64_t *size)
{
	FDT_CHECK_HEADER(fdt);
	*address = fdt64_to_cpu(_fdt_mem_rsv(fdt, n)->address);
	*size = fdt64_to_cpu(_fdt_mem_rsv(fdt, n)->size);
	return 0;
}

// KID 20140314
// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임
int fdt_num_mem_rsv(const void *fdt)
{
	int i = 0;

	// fdt: _edata: data영역의 끝 위치이며 fdt의 시작위치임
	// _fdt_mem_rsv(fdt, 0): memory reserve map 정보가 있는 위치 + 0
	// fdt64_to_cpu(_fdt_mem_rsv(fdt, i)->size: 0
	while (fdt64_to_cpu(_fdt_mem_rsv(fdt, i)->size) != 0)
		i++;
	// i: 0
	return i;
	// return 0
}

// KID 20140319
// fdt: _edata (data영역의 끝 위치), offset: 8
static int _nextprop(const void *fdt, int offset)
{
	uint32_t tag;
	int nextoffset;

	do {
		// fdt: _edata (data영역의 끝 위치), offset: 8
		tag = fdt_next_tag(fdt, offset, &nextoffset);
		// tag: 0x3, nextoffset: 32

		switch (tag) {
		case FDT_END: // FDT_END: 0x9
			if (nextoffset >= 0)
				return -FDT_ERR_BADSTRUCTURE;
			else
				return nextoffset;

		case FDT_PROP: // FDT_PROP: 0x3
			// offset: 8
			return offset;
			// return 8
		}
		offset = nextoffset;
	} while (tag == FDT_NOP);

	return -FDT_ERR_NOTFOUND;
}

// KID 20140319
// fdt: fdt 시작위치, offset: 0, p: "memory", q: path 문자열의 null 문자의 주소, q-p: 6
int fdt_subnode_offset_namelen(const void *fdt, int offset,
			       const char *name, int namelen)
{
	int depth;

	FDT_CHECK_HEADER(fdt);

	// fdt: fdt 시작위치, offset: 0, depth: 0
	for (depth = 0;
	     (offset >= 0) && (depth >= 0);
	     offset = fdt_next_node(fdt, offset, &depth))
		// fdt의 begin node tag를 찾고 offset을 구함
		// ...(계속 루프를 돌며 값계산)
		// offset: 404 (memory), depth: 1

		// depth: 1, offset: 404, name: "memory", namelen: 6
		if ((depth == 1)
		    && _fdt_nodename_eq(fdt, offset, name, namelen))
			// offset: 404
			return offset;
			// return 404

	if (depth < 0)
		return -FDT_ERR_NOTFOUND;
	return offset; /* error */
}

int fdt_subnode_offset(const void *fdt, int parentoffset,
		       const char *name)
{
	return fdt_subnode_offset_namelen(fdt, parentoffset, name, strlen(name));
}

// KID 20140318
// fdt: _edata (data영역의 끝 위치), node_path: "/"
// fdt: fdt 시작위치, node_path: "/memory"
int fdt_path_offset(const void *fdt, const char *path)
{
	// path: "/", strlen(path): 1
	// path: "/memory", strlen(path): 7
	const char *end = path + strlen(path);
	// end: path 문자열의 null 문자의 주소
	// end: path 문자열의 null 문자의 주소

	// path: "/"
	// path: "/memory"
	const char *p = path;
	// p: "/"
	// p: "/memory"
	int offset = 0;

	FDT_CHECK_HEADER(fdt);

	/* see if we have an alias */
	// *path: '/'
	// *path: '/'
	if (*path != '/') {
		const char *q = strchr(path, '/');

		if (!q)
			q = end;

		p = fdt_get_alias_namelen(fdt, p, q - p);
		if (!p)
			return -FDT_ERR_BADPATH;
		offset = fdt_path_offset(fdt, p);

		p = q;
	}

	// *p: '/'
	while (*p) {
		const char *q;

		// *p: '/'
		// *p: '/'
		while (*p == '/')
			p++;

		// *p: NULL
		// *p: 'm'
		if (! *p)
			// offset: 0
			return offset;
			// return 0

		// p: "memory"
		q = strchr(p, '/');
		// q: NULL

		if (! q)
			// q: NULL, end: path 문자열의 null 문자의 주소
			q = end;
			// q: path 문자열의 null 문자의 주소

		// fdt: fdt 시작위치, offset: 0, p: "memory", q: path 문자열의 null 문자의 주소, q-p: 6
		offset = fdt_subnode_offset_namelen(fdt, offset, p, q-p);
		if (offset < 0)
			return offset;

		p = q;
	}

	return offset;
}

const char *fdt_get_name(const void *fdt, int nodeoffset, int *len)
{
	const struct fdt_node_header *nh = _fdt_offset_ptr(fdt, nodeoffset);
	int err;

	if (((err = fdt_check_header(fdt)) != 0)
	    || ((err = _fdt_check_node_offset(fdt, nodeoffset)) < 0))
			goto fail;

	if (len)
		*len = strlen(nh->name);

	return nh->name;

 fail:
	if (len)
		*len = err;
	return NULL;
}

// KID 20140319
// fdt: _edata (data영역의 끝 위치), offset: 0
int fdt_first_property_offset(const void *fdt, int nodeoffset)
{
	int offset;

	// fdt: _edata (data영역의 끝 위치), nodeoffset: 0
	if ((offset = _fdt_check_node_offset(fdt, nodeoffset)) < 0)
		return offset;
	// offset: 8

	// fdt: _edata (data영역의 끝 위치), offset: 8, _nextprop(fdt, 8): 8
	return _nextprop(fdt, offset);
	// return 8
}

// KID 20140319
// fdt: _edata (data영역의 끝 위치), offset: 8
int fdt_next_property_offset(const void *fdt, int offset)
{
	// fdt: _edata (data영역의 끝 위치), offset: 8
	// _fdt_check_prop_offset(fdt, 8): 32
	if ((offset = _fdt_check_prop_offset(fdt, offset)) < 0)
		return offset;
	// offset: 24

	// _nextprop(fdt, 24): 24
	return _nextprop(fdt, offset);
	// return 24
}

// KID 20140319
// fdt: _edata (data영역의 끝 위치), offset: 8
const struct fdt_property *fdt_get_property_by_offset(const void *fdt,
						      int offset,
						      int *lenp)
{
	int err;
	const struct fdt_property *prop;

	// fdt: _edata (data영역의 끝 위치), offset: 8, _fdt_check_prop_offset(fdt, 8): 32
	if ((err = _fdt_check_prop_offset(fdt, offset)) < 0) {
		if (lenp)
			*lenp = err;
		return NULL;
	}

	// fdt: _edata (data영역의 끝 위치), offset: 8
	prop = _fdt_offset_ptr(fdt, offset);
	// prop: fdt시작위치 + 0x40

	if (lenp)
		// fdt32_to_cpu(prop->len): 0x4
		*lenp = fdt32_to_cpu(prop->len);
		// *lenp: 0x4

	// prop: fdt시작위치 + 0x40
	return prop;
	// return fdt시작위치 + 0x40
}

// KID 20140319
// fdt: _edata (data영역의 끝 위치), nodeoffset: 0, name: "#size-cells", namelen: 11
const struct fdt_property *fdt_get_property_namelen(const void *fdt,
						    int offset,
						    const char *name,
						    int namelen, int *lenp)
{
	// fdt: _edata (data영역의 끝 위치), offset: 0
	// fdt_first_property_offset(fdt, 0): 8
	for (offset = fdt_first_property_offset(fdt, offset);
	     (offset >= 0);
	     (offset = fdt_next_property_offset(fdt, offset))) {
		// [2nd] fdt_next_property_offset(fdt, 8): 24
		const struct fdt_property *prop;

		// [1st] fdt: _edata (data영역의 끝 위치), offset: 8
		// [1st] fdt_get_property_by_offset(fdt, 8, lenp): fdt시작위치 + 0x40
		// [2nd] fdt: _edata (data영역의 끝 위치), offset: 24
		// [2nd] fdt_get_property_by_offset(fdt, 24, lenp): fdt시작위치 + 0x50
		if (!(prop = fdt_get_property_by_offset(fdt, offset, lenp))) {
			offset = -FDT_ERR_INTERNAL;
			break;
		}
		// [1st] prop: fdt시작위치 + 0x40, *lenp: 0x4
		// [2nd] prop: fdt시작위치 + 0x50, *lenp: 0x4

		// [1st] fdt: _edata (data영역의 끝 위치), fdt32_to_cpu(prop->nameoff): 0
		// [1st] name: "#size-cells", namelen: 11
		// [1st] _fdt_string_eq(fdt, 0, "#size-cells", 11): 0
		// [2nd] fdt: _edata (data영역의 끝 위치), fdt32_to_cpu(prop->nameoff): 0xf
		// [2nd] name: "#size-cells", namelen: 11
		// [2nd] _fdt_string_eq(fdt, 0xf, "#size-cells", 11): 1
		if (_fdt_string_eq(fdt, fdt32_to_cpu(prop->nameoff),
				   name, namelen))
			// [2nd] prop: fdt시작위치 + 0x50, *lenp: 0x4
			return prop;
	}

	if (lenp)
		*lenp = offset;
	return NULL;
}

// KID 20140319
// fdt: fdt 시작위치, nodeoffset: 404, name: "reg", &oldlen
const struct fdt_property *fdt_get_property(const void *fdt,
					    int nodeoffset,
					    const char *name, int *lenp)
{
	// fdt: fdt 시작위치, nodeoffset: 404, name: "reg", strlen(name): 3, &oldlen
	// fdt_get_property_namelen(ftd, 404, "reg", 3, lenp): fdt 시작위치 + 0x1ec, *lenp: 8
	return fdt_get_property_namelen(fdt, nodeoffset, name,
					strlen(name), lenp);
	// return fdt 시작위치 + 0x1ec
}

// KID 20140319
// fdt: _edata (data영역의 끝 위치), nodeoffset: 0, name: "#size-cells", strlen(name): 11
const void *fdt_getprop_namelen(const void *fdt, int nodeoffset,
				const char *name, int namelen, int *lenp)
{
	const struct fdt_property *prop;

	// fdt: _edata (data영역의 끝 위치), nodeoffset: 0, name: "#size-cells", namelen: 11
	prop = fdt_get_property_namelen(fdt, nodeoffset, name, namelen, lenp);
	// prop: fdt시작위치 + 0x50, *lenp: 0x4

	if (! prop)
		return NULL;
	// prop->data: fdt시작위치 + 0x50 + 12
	return prop->data;
	// return fdt시작위치 + 0x5C
}

const void *fdt_getprop_by_offset(const void *fdt, int offset,
				  const char **namep, int *lenp)
{
	const struct fdt_property *prop;

	prop = fdt_get_property_by_offset(fdt, offset, lenp);
	if (!prop)
		return NULL;
	if (namep)
		*namep = fdt_string(fdt, fdt32_to_cpu(prop->nameoff));
	return prop->data;
}

// KID 20140319
// fdt: _edata (data영역의 끝 위치), offset: 0, property: "#size-cells"
const void *fdt_getprop(const void *fdt, int nodeoffset,
			const char *name, int *lenp)
{
	// fdt: _edata (data영역의 끝 위치), nodeoffset: 0, name: "#size-cells", strlen(name): 11
	// fdt_getprop_namelen(fdt, 0, "#size-cells", 11, lenp): fdt시작위치 + 0x50
	return fdt_getprop_namelen(fdt, nodeoffset, name, strlen(name), lenp);
	// return fdt시작위치 + 0x5C
}

uint32_t fdt_get_phandle(const void *fdt, int nodeoffset)
{
	const uint32_t *php;
	int len;

	/* FIXME: This is a bit sub-optimal, since we potentially scan
	 * over all the properties twice. */
	php = fdt_getprop(fdt, nodeoffset, "phandle", &len);
	if (!php || (len != sizeof(*php))) {
		php = fdt_getprop(fdt, nodeoffset, "linux,phandle", &len);
		if (!php || (len != sizeof(*php)))
			return 0;
	}

	return fdt32_to_cpu(*php);
}

const char *fdt_get_alias_namelen(const void *fdt,
				  const char *name, int namelen)
{
	int aliasoffset;

	aliasoffset = fdt_path_offset(fdt, "/aliases");
	if (aliasoffset < 0)
		return NULL;

	return fdt_getprop_namelen(fdt, aliasoffset, name, namelen, NULL);
}

const char *fdt_get_alias(const void *fdt, const char *name)
{
	return fdt_get_alias_namelen(fdt, name, strlen(name));
}

int fdt_get_path(const void *fdt, int nodeoffset, char *buf, int buflen)
{
	int pdepth = 0, p = 0;
	int offset, depth, namelen;
	const char *name;

	FDT_CHECK_HEADER(fdt);

	if (buflen < 2)
		return -FDT_ERR_NOSPACE;

	for (offset = 0, depth = 0;
	     (offset >= 0) && (offset <= nodeoffset);
	     offset = fdt_next_node(fdt, offset, &depth)) {
		while (pdepth > depth) {
			do {
				p--;
			} while (buf[p-1] != '/');
			pdepth--;
		}

		if (pdepth >= depth) {
			name = fdt_get_name(fdt, offset, &namelen);
			if (!name)
				return namelen;
			if ((p + namelen + 1) <= buflen) {
				memcpy(buf + p, name, namelen);
				p += namelen;
				buf[p++] = '/';
				pdepth++;
			}
		}

		if (offset == nodeoffset) {
			if (pdepth < (depth + 1))
				return -FDT_ERR_NOSPACE;

			if (p > 1) /* special case so that root path is "/", not "" */
				p--;
			buf[p] = '\0';
			return 0;
		}
	}

	if ((offset == -FDT_ERR_NOTFOUND) || (offset >= 0))
		return -FDT_ERR_BADOFFSET;
	else if (offset == -FDT_ERR_BADOFFSET)
		return -FDT_ERR_BADSTRUCTURE;

	return offset; /* error from fdt_next_node() */
}

int fdt_supernode_atdepth_offset(const void *fdt, int nodeoffset,
				 int supernodedepth, int *nodedepth)
{
	int offset, depth;
	int supernodeoffset = -FDT_ERR_INTERNAL;

	FDT_CHECK_HEADER(fdt);

	if (supernodedepth < 0)
		return -FDT_ERR_NOTFOUND;

	for (offset = 0, depth = 0;
	     (offset >= 0) && (offset <= nodeoffset);
	     offset = fdt_next_node(fdt, offset, &depth)) {
		if (depth == supernodedepth)
			supernodeoffset = offset;

		if (offset == nodeoffset) {
			if (nodedepth)
				*nodedepth = depth;

			if (supernodedepth > depth)
				return -FDT_ERR_NOTFOUND;
			else
				return supernodeoffset;
		}
	}

	if ((offset == -FDT_ERR_NOTFOUND) || (offset >= 0))
		return -FDT_ERR_BADOFFSET;
	else if (offset == -FDT_ERR_BADOFFSET)
		return -FDT_ERR_BADSTRUCTURE;

	return offset; /* error from fdt_next_node() */
}

int fdt_node_depth(const void *fdt, int nodeoffset)
{
	int nodedepth;
	int err;

	err = fdt_supernode_atdepth_offset(fdt, nodeoffset, 0, &nodedepth);
	if (err)
		return (err < 0) ? err : -FDT_ERR_INTERNAL;
	return nodedepth;
}

int fdt_parent_offset(const void *fdt, int nodeoffset)
{
	int nodedepth = fdt_node_depth(fdt, nodeoffset);

	if (nodedepth < 0)
		return nodedepth;
	return fdt_supernode_atdepth_offset(fdt, nodeoffset,
					    nodedepth - 1, NULL);
}

int fdt_node_offset_by_prop_value(const void *fdt, int startoffset,
				  const char *propname,
				  const void *propval, int proplen)
{
	int offset;
	const void *val;
	int len;

	FDT_CHECK_HEADER(fdt);

	/* FIXME: The algorithm here is pretty horrible: we scan each
	 * property of a node in fdt_getprop(), then if that didn't
	 * find what we want, we scan over them again making our way
	 * to the next node.  Still it's the easiest to implement
	 * approach; performance can come later. */
	for (offset = fdt_next_node(fdt, startoffset, NULL);
	     offset >= 0;
	     offset = fdt_next_node(fdt, offset, NULL)) {
		val = fdt_getprop(fdt, offset, propname, &len);
		if (val && (len == proplen)
		    && (memcmp(val, propval, len) == 0))
			return offset;
	}

	return offset; /* error from fdt_next_node() */
}

int fdt_node_offset_by_phandle(const void *fdt, uint32_t phandle)
{
	int offset;

	if ((phandle == 0) || (phandle == -1))
		return -FDT_ERR_BADPHANDLE;

	FDT_CHECK_HEADER(fdt);

	/* FIXME: The algorithm here is pretty horrible: we
	 * potentially scan each property of a node in
	 * fdt_get_phandle(), then if that didn't find what
	 * we want, we scan over them again making our way to the next
	 * node.  Still it's the easiest to implement approach;
	 * performance can come later. */
	for (offset = fdt_next_node(fdt, -1, NULL);
	     offset >= 0;
	     offset = fdt_next_node(fdt, offset, NULL)) {
		if (fdt_get_phandle(fdt, offset) == phandle)
			return offset;
	}

	return offset; /* error from fdt_next_node() */
}

static int _fdt_stringlist_contains(const char *strlist, int listlen,
				    const char *str)
{
	int len = strlen(str);
	const char *p;

	while (listlen >= len) {
		if (memcmp(str, strlist, len+1) == 0)
			return 1;
		p = memchr(strlist, '\0', listlen);
		if (!p)
			return 0; /* malformed strlist.. */
		listlen -= (p-strlist) + 1;
		strlist = p + 1;
	}
	return 0;
}

int fdt_node_check_compatible(const void *fdt, int nodeoffset,
			      const char *compatible)
{
	const void *prop;
	int len;

	prop = fdt_getprop(fdt, nodeoffset, "compatible", &len);
	if (!prop)
		return len;
	if (_fdt_stringlist_contains(prop, len, compatible))
		return 0;
	else
		return 1;
}

int fdt_node_offset_by_compatible(const void *fdt, int startoffset,
				  const char *compatible)
{
	int offset, err;

	FDT_CHECK_HEADER(fdt);

	/* FIXME: The algorithm here is pretty horrible: we scan each
	 * property of a node in fdt_node_check_compatible(), then if
	 * that didn't find what we want, we scan over them again
	 * making our way to the next node.  Still it's the easiest to
	 * implement approach; performance can come later. */
	for (offset = fdt_next_node(fdt, startoffset, NULL);
	     offset >= 0;
	     offset = fdt_next_node(fdt, offset, NULL)) {
		err = fdt_node_check_compatible(fdt, offset, compatible);
		if ((err < 0) && (err != -FDT_ERR_NOTFOUND))
			return err;
		else if (err == 0)
			return offset;
	}

	return offset; /* error from fdt_next_node() */
}
