#include "block.h"
#include "buffer.h"
#include "view.h"
#include "hl.h"

#define BLOCK_EDIT_SIZE 512

static void sanity_check(void)
{
	struct block *blk;
	bool cursor_seen = false;

	if (!DEBUG)
		return;

	BUG_ON(list_empty(&buffer->blocks));

	list_for_each_entry(blk, &buffer->blocks, node) {
		BUG_ON(!blk->size && buffer->blocks.next->next != &buffer->blocks);
		BUG_ON(blk->size > blk->alloc);
		BUG_ON(blk->size && blk->data[blk->size - 1] != '\n');
		if (blk == view->cursor.blk)
			cursor_seen = true;
		if (DEBUG > 2)
			BUG_ON(count_nl(blk->data, blk->size) != blk->nl);
	}
	BUG_ON(!cursor_seen);
	BUG_ON(view->cursor.offset > view->cursor.blk->size);
}

static inline size_t ALLOC_ROUND(size_t size)
{
	return ROUND_UP(size, 64);
}

struct block *block_new(long alloc)
{
	struct block *blk = xnew0(struct block, 1);

	alloc = ALLOC_ROUND(alloc);
	blk->data = xnew(char, alloc);
	blk->alloc = alloc;
	return blk;
}

static void delete_block(struct block *blk)
{
	list_del(&blk->node);
	free(blk->data);
	free(blk);
}

static long copy_count_nl(char *dst, const char *src, long len)
{
	long i, nl = 0;
	for (i = 0; i < len; i++) {
		dst[i] = src[i];
		if (src[i] == '\n')
			nl++;
	}
	return nl;
}

static long insert_to_current(const char *buf, long len)
{
	struct block *blk = view->cursor.blk;
	long offset = view->cursor.offset;
	long size = blk->size + len;
	long nl;

	if (size > blk->alloc) {
		blk->alloc = ALLOC_ROUND(size);
		xrenew(blk->data, blk->alloc);
	}
	memmove(blk->data + offset + len, blk->data + offset, blk->size - offset);
	nl = copy_count_nl(blk->data + offset, buf, len);
	blk->nl += nl;
	blk->size = size;
	return nl;
}

/*
 * Combine current block and new data into smaller blocks:
 *   - Block _must_ contain whole lines
 *   - Block _must_ contain at least one line
 *   - Preferred maximum size of block is BLOCK_EDIT_SIZE
 *   - Size of any block can be larger than BLOCK_EDIT_SIZE
 *     only if there's a very long line
 */
static long split_and_insert(const char *buf, long len)
{
	struct block *blk = view->cursor.blk;
	struct list_head *prev_node = blk->node.prev;
	const char *buf1 = blk->data;
	const char *buf2 = buf;
	const char *buf3 = blk->data + view->cursor.offset;
	long size1 = view->cursor.offset;
	long size2 = len;
	long size3 = blk->size - size1;
	long total = size1 + size2 + size3;
	long start = 0; // beginning of new block
	long size = 0;  // size of new block
	long pos = 0;   // current position
	long nl_added = 0;

	while (start < total) {
		// size of new block if next line would be added
		long new_size = 0;
		long copied = 0;
		struct block *new;

		if (pos < size1) {
			const char *nl = memchr(buf1 + pos, '\n', size1 - pos);
			if (nl)
				new_size = nl - buf1 + 1 - start;
		}

		if (!new_size && pos < size1 + size2) {
			long offset = 0;
			const char *nl;

			if (pos > size1)
				offset = pos - size1;

			nl = memchr(buf2 + offset, '\n', size2 - offset);
			if (nl)
				new_size = size1 + nl - buf2 + 1 - start;
		}

		if (!new_size && pos < total) {
			long offset = 0;
			const char *nl;

			if (pos > size1 + size2)
				offset = pos - size1 - size2;

			nl = memchr(buf3 + offset, '\n', size3 - offset);
			if (nl)
				new_size = size1 + size2 + nl - buf3 + 1 - start;
			else
				new_size = total - start;
		}

		if (new_size <= BLOCK_EDIT_SIZE) {
			// fits
			size = new_size;
			pos = start + new_size;
			if (pos < total)
				continue;
		} else {
			// does not fit
			if (!size) {
				// one block containing one very long line
				size = new_size;
				pos = start + new_size;
			}
		}

		BUG_ON(!size);
		new = block_new(size);
		if (start < size1) {
			long avail = size1 - start;
			long count = size;

			if (count > avail)
				count = avail;
			new->nl += copy_count_nl(new->data, buf1 + start, count);
			copied += count;
			start += count;
		}
		if (start >= size1 && start < size1 + size2) {
			long offset = start - size1;
			long avail = size2 - offset;
			long count = size - copied;

			if (count > avail)
				count = avail;
			new->nl += copy_count_nl(new->data + copied, buf2 + offset, count);
			copied += count;
			start += count;
		}
		if (start >= size1 + size2) {
			long offset = start - size1 - size2;
			long avail = size3 - offset;
			long count = size - copied;

			BUG_ON(count > avail);
			new->nl += copy_count_nl(new->data + copied, buf3 + offset, count);
			copied += count;
			start += count;
		}

		new->size = size;
		BUG_ON(copied != size);
		list_add_before(&new->node, &blk->node);

		nl_added += new->nl;
		size = 0;
	}

	view->cursor.blk = BLOCK(prev_node->next);
	while (view->cursor.offset > view->cursor.blk->size) {
		view->cursor.offset -= view->cursor.blk->size;
		view->cursor.blk = BLOCK(view->cursor.blk->node.next);
	}

	nl_added -= blk->nl;
	delete_block(blk);
	return nl_added;
}

static long insert_bytes(const char *buf, long len)
{
	struct block *blk;
	long new_size;

	// blocks must contain whole lines
	// last char of buf might not be newline
	block_iter_normalize(&view->cursor);

	blk = view->cursor.blk;
	new_size = blk->size + len;
	if (new_size <= blk->alloc || new_size <= BLOCK_EDIT_SIZE)
		return insert_to_current(buf, len);

	if (blk->nl <= 1 && !memchr(buf, '\n', len)) {
		// can't split this possibly very long line
		// insert_to_current() is much faster than split_and_insert()
		return insert_to_current(buf, len);
	}
	return split_and_insert(buf, len);
}

void do_insert(const char *buf, long len)
{
	long nl = insert_bytes(buf, len);

	buffer->nl += nl;
	sanity_check();

	view_update_cursor_y(view);
	buffer_mark_lines_changed(view->buffer, view->cy, nl ? INT_MAX : view->cy);
	if (buffer->syn)
		hl_insert(buffer, view->cy, nl);
}

static int only_block(struct block *blk)
{
	return blk->node.prev == &buffer->blocks && blk->node.next == &buffer->blocks;
}

char *do_delete(long len)
{
	struct list_head *saved_prev_node = NULL;
	struct block *blk = view->cursor.blk;
	long offset = view->cursor.offset;
	long pos = 0;
	long deleted_nl = 0;
	char *buf;

	if (!len)
		return NULL;

	if (!offset) {
		// the block where cursor is can become empty and thereby may be deleted
		saved_prev_node = blk->node.prev;
	}

	buf = xnew(char, len);
	while (pos < len) {
		struct list_head *next = blk->node.next;
		long avail = blk->size - offset;
		long count = len - pos;
		long nl;

		if (count > avail)
			count = avail;
		nl = copy_count_nl(buf + pos, blk->data + offset, count);
		if (count < avail)
			memmove(blk->data + offset, blk->data + offset + count, avail - count);

		deleted_nl += nl;
		buffer->nl -= nl;
		blk->nl -= nl;
		blk->size -= count;
		if (!blk->size && !only_block(blk))
			delete_block(blk);

		offset = 0;
		pos += count;
		blk = BLOCK(next);

		BUG_ON(pos < len && next == &buffer->blocks);
	}

	if (saved_prev_node) {
		// cursor was at beginning of a block that was possibly deleted
		if (saved_prev_node->next == &buffer->blocks) {
			view->cursor.blk = BLOCK(saved_prev_node);
			view->cursor.offset = view->cursor.blk->size;
		} else {
			view->cursor.blk = BLOCK(saved_prev_node->next);
		}
	}

	blk = view->cursor.blk;
	if (blk->size && blk->data[blk->size - 1] != '\n' && blk->node.next != &buffer->blocks) {
		struct block *next = BLOCK(blk->node.next);
		long size = blk->size + next->size;

		if (size > blk->alloc) {
			blk->alloc = ALLOC_ROUND(size);
			xrenew(blk->data, blk->alloc);
		}
		memcpy(blk->data + blk->size, next->data, next->size);
		blk->size = size;
		blk->nl += next->nl;
		delete_block(next);
	}

	sanity_check();

	view_update_cursor_y(view);
	buffer_mark_lines_changed(view->buffer, view->cy, deleted_nl ? INT_MAX : view->cy);
	if (buffer->syn)
		hl_delete(buffer, view->cy, deleted_nl);
	return buf;
}

char *do_replace(long del, const char *buf, long ins)
{
	struct block *blk;
	long offset, avail, new_size;
	char *ptr, *deleted;
	long del_nl, ins_nl;

	block_iter_normalize(&view->cursor);
	blk = view->cursor.blk;
	offset = view->cursor.offset;

	avail = blk->size - offset;
	if (del >= avail)
		goto slow;

	new_size = blk->size + ins - del;
	if (new_size > BLOCK_EDIT_SIZE) {
		// should split
		if (blk->nl > 1 || memchr(buf, '\n', ins)) {
			// most likely can be splitted
			goto slow;
		}
	}

	if (new_size > blk->alloc) {
		blk->alloc = ALLOC_ROUND(new_size);
		xrenew(blk->data, blk->alloc);
	}

	// modification is limited to one block
	ptr = blk->data + offset;
	deleted = xmalloc(del);
	del_nl = copy_count_nl(deleted, ptr, del);
	blk->nl -= del_nl;
	buffer->nl -= del_nl;

	if (del != ins)
		memmove(ptr + ins, ptr + del, avail - del);

	ins_nl = copy_count_nl(ptr, buf, ins);
	blk->nl += ins_nl;
	buffer->nl += ins_nl;
	blk->size = new_size;

	sanity_check();

	view_update_cursor_y(view);
	if (del_nl == ins_nl) {
		// some line(s) changed but lines after them did not move up or down
		buffer_mark_lines_changed(view->buffer, view->cy, view->cy + del_nl);
	} else {
		buffer_mark_lines_changed(view->buffer, view->cy, INT_MAX);
	}
	if (buffer->syn) {
		hl_delete(buffer, view->cy, del_nl);
		hl_insert(buffer, view->cy, ins_nl);
	}
	return deleted;
slow:
	deleted = do_delete(del);
	do_insert(buf, ins);
	return deleted;
}
