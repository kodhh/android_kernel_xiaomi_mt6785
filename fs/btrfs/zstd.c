/*
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/bio.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/zstd.h>
#include "compression.h"

#define ZSTD_BTRFS_MAX_WINDOWLOG 17
#define ZSTD_BTRFS_MAX_INPUT (1 << ZSTD_BTRFS_MAX_WINDOWLOG)
#define ZSTD_BTRFS_DEFAULT_LEVEL 3

static zstd_parameters zstd_get_btrfs_parameters(size_t src_len)
{
    zstd_parameters params = zstd_get_params(ZSTD_BTRFS_DEFAULT_LEVEL,
                                             src_len);

    if (params.cParams.windowLog > ZSTD_BTRFS_MAX_WINDOWLOG)
        params.cParams.windowLog = ZSTD_BTRFS_MAX_WINDOWLOG;
    WARN_ON(src_len > ZSTD_BTRFS_MAX_INPUT);
    return params;
}

struct workspace {
    void *mem;
    size_t size;
    char *buf;
    struct list_head list;
    zstd_in_buffer in_buf;
    zstd_out_buffer out_buf;
    unsigned int level;
    zstd_cstream *cstream;
    zstd_dstream *dstream;
    size_t cstream_size;   // 压缩流内存大小
    size_t dstream_size;   // 解压流内存大小
};

static void zstd_set_level(struct list_head *ws, unsigned int type)
{
    struct workspace *workspace = list_entry(ws, struct workspace, list);
    unsigned int level = (type & 0xF0) >> 4;  // 从高4位提取级别
    
    /* ZSTD支持级别1-22，0表示使用默认 */
    if (level == 0) {
        workspace->level = ZSTD_BTRFS_DEFAULT_LEVEL;
    } else if (level >= 1 && level <= 22) {
        workspace->level = level;
    } else {
        /* 超出范围，使用默认并警告 */
        pr_warn("BTRFS: zstd compression level %u out of range (1-22), using default %u\n",
                level, ZSTD_BTRFS_DEFAULT_LEVEL);
        workspace->level = ZSTD_BTRFS_DEFAULT_LEVEL;
    }
    
    pr_debug("BTRFS: zstd set compression level to %u\n", workspace->level);
}

static void zstd_free_workspace(struct list_head *ws)
{
    struct workspace *workspace = list_entry(ws, struct workspace, list);

    kvfree(workspace->mem);
    kfree(workspace->buf);
    kfree(workspace);
}

static struct list_head *zstd_alloc_workspace(void)
{
    struct workspace *workspace;
    size_t cstream_size, dstream_size, total_size;
    int max_level = zstd_max_clevel();
    zstd_parameters params = zstd_get_btrfs_parameters(ZSTD_BTRFS_MAX_INPUT);

    workspace = kzalloc(sizeof(*workspace), GFP_KERNEL);
    if (!workspace)
        return ERR_PTR(-ENOMEM);

    /* 使用最大压缩级别计算工作空间大小 */
    zstd_parameters max_params = zstd_get_params(max_level, ZSTD_BTRFS_MAX_INPUT);
    if (max_params.cParams.windowLog > ZSTD_BTRFS_MAX_WINDOWLOG)
        max_params.cParams.windowLog = ZSTD_BTRFS_MAX_WINDOWLOG;

    cstream_size = zstd_cstream_workspace_bound(&max_params.cParams);
    dstream_size = zstd_dstream_workspace_bound(1 << ZSTD_BTRFS_MAX_WINDOWLOG);

    total_size = cstream_size + dstream_size;

    workspace->size = total_size;
    workspace->mem = kvmalloc(workspace->size, GFP_KERNEL);
    workspace->buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!workspace->mem || !workspace->buf)
        goto fail;

    workspace->level = ZSTD_BTRFS_DEFAULT_LEVEL;
    workspace->cstream_size = cstream_size;
    workspace->dstream_size = dstream_size;

    /* 压缩流使用前部内存 */
    workspace->cstream = zstd_init_cstream(&params, 0, 
                                          workspace->mem, 
                                          cstream_size);
    if (!workspace->cstream) {
        pr_warn("BTRFS: failed to init cstream\n");
        goto fail;
    }

    /* 解压流使用后部内存 */
    workspace->dstream = zstd_init_dstream(1 << ZSTD_BTRFS_MAX_WINDOWLOG,
                                          workspace->mem + cstream_size,
                                          dstream_size);
    if (!workspace->dstream) {
        pr_warn("BTRFS: failed to init dstream\n");
        goto fail;
    }

    INIT_LIST_HEAD(&workspace->list);
    pr_debug("BTRFS: zstd allocated workspace\n");

    return &workspace->list;
fail:
    zstd_free_workspace(&workspace->list);
    return ERR_PTR(-ENOMEM);
}

static int zstd_compress_pages(struct list_head *ws,
                               struct address_space *mapping,
                               u64 start,
                               struct page **pages,
                               unsigned long *out_pages,
                               unsigned long *total_in,
                               unsigned long *total_out)
{
    struct workspace *workspace = list_entry(ws, struct workspace, list);
    zstd_cstream *stream;
    int ret = 0;
    int nr_pages = 0;
    struct page *in_page = NULL;  /* The current page to read */
    struct page *out_page = NULL; /* The current page to write to */
    unsigned long tot_in = 0;
    unsigned long tot_out = 0;
    unsigned long len = *total_out;
    const unsigned long nr_dest_pages = *out_pages;
    unsigned long max_out = nr_dest_pages * PAGE_SIZE;
    
    /* 获取基于当前压缩级别的参数 */
    zstd_parameters params = zstd_get_params(workspace->level, len);
    if (params.cParams.windowLog > ZSTD_BTRFS_MAX_WINDOWLOG)
        params.cParams.windowLog = ZSTD_BTRFS_MAX_WINDOWLOG;
    params.fParams.contentSizeFlag = 1;

    *out_pages = 0;
    *total_out = 0;
    *total_in = 0;

    /* 重置压缩流，使用新的参数 */
    ret = zstd_reset_cstream(workspace->cstream, len);
    if (zstd_is_error(ret)) {
        pr_warn("BTRFS: zstd_reset_cstream failed: %s\n", 
                zstd_get_error_name(ret));
        return -EIO;
    }

    /* map in the first page of input data */
    in_page = find_get_page(mapping, start >> PAGE_SHIFT);
    workspace->in_buf.src = kmap(in_page);
    workspace->in_buf.pos = 0;
    workspace->in_buf.size = min_t(size_t, len, PAGE_SIZE);

    /* Allocate and map in the output buffer */
    out_page = alloc_page(GFP_NOFS | __GFP_HIGHMEM);
    if (out_page == NULL) {
        ret = -ENOMEM;
        goto out;
    }
    pages[nr_pages++] = out_page;
    workspace->out_buf.dst = kmap(out_page);
    workspace->out_buf.pos = 0;
    workspace->out_buf.size = min_t(size_t, max_out, PAGE_SIZE);

    while (1) {
        size_t ret2;

        ret2 = zstd_compress_stream(workspace->cstream, &workspace->out_buf,
                                    &workspace->in_buf);
        if (zstd_is_error(ret2)) {
            pr_debug("BTRFS: zstd_compress_stream returned %d\n",
                     zstd_get_error_code(ret2));
            ret = -EIO;
            goto out;
        }

        /* Check to see if we are making it bigger */
        if (tot_in + workspace->in_buf.pos > 8192 &&
            tot_in + workspace->in_buf.pos <
            tot_out + workspace->out_buf.pos) {
            ret = -E2BIG;
            goto out;
        }

        /* We've reached the end of our output range */
        if (workspace->out_buf.pos >= max_out) {
            tot_out += workspace->out_buf.pos;
            ret = -E2BIG;
            goto out;
        }

        /* Check if we need more output space */
        if (workspace->out_buf.pos == workspace->out_buf.size) {
            tot_out += PAGE_SIZE;
            max_out -= PAGE_SIZE;
            kunmap(out_page);
            if (nr_pages == nr_dest_pages) {
                out_page = NULL;
                ret = -E2BIG;
                goto out;
            }
            out_page = alloc_page(GFP_NOFS | __GFP_HIGHMEM);
            if (out_page == NULL) {
                ret = -ENOMEM;
                goto out;
            }
            pages[nr_pages++] = out_page;
            workspace->out_buf.dst = kmap(out_page);
            workspace->out_buf.pos = 0;
            workspace->out_buf.size = min_t(size_t, max_out,
                                            PAGE_SIZE);
        }

        /* We've reached the end of the input */
        if (workspace->in_buf.pos >= len) {
            tot_in += workspace->in_buf.pos;
            break;
        }

        /* Check if we need more input */
        if (workspace->in_buf.pos == workspace->in_buf.size) {
            tot_in += PAGE_SIZE;
            kunmap(in_page);
            put_page(in_page);

            start += PAGE_SIZE;
            len -= PAGE_SIZE;
            in_page = find_get_page(mapping, start >> PAGE_SHIFT);
            workspace->in_buf.src = kmap(in_page);
            workspace->in_buf.pos = 0;
            workspace->in_buf.size = min_t(size_t, len, PAGE_SIZE);
        }
    }
    
    while (1) {
        size_t ret2;

        ret2 = zstd_end_stream(workspace->cstream, &workspace->out_buf);
        if (zstd_is_error(ret2)) {
            pr_debug("BTRFS: zstd_end_stream returned %d\n",
                     zstd_get_error_code(ret2));
            ret = -EIO;
            goto out;
        }
        if (ret2 == 0) {
            tot_out += workspace->out_buf.pos;
            break;
        }
        if (workspace->out_buf.pos >= max_out) {
            tot_out += workspace->out_buf.pos;
            ret = -E2BIG;
            goto out;
        }

        tot_out += PAGE_SIZE;
        max_out -= PAGE_SIZE;
        kunmap(out_page);
        if (nr_pages == nr_dest_pages) {
            out_page = NULL;
            ret = -E2BIG;
            goto out;
        }
        out_page = alloc_page(GFP_NOFS | __GFP_HIGHMEM);
        if (out_page == NULL) {
            ret = -ENOMEM;
            goto out;
        }
        pages[nr_pages++] = out_page;
        workspace->out_buf.dst = kmap(out_page);
        workspace->out_buf.pos = 0;
        workspace->out_buf.size = min_t(size_t, max_out, PAGE_SIZE);
    }

    if (tot_out >= tot_in) {
        ret = -E2BIG;
        goto out;
    }

    ret = 0;
    *total_in = tot_in;
    *total_out = tot_out;
out:
    *out_pages = nr_pages;
    /* Cleanup */
    if (in_page) {
        kunmap(in_page);
        put_page(in_page);
    }
    if (out_page)
        kunmap(out_page);
    return ret;
}

static int zstd_decompress_bio(struct list_head *ws, struct compressed_bio *cb)
{
    struct workspace *workspace = list_entry(ws, struct workspace, list);
    struct page **pages_in = cb->compressed_pages;
    u64 disk_start = cb->start;
    struct bio *orig_bio = cb->orig_bio;
    size_t srclen = cb->compressed_len;
    int ret = 0;
    unsigned long page_in_index = 0;
    unsigned long total_pages_in = DIV_ROUND_UP(srclen, PAGE_SIZE);
    unsigned long buf_start;
    unsigned long total_out = 0;

    /* 重置流式解压上下文 */
    ret = zstd_reset_dstream(workspace->dstream);
    if (zstd_is_error(ret)) {
        pr_debug("BTRFS: zstd_reset_dstream failed: %s\n", 
                zstd_get_error_name(ret));
        ret = -EIO;
        goto done;
    }

    workspace->in_buf.src = kmap(pages_in[page_in_index]);
    workspace->in_buf.pos = 0;
    workspace->in_buf.size = min_t(size_t, srclen, PAGE_SIZE);

    workspace->out_buf.dst = workspace->buf;
    workspace->out_buf.pos = 0;
    workspace->out_buf.size = PAGE_SIZE;

    while (1) {
        size_t ret2;

        ret2 = zstd_decompress_stream(workspace->dstream, &workspace->out_buf,
                                      &workspace->in_buf);
        if (zstd_is_error(ret2)) {
            pr_debug("BTRFS: zstd_decompress_stream returned %d\n",
                     zstd_get_error_code(ret2));
            ret = -EIO;
            goto done;
        }
        buf_start = total_out;
        total_out += workspace->out_buf.pos;
        workspace->out_buf.pos = 0;

        ret = btrfs_decompress_buf2page(workspace->out_buf.dst,
                                        buf_start, total_out, disk_start, orig_bio);
        if (ret == 0)
            break;

        if (workspace->in_buf.pos >= srclen)
            break;

        /* Check if we've hit the end of a frame */
        if (ret2 == 0)
            break;

        if (workspace->in_buf.pos == workspace->in_buf.size) {
            kunmap(pages_in[page_in_index++]);
            if (page_in_index >= total_pages_in) {
                workspace->in_buf.src = NULL;
                ret = -EIO;
                goto done;
            }
            srclen -= PAGE_SIZE;
            workspace->in_buf.src = kmap(pages_in[page_in_index]);
            workspace->in_buf.pos = 0;
            workspace->in_buf.size = min_t(size_t, srclen, PAGE_SIZE);
        }
    }
    ret = 0;
    zero_fill_bio(orig_bio);
done:
    if (workspace->in_buf.src)
        kunmap(pages_in[page_in_index]);
    return ret;
}

static int zstd_decompress(struct list_head *ws, unsigned char *data_in,
                           struct page *dest_page,
                           unsigned long start_byte,
                           size_t srclen, size_t destlen)
{
    struct workspace *workspace = list_entry(ws, struct workspace, list);
    int ret = 0;
    size_t ret2;
    unsigned long total_out = 0;
    unsigned long pg_offset = 0;
    char *kaddr;

    /* 重置流式解压上下文 */
    ret = zstd_reset_dstream(workspace->dstream);
    if (zstd_is_error(ret)) {
        pr_warn("BTRFS: zstd_reset_dstream failed: %s\n", 
                zstd_get_error_name(ret));
        ret = -EIO;
        goto finish;
    }

    destlen = min_t(size_t, destlen, PAGE_SIZE);

    workspace->in_buf.src = data_in;
    workspace->in_buf.pos = 0;
    workspace->in_buf.size = srclen;

    workspace->out_buf.dst = workspace->buf;
    workspace->out_buf.pos = 0;
    workspace->out_buf.size = PAGE_SIZE;

    ret2 = 1;
    while (pg_offset < destlen
           && workspace->in_buf.pos < workspace->in_buf.size) {
        unsigned long buf_start;
        unsigned long buf_offset;
        unsigned long bytes;

        /* Check if the frame is over and we still need more input */
        if (ret2 == 0) {
            pr_debug("BTRFS: zstd_decompress_stream ended early\n");
            ret = -EIO;
            goto finish;
        }
        ret2 = zstd_decompress_stream(workspace->dstream, &workspace->out_buf,
                                      &workspace->in_buf);
        if (zstd_is_error(ret2)) {
            pr_debug("BTRFS: zstd_decompress_stream returned %d\n",
                     zstd_get_error_code(ret2));
            ret = -EIO;
            goto finish;
        }

        buf_start = total_out;
        total_out += workspace->out_buf.pos;
        workspace->out_buf.pos = 0;

        if (total_out <= start_byte)
            continue;

        if (total_out > start_byte && buf_start < start_byte)
            buf_offset = start_byte - buf_start;
        else
            buf_offset = 0;

        bytes = min_t(unsigned long, destlen - pg_offset,
                      workspace->out_buf.size - buf_offset);

        kaddr = kmap_atomic(dest_page);
        memcpy(kaddr + pg_offset, workspace->out_buf.dst + buf_offset,
               bytes);
        kunmap_atomic(kaddr);

        pg_offset += bytes;
    }
    ret = 0;
finish:
    if (pg_offset < destlen) {
        kaddr = kmap_atomic(dest_page);
        memset(kaddr + pg_offset, 0, destlen - pg_offset);
        kunmap_atomic(kaddr);
    }
    return ret;
}

const struct btrfs_compress_op btrfs_zstd_compress = {
    .alloc_workspace = zstd_alloc_workspace,
    .free_workspace = zstd_free_workspace,
    .compress_pages = zstd_compress_pages,
    .decompress_bio = zstd_decompress_bio,
    .decompress = zstd_decompress,
    .set_level = zstd_set_level,
};
