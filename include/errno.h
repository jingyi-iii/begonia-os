/* errno.h - the error codes the kernel returns.  There is no global `errno`:
 * a function returns 0 on success, or a negated code from here on failure,
 * e.g. `return -EINVAL;`. */
#ifndef ERRNO_H
#define ERRNO_H

#define EPERM             1     /* operation not permitted            */
#define ENOENT            2     /* no such entry                      */
#define EIO               5     /* I/O error                          */
#define EAGAIN           11     /* try again                          */
#define ENOMEM           12     /* out of memory                      */
#define EACCES           13     /* permission denied                  */
#define EFAULT           14     /* bad address                        */
#define EBUSY            16     /* resource busy                      */
#define EEXIST           17     /* already exists                     */
#define ENODEV           19     /* no such device                     */
#define EINVAL           22     /* invalid argument                   */
#define ENOSPC           28     /* no space left                      */
#define ERANGE           34     /* result out of range                */
#define ENOSYS           38     /* not implemented                    */
#define ETIMEDOUT       110     /* timed out                          */

#define E_PERM           -EPERM
#define E_NOENT          -ENOENT
#define E_IO             -EIO
#define E_AGAIN          -EAGAIN
#define E_NOMEM          -ENOMEM
#define E_ACCES          -EACCES
#define E_FAULT          -EFAULT
#define E_BUSY           -EBUSY
#define E_EXIST          -EEXIST
#define E_NODEV          -ENODEV
#define E_INVAL          -EINVAL
#define E_NOSPC          -ENOSPC
#define E_RANGE          -ERANGE
#define E_NOTFOUND       -ENODEV
#define E_NOTIMPL        -ENOSYS
#define E_TIMEDOUT       -ETIMEDOUT

#endif
