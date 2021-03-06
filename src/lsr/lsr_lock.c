/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2013  LiteSpeed Technologies, Inc.                        *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/
#include <lsr/lsr_lock.h>

#include <errno.h>
#include <string.h>
#include <sys/time.h>


/*
 *   futex
 */

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__gnu_linux__)

int lsr_futex_setup( lsr_mutex_t * p )
{
#if 0
    register int * lp = (int *)p;
    assert (*lp != lock_Inuse);
#endif
        
    *((int *)p) = lock_Avail;
    return 0;
}

#endif

int lsr_atomic_spin_setup( lsr_spinlock_t * p )
{
    *((int *)p) = lock_Avail;
    return 0;
}


/*
 *   pthread 
 */
int lsr_pthread_mutex_setup( pthread_mutex_t * p )
{
    pthread_mutexattr_t myAttr;
    pthread_mutexattr_init(&myAttr);
#if defined(USE_MUTEX_ADAPTIVE) 
    pthread_mutexattr_settype(&myAttr, PTHREAD_MUTEX_ADAPTIVE_NP);
#else  /* defined(USE_MUTEX_ADAPTIVE) */
    /* pthread_mutexattr_settype(&myAttr, PTHREAD_MUTEX_NORMAL); */
    pthread_mutexattr_settype(&myAttr, 
#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__gnu_linux__)
                            PTHREAD_MUTEX_ERRORCHECK_NP
#else  /* defined(linux) */                          
                            PTHREAD_MUTEX_ERRORCHECK
#endif  /* defined(linux) */
                             );
#endif  /* defined(USE_MUTEX_ADAPTIVE) */
    /* pthread_mutexattr_settype(&myAttr, PTHREAD_MUTEX_RECURSIVE); */
    /* pthread_mutexattr_settype(&myAttr, PTHREAD_MUTEX_RECURSIVE_NP); */
    
    pthread_mutexattr_setpshared(&myAttr, PTHREAD_PROCESS_SHARED);

    int code = pthread_mutex_init( (pthread_mutex_t *)p, &myAttr );
    if ( (!code) || (code == EBUSY) )
    {
        // already inited... ok..
        return 0;
    }
    return -1;
}


/*
 * pthread spinlock
 */
int lsr_pspinlock_setup( lsr_pspinlock_t *  p )
{
    int code;
#if defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__)
    *p = OS_SPINLOCK_INIT;
    code = 0;
#else
    code = pthread_spin_init( (pthread_spinlock_t *)p,
                            PTHREAD_PROCESS_SHARED
                            /*  PTHREAD_PROCESS_PRIVATE */
                        );
#endif
    if ( (!code) || (code == EBUSY) )
    {
        // already inited... ok..
        return 0;
    }
    return -1;
}

