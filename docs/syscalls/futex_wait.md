# mx_futex_wait

## NAME

futex_wait - Wait on a futex.

## SYNOPSIS

```
#include <magenta/syscalls.h>

mx_status_t mx_futex_wait(int* value_ptr, int current_value,
                          mx_time_t timeout);
```

## DESCRIPTION

Waiting on a futex (or acquiring it) causes a thread to sleep until
the futex is made available by a call to `mx_futex_wake`. Optionally,
the thread can also be woken up after the timeout argument expires.

## RETURN VALUE

**futex_wait**() returns **NO_ERROR** on success.

## ERRORS

**ERR_INVALID_ARGS**  *value_ptr* is not a valid userspace pointer.

**ERR_ALREADY_BOUND**  *current_value* does not match the value at *value_ptr*.

**ERR_TIMED_OUT**  The thread was not woken before *timeout* expired.

## SEE ALSO

[futex_requeue](futex_requeue.md)
[futex_wake](futex_wake.md)
