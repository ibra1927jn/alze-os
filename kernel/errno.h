/*
 * Anykernel OS — Error Codes
 *
 * POSIX-inspired errno values for consistent error reporting.
 * All kernel functions returning int use negative errno on failure.
 */

#ifndef ERRNO_H
#define ERRNO_H

#define ESUCCESS    0   /* No error */
#define ENOMEM      1   /* Out of memory */
#define EINVAL      2   /* Invalid argument */
#define ENOENT      3   /* No such file or entry */
#define EBUSY       4   /* Resource busy */
#define EAGAIN      5   /* Try again (resource temporarily unavailable) */
#define ENOSPC      6   /* No space left */
#define ENOTSUP     7   /* Operation not supported */
#define EBADF       8   /* Bad file descriptor */
#define EMFILE      9   /* Too many open files */
#define EPERM      10   /* Permission denied */
#define EEXIST     11   /* Already exists */
#define EOVERFLOW  12   /* Value too large */
#define ETIMEDOUT  13   /* Timed out */
#define EDEADLK    14   /* Deadlock would occur */
#define ERANGE     15   /* Result out of range */
#define EIO        16   /* I/O error */
#define ESRCH      17   /* No such process/task */

#endif /* ERRNO_H */
