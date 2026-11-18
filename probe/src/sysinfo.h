/* Main.exe's system status table ("system info"): the 72-byte shared memory block
 * "SkSysInfoShareMem" that SkShareMem.dll's SP_SetSysInfo / SP_GetSysInfo read and write.
 * Layout decoded from SkShareMem.dll (147,456 bytes, firmware cne-6982e-a), function at RVA 0x3ff0:
 * 62 indexes, each one byte except index 0x2c (a 32-bit little-endian int at offsets 0x2c to 0x2f).
 * Indexes 0x36 to 0x3a and 0x40 do not exist; offsets 0x39 to 0x3d, 0x46 and 0x47 belong to no index.
 * Portable C so it can be tested on the build machine. */
#ifndef SYSINFO_H
#define SYSINFO_H

#define SYSINFO_SIZE 0x48
#define SYSINFO_FIELDS 62

typedef struct {
    unsigned char index;   /* SysInfoIndex, as passed to SP_GetSysInfo and broadcast in 0x52A wParam */
    unsigned char offset;  /* byte offset in the shared block */
    unsigned char size;    /* 1 (unsigned byte) or 4 (signed little-endian int) */
} SysInfoField;

extern const SysInfoField sysinfo_fields[SYSINFO_FIELDS];

/* The value of one field in a copy of the block. */
long sysinfo_value(const unsigned char *block, const SysInfoField *field);

/* Called once per change. index is -1 for a byte that belongs to no index (offset tells which). */
typedef void (*sysinfo_change_fn)(void *context, int index, int offset, long before, long after);

/* Compares two copies of the block. With use_fields, reports changed fields by index and changed
 * bytes outside all fields by offset; without (layout not verified), reports every changed byte by
 * offset. Returns the number of changes reported. */
int sysinfo_diff(const unsigned char *before, const unsigned char *after, int use_fields,
                 sysinfo_change_fn report, void *context);

/* Formats fields [first, first + count) as "index=value" pairs in hex index order, for example
 * "00=1 01=0 2c=-1". Returns the length written (always zero-terminated). */
int sysinfo_format(const unsigned char *block, int first, int count, char *out, int size);

#endif
