/*
 * Copyright (C) 2018 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file iot_static_memory_common.c
 * @brief Implementation of common static memory functions in iot_static_memory.h
 */

/* Build using a config header, if provided. */
#ifdef IOT_CONFIG_FILE
    #include IOT_CONFIG_FILE
#endif

/* This file should only be compiled if dynamic memory allocation is forbidden. */
#if IOT_STATIC_MEMORY_ONLY == 1

/* Standard includes. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Platform layer includes. */
#include "platform/iot_threads.h"

/* Static memory include. */
#include "private/iot_static_memory.h"

/*-----------------------------------------------------------*/

/**
 * @cond DOXYGEN_IGNORE
 * Doxygen should ignore this section.
 *
 * Provide default values for undefined configuration constants.
 */
#ifndef IOT_MESSAGE_BUFFERS
    #define IOT_MESSAGE_BUFFERS        ( 8 )
#endif
#ifndef IOT_MESSAGE_BUFFER_SIZE
    #define IOT_MESSAGE_BUFFER_SIZE    ( 1024 )
#endif
/** @endcond */

/* Validate static memory configuration settings. */
#if IOT_MESSAGE_BUFFERS <= 0
    #error "IOT_MESSAGE_BUFFERS cannot be 0 or negative."
#endif
#if IOT_MESSAGE_BUFFER_SIZE <= 0
    #error "IOT_MESSAGE_BUFFER_SIZE cannot be 0 or negative."
#endif

/*-----------------------------------------------------------*/

/**
 * @brief Find a free buffer using the "in-use" flags.
 *
 * If a free buffer is found, this function marks the buffer in-use. This function
 * is common to the static memory implementation.
 *
 * @param[in] pInUse The "in-use" flags to search.
 * @param[in] limit How many flags to check.
 *
 * @return The index of a free buffer; -1 if no free buffers are available.
 */
int IotStaticMemory_FindFree( bool * const pInUse,
                              int limit );

/**
 * @brief Return an "in-use" buffer.
 *
 * This function is common to the static memory implementation.
 *
 * @param[in] ptr Pointer to the buffer to return.
 * @param[in] pPool The pool of buffers that the in-use buffer was allocation from.
 * @param[in] pInUse The "in-use" flags for pPool.
 * @param[in] limit How many buffers (and flags) to check while searching for ptr.
 * @param[in] elementSize The size of a single element in pPool.
 */
void IotStaticMemory_ReturnInUse( void * ptr,
                                  void * const pPool,
                                  bool * const pInUse,
                                  int limit,
                                  size_t elementSize );

/*-----------------------------------------------------------*/

/**
 * @brief Guards access to critical sections.
 */
static IotMutex_t _mutex;

/*
 * Static memory buffers and flags, allocated and zeroed at compile-time.
 */
static bool _pInUseMessageBuffers[ IOT_MESSAGE_BUFFERS ] = { 0 };                           /**< @brief Message buffer in-use flags. */
static char _pMessageBuffers[ IOT_MESSAGE_BUFFERS ][ IOT_MESSAGE_BUFFER_SIZE ] = { { 0 } }; /**< @brief Message buffers. */

/*-----------------------------------------------------------*/

int IotStaticMemory_FindFree( bool * const pInUse,
                              int limit )
{
    int i = 0, freeIndex = -1;

    /* Perform the search for a free buffer in a critical section. */
    IotMutex_Lock( &( _mutex ) );

    for( i = 0; i < limit; i++ )
    {
        if( pInUse[ i ] == false )
        {
            /* If a free buffer is found, mark it "in-use" and return its index. */
            pInUse[ i ] = true;
            freeIndex = i;
            break;
        }
    }

    /* Exit the critical section. */
    IotMutex_Unlock( &( _mutex ) );

    return freeIndex;
}

/*-----------------------------------------------------------*/

void IotStaticMemory_ReturnInUse( void * ptr,
                                  void * const pPool,
                                  bool * const pInUse,
                                  int limit,
                                  size_t elementSize )
{
    int i = 0;
    uint8_t * element = NULL;

    /* Clear ptr. */
    ( void ) memset( ptr, 0x00, elementSize );

    /* Perform a search for ptr to make sure it's part of pPool. This search
     * is done in a critical section. */
    IotMutex_Lock( &( _mutex ) );

    for( i = 0; i < limit; i++ )
    {
        /* Calculate address of the i-th element in pPool. */
        element = ( ( uint8_t * ) pPool ) + elementSize * ( size_t ) i;

        /* Check for a match. */
        if( ( ( void * ) element == ptr ) &&
            ( pInUse[ i ] == true ) )
        {
            pInUse[ i ] = false;
            break;
        }
    }

    /* Exit the critical section. */
    IotMutex_Unlock( &( _mutex ) );
}

/*-----------------------------------------------------------*/

bool IotStaticMemory_Init( void )
{
    return IotMutex_Create( &( _mutex ) );
}

/*-----------------------------------------------------------*/

void IotStaticMemory_Cleanup( void )
{
    IotMutex_Destroy( &( _mutex ) );
}

/*-----------------------------------------------------------*/

size_t Iot_MessageBufferSize( void )
{
    return ( size_t ) IOT_MESSAGE_BUFFER_SIZE;
}

/*-----------------------------------------------------------*/

void * Iot_MallocMessageBuffer( size_t size )
{
    int freeIndex = -1;
    void * pNewBuffer = NULL;

    /* Check that size is within the fixed message buffer size. */
    if( size <= IOT_MESSAGE_BUFFER_SIZE )
    {
        /* Get the index of a free message buffer. */
        freeIndex = IotStaticMemory_FindFree( _pInUseMessageBuffers,
                                              IOT_MESSAGE_BUFFERS );

        if( freeIndex != -1 )
        {
            pNewBuffer = &( _pMessageBuffers[ freeIndex ][ 0 ] );
        }
    }

    return pNewBuffer;
}

/*-----------------------------------------------------------*/

void Iot_FreeMessageBuffer( void * ptr )
{
    /* Return the in-use message buffer. */
    IotStaticMemory_ReturnInUse( ptr,
                                 _pMessageBuffers,
                                 _pInUseMessageBuffers,
                                 IOT_MESSAGE_BUFFERS,
                                 IOT_MESSAGE_BUFFER_SIZE );
}

/*-----------------------------------------------------------*/

#endif