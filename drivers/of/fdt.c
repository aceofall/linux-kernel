/*
 * Functions for working with the Flattened Device Tree data format
 *
 * Copyright 2009 Benjamin Herrenschmidt, IBM Corp
 * benh@kernel.crashing.org
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/initrd.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/slab.h>

#include <asm/setup.h>  /* for COMMAND_LINE_SIZE */
#ifdef CONFIG_PPC
#include <asm/machdep.h>
#endif /* CONFIG_PPC */

#include <asm/page.h>

// ARM10C 20131005
// ARM10C 20140208
// blob : DTB 시작 주소, offset : property 이름의 offset
// KID 20140213
// blob: devtree의 주소, noff: 현재 offset위치의 property의  name offset 값 (property name)
char *of_fdt_get_string(struct boot_param_header *blob, u32 offset)
{
	return ((char *)blob) +
		be32_to_cpu(blob->off_dt_strings) + offset;
	// devtree 주소를 기준으로 devtree string offset 시작위치에서 name offset 위치만큼 
	// 떨이진  곳에 존재하는 string 주소를 리턴
}

/**
 * of_fdt_get_property - Given a node in the given flat blob, return
 * the property ptr
 */
// ARM10C 20131005
// KID 20140213
// blob: devtree의 주소, node: devtree의 dt_struct의 위치 (주소),"compatible", &cplen
void *of_fdt_get_property(struct boot_param_header *blob,
		       unsigned long node, const char *name,
		       unsigned long *size)
{
	unsigned long p = node;
        // p: node 주소 할당

	do {
		u32 tag = be32_to_cpup((__be32 *)p);
                // tag: node의 현재 주소의 값을 big endian으로 변환하여 할당
		u32 sz, noff;
		const char *nstr;

		p += 4;
                // tag의 주소만큼 offset 이동

		// OF_DT_NOP: 0x4
		if (tag == OF_DT_NOP)
			continue;
			// tag가 OF_DT_NOP면 skip

		// OF_DT_PROP: 0x3
		if (tag != OF_DT_PROP)
			return NULL;
			// tag가 OF_DT_PROP가 아니면  NULL 리턴 
		// tag가 OF_DT_PROP으로 확인
		
		sz = be32_to_cpup((__be32 *)p);
		// sz: 현재 offset위치의 property의  size 값
		
		noff = be32_to_cpup((__be32 *)(p + 4));
		// noff: 현재 offset위치의 property의  name offset 값 (property name)
		
		p += 8;
		// property의 size 값, name offset 값의 주소만큼 offset 이동

		// be32_to_cpu(blob->version): devtree의 version 정보 - 0x11
		// devtree의 주소에서 20byte 떨어진 위치에 version 정보 값이 있음
		if (be32_to_cpu(blob->version) < 0x10)
			p = ALIGN(p, sz >= 8 ? 8 : 4);

		// blob: devtree의 주소, noff: 현재 offset위치의 property의  name offset 값 (property name)
		nstr = of_fdt_get_string(blob, noff);
		// nstr: property의 name string 주소

		if (nstr == NULL) {
			pr_warning("Can't find property index name !\n");
			return NULL;
		}

		// name: "compatible", nstr: property의 name string 주소
		if (strcmp(name, nstr) == 0) {
			// "compatible" 문자열을 찾음
			if (size)
				*size = sz;
				// "compatible"  property의 size  값을 저장

			return (void *)p;
			// "compatible" property를 찾은 이후에 devtree의 offset 위치를 return
			//  p: "compatible" property의 값의 주소임
		}
		p += sz;
		// "compatible" 못찾음, property의 size값 만큼 devtree의 offset 위치를 이동

		p = ALIGN(p, 4);
		// property의 size이 4byte align 되로록 함
	} while (1);
}

/**
 * of_fdt_is_compatible - Return true if given node from the given blob has
 * compat in its compatible list
 * @blob: A device tree blob
 * @node: node to test
 * @compat: compatible string to compare with compatible list.
 *
 * On match, returns a non-zero value with smaller values returned for more
 * specific compatible values.
 */
// ARM10C 20131005
// KID 20140213
// blob: devtree의 주소, node: devtree의 dt_struct의 위치 (주소),
// compat: compatible 문자열 배열 주소 "samsung,exynos5250", "samsung,exynos5250", "samsung,exynos5250"
int of_fdt_is_compatible(struct boot_param_header *blob,
		      unsigned long node, const char *compat)
{
	const char *cp;
	unsigned long cplen, l, score = 0;

        // blob: devtree의 주소, node: devtree의 dt_struct의 위치 (주소),"compatible", &cplen
	cp = of_fdt_get_property(blob, node, "compatible", &cplen);
	// cp: "compatible" property의 값의 주소

	if (cp == NULL)
		return 0;

	// cplen: "compatible" property의 값의 size 값
	while (cplen > 0) {
		score++;
		// score: 루프롤 돈 횟수

		// devtree의 compatible string 과 target의 compatible string을 비교
		if (of_compat_cmp(cp, compat, strlen(compat)) == 0)
			return score;
			// compatible string 값이 매칭 되었을 경우의 looping count 값을 리턴, 최소 1 이상임
		l = strlen(cp) + 1;
		cp += l;
		cplen -= l;
		// compatible string이 매치 안되었을  경우에 다음 문자열로 이동
	}

	return 0;
}

/**
 * of_fdt_match - Return true if node matches a list of compatible values
 */
// KID 20140213
// initial_boot_params: devtree의 주소, node: devtree의 dt_struct의 위치 (주소),
// compat: compatible 문자열 배열 주소 "samsung,exynos5250", "samsung,exynos5250", "samsung,exynos5250"
int of_fdt_match(struct boot_param_header *blob, unsigned long node,
                 const char *const *compat)
{
	unsigned int tmp, score = 0;

	if (!compat)
		return 0;

	// target의 compatible string 이 없을때까지 찾음 
	while (*compat) {
                // blob: devtree의 주소, node: devtree의 dt_struct의 위치 (주소),
                // compat: compatible 문자열 배열 주소 "samsung,exynos5250", 
                // "samsung,exynos5250", "samsung,exynos5250"
		tmp = of_fdt_is_compatible(blob, node, *compat);
		// tmp: devtree에 target과 맞는 compatible 이  존재시 tmp값은 1이상의 값을 가짐
		// 찾지 못하면 tmp 값은 0

		if (tmp && (score == 0 || (tmp < score)))
			score = tmp;
			// 가장 낮은 score 값을 가지는 문자열을 찾음
		compat++;
	}

	return score;
	// 가장 낮은 score 값을 가지는 문자열의 인덱스를 리턴 
	// 0은 매칭되는 compatible이 없는 것을 의미함
}

// ARM10C 20140208
// mem : &mem, size : 0x3C + 0x1, align : 0x4
// [1] mem : 0x3D, sizeof(struct property) : 24, __alignof__(struct property) : 4
// [Second.root] mem : &할당받은 시작 주소, sizeof(struct device_node) + allocl : 0x3C + 0x2, __alignof__(struct device_node) : 0x4
static void *unflatten_dt_alloc(void **mem, unsigned long size,
				       unsigned long align)
{
	void *res;

	*mem = PTR_ALIGN(*mem, align);
	// *mem : 0
	// [1] *mem : 0x40
	res = *mem;
	// res : 0
	// [1] res : 0x40

	// [Second.root] res : 4바이트로 정렬된 할당 시작 주소
	*mem += size;
	// *mem : 0x3D
	// *mem : 0x58

	return res;
}

/**
 * unflatten_dt_node - Alloc and populate a device_node from the flat tree
 * @blob: The parent device tree blob
 * @mem: Memory chunk to use for allocating device nodes and properties
 * @p: pointer to node in flat tree
 * @dad: Parent struct device_node
 * @allnextpp: pointer to ->allnext from last allocated device_node
 * @fpsize: Size of the node path up at the current depth.
 */
// ARM10C 20140208
// First Pass
// [root] blob : DTB 시작 주소, mem : 0, p : &start, dad : NULL, allnextpp : NULL, fpsize : 0
// [chosen] blob : DTB 시작 주소, mem : ?, p : Node의 시작 주소, np : 0, allnextpp : NULL, fpsize : 1
// Second Pass
// [root] blob : DTB의 시작 주소, mem : size + 4 만큼 할당 받은 공간의 시작 주소,
// 	  p : &start,  dad : NULL, allnextpp : &(&of_allnodes), fpsize : 0
// [chosen] blob : DTB 시작 주소, mem : name property 다음, p : chosen Node의 시작 주소,
//	    dad : root 노드의 struct device_node 주소, allnextpp : &&of_allnodes, fpsize : 1
static void * unflatten_dt_node(struct boot_param_header *blob,
				void *mem,
				void **p,
				struct device_node *dad,
				struct device_node ***allnextpp,
				unsigned long fpsize)
{
	struct device_node *np;
	struct property *pp, **prev_pp = NULL;
	char *pathp;
	u32 tag;
	unsigned int l, allocl;
	int has_name = 0;
	int new_format = 0;
	
	// [root] p : dtb struct의 시작 주소
	// [chosen] p : chosen 노드의 시작 주소
	tag = be32_to_cpup(*p);
	// [root] tag : OF_DT_BEGIN_NODE
	// [chosen] tag : OF_DT_BEGIN_NODE

	if (tag != OF_DT_BEGIN_NODE) {
		pr_err("Weird tag at start of node: %x\n", tag);
		return mem;
	}
	*p += 4;
	pathp = *p;
	// [root] pathp : dtb struct의 시작 주소 + 0x38 + 0x4 (root 이름(NULL)의 시작주소)
	// [chosen] chosen 문자열의 시작 주소
	l = allocl = strlen(pathp) + 1;
	// [root] l, allocl : 1
	// [chosen] l, allocl : 7
	*p = PTR_ALIGN(*p + l, 4);
	// [root] *p : dtb struct의 시작 주소 + 0x38(off_struct) + 0x4(OF_DT_BEGIN_NODE) + 0x4(root str)
	// [chosen] *p : chosen 노드의 시작 주소 + 0x8(chosen str)

	/* version 0x10 has a more compact unit name here instead of the full
	 * path. we accumulate the full path size using "fpsize", we'll rebuild
	 * it later. We detect this because the first character of the name is
	 * not '/'.
	 */
	if ((*pathp) != '/') {		// [root] *pathp : '0'
					// [chosen] *pathp : 'c'
		new_format = 1;
		if (fpsize == 0) {	// [root] fpsize : 0
					// [chosen] fpsize : 1
			/* root node: special case. fpsize accounts for path
			 * plus terminating zero. root node only has '/', so
			 * fpsize should be 2, but we want to avoid the first
			 * level nodes to have two '/' so we use fpsize 1 here
			 */
			fpsize = 1;
			allocl = 2;
			l = 1;
			*pathp = '\0';
		} else {
			/* account for '/' and path size minus terminal 0
			 * already in 'l'
			 */
			fpsize += l;		// [chosen] fpsize : 8
			allocl = fpsize;	// [chosen] allocl : 8
		}
	}
	
	// [root] mem : 0, sizeof(struct device_node) + allocl : 0x3C + 0x2, __alignof__(struct device_node) : 0x4
	// [chosen] mem : ?, sizeof(struct device_node) + allocl : 0x3C + 0x8, __alignof__(struct device_node) : 0x4
	// [Second.root] mem : 할당받은 시작 주소, sizeof(struct device_node) + allocl : 0x3C + 0x2, __alignof__(struct device_node) : 0x4
	np = unflatten_dt_alloc(&mem, sizeof(struct device_node) + allocl,
				__alignof__(struct device_node));
	// [root] np : 0, mem : 0x3D
	// [chosen] np : ?, mem : ?
	// [Second.root] np : 할당받은 시작 주소, mem : np + 0x3D

	// [First] allnextpp : NULL
	// [Second.root] allnextpp : &(&of_allnodes)
	// [Second.chosen] allnextpp : &(&of_allnodes)
	if (allnextpp) {
		char *fn;

		np->full_name = fn = ((char *)np) + sizeof(*np);
		// np->full_name : struct device_node의 바로 뒷 주소
		
		// new_format : 1
		if (new_format) {
			/* rebuild full path for new format */

			// [Second.root] dad : NULL
			// [Second.chosen] dad : root의 np
			if (dad && dad->parent) {
				strcpy(fn, dad->full_name);
#ifdef DEBUG
				if ((strlen(fn) + l + 1) != allocl) {
					pr_debug("%s: p: %d, l: %d, a: %d\n",
						pathp, (int)strlen(fn),
						l, allocl);
				}
#endif
				fn += strlen(fn);
			}
			// 부모 이름을 붙임

			*(fn++) = '/';
		}
		memcpy(fn, pathp, l);
		// 자기 이름을 붙임

		prev_pp = &np->properties;

		// [Second.root] allnextpp : &&of_allnodes
		// [Second.chosen] **allnextpp : root 노드의 np->allnext
		**allnextpp = np;
		// [Second.root] of_allnodes : root의 np
		// [Second.chosen] of_allnodes : chosen의 np
		
		// *allnextpp : &allnextp
		*allnextpp = &np->allnext;

		// [Second.root] dad : NULL
		// [Second.chosen] dad : root의 np
		if (dad != NULL) {
			np->parent = dad;
			/* we temporarily use the next field as `last_child'*/
			if (dad->next == NULL)
				dad->child = np;
			else
				dad->next->sibling = np;
			dad->next = np;
		}
		kref_init(&np->kref);
	}
	/* process properties */
	while (1) {
		u32 sz, noff;
		char *pname;

		tag = be32_to_cpup(*p);
		// tag : 0x3 (OF_DT_PROP)

		if (tag == OF_DT_NOP) {
			*p += 4;
			continue;
		}
		if (tag != OF_DT_PROP)
			break;
		*p += 4;
		sz = be32_to_cpup(*p);
		// sz : property 값의 길이
		// [1] sz : 4
		noff = be32_to_cpup(*p + 4);
		// noff : property 이름의 오프셋
		// [1] noff : 0
		*p += 8;
		// *p : property 값이 저장된 시작 주소
		if (be32_to_cpu(blob->version) < 0x10)	// 통과
			*p = PTR_ALIGN(*p, sz >= 8 ? 8 : 4);

		pname = of_fdt_get_string(blob, noff);
		// pname : property 이름이 저장된 주소
		// pname = "#address_cells"
		if (pname == NULL) {
			pr_info("Can't find property name in list !\n");
			break;
		}
		if (strcmp(pname, "name") == 0)	// [1] 통과
			has_name = 1;
		l = strlen(pname) + 1;
		// [1] l : 15
		
		// [1] mem : 0x3D, sizeof(struct property) : 24, __alignof__(struct property) : 4
		pp = unflatten_dt_alloc(&mem, sizeof(struct property),
					__alignof__(struct property));
		// [1] pp :  0x40, mem : 0x58

		// [1] allnextpp : NULL
		// [Second.root] allnextpp : ?
		if (allnextpp) {
			/* We accept flattened tree phandles either in
			 * ePAPR-style "phandle" properties, or the
			 * legacy "linux,phandle" properties.  If both
			 * appear and have different values, things
			 * will get weird.  Don't do that. */

			// [Second.root] pname : "#address-cells"
			if ((strcmp(pname, "phandle") == 0) ||
			    (strcmp(pname, "linux,phandle") == 0)) {
				if (np->phandle == 0)
					np->phandle = be32_to_cpup((__be32*)*p);
			}
			/* And we process the "ibm,phandle" property
			 * used in pSeries dynamic device tree
			 * stuff */
			if (strcmp(pname, "ibm,phandle") == 0)
				np->phandle = be32_to_cpup((__be32 *)*p);
			
			pp->name = pname;
			// sz : property 데이터의 길이
			pp->length = sz;
			// *p : property 값이 저장된 시작 주소
			pp->value = *p;
			*prev_pp = pp;
			prev_pp = &pp->next;
			// struct property 값을 현재 property에 맞게 설정 후
			// 이전 property 뒤에 리스트로 연결
			// 노드의 첫번째 property일 경우는 struct device_node에 연결
		}
		*p = PTR_ALIGN((*p) + sz, 4);
		// [1] p는 다음 property의 시작 주소
	}
	/* with version 0x10 we may not have the name property, recreate
	 * it here from the unit name if absent
	 */

	// [root] has_name : 0
	// [chosen] has_name : 0
	if (!has_name) {
		// [root] pathp : root node 이름의 시작 주소
		// [chosen] pathp : chosen node 이름의 시작 주소
		char *p1 = pathp, *ps = pathp, *pa = NULL;
		int sz;

		while (*p1) {		// [root] *p1 : 0
					// [chosen] *p1 : 'c'
			if ((*p1) == '@')
				pa = p1;
			if ((*p1) == '/')
				ps = p1 + 1;
			p1++;
		}
		// *p1 : '0'
		if (pa < ps)		// [root] pa < ps 임
					// [chosen] pa < ps 임
			pa = p1;
		// [root] pa : pathp
		// [chosen] pa : chosen 문자열의 마지막 null 위치

		sz = (pa - ps) + 1;
		// [root] sz : 1
		// [chosen] sz : 7
		
		// [root] mem : ?, sizeof(struct property) + sz : 24 + 1, __alignof__(struct property) : 4
		// [root] mem : ?, sizeof(struct property) + sz : 24 + 7, __alignof__(struct property) : 4
		pp = unflatten_dt_alloc(&mem, sizeof(struct property) + sz,
					__alignof__(struct property));
		// [root] pp : ?, mem : ?
		// [chosen] pp : ?, mem : ?

		// [root] allnextpp : NULL
		// [chosen] allnextpp : NULL
		// [Second.root] allnextpp : ?
		if (allnextpp) {
			pp->name = "name";
			pp->length = sz;
			pp->value = pp + 1;
			*prev_pp = pp;
			prev_pp = &pp->next;
			memcpy(pp->value, ps, sz - 1);
			// struct property 값을 만듬
			((char *)pp->value)[sz - 1] = 0;
			pr_debug("fixed up name for %s -> %s\n", pathp,
				(char *)pp->value);
		}
	}
	// 현재 노드에 name property가 없는 경우 생성해줌

	// [root] allnextpp : NULL
	// [chosen] allnextpp : NULL
	// [Second.root] allnextpp : ?
	if (allnextpp) {
		*prev_pp = NULL;
		np->name = of_get_property(np, "name", NULL);
		np->type = of_get_property(np, "device_type", NULL);

		if (!np->name)
			np->name = "<NULL>";
		if (!np->type)
			np->type = "<NULL>";
	}
	
	// [root:1] tag : OF_DT_BEGIN_NODE
	// [chosen] tag : OF_DT_END_NODE
	// [root:2] tag : OF_DT_BEGIN_NODE
	// [root:?] tag : OF_DT_END_NODE
	while (tag == OF_DT_BEGIN_NODE || tag == OF_DT_NOP) {
		if (tag == OF_DT_NOP)
			*p += 4;
		else
			// [root:1] blob : DTB 시작 주소, mem : ?, p : chosen Node의 시작 주소, np : 0, allnextpp : NULL, fpsize : 1
			// [root:2] blob : DTB 시작 주소, mem : ?, p : aliases Node의 시작 주소, np : 0, allnextpp : NULL, fpsize : 1
			// [Second.root] blob : DTB 시작 주소, mem : name property 다음, p : chosen Node의 시작 주소,
			//		 np : root 노드의 struct device_node 주소, allnextpp : &&of_allnodes, fpsize : 1
			// [Second.chosen] blob : DTB 시작 주소, mem : name property 다음, p : aliases Node의 시작 주소,
			//		 np : chosen 노드의 struct device_node 주소, allnextpp : &&of_allnodes, fpsize : 8
			mem = unflatten_dt_node(blob, mem, p, np, allnextpp,
						fpsize);
		tag = be32_to_cpup(*p);
	}
	if (tag != OF_DT_END_NODE) {
		pr_err("Weird tag at end of node: %x\n", tag);
		return mem;
	}
	*p += 4;
	return mem;
}

/**
 * __unflatten_device_tree - create tree of device_nodes from flat blob
 *
 * unflattens a device-tree, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 * @blob: The blob to expand
 * @mynodes: The device_node tree created by the call
 * @dt_alloc: An allocator that provides a virtual address to memory
 * for the resulting tree
 */
// ARM10C 20140208
// blob : dtb 시작 주소, mynodes : &of_allnodes, dt_alloc : early_init_dt_alloc_memory_arch
static void __unflatten_device_tree(struct boot_param_header *blob,
			     struct device_node **mynodes,
			     void * (*dt_alloc)(u64 size, u64 align))
{
	unsigned long size;
	void *start, *mem;
	struct device_node **allnextp = mynodes;
	// allnextp : &of_allnodes

	pr_debug(" -> unflatten_device_tree()\n");

	if (!blob) {	// blob != 0
		pr_debug("No device tree pointer\n");
		return;
	}

	pr_debug("Unflattening device tree:\n");
	pr_debug("magic: %08x\n", be32_to_cpu(blob->magic));
	// magic : 0xDOODFEED
	pr_debug("size: %08x\n", be32_to_cpu(blob->totalsize));
	// size : 0x3236
	pr_debug("version: %08x\n", be32_to_cpu(blob->version));
	// version : 0x11

	if (be32_to_cpu(blob->magic) != OF_DT_HEADER) {	// 통과
		pr_err("Invalid device tree blob header\n");
		return;
	}

	/* First pass, scan for size */
	start = ((void *)blob) + be32_to_cpu(blob->off_dt_struct);
	// start : dtb start + 0x38
	size = (unsigned long)unflatten_dt_node(blob, 0, &start, NULL, NULL, 0);
	// size : ?
	// DTB에 존재하는 모든 node와 property에 대해 각각
	// sturct node + node 이름 문자열 길이와 struct property + property 이름 문자열 길이에 필요한
	// 크기가 size

	size = ALIGN(size, 4);
	// 4 바이트 align

	pr_debug("  size is %lx, allocating...\n", size);

	/* Allocate memory for the expanded device tree */
	// size + 4 : ?, __alignof__(struct device_node) : 4
	// dt_alloc : early_init_dt_alloc_memory_arch
	mem = dt_alloc(size + 4, __alignof__(struct device_node));
	// mem : size + 4만큼 할당 받은 공간의 시작 주소

	memset(mem, 0, size);
	// 0으로 초기화

	*(__be32 *)(mem + size) = cpu_to_be32(0xdeadbeef);
	// mem 공간의 마지막 4바이트 위치에 0xDEADBEEF 저장 (빅 엔디안으로)

	pr_debug("  unflattening %p...\n", mem);

	/* Second pass, do actual unflattening */
	start = ((void *)blob) + be32_to_cpu(blob->off_dt_struct);
	// start : DTB의 struct 시작 주소

	// blob : DTB의 시작 주소, mem : size + 4 만큼 할당 받은 공간의 시작 주소, start : DTB의 struct 시작 주소
	// NULL, &allnextp : &(&of_allnodes), 0
	unflatten_dt_node(blob, mem, &start, NULL, &allnextp, 0);
	if (be32_to_cpup(start) != OF_DT_END)
		pr_warning("Weird tag at end of tree: %08x\n", be32_to_cpup(start));
	if (be32_to_cpup(mem + size) != 0xdeadbeef)
		pr_warning("End of tree marker overwritten: %08x\n",
			   be32_to_cpup(mem + size));
	*allnextp = NULL;

	pr_debug(" <- unflatten_device_tree()\n");
}

static void *kernel_tree_alloc(u64 size, u64 align)
{
	return kzalloc(size, GFP_KERNEL);
}

/**
 * of_fdt_unflatten_tree - create tree of device_nodes from flat blob
 *
 * unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 */
void of_fdt_unflatten_tree(unsigned long *blob,
			struct device_node **mynodes)
{
	struct boot_param_header *device_tree =
		(struct boot_param_header *)blob;
	__unflatten_device_tree(device_tree, mynodes, &kernel_tree_alloc);
}
EXPORT_SYMBOL_GPL(of_fdt_unflatten_tree);

/* Everything below here references initial_boot_params directly. */
// ARM10C 20131012
// KID 20140306
// dt_root_size_cells: 1
int __initdata dt_root_addr_cells;
// KID 20140306
// dt_root_size_cells: 1
int __initdata dt_root_size_cells;

// ARM10C 20131026
struct boot_param_header *initial_boot_params;

#ifdef CONFIG_OF_EARLY_FLATTREE // CONFIG_OF_EARLY_FLATTREE=y

/**
 * of_scan_flat_dt - scan flattened tree blob and call callback on each.
 * @it: callback function
 * @data: context data pointer
 *
 * This function is used to scan the flattened device-tree, it is
 * used to extract the memory information at boot before we can
 * unflatten the tree
 */
// ARM10C 20131005
// ARM10C 20131012
int __init of_scan_flat_dt(int (*it)(unsigned long node,
				     const char *uname, int depth,
				     void *data),
			   void *data)
{
	unsigned long p = ((unsigned long)initial_boot_params) +
		be32_to_cpu(initial_boot_params->off_dt_struct);
	int rc = 0;
	int depth = -1;

	do {
		u32 tag = be32_to_cpup((__be32 *)p);
		const char *pathp;

		p += 4;
		if (tag == OF_DT_END_NODE) {
			depth--;
			continue;
		}
		if (tag == OF_DT_NOP)
			continue;
		if (tag == OF_DT_END)
			break;
		if (tag == OF_DT_PROP) {
			u32 sz = be32_to_cpup((__be32 *)p);
			p += 8;
			if (be32_to_cpu(initial_boot_params->version) < 0x10)
				p = ALIGN(p, sz >= 8 ? 8 : 4);
			p += sz;
			p = ALIGN(p, 4);
			continue;
		}
		if (tag != OF_DT_BEGIN_NODE) {
			pr_err("Invalid tag %x in flat device tree!\n", tag);
			return -EINVAL;
		}
		depth++;
		pathp = (char *)p;
		p = ALIGN(p + strlen(pathp) + 1, 4);

		// FIXME: pathp의 값이 절대 경로라면 최하위 경로값 추출
		if (*pathp == '/')
			pathp = kbasename(pathp);
		rc = it(p, pathp, depth, data);
		if (rc != 0)
			break;
	} while (1);

	return rc;
}

/**
 * of_get_flat_dt_root - find the root node in the flat blob
 */
// ARM10C 20131005
unsigned long __init of_get_flat_dt_root(void)
{
	unsigned long p = ((unsigned long)initial_boot_params) +
		be32_to_cpu(initial_boot_params->off_dt_struct);

	// OF_DT_NOP: 0x4
	while (be32_to_cpup((__be32 *)p) == OF_DT_NOP)
		p += 4;

	// OF_DT_BEGIN_NODE: 0x1
	BUG_ON(be32_to_cpup((__be32 *)p) != OF_DT_BEGIN_NODE);
	p += 4;
	return ALIGN(p + strlen((char *)p) + 1, 4);
}

/**
 * of_get_flat_dt_prop - Given a node in the flat blob, return the property ptr
 *
 * This function can be used within scan_flattened_dt callback to get
 * access to properties
 */
// ARM10C 20131005
void *__init of_get_flat_dt_prop(unsigned long node, const char *name,
				 unsigned long *size)
{
	return of_fdt_get_property(initial_boot_params, node, name, size);
}

/**
 * of_flat_dt_is_compatible - Return true if given node has compat in compatible list
 * @node: node to test
 * @compat: compatible string to compare with compatible list.
 */
// ARM10C 20131005
int __init of_flat_dt_is_compatible(unsigned long node, const char *compat)
{
	return of_fdt_is_compatible(initial_boot_params, node, compat);
}

/**
 * of_flat_dt_match - Return true if node matches a list of compatible values
 */
// KID 20140213
// dt_root: devtree의 dt_struct의 위치 (주소)
// mdesc->dt_compat[0]: "samsung,exynos5250"
// mdesc->dt_compat[1]: "samsung,exynos5420"
// mdesc->dt_compat[2]: "samsung,exynos5440"
int __init of_flat_dt_match(unsigned long node, const char *const *compat)
{
        // initial_boot_params: devtree의 주소, node: devtree의 dt_struct의 위치 (주소),
        // compat: compatible 문자열 배열 주소 "samsung,exynos5250", "samsung,exynos5250", "samsung,exynos5250"
	return of_fdt_match(initial_boot_params, node, compat);
	// devtree와 가장 잘 매칭되는 compatible string의 index를 리턴
}

struct fdt_scan_status {
	const char *name;
	int namelen;
	int depth;
	int found;
	int (*iterator)(unsigned long node, const char *uname, int depth, void *data);
	void *data;
};

/**
 * fdt_scan_node_by_path - iterator for of_scan_flat_dt_by_path function
 */
static int __init fdt_scan_node_by_path(unsigned long node, const char *uname,
					int depth, void *data)
{
	struct fdt_scan_status *st = data;

	/*
	 * if scan at the requested fdt node has been completed,
	 * return -ENXIO to abort further scanning
	 */
	if (depth <= st->depth)
		return -ENXIO;

	/* requested fdt node has been found, so call iterator function */
	if (st->found)
		return st->iterator(node, uname, depth, st->data);

	/* check if scanning automata is entering next level of fdt nodes */
	if (depth == st->depth + 1 &&
	    strncmp(st->name, uname, st->namelen) == 0 &&
	    uname[st->namelen] == 0) {
		st->depth += 1;
		if (st->name[st->namelen] == 0) {
			st->found = 1;
		} else {
			const char *next = st->name + st->namelen + 1;
			st->name = next;
			st->namelen = strcspn(next, "/");
		}
		return 0;
	}

	/* scan next fdt node */
	return 0;
}

/**
 * of_scan_flat_dt_by_path - scan flattened tree blob and call callback on each
 *			     child of the given path.
 * @path: path to start searching for children
 * @it: callback function
 * @data: context data pointer
 *
 * This function is used to scan the flattened device-tree starting from the
 * node given by path. It is used to extract information (like reserved
 * memory), which is required on ealy boot before we can unflatten the tree.
 */
int __init of_scan_flat_dt_by_path(const char *path,
	int (*it)(unsigned long node, const char *name, int depth, void *data),
	void *data)
{
	struct fdt_scan_status st = {path, 0, -1, 0, it, data};
	int ret = 0;

	if (initial_boot_params)
                ret = of_scan_flat_dt(fdt_scan_node_by_path, &st);

	if (!st.found)
		return -ENOENT;
	else if (ret == -ENXIO)	/* scan has been completed */
		return 0;
	else
		return ret;
}

const char * __init of_flat_dt_get_machine_name(void)
{
	const char *name;
	unsigned long dt_root = of_get_flat_dt_root();

	name = of_get_flat_dt_prop(dt_root, "model", NULL);
	if (!name)
		name = of_get_flat_dt_prop(dt_root, "compatible", NULL);
	return name;
}

/**
 * of_flat_dt_match_machine - Iterate match tables to find matching machine.
 *
 * @default_match: A machine specific ptr to return in case of no match.
 * @get_next_compat: callback function to return next compatible match table.
 *
 * Iterate through machine match tables to find the best match for the machine
 * compatible string in the FDT.
 */
const void * __init of_flat_dt_match_machine(const void *default_match,
		const void * (*get_next_compat)(const char * const**))
{
	const void *data = NULL;
	const void *best_data = default_match;
	const char *const *compat;
	unsigned long dt_root;
	unsigned int best_score = ~1, score = 0;

	dt_root = of_get_flat_dt_root();
	while ((data = get_next_compat(&compat))) {
		score = of_flat_dt_match(dt_root, compat);
		if (score > 0 && score < best_score) {
			best_data = data;
			best_score = score;
		}
	}
	if (!best_data) {
		const char *prop;
		long size;

		pr_err("\n unrecognized device tree list:\n[ ");

		prop = of_get_flat_dt_prop(dt_root, "compatible", &size);
		if (prop) {
			while (size > 0) {
				printk("'%s' ", prop);
				size -= strlen(prop) + 1;
				prop += strlen(prop) + 1;
			}
		}
		printk("]\n\n");
		return NULL;
	}

	pr_info("Machine model: %s\n", of_flat_dt_get_machine_name());

	return best_data;
}

#ifdef CONFIG_BLK_DEV_INITRD // CONFIG_BLK_DEV_INITRD=y
/**
 * early_init_dt_check_for_initrd - Decode initrd location from flat tree
 * @node: reference to node containing initrd location ('chosen')
 */
// ARM10C 20131005
// ARM10C 20131012
static void __init early_init_dt_check_for_initrd(unsigned long node)
{
	u64 start, end;
	unsigned long len;
	__be32 *prop;

	pr_debug("Looking for initrd properties... ");

	prop = of_get_flat_dt_prop(node, "linux,initrd-start", &len);
	if (!prop)
		return;
	start = of_read_number(prop, len/4);

	prop = of_get_flat_dt_prop(node, "linux,initrd-end", &len);
	if (!prop)
		return;
	end = of_read_number(prop, len/4);

	initrd_start = (unsigned long)__va(start);
	initrd_end = (unsigned long)__va(end);
	initrd_below_start_ok = 1;

	pr_debug("initrd_start=0x%llx  initrd_end=0x%llx\n",
		 (unsigned long long)start, (unsigned long long)end);
}
#else
static inline void early_init_dt_check_for_initrd(unsigned long node)
{
}
#endif /* CONFIG_BLK_DEV_INITRD */

/**
 * early_init_dt_scan_root - fetch the top level address and size cells
 */
// ARM10C 20131012
// KID 20140306
int __init early_init_dt_scan_root(unsigned long node, const char *uname,
				   int depth, void *data)
{
	__be32 *prop;

	if (depth != 0)
		return 0;

	// OF_ROOT_NODE_SIZE_CELLS_DEFAULT: 1
	dt_root_size_cells = OF_ROOT_NODE_SIZE_CELLS_DEFAULT;
	// dt_root_size_cells: 1

	// OF_ROOT_NODE_ADDR_CELLS_DEFAULT: 1
	dt_root_addr_cells = OF_ROOT_NODE_ADDR_CELLS_DEFAULT;
	// dt_root_addr_cells: 1

	prop = of_get_flat_dt_prop(node, "#size-cells", NULL);
	// prop: "#size-cells"의 값이 위치한 devtree의 주소

	if (prop)
		// be32_to_cpup(prop): 1
		dt_root_size_cells = be32_to_cpup(prop);
		// dt_root_size_cells: 1

	// dt_root_size_cells: 1
	pr_debug("dt_root_size_cells = %x\n", dt_root_size_cells);

	prop = of_get_flat_dt_prop(node, "#address-cells", NULL);
	// prop: "#address-cells"의 값이 위치한 devtree의 주소

	if (prop)
		// be32_to_cpup(prop): 1
		dt_root_addr_cells = be32_to_cpup(prop);
		// dt_root_addr_cells: 1

	// dt_root_addr_cells=1
	pr_debug("dt_root_addr_cells = %x\n", dt_root_addr_cells);

	/* break now */
	return 1;
}

// ARM10C 20131012
// KID 20140306
// dt_root_addr_cells: 1, &reg
// dt_root_size_cells: 1, &reg
u64 __init dt_mem_next_cell(int s, __be32 **cellp)
{
	__be32 *p = *cellp;
	// p: reg

	// cellp: &reg, s: 1
	*cellp = p + s;
	// reg: reg + 1

	// p: reg, s: 1
	return of_read_number(p, s);
	// return 0x0000000020000000
	// return 0x0000000080000000
}

/**
 * early_init_dt_scan_memory - Look for an parse memory nodes
 */
// ARM10C 20131012
// KID 20140306
int __init early_init_dt_scan_memory(unsigned long node, const char *uname,
				     int depth, void *data)
{
	char *type = of_get_flat_dt_prop(node, "device_type", NULL);
	// type: "device_type"의 값이 위치한 devtree의 주소
	__be32 *reg, *endp;
	unsigned long l;

	/* We are scanning "memory" nodes only */
	// type: "device_type"의 값이 위치한 devtree의 주소
	if (type == NULL) {
		/*
		 * The longtrail doesn't have a device_type on the
		 * /memory node, so look for the node called /memory@0.
		 */
		if (depth != 1 || strcmp(uname, "memory@0") != 0)
			return 0;
	} else if (strcmp(type, "memory") != 0)
		return 0;

	// memory 노드 안의 프로퍼티를 찾음 
	reg = of_get_flat_dt_prop(node, "linux,usable-memory", &l);
	// reg: NULL

	// 프로퍼터의 length값 l = 8
	if (reg == NULL)
		reg = of_get_flat_dt_prop(node, "reg", &l);
		// reg: "reg"의 값이 위치한 devtree의 주소, l: 8

	if (reg == NULL)
		return 0;

	// memory의 reg 값의 끝 위치를 계산
	// reg: "reg"의 값이 위치한 devtree의 주소, l: 8, sizeof(__be32): 4
	endp = reg + (l / sizeof(__be32));
	// endp: "reg"의 값이 위치한 devtree의 주소 + 2

	// uname: "/memory", 1: 8
	// reg[0]: 0x20000000, reg[1]: 0x80000000, reg[2]: 0x00000002, reg[3]: 0x00000001
	pr_debug("memory scan node %s, reg size %ld, data: %x %x %x %x,\n",
	    uname, l, reg[0], reg[1], reg[2], reg[3]);

	// endp - reg: 2, dt_root_addr_cells: 1, dt_root_size_cells: 1
	while ((endp - reg) >= (dt_root_addr_cells + dt_root_size_cells)) {
		u64 base, size;

		// dt_root_addr_cells: 1
		base = dt_mem_next_cell(dt_root_addr_cells, &reg);
		// base: 0x20000000

		// dt_root_size_cells: 1
		size = dt_mem_next_cell(dt_root_size_cells, &reg);
		// size: 0x80000000

		if (size == 0)
			continue;

		// base: 0x20000000, size: 0x80000000
		pr_debug(" - %llx ,  %llx\n", (unsigned long long)base,
		    (unsigned long long)size);

		// base: 0x20000000, size: 0x80000000
		early_init_dt_add_memory_arch(base, size);
	}

	return 0;
}

// ARM10C 20131005
// ARM10C 20131012
int __init early_init_dt_scan_chosen(unsigned long node, const char *uname,
				     int depth, void *data)
{
	unsigned long l;
	char *p;

	// exynos5420-smdk5420.dtb 값의 chosen 부분 
	// 00000d0: 4f53 3534 3230 0000 0000 0001 6368 6f73  OS5420......chos
	// 00000e0: 656e 0000 0000 0003 0000 0025 0000 003d  en.........%...=
	// 00000f0: 636f 6e73 6f6c 653d 7474 7953 4143 322c  console=ttySAC2,
	// 0000100: 3131 3532 3030 2069 6e69 743d 2f6c 696e  115200 init=/lin
	// 0000110: 7578 7263 0000 0000 0000 0002 0000 0001  uxrc............
	// 0000120: 616c 6961 7365 7300 0000 0003 0000 0012  aliases.........

	// depth 값: 1, uname: chosen
	pr_debug("search \"chosen\", depth: %d, uname: %s\n", depth, uname);

	if (depth != 1 || !data ||
	    (strcmp(uname, "chosen") != 0 && strcmp(uname, "chosen@0") != 0))
		return 0;

	early_init_dt_check_for_initrd(node);

	/* Retrieve command line */
	p = of_get_flat_dt_prop(node, "bootargs", &l);
	if (p != NULL && l > 0)
		strlcpy(data, p, min((int)l, COMMAND_LINE_SIZE));
		// data: "console=ttySAC2,115200 init=/linuxrc"

	/*
	 * CONFIG_CMDLINE is meant to be a default in case nothing else
	 * managed to set the command line, unless CONFIG_CMDLINE_FORCE
	 * is set in which case we override whatever was found earlier.
	 */
#ifdef CONFIG_CMDLINE // defined
// CONFIG_CMDLINE="root=/dev/ram0 rw ramdisk=8192 initrd=0x41000000,8M console=ttySAC1,115200 init=/linuxrc mem=256M"
#ifndef CONFIG_CMDLINE_FORCE // not defined
	if (!((char *)data)[0])
#endif
		strlcpy(data, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
#endif /* CONFIG_CMDLINE */

	pr_debug("Command line is: %s\n", (char*)data);

	/* break now */
	return 1;
}

#ifdef CONFIG_HAVE_MEMBLOCK
void __init __weak early_init_dt_add_memory_arch(u64 base, u64 size)
{
	const u64 phys_offset = __pa(PAGE_OFFSET);
	base &= PAGE_MASK;
	size &= PAGE_MASK;
	if (base + size < phys_offset) {
		pr_warning("Ignoring memory block 0x%llx - 0x%llx\n",
			   base, base + size);
		return;
	}
	if (base < phys_offset) {
		pr_warning("Ignoring memory range 0x%llx - 0x%llx\n",
			   base, phys_offset);
		size -= phys_offset - base;
		base = phys_offset;
	}
	memblock_add(base, size);
}

/*
 * called from unflatten_device_tree() to bootstrap devicetree itself
 * Architectures can override this definition if memblock isn't used
 */
void * __init __weak early_init_dt_alloc_memory_arch(u64 size, u64 align)
{
	return __va(memblock_alloc(size, align));
}
#endif

bool __init early_init_dt_scan(void *params)
{
	if (!params)
		return false;

	/* Setup flat device-tree pointer */
	initial_boot_params = params;

	/* check device tree validity */
	if (be32_to_cpu(initial_boot_params->magic) != OF_DT_HEADER) {
		initial_boot_params = NULL;
		return false;
	}

	/* Retrieve various information from the /chosen node */
	of_scan_flat_dt(early_init_dt_scan_chosen, boot_command_line);

	/* Initialize {size,address}-cells info */
	of_scan_flat_dt(early_init_dt_scan_root, NULL);

	/* Setup memory, calling early_init_dt_add_memory_arch */
	of_scan_flat_dt(early_init_dt_scan_memory, NULL);

	return true;
}

/**
 * unflatten_device_tree - create tree of device_nodes from flat blob
 *
 * unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 */
// ARM10C 20140208
void __init unflatten_device_tree(void)
{
	// initial_boot_params : dtb 시작 주소
	__unflatten_device_tree(initial_boot_params, &of_allnodes,
				early_init_dt_alloc_memory_arch);
	// tree를 만듬. root node의 struct device_node는 of_allnodes에 저장함

	/* Get pointer to "/chosen" and "/aliases" nodes for use everywhere */
	of_alias_scan(early_init_dt_alloc_memory_arch);
        // aliases에 노드에 있는 node value를가지고 해당 node를 찾고
        // aliases_lookup에 연결되는alias_prop 의 메모리를 생성하고 aliases_lookup에 리스트를 생성함
}

/**
 * unflatten_and_copy_device_tree - copy and create tree of device_nodes from flat blob
 *
 * Copies and unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used. This should only be used when the FDT memory has not been
 * reserved such is the case when the FDT is built-in to the kernel init
 * section. If the FDT memory is reserved already then unflatten_device_tree
 * should be used instead.
 */
void __init unflatten_and_copy_device_tree(void)
{
	int size;
	void *dt;

	if (!initial_boot_params) {
		pr_warn("No valid device tree found, continuing without\n");
		return;
	}

	size = __be32_to_cpu(initial_boot_params->totalsize);
	dt = early_init_dt_alloc_memory_arch(size,
		__alignof__(struct boot_param_header));

	if (dt) {
		memcpy(dt, initial_boot_params, size);
		initial_boot_params = dt;
	}
	unflatten_device_tree();
}

#endif /* CONFIG_OF_EARLY_FLATTREE */
