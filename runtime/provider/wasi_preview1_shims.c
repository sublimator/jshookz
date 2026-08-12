/*
 * Deterministic, capability-free replacements for the four WASI preview1
 * calls pulled in by wasi-libc.  The Xahau Hook provider must import no WASI:
 * it has no clock, files, descriptors, environment, or output capability.
 *
 * These symbols satisfy libc's otherwise-unresolved syscall layer inside the
 * provider.  They are not a substitute host.  QuickJS's observable Date clock
 * is supplied separately from the applying ledger context.
 */

#include <wasi/api.h>

__wasi_errno_t
__imported_wasi_snapshot_preview1_clock_time_get(
    __wasi_clockid_t id,
    __wasi_timestamp_t precision,
    __wasi_timestamp_t* result)
{
    (void)id;
    (void)precision;
    *result = 0;
    return __WASI_ERRNO_SUCCESS;
}

__wasi_errno_t
__imported_wasi_snapshot_preview1_fd_close(__wasi_fd_t fd)
{
    (void)fd;
    return __WASI_ERRNO_BADF;
}

__wasi_errno_t
__imported_wasi_snapshot_preview1_fd_seek(
    __wasi_fd_t fd,
    __wasi_filedelta_t offset,
    __wasi_whence_t whence,
    __wasi_filesize_t* result)
{
    (void)fd;
    (void)offset;
    (void)whence;
    *result = 0;
    return __WASI_ERRNO_BADF;
}

__wasi_errno_t
__imported_wasi_snapshot_preview1_fd_write(
    __wasi_fd_t fd,
    __wasi_ciovec_t const* iovs,
    size_t iovs_len,
    __wasi_size_t* result)
{
    (void)fd;
    (void)iovs;
    (void)iovs_len;
    *result = 0;
    return __WASI_ERRNO_BADF;
}
