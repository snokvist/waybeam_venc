#ifndef CV610_PQ_BIN_LOAD_H
#define CV610_PQ_BIN_LOAD_H

#include <stddef.h>

/* Whole-file loader for the PQTools `.bin` import, split out so the size
 * policy is host-testable without the vendor blob or the SigmaStar SDK.
 *
 * The vendor NRX reader (PQ_BIN_SetNRDataV2 in libbin.so, memcpy_s at 0x12fc)
 * copies a fixed 1298 bytes from a section offset that grows with a count field
 * taken from the file, up to 12 + 4*16.  The import's library-size gate
 * (OT_PQ_GetStructParamLen) reports a smaller section than that copy can reach
 * -- 1350 against 1374 -- so a buffer sized exactly for the file can be read
 * past its end.  This loader therefore allocates that difference as zeroed
 * slack past the file's bytes.  The reader's exact interior offset is a blob
 * detail; the constant is the worst case it can reach. */
#define CV610_PQ_NRX_VENDOR_MAX 1374u

/* Read `path` whole into a fresh buffer carrying CV610_PQ_NRX_VENDOR_MAX of
 * zeroed slack past the file.  Refuses files larger than `max_bytes`.
 * Returns NULL on any failure, otherwise a malloc'd buffer the caller frees;
 * `*out_len` receives the file length (not the allocation size). */
unsigned char *cv610_pq_bin_load(const char *path, size_t max_bytes,
	size_t *out_len);

#endif /* CV610_PQ_BIN_LOAD_H */
