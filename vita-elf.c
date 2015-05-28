#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <err.h>

#include <libelf.h>
#include <gelf.h>
/* Note:
 * Even though we know the Vita is a 32-bit platform and we specifically check
 * that we're operating on a 32-bit ELF only, we still use the GElf family of
 * functions.  This is because they have extra sanity checking baked in.
 */

#include <endian.h>

#include "vita-elf.h"
#include "vita-import.h"
#include "elf-defs.h"

#define FAIL_EX(label, function, fmt...) do { \
	function(fmt); \
	goto label; \
} while (0)
#define FAIL(fmt...) FAIL_EX(failure, warn, fmt)
#define FAILX(fmt...) FAIL_EX(failure, warnx, fmt)
#define FAILE(fmt, args...) FAIL_EX(failure, warnx, fmt ": %s", ##args, elf_errmsg(-1))
#define ASSERT(condition) do { \
	if (!(condition)) FAILX("Assertion failed: (" #condition ")"); \
} while (0)

static void free_rela_table(vita_elf_rela_table_t *rtable);

static int load_stubs(Elf_Scn *scn, int *num_stubs, vita_elf_stub_t **stubs)
{
	GElf_Shdr shdr;
	Elf_Data *data;
	uint32_t *stub_data;
	int chunk_offset, total_bytes;
	vita_elf_stub_t *curstub;

	gelf_getshdr(scn, &shdr);

	*num_stubs = shdr.sh_size / 16;
	*stubs = calloc(*num_stubs, sizeof(vita_elf_stub_t));

	curstub = *stubs;
	data = NULL; total_bytes = 0;
	while (total_bytes < shdr.sh_size &&
			(data = elf_getdata(scn, data)) != NULL) {

		for (stub_data = (uint32_t *)data->d_buf, chunk_offset = 0;
				chunk_offset < data->d_size;
				stub_data += 4, chunk_offset += 16) {
			curstub->addr = shdr.sh_addr + data->d_off + chunk_offset;
			curstub->library_nid = le32toh(stub_data[0]);
			curstub->module_nid = le32toh(stub_data[1]);
			curstub->target_nid = le32toh(stub_data[2]);
			curstub++;
		}

		total_bytes += data->d_size;
	}

	return 1;
}

static int load_symbols(vita_elf_t *ve, Elf_Scn *scn)
{
	GElf_Shdr shdr;
	Elf_Data *data;
	GElf_Sym sym;
	int total_bytes;
	int data_beginsym, symndx;
	vita_elf_symbol_t *cursym;

	if (elf_ndxscn(scn) == ve->symtab_ndx)
		return 1; /* Already loaded */

	if (ve->symtab != NULL)
		FAILX("ELF file appears to have multiple symbol tables!");

	gelf_getshdr(scn, &shdr);

	ve->num_symbols = shdr.sh_size / shdr.sh_entsize;
	ve->symtab = calloc(ve->num_symbols, sizeof(vita_elf_symbol_t));
	ve->symtab_ndx = elf_ndxscn(scn);

	data = NULL; total_bytes = 0;
	while (total_bytes < shdr.sh_size &&
			(data = elf_getdata(scn, data)) != NULL) {

		data_beginsym = data->d_off / shdr.sh_entsize;
		for (symndx = 0; symndx < data->d_size / shdr.sh_entsize; symndx++) {
			if (gelf_getsym(data, symndx, &sym) != &sym)
				FAILE("gelf_getsym() failed");

			cursym = ve->symtab + symndx + data_beginsym;

			cursym->name = elf_strptr(ve->elf, shdr.sh_link, sym.st_name);
			cursym->value = sym.st_value;
			cursym->type = GELF_ST_TYPE(sym.st_info);
			cursym->binding = GELF_ST_BIND(sym.st_info);
			cursym->shndx = sym.st_shndx;
		}

		total_bytes += data->d_size;
	}

	return 1;
failure:
	return 0;
}

#define THUMB_SHUFFLE(x) ((((x) & 0xFFFF0000) >> 16) | (((x) & 0xFFFF) << 16))
static uint32_t decode_rel_target(uint32_t data, int type, uint32_t addr)
{
	switch(type) {
		case R_ARM_NONE:
		case R_ARM_V4BX:
			return 0xdeadbeef;
		case R_ARM_ABS32:
		case R_ARM_TARGET1:
			return data;
		case R_ARM_REL32:
		case R_ARM_TARGET2:
			return data + addr;
		case R_ARM_PREL31:
			return data + addr;
		case R_ARM_THM_CALL: // bl (THUMB)
			data = THUMB_SHUFFLE(data);
			return (((((data >> 16) & 0x7ff) << 11)
				| ((data & 0x7ff))) << 1) + addr + 4;
		case R_ARM_CALL: // bl/blx
		case R_ARM_JUMP24: // b/bl<cond>
			return ((data & 0x00ffffff) << 2) + addr + 8;
		case R_ARM_MOVW_ABS_NC: //movw
			return ((data & 0xf0000) >> 4) | (data & 0xfff);
		case R_ARM_MOVT_ABS: //movt
			return (((data & 0xf0000) >> 4) | (data & 0xfff)) << 16;
		case R_ARM_THM_MOVW_ABS_NC: //MOVW (THUMB)
			data = THUMB_SHUFFLE(data);
			return (((data >> 16) & 0xf) << 12)
				| (((data >> 26) & 0x1) << 11)
				| (((data >> 12) & 0x7) << 8)
				| (data & 0xff);
		case R_ARM_THM_MOVT_ABS: //MOVT (THUMB)
			data = THUMB_SHUFFLE(data);
			return (((data >> 16) & 0xf) << 28)
				| (((data >> 26) & 0x1) << 27)
				| (((data >> 12) & 0x7) << 24)
				| ((data & 0xff) << 16);
	}

	errx(EXIT_FAILURE, "Invalid relocation type: %d", type);
}

#define REL_HANDLE_NORMAL 0
#define REL_HANDLE_IGNORE -1
#define REL_HANDLE_EXPECTED -2
#define REL_HANDLE_INVALID -3
static int get_rel_handling(int type)
{
	switch(type) {
		case R_ARM_NONE:
		case R_ARM_V4BX:
			return REL_HANDLE_IGNORE;
		case R_ARM_ABS32:
		case R_ARM_TARGET1:
		case R_ARM_REL32:
		case R_ARM_TARGET2:
		case R_ARM_PREL31:
		case R_ARM_THM_CALL:
		case R_ARM_CALL:
		case R_ARM_JUMP24:
			return REL_HANDLE_NORMAL;
		case R_ARM_MOVW_ABS_NC:
			return R_ARM_MOVT_ABS;
		case R_ARM_MOVT_ABS:
			return REL_HANDLE_EXPECTED;
		case R_ARM_THM_MOVW_ABS_NC:
			return R_ARM_THM_MOVT_ABS;
		case R_ARM_THM_MOVT_ABS:
			return REL_HANDLE_EXPECTED;
	}

	return REL_HANDLE_INVALID;
}

static int load_rel_table(vita_elf_t *ve, Elf_Scn *scn)
{
	Elf_Scn *text_scn;
	GElf_Shdr shdr, text_shdr;
	Elf_Data *data, *text_data;
	GElf_Rel rel;
	int relndx;

	int expect_type = 0;
	int rel_sym;
	int handling;

	vita_elf_rela_table_t *rtable = NULL;
	vita_elf_rela_t *currela = NULL, *prevrela = NULL;
	uint32_t insn, target = 0;

	gelf_getshdr(scn, &shdr);

	if (!load_symbols(ve, elf_getscn(ve->elf, shdr.sh_link)))
		goto failure;

	rtable = calloc(1, sizeof(vita_elf_rela_table_t));
	ASSERT(rtable != NULL);
	rtable->num_relas = shdr.sh_size / shdr.sh_entsize;
	rtable->relas = calloc(rtable->num_relas, sizeof(vita_elf_rela_t));
	ASSERT(rtable->relas != NULL);

	rtable->target_ndx = shdr.sh_info;
	text_scn = elf_getscn(ve->elf, shdr.sh_info);
	gelf_getshdr(text_scn, &text_shdr);
	text_data = elf_getdata(text_scn, NULL);

	data = elf_getdata(scn, NULL);
	for (relndx = 0; relndx < data->d_size / shdr.sh_entsize; relndx++) {
		if (gelf_getrel(data, relndx, &rel) != &rel)
			FAILX("gelf_getrel() failed");

		currela = rtable->relas + relndx;
		currela->type = GELF_R_TYPE(rel.r_info);
		currela->offset = rel.r_offset;

		if (expect_type != 0 && currela->type != expect_type)
			FAILX("Expected %s relocation to follow %s, got %s!",
					elf_decode_r_type(expect_type), elf_decode_r_type(prevrela->type), elf_decode_r_type(currela->type));

		handling = get_rel_handling(currela->type);

		if (handling == REL_HANDLE_IGNORE)
			continue;
		else if (handling == REL_HANDLE_EXPECTED && expect_type == 0)
			FAILX("Encountered unexpected %s relocation!", elf_decode_r_type(currela->type));
		else if (handling == REL_HANDLE_INVALID)
			FAILX("Invalid relocation type %d!", currela->type);

		expect_type = 0;

		rel_sym = GELF_R_SYM(rel.r_info);
		if (rel_sym >= ve->num_symbols)
			FAILX("REL entry tried to access symbol %d, but only %d symbols loaded", rel_sym, ve->num_symbols);

		currela->symbol = ve->symtab + rel_sym;

		insn = le32toh(*((uint32_t*)(text_data->d_buf+(rel.r_offset - text_shdr.sh_addr))));

		if (handling == REL_HANDLE_EXPECTED) {
			if (currela->symbol != prevrela->symbol)
				FAILX("Paired MOVW/MOVT relocations do not reference same symbol!");
			if (currela->offset != prevrela->offset + 4)
				FAILX("Paired MOVW/MOVT relocation not adjacent!");
			target |= decode_rel_target(insn, currela->type, rel.r_offset);
		} else
			target = decode_rel_target(insn, currela->type, rel.r_offset);

		if (handling > 0) {
			expect_type = handling;
			prevrela = currela;
			continue;
		}

		currela->addend = target - currela->symbol->value;

		if (handling == REL_HANDLE_EXPECTED)
			prevrela->addend = target - currela->symbol->value;


		prevrela = NULL;
	}

	if (expect_type != 0)
		FAILX("Found %s relocation without corresponding %s!",
				elf_decode_r_type(prevrela->type), elf_decode_r_type(expect_type));

	rtable->next = ve->rela_tables;
	ve->rela_tables = rtable;

	return 1;
failure:
	free_rela_table(rtable);
	return 0;
}

static int load_rela_table(vita_elf_t *ve, Elf_Scn *scn)
{
	warnx("RELA sections currently unsupported");
	return 0;
}

static int lookup_stub_symbols(vita_elf_t *ve, int num_stubs, vita_elf_stub_t *stubs, int stubs_ndx, int sym_type)
{
	int symndx;
	vita_elf_symbol_t *cursym;
	int stub;

	for (symndx = 0; symndx < ve->num_symbols; symndx++) {
		cursym = ve->symtab + symndx;

		if (cursym->binding != STB_GLOBAL)
			continue;
		if (cursym->type != STT_FUNC && cursym->type != STT_OBJECT)
			continue;
		if (cursym->shndx != stubs_ndx)
			continue;

		if (cursym->type != sym_type)
			FAILX("Global symbol %s in section %d expected to have type %s; instead has type %s",
					cursym->name, stubs_ndx, elf_decode_st_type(sym_type), elf_decode_st_type(cursym->type));

		for (stub = 0; stub < num_stubs; stub++) {
			if (stubs[stub].addr != cursym->value)
				continue;
			if (stubs[stub].symbol != NULL)
				FAILX("Stub at %06x in section %d has duplicate symbols: %s, %s",
						cursym->value, stubs_ndx, stubs[stub].symbol->name, cursym->name);
			stubs[stub].symbol = cursym;
			break;
		}

		if (stub == num_stubs)
			FAILX("Global symbol %s in section %d not pointing to a valid stub",
					cursym->name, cursym->shndx);
	}

	return 1;

failure:
	return 0;
}

vita_elf_t *vita_elf_load(const char *filename)
{
	vita_elf_t *ve = NULL;
	GElf_Ehdr ehdr;
	Elf_Scn *scn;
	GElf_Shdr shdr;
	size_t shstrndx;
	char *name;

	if (elf_version(EV_CURRENT) == EV_NONE)
		FAILX("ELF library initialization failed: %s", elf_errmsg(-1));

	ve = calloc(1, sizeof(vita_elf_t));
	ASSERT(ve != NULL);
	ve->fd = -1;

	if ((ve->fd = open(filename, O_RDONLY)) < 0)
		FAIL("open %s failed", filename);

	if ((ve->elf = elf_begin(ve->fd, ELF_C_READ, NULL)) == NULL)
		FAILE("elf_begin() failed");

	if (elf_kind(ve->elf) != ELF_K_ELF)
		FAILX("%s is not an ELF file", filename);

	if (gelf_getehdr(ve->elf, &ehdr) == NULL)
		FAILE("getehdr() failed");

	if (ehdr.e_machine != EM_ARM)
		FAILX("%s is not an ARM binary", filename);

	if (ehdr.e_ident[EI_CLASS] != ELFCLASS32 || ehdr.e_ident[EI_DATA] != ELFDATA2LSB)
		FAILX("%s is not a 32-bit, little-endian binary", filename);

	if (elf_getshdrstrndx(ve->elf, &shstrndx) != 0)
		FAILE("elf_getshdrstrndx() failed");

	scn = NULL;

	while ((scn = elf_nextscn(ve->elf, scn)) != NULL) {
		if (gelf_getshdr(scn, &shdr) != &shdr)
			FAILE("getshdr() failed");

		if ((name = elf_strptr(ve->elf, shstrndx, shdr.sh_name)) == NULL)
			FAILE("elf_strptr() failed");

		if (shdr.sh_type == SHT_PROGBITS && strcmp(name, ".vitalink.fstubs") == 0) {
			if (ve->fstubs_ndx != 0)
				FAILX("Multiple .vitalink.fstubs sections in binary");
			ve->fstubs_ndx = elf_ndxscn(scn);
			if (!load_stubs(scn, &ve->num_fstubs, &ve->fstubs))
				goto failure;
		} else if (shdr.sh_type == SHT_PROGBITS && strcmp(name, ".vitalink.vstubs") == 0) {
			if (ve->vstubs_ndx != 0)
				FAILX("Multiple .vitalink.vstubs sections in binary");
			ve->vstubs_ndx = elf_ndxscn(scn);
			if (!load_stubs(scn, &ve->num_vstubs, &ve->vstubs))
				goto failure;
		}

		if (shdr.sh_type == SHT_SYMTAB) {
			if (!load_symbols(ve, scn))
				goto failure;
		} else if (shdr.sh_type == SHT_REL) {
			if (!load_rel_table(ve, scn))
				goto failure;
		} else if (shdr.sh_type == SHT_RELA) {
			if (!load_rela_table(ve, scn))
				goto failure;
		}
	}

	if (ve->fstubs_ndx == 0 && ve->vstubs_ndx == 0)
		FAILX("No .vitalink stub sections in binary, probably not a Vita binary");

	if (ve->symtab == NULL)
		FAILX("No symbol table in binary, perhaps stripped out");

	if (ve->rela_tables == NULL)
		FAILX("No relocation sections in binary; use -Wl,-q while linking");

	if (ve->fstubs_ndx != 0) {
		if (!lookup_stub_symbols(ve, ve->num_fstubs, ve->fstubs, ve->fstubs_ndx, STT_FUNC)) goto failure;
	}

	if (ve->vstubs_ndx != 0) {
		if (!lookup_stub_symbols(ve, ve->num_vstubs, ve->vstubs, ve->vstubs_ndx, STT_OBJECT)) goto failure;
	}


	return ve;

failure:
	if (ve != NULL)
		vita_elf_free(ve);
	return NULL;
}

static void free_rela_table(vita_elf_rela_table_t *rtable)
{
	if (rtable == NULL) return;
	free(rtable->relas);
	free_rela_table(rtable->next);
	free(rtable);
}

void vita_elf_free(vita_elf_t *ve)
{
	/* free() is safe to call on NULL */
	free(ve->fstubs);
	free(ve->vstubs);
	free(ve->symtab);
	if (ve->elf != NULL)
		elf_end(ve->elf);
	if (ve->fd >= 0)
		close(ve->fd);
	free(ve);
}

typedef vita_imports_stub_t *(*find_stub_func_ptr)(vita_imports_module_t *, uint32_t);
static int lookup_stubs(vita_elf_stub_t *stubs, int num_stubs, vita_imports_t *imports, find_stub_func_ptr find_stub, const char *stub_type_name)
{
	int found_all = 1;
	int i;
	vita_elf_stub_t *stub;

	for (i = 0; i < num_stubs; i++) {
		stub = &(stubs[i]);

		stub->library = vita_imports_find_lib(imports, stub->library_nid);
		if (stub->library == NULL) {
			warnx("Unable to find library with NID %u for %s symbol %s",
					stub->library_nid, stub_type_name,
					stub->symbol ? stub->symbol->name : "(unreferenced stub)");
			found_all = 0;
			continue;
		}

		stub->module = vita_imports_find_module(stub->library, stub->module_nid);
		if (stub->module == NULL) {
			warnx("Unable to find module with NID %u for %s symbol %s",
					stub->module_nid, stub_type_name,
					stub->symbol ? stub->symbol->name : "(unreferenced stub)");
			found_all = 0;
			continue;
		}

		stub->target = find_stub(stub->module, stub->target_nid);
		if (stub->target == NULL) {
			warnx("Unable to find %s with NID %u for symbol %s",
					stub_type_name, stub->module_nid,
					stub->symbol ? stub->symbol->name : "(unreferenced stub)");
			found_all = 0;
		}
	}

	return found_all;
}

int vita_elf_lookup_imports(vita_elf_t *ve, vita_imports_t *imports)
{
	int found_all = 1;

	if (!lookup_stubs(ve->fstubs, ve->num_fstubs, imports, &vita_imports_find_function, "function"))
		found_all = 0;
	if (!lookup_stubs(ve->vstubs, ve->num_vstubs, imports, &vita_imports_find_variable, "variable"))
		found_all = 0;

	return found_all;
}
