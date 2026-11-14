#include "busring.h"

#include <string.h>

#define RMSG_MAGIC 0x47534D52u /* "RMSG" read as a little-endian u32 */

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned sum16(const unsigned char *p, unsigned n)
{
    unsigned s = 0, i;
    for (i = 0; i < n; i++)
        s = (s + p[i]) & 0xFFFF;
    return s;
}

int busring_header_ok(const unsigned char *shm, unsigned size)
{
    unsigned start, write, end;
    if (shm == 0 || size < 16)
        return 0;
    start = rd32(shm + 4);
    write = rd32(shm + 8);
    end = rd32(shm + 12);
    return start >= 16 && start < end && end <= size && write >= start && write <= end;
}

int busring_poll(BusRing *r, const unsigned char *shm, unsigned size, int max_records,
                 busring_record_fn fn, void *context)
{
    unsigned start, write, end;
    int delivered = 0, wraps = 0;

    if (!busring_header_ok(shm, size))
        return -1;
    start = rd32(shm + 4);
    write = rd32(shm + 8);
    end = rd32(shm + 12);
    if (r->cursor < start || r->cursor > end)
        r->cursor = write;

    while (delivered < max_records) {
        unsigned c = r->cursor, len;
        const unsigned char *rec = shm + c;

        if (c == write)
            break;
        if (c + BUSRING_OVERHEAD > end)
            goto wrap;
        if (memcmp(rec, "msg", 4) != 0)
            goto bad;
        len = rd16(rec + 4);
        if (c + BUSRING_OVERHEAD + len > end)
            goto bad;
        if (c < write && c + BUSRING_OVERHEAD + len > write)
            goto bad;
        if (sum16(rec + 6, len) != rd16(rec + 6 + len))
            goto bad;
        r->stall = 0;
        if (fn)
            fn(context, c, rec + 6, (int)len);
        r->cursor = c + BUSRING_OVERHEAD + len;
        delivered++;
        continue;
    bad:
        if (c > write)
            goto wrap; /* end of the previous lap */
        /* Behind the write offset: the writer may have reserved this space and not copied the
         * record yet. Wait a few polls before giving up and jumping to the write offset. */
        if (++r->stall > BUSRING_STALL_LIMIT) {
            r->cursor = write;
            r->stall = 0;
            r->resyncs++;
        }
        break;
    wrap:
        if (wraps++) {
            r->cursor = write;
            r->resyncs++;
            break;
        }
        r->cursor = start;
    }
    return delivered;
}

int busring_message_header(const unsigned char *m, int length, BusHeader *h)
{
    if (m == 0 || length < 20 || rd32(m) != RMSG_MAGIC)
        return 0;
    h->what = (int)rd32(m + 4);
    h->arg1 = (int)rd32(m + 8);
    h->arg2 = (int)rd32(m + 12);
    h->sender = (int)rd32(m + 16);
    return 1;
}

#define BNDL_MAGIC 0x4C444E42u /* "BNDL" read as a little-endian u32 */

int busring_bundle_entries(const unsigned char *m, int length, BundleEntry *e, int max, int *complete)
{
    int pos, count, i, n = 0;

    *complete = 0;
    if (m == 0 || length < 28 || rd32(m) != RMSG_MAGIC || rd32(m + 20) != BNDL_MAGIC)
        return 0;
    count = (int)rd32(m + 24);
    pos = 28;
    if (count < 0 || count > 256)
        return 0;
    for (i = 0; i < count; i++) {
        int key_len, type;
        BundleEntry entry;
        if (pos + 4 > length) return n;
        key_len = (int)rd32(m + pos);
        pos += 4;
        if (key_len < 1 || pos + key_len + 4 > length || m[pos + key_len - 1] != 0) return n;
        entry.key = (const char *)(m + pos);
        pos += key_len;
        type = (int)rd32(m + pos);
        pos += 4;
        entry.type = type;
        entry.int_value = 0;
        entry.data = 0;
        entry.data_length = 0;
        if (type == BUNDLE_INT) {
            if (pos + 4 > length) return n;
            entry.int_value = (int)rd32(m + pos);
            pos += 4;
        } else if (type == BUNDLE_STRING || type == BUNDLE_BYTES) {
            int len;
            if (pos + 4 > length) return n;
            len = (int)rd32(m + pos);
            pos += 4;
            if (len < 0 || pos + len > length) return n;
            entry.data = m + pos;
            entry.data_length = len;
            pos += len;
        } else {
            return n; /* unknown type: its size is unknown, so stop */
        }
        if (n < max)
            e[n++] = entry;
    }
    *complete = 1;
    return n;
}

int busring_bundle_bytes(const unsigned char *m, int length, const char *key, const unsigned char **data)
{
    BundleEntry entries[16];
    int complete, n = busring_bundle_entries(m, length, entries, 16, &complete), i;
    for (i = 0; i < n; i++)
        if (entries[i].type == BUNDLE_BYTES && strcmp(entries[i].key, key) == 0) {
            *data = entries[i].data;
            return entries[i].data_length;
        }
    return -1;
}

static int put(char *out, int size, int used, const char *text, int len)
{
    if (len < 0)
        len = (int)strlen(text);
    if (used + len >= size)
        len = size - used - 1;
    if (len > 0) {
        memcpy(out + used, text, (size_t)len);
        used += len;
    }
    out[used] = '\0';
    return used;
}

int busring_bundle_format(const unsigned char *m, int length, char *out, int size)
{
    static const char hex[] = "0123456789abcdef";
    BundleEntry entries[24];
    char num[24];
    int n, i, k, used = 0, complete;

    if (out == 0 || size <= 0)
        return 0;
    out[0] = '\0';
    n = busring_bundle_entries(m, length, entries, 24, &complete);
    for (i = 0; i < n; i++) {
        const BundleEntry *e = &entries[i];
        if (used)
            used = put(out, size, used, " ", 1);
        used = put(out, size, used, e->key, -1);
        used = put(out, size, used, "=", 1);
        if (e->type == BUNDLE_INT) {
            int v = e->int_value, len = 0;
            unsigned u = v < 0 ? (unsigned)(-(long)v) : (unsigned)v;
            char rev[16];
            do { rev[len++] = (char)('0' + u % 10); u /= 10; } while (u && len < 15);
            k = 0;
            if (v < 0) num[k++] = '-';
            while (len) num[k++] = rev[--len];
            used = put(out, size, used, num, k);
        } else if (e->type == BUNDLE_STRING) {
            int len = e->data_length > 0 && e->data[e->data_length - 1] == 0 ? e->data_length - 1 : e->data_length;
            used = put(out, size, used, "\"", 1);
            used = put(out, size, used, (const char *)e->data, len);
            used = put(out, size, used, "\"", 1);
        } else {
            used = put(out, size, used, "<", 1);
            for (k = 0; k < e->data_length && k < 32; k++) {
                char pair[3] = {hex[e->data[k] >> 4], hex[e->data[k] & 15], ' '};
                used = put(out, size, used, pair, k + 1 < e->data_length && k < 31 ? 3 : 2);
            }
            used = put(out, size, used, ">", 1);
        }
    }
    if (!complete && length > 20)
        used = put(out, size, used, used ? " (rest not parsed)" : "(no readable bundle)", -1);
    return used;
}
