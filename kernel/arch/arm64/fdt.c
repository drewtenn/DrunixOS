/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * fdt.c - Flattened device tree walker for arm64.
 *
 * Phase 1 M2.4a of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md.
 * Implements just enough of the device-tree-spec v0.4 token stream to
 * find /memory and /chosen/bootargs. Header validation rejects bad
 * magic, version, and a sanity ceiling on totalsize.
 *
 * The walker treats the blob as read-only data: no allocations, no
 * mutation. Strings and big-endian integers are accessed through
 * helper inlines.
 */

#include "fdt.h"
#include "kstring.h"
#include <stdint.h>

/*
 * FDT structure-block tokens. dtspec v0.4 §5.4 defines five tokens
 * encoded big-endian; we accept v17 layout (the only version any
 * recent QEMU emits).
 */
#define FDT_BEGIN_NODE 0x00000001u
#define FDT_END_NODE 0x00000002u
#define FDT_PROP 0x00000003u
#define FDT_NOP 0x00000004u
#define FDT_END 0x00000009u

#define FDT_MIN_VERSION 17u
#define FDT_MAX_VERSION 17u
#define FDT_TOTALSIZE_CEILING 0x00100000u /* 1 MiB; a safety net */

#define FDT_DEFAULT_ADDRESS_CELLS 2u
#define FDT_DEFAULT_SIZE_CELLS 1u

/* On-disk FDT header. All fields are big-endian. */
struct fdt_header_be {
	uint32_t magic;
	uint32_t totalsize;
	uint32_t off_dt_struct;
	uint32_t off_dt_strings;
	uint32_t off_mem_rsvmap;
	uint32_t version;
	uint32_t last_comp_version;
	uint32_t boot_cpuid_phys;
	uint32_t size_dt_strings;
	uint32_t size_dt_struct;
};

uint64_t g_fdt_blob_phys;

static uint32_t fdt_be32(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;

	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	       ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static uint64_t fdt_be64(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;

	return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
	       ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
	       ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
	       ((uint64_t)b[6] << 8) | (uint64_t)b[7];
}

static uint32_t fdt_align4(uint32_t v)
{
	return (v + 3u) & ~3u;
}

int fdt_validate(const void *fdt)
{
	const struct fdt_header_be *h = (const struct fdt_header_be *)fdt;
	uint32_t magic;
	uint32_t totalsize;
	uint32_t version;

	if (!fdt)
		return -1;

	magic = fdt_be32(&h->magic);
	if (magic != FDT_MAGIC)
		return -1;

	totalsize = fdt_be32(&h->totalsize);
	if (totalsize < sizeof(*h) || totalsize > FDT_TOTALSIZE_CEILING)
		return -1;

	version = fdt_be32(&h->version);
	if (version < FDT_MIN_VERSION || version > FDT_MAX_VERSION)
		return -1;

	return 0;
}

/*
 * Cursor into the structure block. `cur` always points to the next
 * 4-byte-aligned token to consume.
 */
struct fdt_cursor {
	const uint8_t *base;
	const uint8_t *cur;
	const uint8_t *end;
	const char *strings;
};

static int fdt_cursor_init(struct fdt_cursor *c, const void *fdt)
{
	const struct fdt_header_be *h = (const struct fdt_header_be *)fdt;
	uint32_t off_struct;
	uint32_t size_struct;
	uint32_t off_strings;
	uint32_t totalsize;

	off_struct = fdt_be32(&h->off_dt_struct);
	size_struct = fdt_be32(&h->size_dt_struct);
	off_strings = fdt_be32(&h->off_dt_strings);
	totalsize = fdt_be32(&h->totalsize);

	if (off_struct + size_struct > totalsize)
		return -1;
	if (off_strings >= totalsize)
		return -1;

	c->base = (const uint8_t *)fdt;
	c->cur = c->base + off_struct;
	c->end = c->cur + size_struct;
	c->strings = (const char *)c->base + off_strings;
	return 0;
}

static int fdt_cursor_read32(struct fdt_cursor *c, uint32_t *out)
{
	if (c->cur + 4u > c->end)
		return -1;
	*out = fdt_be32(c->cur);
	c->cur += 4u;
	return 0;
}

static const char *fdt_cursor_read_string(struct fdt_cursor *c)
{
	const char *s = (const char *)c->cur;
	uint32_t len;

	while (c->cur < c->end && *c->cur != 0)
		c->cur++;
	if (c->cur >= c->end)
		return 0;
	c->cur++; /* skip the NUL */
	len = (uint32_t)(c->cur - (const uint8_t *)s);
	c->cur += fdt_align4(len) - len;
	return s;
}

static int fdt_cursor_read_prop(struct fdt_cursor *c,
                                const char **name,
                                const uint8_t **value,
                                uint32_t *value_len)
{
	uint32_t len;
	uint32_t nameoff;

	if (fdt_cursor_read32(c, &len) != 0)
		return -1;
	if (fdt_cursor_read32(c, &nameoff) != 0)
		return -1;
	if (c->cur + len > c->end)
		return -1;

	*name = c->strings + nameoff;
	*value = c->cur;
	*value_len = len;
	c->cur += fdt_align4(len);
	return 0;
}

/*
 * Scan structure tokens until we land at the start of a node whose
 * name matches `target`. On success `*out_cur` points just past the
 * FDT_BEGIN_NODE token (i.e., on the first prop or child).
 *
 * `target_prefix` is matched against the node name up to the '@'
 * separator so "memory@40000000" matches the prefix "memory".
 *
 * Callers position the cursor just past the root's FDT_BEGIN_NODE +
 * name (i.e., inside the root node), so we start at depth 1. The
 * `depth == 1` check below then fires on direct children of root.
 */
static int fdt_find_top_level_node(struct fdt_cursor *c,
                                   const char *target,
                                   uint32_t target_len,
                                   struct fdt_cursor *out_cur)
{
	int depth = 1;

	while (c->cur < c->end) {
		uint32_t token;

		if (fdt_cursor_read32(c, &token) != 0)
			return -1;
		switch (token) {
		case FDT_BEGIN_NODE: {
			const char *name = fdt_cursor_read_string(c);
			uint32_t i;

			if (!name)
				return -1;
			if (depth == 1) {
				/* Direct child of root; check the prefix
				 * before the optional '@'. */
				int matches = 1;

				for (i = 0; i < target_len; i++) {
					if (name[i] != target[i]) {
						matches = 0;
						break;
					}
				}
				if (matches &&
				    (name[target_len] == '\0' || name[target_len] == '@')) {
					*out_cur = *c;
					return 0;
				}
			}
			depth++;
			break;
		}
		case FDT_END_NODE:
			depth--;
			if (depth < 0)
				return -1;
			break;
		case FDT_PROP: {
			const char *prop_name;
			const uint8_t *prop_val;
			uint32_t prop_len;

			if (fdt_cursor_read_prop(c, &prop_name, &prop_val, &prop_len) != 0)
				return -1;
			break;
		}
		case FDT_NOP:
			break;
		case FDT_END:
			return -1;
		default:
			return -1;
		}
	}
	return -1;
}

/*
 * Walk the props of the node `c` is positioned at and accumulate
 * memory ranges from a `reg = <base size base size ...>` property.
 * Address-cells and size-cells default to 2/1; on QEMU virt both are
 * 2. We honour explicit #address-cells / #size-cells from the parent
 * (root) properties when present in the upcoming node-prop loop, but
 * for simplicity treat the canonical virt encoding as the spec.
 */
static int fdt_node_collect_memory(struct fdt_cursor *c,
                                   uint32_t address_cells,
                                   uint32_t size_cells,
                                   fdt_memory_range_t *out,
                                   uint32_t max,
                                   uint32_t *count)
{
	while (c->cur < c->end) {
		uint32_t token;

		if (fdt_cursor_read32(c, &token) != 0)
			return -1;
		if (token == FDT_END_NODE || token == FDT_END)
			return 0;
		if (token == FDT_NOP)
			continue;
		if (token == FDT_BEGIN_NODE) {
			/* Skip child node body. */
			(void)fdt_cursor_read_string(c);
			while (c->cur < c->end) {
				uint32_t inner;

				if (fdt_cursor_read32(c, &inner) != 0)
					return -1;
				if (inner == FDT_END_NODE)
					break;
			}
			continue;
		}
		if (token != FDT_PROP)
			return -1;
		{
			const char *prop_name;
			const uint8_t *prop_val;
			uint32_t prop_len;
			uint32_t pair_bytes = (address_cells + size_cells) * 4u;

			if (fdt_cursor_read_prop(c, &prop_name, &prop_val, &prop_len) != 0)
				return -1;
			if (k_strcmp(prop_name, "reg") != 0)
				continue;
			if (pair_bytes == 0u)
				return -1;
			while (prop_len >= pair_bytes && *count < max) {
				uint64_t base;
				uint64_t size;

				if (address_cells == 1u)
					base = fdt_be32(prop_val);
				else
					base = fdt_be64(prop_val);
				prop_val += address_cells * 4u;

				if (size_cells == 1u)
					size = fdt_be32(prop_val);
				else
					size = fdt_be64(prop_val);
				prop_val += size_cells * 4u;

				out[*count].base = base;
				out[*count].size = size;
				(*count)++;
				prop_len -= pair_bytes;
			}
		}
	}
	return -1;
}

int fdt_get_memory(const void *fdt,
                   fdt_memory_range_t *out,
                   uint32_t max,
                   uint32_t *count)
{
	struct fdt_cursor cursor;
	struct fdt_cursor node;
	uint32_t address_cells = FDT_DEFAULT_ADDRESS_CELLS;
	uint32_t size_cells = FDT_DEFAULT_SIZE_CELLS;
	uint32_t token;

	if (!fdt || !out || !count || max == 0u)
		return -1;
	*count = 0;

	if (fdt_validate(fdt) != 0)
		return -1;
	if (fdt_cursor_init(&cursor, fdt) != 0)
		return -1;

	/* The first token must be FDT_BEGIN_NODE for the root. */
	if (fdt_cursor_read32(&cursor, &token) != 0)
		return -1;
	if (token != FDT_BEGIN_NODE)
		return -1;
	(void)fdt_cursor_read_string(&cursor); /* root name (empty) */

	/* Walk root-level props before recursing into children to learn
	 * #address-cells / #size-cells. */
	while (cursor.cur < cursor.end) {
		uint32_t peek_token;
		const uint8_t *save = cursor.cur;

		if (fdt_cursor_read32(&cursor, &peek_token) != 0)
			return -1;
		if (peek_token == FDT_PROP) {
			const char *prop_name;
			const uint8_t *prop_val;
			uint32_t prop_len;

			if (fdt_cursor_read_prop(
			        &cursor, &prop_name, &prop_val, &prop_len) != 0)
				return -1;
			if (k_strcmp(prop_name, "#address-cells") == 0 && prop_len == 4u)
				address_cells = fdt_be32(prop_val);
			else if (k_strcmp(prop_name, "#size-cells") == 0 && prop_len == 4u)
				size_cells = fdt_be32(prop_val);
			continue;
		}
		if (peek_token == FDT_NOP)
			continue;
		/* First non-prop token: rewind so the node finder sees it. */
		cursor.cur = save;
		break;
	}

	if (fdt_find_top_level_node(&cursor, "memory", 6u, &node) != 0)
		return -1;

	/* Position `node.cur` after the FDT_BEGIN_NODE token + name; the
	 * fdt_find_top_level_node helper already consumed those.
	 * fdt_node_collect_memory now reads props until FDT_END_NODE. */
	if (fdt_node_collect_memory(
	        &node, address_cells, size_cells, out, max, count) != 0)
		return -1;
	if (*count == 0u)
		return -1;
	return 0;
}

const char *fdt_get_chosen_bootargs(const void *fdt)
{
	struct fdt_cursor cursor;
	struct fdt_cursor node;
	uint32_t token;

	if (!fdt)
		return 0;
	if (fdt_validate(fdt) != 0)
		return 0;
	if (fdt_cursor_init(&cursor, fdt) != 0)
		return 0;

	if (fdt_cursor_read32(&cursor, &token) != 0 || token != FDT_BEGIN_NODE)
		return 0;
	(void)fdt_cursor_read_string(&cursor);

	if (fdt_find_top_level_node(&cursor, "chosen", 6u, &node) != 0)
		return 0;

	while (node.cur < node.end) {
		uint32_t inner;

		if (fdt_cursor_read32(&node, &inner) != 0)
			return 0;
		if (inner == FDT_END_NODE)
			return 0;
		if (inner == FDT_NOP)
			continue;
		if (inner == FDT_BEGIN_NODE) {
			(void)fdt_cursor_read_string(&node);
			continue;
		}
		if (inner == FDT_PROP) {
			const char *prop_name;
			const uint8_t *prop_val;
			uint32_t prop_len;

			if (fdt_cursor_read_prop(&node, &prop_name, &prop_val, &prop_len) !=
			    0)
				return 0;
			if (k_strcmp(prop_name, "bootargs") == 0)
				return (const char *)prop_val;
			continue;
		}
		return 0;
	}
	return 0;
}
