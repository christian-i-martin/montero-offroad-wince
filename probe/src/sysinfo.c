/* Main.exe's system status table; see sysinfo.h. */
#include "sysinfo.h"

#include <stdio.h>

#ifdef _WIN32_WCE
#define snprintf _snprintf /* returns -1 without a terminating zero when the text does not fit */
#endif

/* In index order. From SkShareMem.dll RVA 0x3ff0, which adds the entries in a slightly different
 * order (0x2f after 0x17, 0x42 after 0x3d, 0x43 last). */
const SysInfoField sysinfo_fields[SYSINFO_FIELDS] = {
    {0x00, 0x00, 1}, {0x01, 0x01, 1}, {0x02, 0x02, 1}, {0x03, 0x03, 1},
    {0x04, 0x04, 1}, {0x05, 0x05, 1}, {0x06, 0x06, 1}, {0x07, 0x07, 1},
    {0x08, 0x08, 1}, {0x09, 0x09, 1}, {0x0a, 0x0a, 1}, {0x0b, 0x0b, 1},
    {0x0c, 0x0c, 1}, {0x0d, 0x0d, 1}, {0x0e, 0x0e, 1}, {0x0f, 0x0f, 1},
    {0x10, 0x10, 1}, {0x11, 0x11, 1}, {0x12, 0x12, 1}, {0x13, 0x13, 1},
    {0x14, 0x14, 1}, {0x15, 0x15, 1}, {0x16, 0x16, 1}, {0x17, 0x17, 1},
    {0x18, 0x18, 1}, {0x19, 0x19, 1}, {0x1a, 0x1a, 1}, {0x1b, 0x1b, 1},
    {0x1c, 0x1c, 1}, {0x1d, 0x1d, 1}, {0x1e, 0x1e, 1}, {0x1f, 0x1f, 1},
    {0x20, 0x20, 1}, {0x21, 0x21, 1}, {0x22, 0x22, 1}, {0x23, 0x23, 1},
    {0x24, 0x24, 1}, {0x25, 0x25, 1}, {0x26, 0x26, 1}, {0x27, 0x27, 1},
    {0x28, 0x28, 1}, {0x29, 0x29, 1}, {0x2a, 0x2a, 1}, {0x2b, 0x2b, 1},
    {0x2c, 0x2c, 4}, {0x2d, 0x30, 1}, {0x2e, 0x31, 1}, {0x2f, 0x32, 1},
    {0x30, 0x33, 1}, {0x31, 0x34, 1}, {0x32, 0x35, 1}, {0x33, 0x36, 1},
    {0x34, 0x37, 1}, {0x35, 0x38, 1}, {0x3b, 0x3e, 1}, {0x3c, 0x3f, 1},
    {0x3d, 0x41, 1}, {0x3e, 0x42, 1}, {0x3f, 0x43, 1}, {0x41, 0x45, 1},
    {0x42, 0x44, 1}, {0x43, 0x40, 1},
};

long sysinfo_value(const unsigned char *block, const SysInfoField *field)
{
    const unsigned char *p = block + field->offset;
    if (field->size == 4)
        return (long)(int)((unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24);
    return p[0];
}

static int owner_of(int offset)
{
    int i;
    for (i = 0; i < SYSINFO_FIELDS; i++)
        if (offset >= sysinfo_fields[i].offset && offset < sysinfo_fields[i].offset + sysinfo_fields[i].size)
            return i;
    return -1;
}

int sysinfo_diff(const unsigned char *before, const unsigned char *after, int use_fields,
                 sysinfo_change_fn report, void *context)
{
    int i, changes = 0;

    if (use_fields) {
        for (i = 0; i < SYSINFO_FIELDS; i++) {
            long a = sysinfo_value(before, &sysinfo_fields[i]), b = sysinfo_value(after, &sysinfo_fields[i]);
            if (a != b) {
                report(context, sysinfo_fields[i].index, sysinfo_fields[i].offset, a, b);
                changes++;
            }
        }
    }
    for (i = 0; i < SYSINFO_SIZE; i++) {
        if (before[i] == after[i] || (use_fields && owner_of(i) >= 0))
            continue;
        report(context, -1, i, before[i], after[i]);
        changes++;
    }
    return changes;
}

int sysinfo_format(const unsigned char *block, int first, int count, char *out, int size)
{
    int i, n = 0;

    if (size <= 0)
        return 0;
    out[0] = '\0';
    for (i = first; i < first + count && i < SYSINFO_FIELDS; i++) {
        int k = snprintf(out + n, (size_t)(size - n), "%s%02x=%ld", n ? " " : "", sysinfo_fields[i].index,
                         sysinfo_value(block, &sysinfo_fields[i]));
        if (k < 0 || k >= size - n) {
            out[n] = '\0';
            break;
        }
        n += k;
    }
    return n;
}
