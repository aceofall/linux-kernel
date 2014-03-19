#include <asm/setup.h>
#include <libfdt.h>

#if defined(CONFIG_ARM_ATAG_DTB_COMPAT_CMDLINE_EXTEND)
#define do_extend_cmdline 1
#else
#define do_extend_cmdline 0
#endif

// KID 20140319
// fdt: fdt 시작위치, node_path: "/memory"
static int node_offset(void *fdt, const char *node_path)
{
	// fdt: fdt 시작위치, node_path: "/memory"
	int offset = fdt_path_offset(fdt, node_path);
	// offset: 404

	if (offset == -FDT_ERR_NOTFOUND)
		offset = fdt_add_subnode(fdt, 0, node_path);

	// offset: 404
	return offset;
	// return 404
}

// KID 20140319
// fdt: fdt 시작위치, "/memory", "reg", mem_reg_property, 8
static int setprop(void *fdt, const char *node_path, const char *property,
		   uint32_t *val_array, int size)
{
	// fdt: fdt 시작위치, node_path: "/memory"
	int offset = node_offset(fdt, node_path);
	// offset: 404

	if (offset < 0)
		return offset;

	// fdt: fdt 시작위치, offset: 404, property: "reg", mem_reg_property, 8
	return fdt_setprop(fdt, offset, property, val_array, size);
	// 설정하고자하는 property를 ftd에서 찾고 설정하고자 하는 값과 헤더 값을 ftd에 업데이트함
}

static int setprop_string(void *fdt, const char *node_path,
			  const char *property, const char *string)
{
	int offset = node_offset(fdt, node_path);
	if (offset < 0)
		return offset;
	return fdt_setprop_string(fdt, offset, property, string);
}

// ARM10C 20131012
static int setprop_cell(void *fdt, const char *node_path,
			const char *property, uint32_t val)
{
	int offset = node_offset(fdt, node_path);
	if (offset < 0)
		return offset;
	return fdt_setprop_cell(fdt, offset, property, val);
}

// KID 20140318
// fdt: _edata (data영역의 끝 위치), "/", "#size-cells", &len
static const void *getprop(const void *fdt, const char *node_path,
			   const char *property, int *len)
{
	// fdt: _edata (data영역의 끝 위치), node_path: "/"
	int offset = fdt_path_offset(fdt, node_path);
	// offset: 0

	// FDT_ERR_NOTFOUND: 1
	if (offset == -FDT_ERR_NOTFOUND)
		return NULL;

	// fdt: _edata (data영역의 끝 위치), offset: 0, property: "#size-cells"
	return fdt_getprop(fdt, offset, property, len);
	// return fdt시작위치 + 0x5C
}

// KID 20140318
// fdt: _edata (data영역의 끝 위치)
static uint32_t get_cell_size(const void *fdt)
{
	int len;
	uint32_t cell_size = 1;
	// fdt: _edata (data영역의 끝 위치)
	// getprop(fdt, "/", "#size-cells", &len): fdt시작위치 + 0x5C
	const uint32_t *size_len =  getprop(fdt, "/", "#size-cells", &len);
	// size_len: fdt시작위치 + 0x5C

	if (size_len)
		// cell_size: 1, fdt32_to_cpu(*size_len): 0x1
		cell_size = fdt32_to_cpu(*size_len);
		// cell_size: 1

	return cell_size;
	// return 1
}

static void merge_fdt_bootargs(void *fdt, const char *fdt_cmdline)
{
	char cmdline[COMMAND_LINE_SIZE];
	const char *fdt_bootargs;
	char *ptr = cmdline;
	int len = 0;

	/* copy the fdt command line into the buffer */
	fdt_bootargs = getprop(fdt, "/chosen", "bootargs", &len);
	if (fdt_bootargs)
		if (len < COMMAND_LINE_SIZE) {
			memcpy(ptr, fdt_bootargs, len);
			/* len is the length of the string
			 * including the NULL terminator */
			ptr += len - 1;
		}

	/* and append the ATAG_CMDLINE */
	if (fdt_cmdline) {
		len = strlen(fdt_cmdline);
		if (ptr - cmdline + len + 2 < COMMAND_LINE_SIZE) {
			*ptr++ = ' ';
			memcpy(ptr, fdt_cmdline, len);
			ptr += len;
		}
	}
	*ptr = '\0';

	setprop_string(fdt, "/chosen", "bootargs", cmdline);
}

/*
 * Convert and fold provided ATAGs into the provided FDT.
 *
 * REturn values:
 *    = 0 -> pretend success
 *    = 1 -> bad ATAG (may retry with another possible ATAG pointer)
 *    < 0 -> error from libfdt
 */
// ARM10C 20130720
// arch/arm/boot/compressed/head.S 에서 호출됨
// void *atag_list = r0 = atags/device tree pointer
// void *fdt = r1 = _edata
// int total_space = r2 = (sp - _edata)
// KID 20140313
// atag_list: atags/device tree pointer, fdt: _edata, total_space: sp - _edata
int atags_to_fdt(void *atag_list, void *fdt, int total_space)
{
	// atag_list: bootloader에서 넘겨 받은 ATAG(DTB) 주소
	struct tag *atag = atag_list;
	// atag: bootloader에서 넘겨 받은 ATAG(DTB) 주소

	/* In the case of 64 bits memory size, need to reserve 2 cells for
	 * address and size for each bank */
	// NR_BANKS: 8
	uint32_t mem_reg_property[2 * 2 * NR_BANKS];
	int memcount = 0;
	int ret, memsize;

	/* make sure we've got an aligned pointer */
	// 4BYTE align 확인 (유효한 atag_list인지 확인)
	if ((u32)atag_list & 0x3)
		return 1;

	/* if we get a DTB here we're done already */
	// fdt32_to_cpu() endian CONFIG 설정에 맞추어 FDT_MAGIC를 바이트오더 변경
	// FDT_MAGIC: 0xd00dfeed, fdt32_to_cpu(0xd00dfeed): 0xedfe0dd0
	if (*(u32 *)atag_list == fdt32_to_cpu(FDT_MAGIC))
		// atags의 위치 주소가 DTB형태로 만들어져 왔음
		return 0;
		// return 0
	
	// ATAG가 왔다고 가정하고 코드 분석
	// ATAG_CORE: 0x54410001, ATAG_MEM: 0x54410002, ATAG_NONE: 0x00000000
	// PAGE_SIZE: 4096 (4K), MEM_SIZE: 0x1000000 (16MB)
	// tag_size(tag_core): 20, tag_size(tag_mem32): 16
	//
	// static struct {
	// 	struct tag_header hdr1;
	// 	struct tag_core   core;
	// 	struct tag_header hdr2;
	// 	struct tag_mem32  mem;
	// 	struct tag_header hdr3;
	// } default_tags __initdata = {
	// 	{ 20, 0x54410001 },
	// 	{ 1, 4096, 0xff },
	// 	{ 16, 0x54410002 },
	// 	{ 0x1000000 },
	// 	{ 0, 0x00000000 }
	// };

	/* validate the ATAG */
	// ATAG_CORE: 0x54410001, atag->hdr.tag: 0x54410001,
	// atag->hdr.size: 20, tag_size(tag_core): 20
	if (atag->hdr.tag != ATAG_CORE ||
	    (atag->hdr.size != tag_size(tag_core) &&
	     atag->hdr.size != 2))
		return 1;

	/* let's give it all the room it could need */
	// ftd의 struct alloc 초기화 해준다.
	// _edata: data영역의 끝 위치이며 fdt의 시작위치를 뜻함.
	// fdt: _edata (data영역의 끝 위치) , total_space: sp - _edata (실제로 사용할 수 있는 memory 공간)
	ret = fdt_open_into(fdt, fdt, total_space);
	// _edata (data영역의 끝 위치)위치부터 fdt를 복사함. fdt의 header 정보 업데이트 수행

	if (ret < 0)
		return ret;

	// atag_list hdr의 tag(type)을 보고 ftd를 append 한다.
	// atag: bootloader에서 넘겨 받은 ATAG(DTB) 주소, atag_list: bootloader에서 넘겨 받은 ATAG(DTB) 주소
	for_each_tag(atag, atag_list) {
	// for (atag = atag_list; atag->hdr.size; atag = tag_next(atag))

		if (atag->hdr.tag == ATAG_CMDLINE) { // ATAG_CMDLINE: 0x54410009
			/* Append the ATAGS command line to the device tree
			 * command line.
			 * NB: This means that if the same parameter is set in
			 * the device tree and in the tags, the one from the
			 * tags will be chosen.
			 */
			if (do_extend_cmdline)
				merge_fdt_bootargs(fdt,
						   atag->u.cmdline.cmdline);
			else
				setprop_string(fdt, "/chosen", "bootargs",
					       atag->u.cmdline.cmdline);
		} else if (atag->hdr.tag == ATAG_MEM) { // ATAG_MEM: 0x54410002
			// atag->hdr.tag: 0x54410002
			// memcount: 0, sizeof(mem_reg_property): 128
			if (memcount >= sizeof(mem_reg_property)/4)
				continue;

			// atag->u.mem.size: 0x1000000
			if (!atag->u.mem.size)
				continue;

			// fdt: _edata (data영역의 끝 위치)
			memsize = get_cell_size(fdt);
			// memsize: 1

			if (memsize == 2) {
				/* if memsize is 2, that means that
				 * each data needs 2 cells of 32 bits,
				 * so the data are 64 bits */
				uint64_t *mem_reg_prop64 =
					(uint64_t *)mem_reg_property;
				mem_reg_prop64[memcount++] =
					cpu_to_fdt64(atag->u.mem.start);
				mem_reg_prop64[memcount++] =
					cpu_to_fdt64(atag->u.mem.size);
			} else {
				// memcount: 0, atag->u.mem.start: 0
				mem_reg_property[memcount++] =
					cpu_to_fdt32(atag->u.mem.start);
				// memcount: 1, mem_reg_property[0]: 0

				// memcount: 1, atag->u.mem.size: 0x1000000
				mem_reg_property[memcount++] =
					cpu_to_fdt32(atag->u.mem.size);
				// memcount: 2, mem_reg_property[1]: 0x1000000

			}

		} else if (atag->hdr.tag == ATAG_INITRD2) { // ATAG_INITRD2: 0x54420005
			uint32_t initrd_start, initrd_size;
			initrd_start = atag->u.initrd.start;
			initrd_size = atag->u.initrd.size;
			setprop_cell(fdt, "/chosen", "linux,initrd-start",
					initrd_start);
			setprop_cell(fdt, "/chosen", "linux,initrd-end",
					initrd_start + initrd_size);
		}
	}

	// memcount: 2
	if (memcount) {
		// fdt: fdt 시작위치, memcount: 2, memsize: 1
		setprop(fdt, "/memory", "reg", mem_reg_property,
			4 * memcount * memsize);
		// 설정하고자하는 property를 ftd에서 찾고 설정하고자 하는 값과 헤더 값을 ftd에 업데이트함
	}

	// fdt: fdt 시작위치
	return fdt_pack(fdt);
}
