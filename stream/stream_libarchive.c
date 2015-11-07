/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <archive.h>
#include <archive_entry.h>

#include "common/common.h"
#include "stream.h"

#include "stream_libarchive.h"

struct mp_archive_volume {
    struct stream *src;
    bool own_stream;
    char *buffer;
};

static ssize_t read_cb(struct archive *arch, void *priv, const void **buffer)
{
    struct mp_archive_volume *vol = priv;
    int res = stream_read_partial(vol->src, vol->buffer, MP_ARCHIVE_READ_SIZE);
    *buffer = vol->buffer;
    return MPMAX(res, 0);
}

static int64_t seek_cb(struct archive *arch, void *priv,
                       int64_t offset, int whence)
{
    struct mp_archive_volume *vol = priv;
    switch (whence) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        offset += vol->src->pos;
        break;
    case SEEK_END: ;
        int64_t size = stream_get_size(vol->src);
        if (size < 0)
            return -1;
        offset += size;
        break;
    default:
        return -1;
    }
    return stream_seek(vol->src, offset) ? stream_tell(vol->src) : -1;
}

static int64_t skip_cb(struct archive *arch, void *priv, int64_t request)
{
    struct mp_archive_volume *vol = priv;
    int64_t old = stream_tell(vol->src);
    stream_skip(vol->src, request);
    return stream_tell(vol->src) - old;
}

static int switch_cb(struct archive *arch, void *oldpriv, void *newpriv)
{
    struct mp_archive_volume *newvol = newpriv;
    return stream_seek(newvol->src, 0) ? ARCHIVE_OK : ARCHIVE_FATAL;
}

static int close_cb(struct archive *arch, void *priv)
{
    struct mp_archive_volume *vol = priv;
    if (vol->own_stream)
        free_stream(vol->src);
    talloc_free(vol);
    return ARCHIVE_OK;
}

void mp_archive_free(struct mp_archive *mpa)
{
    if (mpa && mpa->arch) {
        archive_read_close(mpa->arch);
        archive_read_free(mpa->arch);
    }
    talloc_free(mpa);
}

struct file_pattern {
    const char *match;
    const char *format;
    char* (*volume_url)(const struct file_pattern *p,
                        const char* volume_base, int index);
    int start;
    int stop;
};

static char* standard_volume_url(const struct file_pattern *p,
                                 const char *volume_base, int index)
{
    return talloc_asprintf(NULL, p->format, volume_base, index);
}

static char* old_rar_volume_url(const struct file_pattern *p,
                                const char *volume_base, int index)
{
    // dumb, but unfortunately very common.
    // WinRAR actually does increment the "r" above 100
    return talloc_asprintf(NULL, p->format, volume_base,
                           'r' + index / 100, index % 100);
}

static const struct file_pattern *find_volume_pattern(const char *location)
{
    static const struct file_pattern patterns[] = {
        { ".part1.rar",   "%s.part%.1d.rar", standard_volume_url, 2,   9},
        { ".part01.rar",  "%s.part%.2d.rar", standard_volume_url, 2,  99},
        { ".part001.rar", "%s.part%.3d.rar", standard_volume_url, 2, 999},
        { ".rar",         "%s.%c%.2d",        old_rar_volume_url, 0, 999},
        { NULL, NULL, 0, 0 },
    };

    const size_t location_size = strlen(location);
    for (int i = 0; patterns[i].match != NULL; i++) {
        const size_t match_size = strlen(patterns[i].match);

        if (location_size < match_size)
            continue;
        if (!strcmp(&location[location_size - match_size], patterns[i].match))
            return &patterns[i];
    }
    return NULL;
}

static bool add_volume(struct mp_archive *mpa, struct stream *s, bool own_stream)
{
    struct mp_archive_volume *vol = talloc_zero(mpa, struct mp_archive_volume);
    stream_seek(s, 0);
    vol->src = s;
    vol->own_stream = own_stream;
    vol->buffer = mpa->buffer;
    return archive_read_append_callback_data(mpa->arch, vol) == ARCHIVE_OK;
}

struct mp_archive *mp_archive_new(struct mp_log *log, struct stream *src,
                                  int flags)
{
    struct mp_archive *mpa = talloc_zero(NULL, struct mp_archive);
    mpa->arch = archive_read_new();
    if (!mpa->arch)
        goto err;

    // first volume is the primary stream
    if (!add_volume(mpa, src, false))
        goto err;

    // try to open other volumes
    const struct file_pattern *pattern = find_volume_pattern(src->url);
    if (pattern) {
        int base_len = strlen(src->url) - strlen(pattern->match);
        char *volume_base = talloc_asprintf(mpa, "%.*s", base_len, src->url);
        for (int i = pattern->start; i <= pattern->stop; i++) {
            char *volume_url = pattern->volume_url(pattern, volume_base, i);
            struct stream *s = stream_create(volume_url, STREAM_READ, src->cancel, src->global);
            talloc_free(volume_url);
            if (!s)
                break;
            if (!add_volume(mpa, s, true))
                goto err;
        }
        talloc_free(volume_base);
    }

    archive_read_support_format_7zip(mpa->arch);
    archive_read_support_format_iso9660(mpa->arch);
    archive_read_support_format_rar(mpa->arch);
    archive_read_support_format_zip(mpa->arch);
    archive_read_support_filter_bzip2(mpa->arch);
    archive_read_support_filter_gzip(mpa->arch);
    archive_read_support_filter_xz(mpa->arch);
    if (flags & MP_ARCHIVE_FLAG_UNSAFE) {
        archive_read_support_format_gnutar(mpa->arch);
        archive_read_support_format_tar(mpa->arch);
    }

    archive_read_set_read_callback(mpa->arch, read_cb);
    archive_read_set_skip_callback(mpa->arch, skip_cb);
    archive_read_set_switch_callback(mpa->arch, switch_cb);
    archive_read_set_close_callback(mpa->arch, close_cb);
    if (src->seekable)
        archive_read_set_seek_callback(mpa->arch, seek_cb);
    if (archive_read_open1(mpa->arch) < ARCHIVE_OK)
        goto err;
    return mpa;

err:
    mp_archive_free(mpa);
    return NULL;
}

struct priv {
    struct mp_archive *mpa;
    struct stream *src;
    int64_t entry_size;
    char *entry_name;
};

static int reopen_archive(stream_t *s)
{
    struct priv *p = s->priv;
    mp_archive_free(p->mpa);
    p->mpa = mp_archive_new(s->log, p->src, MP_ARCHIVE_FLAG_UNSAFE);
    if (!p->mpa)
        return STREAM_ERROR;

    // Follows the same logic as demux_libarchive.c.
    struct mp_archive *mpa = p->mpa;
    int num_files = 0;
    for (;;) {
        struct archive_entry *entry;
        int r = archive_read_next_header(mpa->arch, &entry);
        if (r == ARCHIVE_EOF) {
            MP_ERR(s, "archive entry not found. '%s'\n", p->entry_name);
            goto error;
        }
        if (r < ARCHIVE_OK)
            MP_ERR(s, "%s\n", archive_error_string(mpa->arch));
        if (r < ARCHIVE_WARN)
            goto error;
        if (archive_entry_filetype(entry) != AE_IFREG)
            continue;
        const char *fn = archive_entry_pathname(entry);
        char buf[64];
        if (!fn) {
            snprintf(buf, sizeof(buf), "mpv_unknown#%d\n", num_files);
            fn = buf;
        }
        if (strcmp(p->entry_name, fn) == 0) {
            p->entry_size = -1;
            if (archive_entry_size_is_set(entry))
                p->entry_size = archive_entry_size(entry);
            return STREAM_OK;
        }
        num_files++;
    }

error:
    mp_archive_free(p->mpa);
    p->mpa = NULL;
    MP_ERR(s, "could not open archive\n");
    return STREAM_ERROR;
}

static int archive_entry_fill_buffer(stream_t *s, char *buffer, int max_len)
{
    struct priv *p = s->priv;
    if (!p->mpa)
        return 0;
    int r = archive_read_data(p->mpa->arch, buffer, max_len);
    if (r < 0)
        MP_ERR(s, "%s\n", archive_error_string(p->mpa->arch));
    return r;
}

static int archive_entry_seek(stream_t *s, int64_t newpos)
{
    struct priv *p = s->priv;
    if (!p->mpa)
        return -1;
    if (archive_seek_data(p->mpa->arch, newpos, SEEK_SET) >= 0)
        return 1;
    // libarchive can't seek in most formats.
    if (newpos < s->pos) {
        // Hack seeking backwards into working by reopening the archive and
        // starting over.
        MP_VERBOSE(s, "trying to reopen archive for performing seek\n");
        if (reopen_archive(s) < STREAM_OK)
            return -1;
        s->pos = 0;
    }
    if (newpos > s->pos) {
        // For seeking forwards, just keep reading data (there's no libarchive
        // skip function either).
        char buffer[4096];
        while (newpos > s->pos) {
            int size = MPMIN(newpos - s->pos, sizeof(buffer));
            int r = archive_read_data(p->mpa->arch, buffer, size);
            if (r < 0) {
                MP_ERR(s, "%s\n", archive_error_string(p->mpa->arch));
                return -1;
            }
            s->pos += r;
        }
    }
    return 1;
}

static void archive_entry_close(stream_t *s)
{
    struct priv *p = s->priv;
    mp_archive_free(p->mpa);
    free_stream(p->src);
}

static int archive_entry_control(stream_t *s, int cmd, void *arg)
{
    struct priv *p = s->priv;
    switch (cmd) {
    case STREAM_CTRL_GET_BASE_FILENAME:
        *(char **)arg = talloc_strdup(NULL, p->src->url);
        return STREAM_OK;
    case STREAM_CTRL_GET_SIZE:
        if (p->entry_size < 0)
            break;
        *(int64_t *)arg = p->entry_size;
        return STREAM_OK;
    }
    return STREAM_UNSUPPORTED;
}

static int archive_entry_open(stream_t *stream)
{
    struct priv *p = talloc_zero(stream, struct priv);
    stream->priv = p;

    if (!strchr(stream->path, '|'))
        return STREAM_ERROR;

    char *base = talloc_strdup(p, stream->path);
    char *name = strchr(base, '|');
    *name++ = '\0';
    p->entry_name = name;
    mp_url_unescape_inplace(base);

    p->src = stream_create(base, STREAM_READ | STREAM_SAFE_ONLY,
                           stream->cancel, stream->global);
    if (!p->src) {
        archive_entry_close(stream);
        return STREAM_ERROR;
    }

    int r = reopen_archive(stream);
    if (r < STREAM_OK) {
        archive_entry_close(stream);
        return r;
    }

    stream->fill_buffer = archive_entry_fill_buffer;
    if (p->src->seekable) {
        stream->seek = archive_entry_seek;
        stream->seekable = true;
    }
    stream->close = archive_entry_close;
    stream->control = archive_entry_control;

    return STREAM_OK;
}

const stream_info_t stream_info_libarchive = {
    .name = "libarchive",
    .open = archive_entry_open,
    .protocols = (const char*const[]){ "archive", NULL },
};
