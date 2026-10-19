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

#endif
