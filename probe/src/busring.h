/* Reader for the message bus ring buffer in the SkIPCMsgShareMem shared memory.
 *
 * Layout (decoded from SkCom.dll, see docs/vehicle-data-research.md):
 *   header at offset 0: u32 size, u32 start (0x10), u32 write offset, u32 end   (little-endian)
 *   records from start: "msg\0", u16 length, <length bytes: serialized message>, u16 checksum
 *   checksum = 16-bit sum of the message bytes; a writer that cannot fit a record wraps to start.
 * A writer reserves space (moves the write offset) before copying the record, so a record just
 * behind the write offset may still be incomplete: its checksum tells.
 * Portable C so it can be tested on the build machine.
 */
#ifndef BUSRING_H
#define BUSRING_H

#define BUSRING_OVERHEAD 8      /* magic + length + checksum */
#define BUSRING_STALL_LIMIT 20  /* polls to wait for an incomplete record before resyncing */

typedef struct {
    unsigned cursor;   /* offset of the next record to read; 0 = start at the current write offset */
    unsigned stall;
    unsigned resyncs;  /* times the reader lost its place and jumped to the write offset */
} BusRing;

typedef struct {
    int what, arg1, arg2, sender;
} BusHeader;

typedef void (*busring_record_fn)(void *context, unsigned offset, const unsigned char *message, int length);

/* Check the shared-memory header. Returns 1 if it looks like the bus ring. */
int busring_header_ok(const unsigned char *shm, unsigned shm_size);

/* Deliver complete records between the cursor and the write offset, at most max_records.
 * Returns the number delivered, or -1 if the header is invalid. */
int busring_poll(BusRing *ring, const unsigned char *shm, unsigned shm_size, int max_records,
                 busring_record_fn fn, void *context);

/* Parse the RemoteMessage prefix: "RMSG", what, arg1, arg2, sender. Returns 1 on success. */
int busring_message_header(const unsigned char *message, int length, BusHeader *header);

/* Bundle attached to a message (seen on the car): after the 20-byte RemoteMessage prefix,
 *   "BNDL", u32 entry count, then per entry: u32 key length incl. NUL, key bytes, u32 type, value
 *   type 1 = int (4 bytes), type 4 = string (u32 length incl. NUL, bytes), type 5 = bytes (u32 length,
 *   bytes). Other types are unknown and stop the parse. */
#define BUNDLE_INT 1
#define BUNDLE_STRING 4
#define BUNDLE_BYTES 5

typedef struct {
    const char *key;            /* points into the message, NUL-terminated */
    int type;
    int int_value;              /* BUNDLE_INT */
    const unsigned char *data;  /* BUNDLE_STRING / BUNDLE_BYTES */
    int data_length;
} BundleEntry;

/* Returns the number of entries filled in (at most max_entries; 0 if there is no bundle).
 * *complete is set to 1 when the whole bundle parsed, 0 when it stopped early (malformed data or
 * an unknown value type). */
int busring_bundle_entries(const unsigned char *message, int length, BundleEntry *entries, int max_entries,
                           int *complete);

/* Find a byte-array entry by key. Returns its length and sets *data, or -1 if absent. */
int busring_bundle_bytes(const unsigned char *message, int length, const char *key, const unsigned char **data);

/* Render a bundle as "key=value key2="text" key3=<n bytes: hex>" for logging. */
int busring_bundle_format(const unsigned char *message, int length, char *out, int out_size);

#endif
