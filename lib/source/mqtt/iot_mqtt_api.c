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
 * @file iot_mqtt_api.c
 * @brief Implements most user-facing functions of the MQTT library.
 */

/* Build using a config header, if provided. */
#ifdef IOT_CONFIG_FILE
    #include IOT_CONFIG_FILE
#endif

/* Standard includes. */
#include <string.h>

/* Error handling include. */
#include "private/iot_error.h"

/* MQTT internal include. */
#include "private/iot_mqtt_internal.h"

/* Platform layer includes. */
#include "platform/iot_clock.h"
#include "platform/iot_threads.h"

/* Validate MQTT configuration settings. */
#if IOT_MQTT_ENABLE_ASSERTS != 0 && IOT_MQTT_ENABLE_ASSERTS != 1
    #error "IOT_MQTT_ENABLE_ASSERTS must be 0 or 1."
#endif
#if IOT_MQTT_ENABLE_METRICS != 0 && IOT_MQTT_ENABLE_METRICS != 1
    #error "IOT_MQTT_ENABLE_METRICS must be 0 or 1."
#endif
#if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES != 0 && IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES != 1
    #error "IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES must be 0 or 1."
#endif
#if IOT_MQTT_TEST != 0 && IOT_MQTT_TEST != 1
    #error "IOT_MQTT_MQTT_TEST must be 0 or 1."
#endif
#if IOT_MQTT_RESPONSE_WAIT_MS <= 0
    #error "IOT_MQTT_RESPONSE_WAIT_MS cannot be 0 or negative."
#endif
#if IOT_MQTT_RETRY_MS_CEILING <= 0
    #error "IOT_MQTT_RETRY_MS_CEILING cannot be 0 or negative."
#endif

/*-----------------------------------------------------------*/

/**
 * @brief Set the unsubscribed flag of an MQTT subscription.
 *
 * @param[in] pSubscriptionLink Pointer to the link member of an #_mqttSubscription_t.
 * @param[in] pMatch Not used.
 *
 * @return Always returns `true`.
 */
static bool _mqttSubscription_setUnsubscribe( const IotLink_t * pSubscriptionLink,
                                              void * pMatch );

/**
 * @brief Destroy an MQTT subscription if its reference count is 0.
 *
 * @param[in] pData The subscription to destroy. This parameter is of type
 * `void*` for compatibility with [free]
 * (http://pubs.opengroup.org/onlinepubs/9699919799/functions/free.html).
 */
static void _mqttSubscription_tryDestroy( void * pData );

/**
 * @brief Decrement the reference count of an MQTT operation and attempt to
 * destroy it.
 *
 * @param[in] pData The operation data to destroy. This parameter is of type
 * `void*` for compatibility with [free]
 * (http://pubs.opengroup.org/onlinepubs/9699919799/functions/free.html).
 */
static void _mqttOperation_tryDestroy( void * pData );

/**
 * @brief Create a keep-alive job for an MQTT connection.
 *
 * @param[in] pNetworkInfo User-provided network information for the new
 * connection.
 * @param[in] keepAliveSeconds User-provided keep-alive interval.
 * @param[out] pMqttConnection The MQTT connection associated with the keep-alive.
 *
 * @return `true` if the keep-alive job was successfully created; `false` otherwise.
 */
static bool _createKeepAliveJob( const IotMqttNetworkInfo_t * pNetworkInfo,
                                 uint16_t keepAliveSeconds,
                                 _mqttConnection_t * pMqttConnection );

/**
 * @brief Creates a new MQTT connection and initializes its members.
 *
 * @param[in] awsIotMqttMode Specifies if this connection is to an AWS IoT MQTT server.
 * @param[in] pNetworkInfo User-provided network information for the new
 * connection.
 * @param[in] keepAliveSeconds User-provided keep-alive interval for the new connection.
 *
 * @return Pointer to a newly-created MQTT connection; `NULL` on failure.
 */
static _mqttConnection_t * _createMqttConnection( bool awsIotMqttMode,
                                                  const IotMqttNetworkInfo_t * pNetworkInfo,
                                                  uint16_t keepAliveSeconds );

/**
 * @brief Destroys the members of an MQTT connection.
 *
 * @param[in] pMqttConnection Which connection to destroy.
 */
static void _destroyMqttConnection( _mqttConnection_t * pMqttConnection );

/**
 * @brief The common component of both @ref mqtt_function_subscribe and @ref
 * mqtt_function_unsubscribe.
 *
 * See @ref mqtt_function_subscribe or @ref mqtt_function_unsubscribe for a
 * description of the parameters and return values.
 */
static IotMqttError_t _subscriptionCommon( IotMqttOperationType_t operation,
                                           IotMqttConnection_t mqttConnection,
                                           const IotMqttSubscription_t * pSubscriptionList,
                                           size_t subscriptionCount,
                                           uint32_t flags,
                                           const IotMqttCallbackInfo_t * pCallbackInfo,
                                           IotMqttReference_t * pSubscriptionRef );

/*-----------------------------------------------------------*/

static bool _mqttSubscription_setUnsubscribe( const IotLink_t * pSubscriptionLink,
                                              void * pMatch )
{
    /* Because this function is called from a container function, the given link
     * must never be NULL. */
    IotMqtt_Assert( pSubscriptionLink != NULL );

    _mqttSubscription_t * pSubscription = IotLink_Container( _mqttSubscription_t,
                                                             pSubscriptionLink,
                                                             link );

    /* Silence warnings about unused parameters. */
    ( void ) pMatch;

    /* Set the unsubscribed flag. */
    pSubscription->unsubscribed = true;

    return true;
}

/*-----------------------------------------------------------*/

static void _mqttSubscription_tryDestroy( void * pData )
{
    _mqttSubscription_t * pSubscription = ( _mqttSubscription_t * ) pData;

    /* Reference count must not be negative. */
    IotMqtt_Assert( pSubscription->references >= 0 );

    /* Unsubscribed flag should be set. */
    IotMqtt_Assert( pSubscription->unsubscribed == true );

    /* Free the subscription if it has no references. */
    if( pSubscription->references == 0 )
    {
        IotMqtt_FreeSubscription( pSubscription );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }
}

/*-----------------------------------------------------------*/

static void _mqttOperation_tryDestroy( void * pData )
{
    _mqttOperation_t * pOperation = ( _mqttOperation_t * ) pData;

    /* Decrement reference count and destroy operation if possible. */
    if( _IotMqtt_DecrementOperationReferences( pOperation, true ) == true )
    {
        _IotMqtt_DestroyOperation( pOperation );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }
}

/*-----------------------------------------------------------*/

static bool _createKeepAliveJob( const IotMqttNetworkInfo_t * pNetworkInfo,
                                 uint16_t keepAliveSeconds,
                                 _mqttConnection_t * pMqttConnection )
{
    bool status = true;
    IotMqttError_t serializeStatus = IOT_MQTT_SUCCESS;
    IotTaskPoolError_t jobStatus = IOT_TASKPOOL_SUCCESS;

    /* Network information is not used when MQTT packet serializers are disabled. */
    ( void ) pNetworkInfo;

    /* Default PINGREQ serializer function. */
    IotMqttError_t ( * serializePingreq )( uint8_t **,
                                           size_t * ) = _IotMqtt_SerializePingreq;

    /* Convert the keep-alive interval to milliseconds. */
    pMqttConnection->keepAliveMs = keepAliveSeconds * 1000;
    pMqttConnection->nextKeepAliveMs = pMqttConnection->keepAliveMs;

    /* Choose a PINGREQ serializer function. */
    #if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1
        if( pNetworkInfo->pMqttSerializer != NULL )
        {
            if( pNetworkInfo->pMqttSerializer->serialize.pingreq != NULL )
            {
                serializePingreq = pNetworkInfo->pMqttSerializer->serialize.pingreq;
            }
            else
            {
                _EMPTY_ELSE_MARKER;
            }
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    #endif /* if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1 */

    /* Generate a PINGREQ packet. */
    serializeStatus = serializePingreq( &( pMqttConnection->pPingreqPacket ),
                                        &( pMqttConnection->pingreqPacketSize ) );

    if( serializeStatus != IOT_MQTT_SUCCESS )
    {
        IotLogError( "Failed to allocate PINGREQ packet for new connection." );

        status = false;
    }
    else
    {
        /* Create the task pool job that processes keep-alive. */
        jobStatus = IotTaskPool_CreateJob( _IotMqtt_ProcessKeepAlive,
                                           pMqttConnection,
                                           &( pMqttConnection->keepAliveJob ) );

        /* Task pool job creation for a pre-allocated job should never fail.
         * Abort the program if it does. */
        if( jobStatus != IOT_TASKPOOL_SUCCESS )
        {
            IotLogError( "Failed to create keep-alive job for new connection." );

            IotMqtt_Assert( false );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }

        /* Keep-alive references its MQTT connection, so increment reference. */
        ( pMqttConnection->references )++;
    }

    return status;
}

/*-----------------------------------------------------------*/

static _mqttConnection_t * _createMqttConnection( bool awsIotMqttMode,
                                                  const IotMqttNetworkInfo_t * pNetworkInfo,
                                                  uint16_t keepAliveSeconds )
{
    _IOT_FUNCTION_ENTRY( bool, true );
    _mqttConnection_t * pMqttConnection = NULL;
    bool referencesMutexCreated = false, subscriptionMutexCreated = false;

    /* Allocate memory for the new MQTT connection. */
    pMqttConnection = IotMqtt_MallocConnection( sizeof( _mqttConnection_t ) );

    if( pMqttConnection == NULL )
    {
        IotLogError( "Failed to allocate memory for new connection." );

        _IOT_SET_AND_GOTO_CLEANUP( false );
    }
    else
    {
        /* Clear the MQTT connection, then copy the MQTT server mode and network
         * interface. */
        ( void ) memset( pMqttConnection, 0x00, sizeof( _mqttConnection_t ) );
        pMqttConnection->awsIotMqttMode = awsIotMqttMode;
        pMqttConnection->pNetworkInterface = pNetworkInfo->pNetworkInterface;

        /* Start a new MQTT connection with a reference count of 1. */
        pMqttConnection->references = 1;
    }

    /* Create the references mutex for a new connection. It is a recursive mutex. */
    referencesMutexCreated = IotMutex_Create( &( pMqttConnection->referencesMutex ), true );

    if( referencesMutexCreated == false )
    {
        IotLogError( "Failed to create references mutex for new connection." );

        _IOT_SET_AND_GOTO_CLEANUP( false );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Create the subscription mutex for a new connection. */
    subscriptionMutexCreated = IotMutex_Create( &( pMqttConnection->subscriptionMutex ), false );

    if( subscriptionMutexCreated == false )
    {
        IotLogError( "Failed to create subscription mutex for new connection." );

        _IOT_SET_AND_GOTO_CLEANUP( false );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Create the new connection's subscription and operation lists. */
    IotListDouble_Create( &( pMqttConnection->subscriptionList ) );
    IotListDouble_Create( &( pMqttConnection->pendingProcessing ) );
    IotListDouble_Create( &( pMqttConnection->pendingResponse ) );

    /* AWS IoT service limits set minimum and maximum values for keep-alive interval.
     * Adjust the user-provided keep-alive interval based on these requirements. */
    if( awsIotMqttMode == true )
    {
        if( keepAliveSeconds < _AWS_IOT_MQTT_SERVER_MIN_KEEPALIVE )
        {
            keepAliveSeconds = _AWS_IOT_MQTT_SERVER_MIN_KEEPALIVE;
        }
        else if( keepAliveSeconds > _AWS_IOT_MQTT_SERVER_MAX_KEEPALIVE )
        {
            keepAliveSeconds = _AWS_IOT_MQTT_SERVER_MAX_KEEPALIVE;
        }
        else if( keepAliveSeconds == 0 )
        {
            keepAliveSeconds = _AWS_IOT_MQTT_SERVER_MAX_KEEPALIVE;
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Check if keep-alive is active for this connection. */
    if( keepAliveSeconds != 0 )
    {
        if( _createKeepAliveJob( pNetworkInfo,
                                 keepAliveSeconds,
                                 pMqttConnection ) == false )
        {
            _IOT_SET_AND_GOTO_CLEANUP( false );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Clean up mutexes and connection if this function failed. */
    _IOT_FUNCTION_CLEANUP_BEGIN();

    if( status == false )
    {
        if( subscriptionMutexCreated == true )
        {
            IotMutex_Destroy( &( pMqttConnection->subscriptionMutex ) );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }

        if( referencesMutexCreated == true )
        {
            IotMutex_Destroy( &( pMqttConnection->referencesMutex ) );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }

        if( pMqttConnection != NULL )
        {
            IotMqtt_FreeConnection( pMqttConnection );
            pMqttConnection = NULL;
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    return pMqttConnection;
}

/*-----------------------------------------------------------*/

static void _destroyMqttConnection( _mqttConnection_t * pMqttConnection )
{
    IotNetworkError_t networkStatus = IOT_NETWORK_SUCCESS;

    /* Clean up keep-alive if still allocated. */
    if( pMqttConnection->keepAliveMs != 0 )
    {
        IotLogDebug( "(MQTT connection %p) Cleaning up keep-alive.", pMqttConnection );

        _IotMqtt_FreePacket( pMqttConnection->pPingreqPacket );

        /* Clear data about the keep-alive. */
        pMqttConnection->keepAliveMs = 0;
        pMqttConnection->pPingreqPacket = NULL;
        pMqttConnection->pingreqPacketSize = 0;

        /* Decrement reference count. */
        pMqttConnection->references--;
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* A connection to be destroyed should have no keep-alive and at most 1
     * reference. */
    IotMqtt_Assert( pMqttConnection->references <= 1 );
    IotMqtt_Assert( pMqttConnection->keepAliveMs == 0 );
    IotMqtt_Assert( pMqttConnection->pPingreqPacket == NULL );
    IotMqtt_Assert( pMqttConnection->pingreqPacketSize == 0 );

    /* Remove all subscriptions. */
    IotMutex_Lock( &( pMqttConnection->subscriptionMutex ) );
    IotListDouble_RemoveAllMatches( &( pMqttConnection->subscriptionList ),
                                    _mqttSubscription_setUnsubscribe,
                                    NULL,
                                    _mqttSubscription_tryDestroy,
                                    offsetof( _mqttSubscription_t, link ) );
    IotMutex_Unlock( &( pMqttConnection->subscriptionMutex ) );

    /* Destroy mutexes. */
    IotMutex_Destroy( &( pMqttConnection->referencesMutex ) );
    IotMutex_Destroy( &( pMqttConnection->subscriptionMutex ) );

    /* An MQTT connection that owns its network connection should destroy it. */
    if( pMqttConnection->ownNetworkConnection == true )
    {
        networkStatus = pMqttConnection->pNetworkInterface->destroy( pMqttConnection->pNetworkConnection );

        if( networkStatus != IOT_NETWORK_SUCCESS )
        {
            IotLogWarn( "Failed to destroy network connection." );
        }
        else
        {
            IotLogInfo( "Network connection destroyed." );
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    IotLogDebug( "(MQTT connection %p) Connection destroyed.", pMqttConnection );

    /* Free connection. */
    IotMqtt_FreeConnection( pMqttConnection );
}

/*-----------------------------------------------------------*/

static IotMqttError_t _subscriptionCommon( IotMqttOperationType_t operation,
                                           IotMqttConnection_t mqttConnection,
                                           const IotMqttSubscription_t * pSubscriptionList,
                                           size_t subscriptionCount,
                                           uint32_t flags,
                                           const IotMqttCallbackInfo_t * pCallbackInfo,
                                           IotMqttReference_t * pSubscriptionRef )
{
    _IOT_FUNCTION_ENTRY( IotMqttError_t, IOT_MQTT_SUCCESS );
    _mqttOperation_t * pSubscriptionOperation = NULL;

    /* Subscription serializer function. */
    IotMqttError_t ( * serializeSubscription )( const IotMqttSubscription_t *,
                                                size_t,
                                                uint8_t **,
                                                size_t *,
                                                uint16_t * ) = NULL;

    /* This function should only be called for subscribe or unsubscribe. */
    IotMqtt_Assert( ( operation == IOT_MQTT_SUBSCRIBE ) ||
                    ( operation == IOT_MQTT_UNSUBSCRIBE ) );

    /* Check that all elements in the subscription list are valid. */
    if( _IotMqtt_ValidateSubscriptionList( operation,
                                           mqttConnection->awsIotMqttMode,
                                           pSubscriptionList,
                                           subscriptionCount ) == false )
    {
        _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_BAD_PARAMETER );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Check that a reference pointer is provided for a waitable operation. */
    if( ( flags & IOT_MQTT_FLAG_WAITABLE ) == IOT_MQTT_FLAG_WAITABLE )
    {
        if( pSubscriptionRef == NULL )
        {
            IotLogError( "Reference must be provided for a waitable %s.",
                         IotMqtt_OperationType( operation ) );

            _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_BAD_PARAMETER );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Choose a subscription serialize function. */
    if( operation == IOT_MQTT_SUBSCRIBE )
    {
        serializeSubscription = _IotMqtt_SerializeSubscribe;

        #if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1
            if( mqttConnection->pSerializer != NULL )
            {
                if( mqttConnection->pSerializer->serialize.subscribe != NULL )
                {
                    serializeSubscription = mqttConnection->pSerializer->serialize.subscribe;
                }
                else
                {
                    _EMPTY_ELSE_MARKER;
                }
            }
            else
            {
                _EMPTY_ELSE_MARKER;
            }
        #endif /* if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1 */
    }
    else
    {
        serializeSubscription = _IotMqtt_SerializeUnsubscribe;

        #if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1
            if( mqttConnection->pSerializer != NULL )
            {
                if( mqttConnection->pSerializer->serialize.unsubscribe != NULL )
                {
                    serializeSubscription = mqttConnection->pSerializer->serialize.unsubscribe;
                }
                else
                {
                    _EMPTY_ELSE_MARKER;
                }
            }
            else
            {
                _EMPTY_ELSE_MARKER;
            }
        #endif /* if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1 */
    }

    /* Remove the MQTT subscription list for an UNSUBSCRIBE. */
    if( operation == IOT_MQTT_UNSUBSCRIBE )
    {
        _IotMqtt_RemoveSubscriptionByTopicFilter( mqttConnection,
                                                  pSubscriptionList,
                                                  subscriptionCount );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Create a subscription operation. */
    status = _IotMqtt_CreateOperation( mqttConnection,
                                       flags,
                                       pCallbackInfo,
                                       &pSubscriptionOperation );

    if( status != IOT_MQTT_SUCCESS )
    {
        _IOT_GOTO_CLEANUP();
    }

    /* Check the subscription operation data and set the operation type. */
    IotMqtt_Assert( pSubscriptionOperation->status == IOT_MQTT_STATUS_PENDING );
    IotMqtt_Assert( pSubscriptionOperation->retry.limit == 0 );
    pSubscriptionOperation->operation = operation;

    /* Generate a subscription packet from the subscription list. */
    status = serializeSubscription( pSubscriptionList,
                                    subscriptionCount,
                                    &( pSubscriptionOperation->pMqttPacket ),
                                    &( pSubscriptionOperation->packetSize ),
                                    &( pSubscriptionOperation->packetIdentifier ) );

    if( status != IOT_MQTT_SUCCESS )
    {
        _IOT_GOTO_CLEANUP();
    }

    /* Check the serialized MQTT packet. */
    IotMqtt_Assert( pSubscriptionOperation->pMqttPacket != NULL );
    IotMqtt_Assert( pSubscriptionOperation->packetSize > 0 );

    /* Add the subscription list for a SUBSCRIBE. */
    if( operation == IOT_MQTT_SUBSCRIBE )
    {
        status = _IotMqtt_AddSubscriptions( mqttConnection,
                                            pSubscriptionOperation->packetIdentifier,
                                            pSubscriptionList,
                                            subscriptionCount );

        if( status != IOT_MQTT_SUCCESS )
        {
            _IOT_GOTO_CLEANUP();
        }
    }

    /* Set the reference, if provided. */
    if( pSubscriptionRef != NULL )
    {
        *pSubscriptionRef = pSubscriptionOperation;
    }

    /* Schedule the subscription operation for network transmission. */
    status = _IotMqtt_ScheduleOperation( pSubscriptionOperation,
                                         _IotMqtt_ProcessSend,
                                         0 );

    if( status != IOT_MQTT_SUCCESS )
    {
        IotLogError( "(MQTT connection %p) Failed to schedule %s for sending.",
                     mqttConnection,
                     IotMqtt_OperationType( operation ) );

        if( operation == IOT_MQTT_SUBSCRIBE )
        {
            _IotMqtt_RemoveSubscriptionByPacket( mqttConnection,
                                                 pSubscriptionOperation->packetIdentifier,
                                                 -1 );
        }

        /* Clear the previously set (and now invalid) reference. */
        if( pSubscriptionRef != NULL )
        {
            *pSubscriptionRef = IOT_MQTT_REFERENCE_INITIALIZER;
        }

        _IOT_GOTO_CLEANUP();
    }

    /* Clean up if this function failed. */
    _IOT_FUNCTION_CLEANUP_BEGIN();

    if( status != IOT_MQTT_SUCCESS )
    {
        if( pSubscriptionOperation != NULL )
        {
            _IotMqtt_DestroyOperation( pSubscriptionOperation );
        }
    }
    else
    {
        status = IOT_MQTT_STATUS_PENDING;

        IotLogInfo( "(MQTT connection %p) %s operation scheduled.",
                    mqttConnection,
                    IotMqtt_OperationType( operation ) );
    }

    _IOT_FUNCTION_CLEANUP_END();
}

/*-----------------------------------------------------------*/

bool _IotMqtt_IncrementConnectionReferences( _mqttConnection_t * pMqttConnection )
{
    bool disconnected = false;

    /* Lock the mutex protecting the reference count. */
    IotMutex_Lock( &( pMqttConnection->referencesMutex ) );

    /* Reference count must not be negative. */
    IotMqtt_Assert( pMqttConnection->references >= 0 );

    /* Read connection status. */
    disconnected = pMqttConnection->disconnected;

    /* Increment the connection's reference count if it is not disconnected. */
    if( disconnected == false )
    {
        ( pMqttConnection->references )++;
        IotLogDebug( "(MQTT connection %p) Reference count changed from %ld to %ld.",
                     pMqttConnection,
                     ( long int ) pMqttConnection->references - 1,
                     ( long int ) pMqttConnection->references );
    }
    else
    {
        IotLogWarn( "(MQTT connection %p) Attempt to use closed connection.", pMqttConnection );
    }

    IotMutex_Unlock( &( pMqttConnection->referencesMutex ) );

    return( disconnected == false );
}

/*-----------------------------------------------------------*/

void _IotMqtt_DecrementConnectionReferences( _mqttConnection_t * pMqttConnection )
{
    bool destroyConnection = false;

    /* Lock the mutex protecting the reference count. */
    IotMutex_Lock( &( pMqttConnection->referencesMutex ) );

    /* Decrement reference count. It must not be negative. */
    ( pMqttConnection->references )--;
    IotMqtt_Assert( pMqttConnection->references >= 0 );

    IotLogDebug( "(MQTT connection %p) Reference count changed from %ld to %ld.",
                 pMqttConnection,
                 ( long int ) pMqttConnection->references + 1,
                 ( long int ) pMqttConnection->references );

    /* Check if this connection may be destroyed. */
    if( pMqttConnection->references == 0 )
    {
        destroyConnection = true;
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    IotMutex_Unlock( &( pMqttConnection->referencesMutex ) );

    /* Destroy an unreferenced MQTT connection. */
    if( destroyConnection == true )
    {
        IotLogDebug( "(MQTT connection %p) Connection will be destroyed now.",
                     pMqttConnection );
        _destroyMqttConnection( pMqttConnection );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }
}

/*-----------------------------------------------------------*/

IotMqttError_t IotMqtt_Init( void )
{
    IotMqttError_t status = IOT_MQTT_SUCCESS;

    /* Initialize MQTT serializer. */
    if( _IotMqtt_InitSerialize() != IOT_MQTT_SUCCESS )
    {
        IotLogError( "Failed to initialize MQTT library serializer. " );

        status = IOT_MQTT_INIT_FAILED;
    }
    else
    {
        IotLogInfo( "MQTT library successfully initialized." );
    }

    return status;
}

/*-----------------------------------------------------------*/

void IotMqtt_Cleanup()
{
    /* Clean up MQTT serializer. */
    _IotMqtt_CleanupSerialize();

    IotLogInfo( "MQTT library cleanup done." );
}

/*-----------------------------------------------------------*/

IotMqttError_t IotMqtt_Connect( const IotMqttNetworkInfo_t * pNetworkInfo,
                                const IotMqttConnectInfo_t * pConnectInfo,
                                uint64_t timeoutMs,
                                IotMqttConnection_t * pMqttConnection )
{
    _IOT_FUNCTION_ENTRY( IotMqttError_t, IOT_MQTT_SUCCESS );
    bool networkCreated = false, ownNetworkConnection = false;
    IotNetworkError_t networkStatus = IOT_NETWORK_SUCCESS;
    IotTaskPoolError_t taskPoolStatus = IOT_TASKPOOL_SUCCESS;
    void * pNetworkConnection = NULL;
    _mqttOperation_t * pConnectOperation = NULL;
    _mqttConnection_t * pNewMqttConnection = NULL;

    /* Default CONNECT serializer function. */
    IotMqttError_t ( * serializeConnect )( const IotMqttConnectInfo_t *,
                                           uint8_t **,
                                           size_t * ) = _IotMqtt_SerializeConnect;

    /* Network info must not be NULL. */
    if( pNetworkInfo == NULL )
    {
        _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_BAD_PARAMETER );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Validate network interface and connect info. */
    if( _IotMqtt_ValidateConnect( pConnectInfo ) == false )
    {
        _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_BAD_PARAMETER );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* If will info is provided, check that it is valid. */
    if( pConnectInfo->pWillInfo != NULL )
    {
        if( _IotMqtt_ValidatePublish( pConnectInfo->awsIotMqttMode,
                                      pConnectInfo->pWillInfo ) == false )
        {
            _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_BAD_PARAMETER );
        }
        else if( pConnectInfo->pWillInfo->payloadLength > UINT16_MAX )
        {
            /* Will message payloads cannot be larger than 65535. This restriction
             * applies only to will messages, and not normal PUBLISH messages. */
            IotLogError( "Will payload cannot be larger than 65535." );

            _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_BAD_PARAMETER );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* If previous subscriptions are provided, check that they are valid. */
    if( pConnectInfo->cleanSession == false )
    {
        if( pConnectInfo->pPreviousSubscriptions != NULL )
        {
            if( _IotMqtt_ValidateSubscriptionList( IOT_MQTT_SUBSCRIBE,
                                                   pConnectInfo->awsIotMqttMode,
                                                   pConnectInfo->pPreviousSubscriptions,
                                                   pConnectInfo->previousSubscriptionCount ) == false )
            {
                _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_BAD_PARAMETER );
            }
            else
            {
                _EMPTY_ELSE_MARKER;
            }
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Create a new MQTT connection if requested. Otherwise, copy the existing
     * network connection. */
    if( pNetworkInfo->createNetworkConnection == true )
    {
        networkStatus = pNetworkInfo->pNetworkInterface->create( pNetworkInfo->pNetworkServerInfo,
                                                                 pNetworkInfo->pNetworkCredentialInfo,
                                                                 &pNetworkConnection );

        if( networkStatus == IOT_NETWORK_SUCCESS )
        {
            networkCreated = true;

            /* This MQTT connection owns the network connection it created and
             * should destroy it on cleanup. */
            ownNetworkConnection = true;
        }
        else
        {
            _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_NETWORK_ERROR );
        }
    }
    else
    {
        pNetworkConnection = pNetworkInfo->pNetworkConnection;
        networkCreated = true;
    }

    IotLogInfo( "Establishing new MQTT connection." );

    /* Initialize a new MQTT connection object. */
    pNewMqttConnection = _createMqttConnection( pConnectInfo->awsIotMqttMode,
                                                pNetworkInfo,
                                                pConnectInfo->keepAliveSeconds );

    if( pNewMqttConnection == NULL )
    {
        _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_NO_MEMORY );
    }
    else
    {
        /* Set the network connection associated with the MQTT connection. */
        pNewMqttConnection->pNetworkConnection = pNetworkConnection;
        pNewMqttConnection->ownNetworkConnection = ownNetworkConnection;

        /* Set the MQTT packet serializer overrides. */
        #if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1
            pNewMqttConnection->pSerializer = pNetworkInfo->pMqttSerializer;
        #endif
    }

    /* Set the MQTT receive callback. */
    networkStatus = pNewMqttConnection->pNetworkInterface->setReceiveCallback( pNetworkConnection,
                                                                               IotMqtt_ReceiveCallback,
                                                                               pNewMqttConnection );

    if( networkStatus != IOT_NETWORK_SUCCESS )
    {
        IotLogError( "Failed to set MQTT network receive callback." );

        _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_NETWORK_ERROR );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Create a CONNECT operation. */
    status = _IotMqtt_CreateOperation( pNewMqttConnection,
                                       IOT_MQTT_FLAG_WAITABLE,
                                       NULL,
                                       &pConnectOperation );

    if( status != IOT_MQTT_SUCCESS )
    {
        _IOT_GOTO_CLEANUP();
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Ensure the members set by operation creation and serialization
     * are appropriate for a blocking CONNECT. */
    IotMqtt_Assert( pConnectOperation->status == IOT_MQTT_STATUS_PENDING );
    IotMqtt_Assert( ( pConnectOperation->flags & IOT_MQTT_FLAG_WAITABLE )
                    == IOT_MQTT_FLAG_WAITABLE );
    IotMqtt_Assert( pConnectOperation->retry.limit == 0 );

    /* Set the operation type. */
    pConnectOperation->operation = IOT_MQTT_CONNECT;

    /* Add previous session subscriptions. */
    if( pConnectInfo->pPreviousSubscriptions != NULL )
    {
        /* Previous subscription count should have been validated as nonzero. */
        IotMqtt_Assert( pConnectInfo->previousSubscriptionCount > 0 );

        status = _IotMqtt_AddSubscriptions( pNewMqttConnection,
                                            2,
                                            pConnectInfo->pPreviousSubscriptions,
                                            pConnectInfo->previousSubscriptionCount );

        if( status != IOT_MQTT_SUCCESS )
        {
            _IOT_GOTO_CLEANUP();
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Choose a CONNECT serializer function. */
    #if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1
        if( pNewMqttConnection->pSerializer != NULL )
        {
            if( pNewMqttConnection->pSerializer->serialize.connect != NULL )
            {
                serializeConnect = pNewMqttConnection->pSerializer->serialize.connect;
            }
            else
            {
                _EMPTY_ELSE_MARKER;
            }
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    #endif /* if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1 */

    /* Convert the connect info and will info objects to an MQTT CONNECT packet. */
    status = serializeConnect( pConnectInfo,
                               &( pConnectOperation->pMqttPacket ),
                               &( pConnectOperation->packetSize ) );

    if( status != IOT_MQTT_SUCCESS )
    {
        _IOT_GOTO_CLEANUP();
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Check the serialized MQTT packet. */
    IotMqtt_Assert( pConnectOperation->pMqttPacket != NULL );
    IotMqtt_Assert( pConnectOperation->packetSize > 0 );

    /* Add the CONNECT operation to the send queue for network transmission. */
    status = _IotMqtt_ScheduleOperation( pConnectOperation,
                                         _IotMqtt_ProcessSend,
                                         0 );

    if( status != IOT_MQTT_SUCCESS )
    {
        IotLogError( "Failed to enqueue CONNECT for sending." );
    }
    else
    {
        /* Wait for the CONNECT operation to complete, i.e. wait for CONNACK. */
        status = IotMqtt_Wait( ( IotMqttReference_t ) pConnectOperation,
                               timeoutMs );

        /* The call to wait cleans up the CONNECT operation, so set the pointer
         * to NULL. */
        pConnectOperation = NULL;
    }

    /* When a connection is successfully established, schedule keep-alive job. */
    if( status == IOT_MQTT_SUCCESS )
    {
        /* Check if a keep-alive job should be scheduled. */
        if( pNewMqttConnection->keepAliveMs != 0 )
        {
            IotLogDebug( "Scheduling first MQTT keep-alive job." );

            taskPoolStatus = IotTaskPool_ScheduleDeferred( IOT_SYSTEM_TASKPOOL,
                                                           &( pNewMqttConnection->keepAliveJob ),
                                                           pNewMqttConnection->nextKeepAliveMs );

            if( taskPoolStatus != IOT_TASKPOOL_SUCCESS )
            {
                _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_SCHEDULING_ERROR );
            }
            else
            {
                _EMPTY_ELSE_MARKER;
            }
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    _IOT_FUNCTION_CLEANUP_BEGIN();

    if( status != IOT_MQTT_SUCCESS )
    {
        IotLogError( "Failed to establish new MQTT connection, error %s.",
                     IotMqtt_strerror( status ) );

        /* The network connection must be closed if it was created. */
        if( networkCreated == true )
        {
            networkStatus = pNetworkInfo->pNetworkInterface->close( pNetworkConnection );

            if( networkStatus != IOT_NETWORK_SUCCESS )
            {
                IotLogWarn( "Failed to close network connection." );
            }
            else
            {
                IotLogInfo( "Network connection closed on error." );
            }
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }

        if( pConnectOperation != NULL )
        {
            _IotMqtt_DestroyOperation( pConnectOperation );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }

        if( pNewMqttConnection != NULL )
        {
            _destroyMqttConnection( pNewMqttConnection );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        IotLogInfo( "New MQTT connection %p established.", pMqttConnection );

        /* Set the output parameter. */
        *pMqttConnection = pNewMqttConnection;
    }

    _IOT_FUNCTION_CLEANUP_END();
}

/*-----------------------------------------------------------*/

void IotMqtt_Disconnect( IotMqttConnection_t mqttConnection,
                         uint32_t flags )
{
    bool disconnected = false;
    IotMqttError_t status = IOT_MQTT_STATUS_PENDING;
    _mqttOperation_t * pDisconnectOperation = NULL;

    IotLogInfo( "(MQTT connection %p) Disconnecting connection.", mqttConnection );

    /* Read the connection status. */
    IotMutex_Lock( &( mqttConnection->referencesMutex ) );
    disconnected = mqttConnection->disconnected;
    IotMutex_Unlock( &( mqttConnection->referencesMutex ) );

    /* Only send a DISCONNECT packet if the connection is active and the "cleanup only"
     * flag is not set. */
    if( disconnected == false )
    {
        if( ( flags & IOT_MQTT_FLAG_CLEANUP_ONLY ) == 0 )
        {
            /* Create a DISCONNECT operation. This function blocks until the DISCONNECT
             * packet is sent, so it sets IOT_MQTT_FLAG_WAITABLE. */
            status = _IotMqtt_CreateOperation( mqttConnection,
                                               IOT_MQTT_FLAG_WAITABLE,
                                               NULL,
                                               &pDisconnectOperation );

            if( status == IOT_MQTT_SUCCESS )
            {
                /* Ensure that the members set by operation creation and serialization
                 * are appropriate for a blocking DISCONNECT. */
                IotMqtt_Assert( pDisconnectOperation->status == IOT_MQTT_STATUS_PENDING );
                IotMqtt_Assert( ( pDisconnectOperation->flags & IOT_MQTT_FLAG_WAITABLE )
                                == IOT_MQTT_FLAG_WAITABLE );
                IotMqtt_Assert( pDisconnectOperation->retry.limit == 0 );

                /* Set the operation type. */
                pDisconnectOperation->operation = IOT_MQTT_DISCONNECT;

                /* Choose a disconnect serializer. */
                IotMqttError_t ( * serializeDisconnect )( uint8_t **,
                                                          size_t * ) = _IotMqtt_SerializeDisconnect;

                #if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1
                    if( mqttConnection->pSerializer != NULL )
                    {
                        if( mqttConnection->pSerializer->serialize.disconnect != NULL )
                        {
                            serializeDisconnect = mqttConnection->pSerializer->serialize.disconnect;
                        }
                        else
                        {
                            _EMPTY_ELSE_MARKER;
                        }
                    }
                    else
                    {
                        _EMPTY_ELSE_MARKER;
                    }
                #endif /* if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1 */

                /* Generate a DISCONNECT packet. */
                status = serializeDisconnect( &( pDisconnectOperation->pMqttPacket ),
                                              &( pDisconnectOperation->packetSize ) );
            }
            else
            {
                _EMPTY_ELSE_MARKER;
            }

            if( status == IOT_MQTT_SUCCESS )
            {
                /* Check the serialized MQTT packet. */
                IotMqtt_Assert( pDisconnectOperation->pMqttPacket != NULL );
                IotMqtt_Assert( pDisconnectOperation->packetSize > 0 );

                /* Schedule the DISCONNECT operation for network transmission. */
                if( _IotMqtt_ScheduleOperation( pDisconnectOperation,
                                                _IotMqtt_ProcessSend,
                                                0 ) != IOT_MQTT_SUCCESS )
                {
                    IotLogWarn( "(MQTT connection %p) Failed to schedule DISCONNECT for sending.",
                                mqttConnection );
                    _IotMqtt_DestroyOperation( pDisconnectOperation );
                }
                else
                {
                    /* Wait a short time for the DISCONNECT packet to be transmitted. */
                    status = IotMqtt_Wait( ( IotMqttReference_t ) pDisconnectOperation,
                                           IOT_MQTT_RESPONSE_WAIT_MS );

                    /* A wait on DISCONNECT should only ever return SUCCESS, TIMEOUT,
                     * or NETWORK ERROR. */
                    if( status == IOT_MQTT_SUCCESS )
                    {
                        IotLogInfo( "(MQTT connection %p) Connection disconnected.", mqttConnection );
                    }
                    else
                    {
                        IotMqtt_Assert( ( status == IOT_MQTT_TIMEOUT ) ||
                                        ( status == IOT_MQTT_NETWORK_ERROR ) );

                        IotLogWarn( "(MQTT connection %p) DISCONNECT not sent, error %s.",
                                    mqttConnection,
                                    IotMqtt_strerror( status ) );
                    }
                }
            }
            else
            {
                _EMPTY_ELSE_MARKER;
            }
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Close the underlying network connection. This also cleans up keep-alive. */
    _IotMqtt_CloseNetworkConnection( mqttConnection );

    /* Check if the connection may be destroyed. */
    IotMutex_Lock( &( mqttConnection->referencesMutex ) );

    /* At this point, the connection should be marked disconnected. */
    IotMqtt_Assert( mqttConnection->disconnected == true );

    /* Attempt cancel and destroy each operation in the connection's lists. */
    IotListDouble_RemoveAll( &( mqttConnection->pendingProcessing ),
                             _mqttOperation_tryDestroy,
                             offsetof( _mqttOperation_t, link ) );

    IotListDouble_RemoveAll( &( mqttConnection->pendingResponse ),
                             _mqttOperation_tryDestroy,
                             offsetof( _mqttOperation_t, link ) );

    IotMutex_Unlock( &( mqttConnection->referencesMutex ) );

    /* Decrement the connection reference count and destroy it if possible. */
    _IotMqtt_DecrementConnectionReferences( mqttConnection );
}

/*-----------------------------------------------------------*/

IotMqttError_t IotMqtt_Subscribe( IotMqttConnection_t mqttConnection,
                                  const IotMqttSubscription_t * pSubscriptionList,
                                  size_t subscriptionCount,
                                  uint32_t flags,
                                  const IotMqttCallbackInfo_t * pCallbackInfo,
                                  IotMqttReference_t * pSubscribeRef )
{
    return _subscriptionCommon( IOT_MQTT_SUBSCRIBE,
                                mqttConnection,
                                pSubscriptionList,
                                subscriptionCount,
                                flags,
                                pCallbackInfo,
                                pSubscribeRef );
}

/*-----------------------------------------------------------*/

IotMqttError_t IotMqtt_TimedSubscribe( IotMqttConnection_t mqttConnection,
                                       const IotMqttSubscription_t * pSubscriptionList,
                                       size_t subscriptionCount,
                                       uint32_t flags,
                                       uint64_t timeoutMs )
{
    IotMqttError_t status = IOT_MQTT_STATUS_PENDING;
    IotMqttReference_t subscribeRef = IOT_MQTT_REFERENCE_INITIALIZER;

    /* Flags are not used, but the parameter is present for future compatibility. */
    ( void ) flags;

    /* Call the asynchronous SUBSCRIBE function. */
    status = IotMqtt_Subscribe( mqttConnection,
                                pSubscriptionList,
                                subscriptionCount,
                                IOT_MQTT_FLAG_WAITABLE,
                                NULL,
                                &subscribeRef );

    /* Wait for the SUBSCRIBE operation to complete. */
    if( status == IOT_MQTT_STATUS_PENDING )
    {
        status = IotMqtt_Wait( subscribeRef, timeoutMs );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Ensure that a status was set. */
    IotMqtt_Assert( status != IOT_MQTT_STATUS_PENDING );

    return status;
}

/*-----------------------------------------------------------*/

IotMqttError_t IotMqtt_Unsubscribe( IotMqttConnection_t mqttConnection,
                                    const IotMqttSubscription_t * pSubscriptionList,
                                    size_t subscriptionCount,
                                    uint32_t flags,
                                    const IotMqttCallbackInfo_t * pCallbackInfo,
                                    IotMqttReference_t * pUnsubscribeRef )
{
    return _subscriptionCommon( IOT_MQTT_UNSUBSCRIBE,
                                mqttConnection,
                                pSubscriptionList,
                                subscriptionCount,
                                flags,
                                pCallbackInfo,
                                pUnsubscribeRef );
}

/*-----------------------------------------------------------*/

IotMqttError_t IotMqtt_TimedUnsubscribe( IotMqttConnection_t mqttConnection,
                                         const IotMqttSubscription_t * pSubscriptionList,
                                         size_t subscriptionCount,
                                         uint32_t flags,
                                         uint64_t timeoutMs )
{
    IotMqttError_t status = IOT_MQTT_STATUS_PENDING;
    IotMqttReference_t unsubscribeRef = IOT_MQTT_REFERENCE_INITIALIZER;

    /* Flags are not used, but the parameter is present for future compatibility. */
    ( void ) flags;

    /* Call the asynchronous UNSUBSCRIBE function. */
    status = IotMqtt_Unsubscribe( mqttConnection,
                                  pSubscriptionList,
                                  subscriptionCount,
                                  IOT_MQTT_FLAG_WAITABLE,
                                  NULL,
                                  &unsubscribeRef );

    /* Wait for the UNSUBSCRIBE operation to complete. */
    if( status == IOT_MQTT_STATUS_PENDING )
    {
        status = IotMqtt_Wait( unsubscribeRef, timeoutMs );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Ensure that a status was set. */
    IotMqtt_Assert( status != IOT_MQTT_STATUS_PENDING );

    return status;
}

/*-----------------------------------------------------------*/

IotMqttError_t IotMqtt_Publish( IotMqttConnection_t mqttConnection,
                                const IotMqttPublishInfo_t * pPublishInfo,
                                uint32_t flags,
                                const IotMqttCallbackInfo_t * pCallbackInfo,
                                IotMqttReference_t * pPublishRef )
{
    _IOT_FUNCTION_ENTRY( IotMqttError_t, IOT_MQTT_SUCCESS );
    _mqttOperation_t * pPublishOperation = NULL;
    uint8_t ** pPacketIdentifierHigh = NULL;

    /* Default PUBLISH serializer function. */
    IotMqttError_t ( * serializePublish )( const IotMqttPublishInfo_t *,
                                           uint8_t **,
                                           size_t *,
                                           uint16_t *,
                                           uint8_t ** ) = _IotMqtt_SerializePublish;

    /* Check that the PUBLISH information is valid. */
    if( _IotMqtt_ValidatePublish( mqttConnection->awsIotMqttMode,
                                  pPublishInfo ) == false )
    {
        _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_BAD_PARAMETER );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Check that no notification is requested for a QoS 0 publish. */
    if( pPublishInfo->qos == IOT_MQTT_QOS_0 )
    {
        if( pCallbackInfo != NULL )
        {
            IotLogError( "QoS 0 PUBLISH should not have notification parameters set." );

            _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_BAD_PARAMETER );
        }
        else if( ( flags & IOT_MQTT_FLAG_WAITABLE ) != 0 )
        {
            IotLogError( "QoS 0 PUBLISH should not have notification parameters set." );

            _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_BAD_PARAMETER );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }

        if( pPublishRef != NULL )
        {
            IotLogWarn( "Ignoring reference parameter for QoS 0 publish." );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Check that a reference pointer is provided for a waitable operation. */
    if( ( flags & IOT_MQTT_FLAG_WAITABLE ) == IOT_MQTT_FLAG_WAITABLE )
    {
        if( pPublishRef == NULL )
        {
            IotLogError( "Reference must be provided for a waitable PUBLISH." );

            _IOT_SET_AND_GOTO_CLEANUP( IOT_MQTT_BAD_PARAMETER );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Create a PUBLISH operation. */
    status = _IotMqtt_CreateOperation( mqttConnection,
                                       flags,
                                       pCallbackInfo,
                                       &pPublishOperation );

    if( status != IOT_MQTT_SUCCESS )
    {
        _IOT_GOTO_CLEANUP();
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Check the PUBLISH operation data and set the operation type. */
    IotMqtt_Assert( pPublishOperation->status == IOT_MQTT_STATUS_PENDING );
    pPublishOperation->operation = IOT_MQTT_PUBLISH_TO_SERVER;

    /* Choose a PUBLISH serializer function. */
    #if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1
        if( mqttConnection->pSerializer != NULL )
        {
            if( mqttConnection->pSerializer->serialize.publish != NULL )
            {
                serializePublish = mqttConnection->pSerializer->serialize.publish;
            }
            else
            {
                _EMPTY_ELSE_MARKER;
            }
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    #endif /* if IOT_MQTT_ENABLE_SERIALIZER_OVERRIDES == 1 */

    /* In AWS IoT MQTT mode, a pointer to the packet identifier must be saved. */
    if( mqttConnection->awsIotMqttMode == true )
    {
        pPacketIdentifierHigh = &( pPublishOperation->pPacketIdentifierHigh );
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Generate a PUBLISH packet from pPublishInfo. */
    status = serializePublish( pPublishInfo,
                               &( pPublishOperation->pMqttPacket ),
                               &( pPublishOperation->packetSize ),
                               &( pPublishOperation->packetIdentifier ),
                               pPacketIdentifierHigh );

    if( status != IOT_MQTT_SUCCESS )
    {
        _IOT_GOTO_CLEANUP();
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Check the serialized MQTT packet. */
    IotMqtt_Assert( pPublishOperation->pMqttPacket != NULL );
    IotMqtt_Assert( pPublishOperation->packetSize > 0 );

    /* Initialize PUBLISH retry if retryLimit is set. */
    if( pPublishInfo->retryLimit > 0 )
    {
        /* A QoS 0 PUBLISH may not be retried. */
        if( pPublishInfo->qos != IOT_MQTT_QOS_0 )
        {
            pPublishOperation->retry.limit = pPublishInfo->retryLimit;
            pPublishOperation->retry.nextPeriod = pPublishInfo->retryMs;
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Set the reference, if provided. */
    if( pPublishInfo->qos != IOT_MQTT_QOS_0 )
    {
        if( pPublishRef != NULL )
        {
            *pPublishRef = pPublishOperation;
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Add the PUBLISH operation to the send queue for network transmission. */
    status = _IotMqtt_ScheduleOperation( pPublishOperation,
                                         _IotMqtt_ProcessSend,
                                         0 );

    if( status != IOT_MQTT_SUCCESS )
    {
        IotLogError( "(MQTT connection %p) Failed to enqueue PUBLISH for sending.",
                     mqttConnection );

        /* Clear the previously set (and now invalid) reference. */
        if( pPublishInfo->qos != IOT_MQTT_QOS_0 )
        {
            if( pPublishRef != NULL )
            {
                *pPublishRef = IOT_MQTT_REFERENCE_INITIALIZER;
            }
            else
            {
                _EMPTY_ELSE_MARKER;
            }
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }

        _IOT_GOTO_CLEANUP();
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Clean up the PUBLISH operation if this function fails. Otherwise, set the
     * appropriate return code based on QoS. */
    _IOT_FUNCTION_CLEANUP_BEGIN();

    if( status != IOT_MQTT_SUCCESS )
    {
        if( pPublishOperation != NULL )
        {
            _IotMqtt_DestroyOperation( pPublishOperation );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        if( pPublishInfo->qos > IOT_MQTT_QOS_0 )
        {
            status = IOT_MQTT_STATUS_PENDING;
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }

        IotLogInfo( "(MQTT connection %p) MQTT PUBLISH operation queued.",
                    mqttConnection );
    }

    _IOT_FUNCTION_CLEANUP_END();
}

/*-----------------------------------------------------------*/

IotMqttError_t IotMqtt_TimedPublish( IotMqttConnection_t mqttConnection,
                                     const IotMqttPublishInfo_t * pPublishInfo,
                                     uint32_t flags,
                                     uint64_t timeoutMs )
{
    IotMqttError_t status = IOT_MQTT_STATUS_PENDING;
    IotMqttReference_t publishRef = IOT_MQTT_REFERENCE_INITIALIZER,
                       * pPublishRef = NULL;

    /* Clear the flags. */
    flags = 0;

    /* Set the waitable flag and reference for QoS 1 PUBLISH. */
    if( pPublishInfo->qos == IOT_MQTT_QOS_1 )
    {
        flags = IOT_MQTT_FLAG_WAITABLE;
        pPublishRef = &publishRef;
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Call the asynchronous PUBLISH function. */
    status = IotMqtt_Publish( mqttConnection,
                              pPublishInfo,
                              flags,
                              NULL,
                              pPublishRef );

    /* Wait for a queued QoS 1 PUBLISH to complete. */
    if( pPublishInfo->qos == IOT_MQTT_QOS_1 )
    {
        if( status == IOT_MQTT_STATUS_PENDING )
        {
            status = IotMqtt_Wait( publishRef, timeoutMs );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    return status;
}

/*-----------------------------------------------------------*/

IotMqttError_t IotMqtt_Wait( IotMqttReference_t reference,
                             uint64_t timeoutMs )
{
    IotMqttError_t status = IOT_MQTT_SUCCESS;
    _mqttOperation_t * pOperation = reference;
    _mqttConnection_t * pMqttConnection = pOperation->pMqttConnection;

    /* Validate the given reference. */
    if( _IotMqtt_ValidateReference( reference ) == false )
    {
        status = IOT_MQTT_BAD_PARAMETER;
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    /* Check the MQTT connection status. */
    if( status == IOT_MQTT_SUCCESS )
    {
        IotMutex_Lock( &( pMqttConnection->referencesMutex ) );

        if( pMqttConnection->disconnected == true )
        {
            IotLogError( "(MQTT connection %p, %s operation %p) MQTT connection is closed. "
                         "Operation cannot be waited on.",
                         pMqttConnection,
                         IotMqtt_OperationType( pOperation->operation ),
                         pOperation );

            status = IOT_MQTT_NETWORK_ERROR;
        }
        else
        {
            IotLogInfo( "(MQTT connection %p, %s operation %p) Waiting for operation completion.",
                        pMqttConnection,
                        IotMqtt_OperationType( pOperation->operation ),
                        pOperation );
        }

        IotMutex_Unlock( &( pMqttConnection->referencesMutex ) );

        /* Only wait on an operation if the MQTT connection is active. */
        if( status == IOT_MQTT_SUCCESS )
        {
            if( IotSemaphore_TimedWait( &( pOperation->notify.waitSemaphore ),
                                        timeoutMs ) == false )
            {
                status = IOT_MQTT_TIMEOUT;

                /* Attempt to cancel the job of the timed out operation. */
                ( void ) _IotMqtt_DecrementOperationReferences( pOperation, true );

                /* Clean up lingering subscriptions from a timed-out SUBSCRIBE. */
                if( pOperation->operation == IOT_MQTT_SUBSCRIBE )
                {
                    IotLogDebug( "(MQTT connection %p, SUBSCRIBE operation %p) Cleaning up"
                                 " subscriptions of timed-out SUBSCRIBE.",
                                 pMqttConnection,
                                 pOperation );

                    _IotMqtt_RemoveSubscriptionByPacket( pMqttConnection,
                                                         pOperation->packetIdentifier,
                                                         -1 );
                }
                else
                {
                    _EMPTY_ELSE_MARKER;
                }
            }
            else
            {
                /* Retrieve the status of the completed operation. */
                status = pOperation->status;
            }

            IotLogInfo( "(MQTT connection %p, %s operation %p) Wait complete with result %s.",
                        pMqttConnection,
                        IotMqtt_OperationType( pOperation->operation ),
                        pOperation,
                        IotMqtt_strerror( status ) );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }

        /* Wait is finished; decrement operation reference count. */
        if( _IotMqtt_DecrementOperationReferences( pOperation, false ) == true )
        {
            _IotMqtt_DestroyOperation( pOperation );
        }
        else
        {
            _EMPTY_ELSE_MARKER;
        }
    }
    else
    {
        _EMPTY_ELSE_MARKER;
    }

    return status;
}

/*-----------------------------------------------------------*/

const char * IotMqtt_strerror( IotMqttError_t status )
{
    const char * pMessage = NULL;

    switch( status )
    {
        case IOT_MQTT_SUCCESS:
            pMessage = "SUCCESS";
            break;

        case IOT_MQTT_STATUS_PENDING:
            pMessage = "PENDING";
            break;

        case IOT_MQTT_INIT_FAILED:
            pMessage = "INITIALIZATION FAILED";
            break;

        case IOT_MQTT_BAD_PARAMETER:
            pMessage = "BAD PARAMETER";
            break;

        case IOT_MQTT_NO_MEMORY:
            pMessage = "NO MEMORY";
            break;

        case IOT_MQTT_NETWORK_ERROR:
            pMessage = "NETWORK ERROR";
            break;

        case IOT_MQTT_SCHEDULING_ERROR:
            pMessage = "SCHEDULING ERROR";
            break;

        case IOT_MQTT_BAD_RESPONSE:
            pMessage = "BAD RESPONSE RECEIVED";
            break;

        case IOT_MQTT_TIMEOUT:
            pMessage = "TIMEOUT";
            break;

        case IOT_MQTT_SERVER_REFUSED:
            pMessage = "SERVER REFUSED";
            break;

        case IOT_MQTT_RETRY_NO_RESPONSE:
            pMessage = "NO RESPONSE";
            break;

        default:
            pMessage = "INVALID STATUS";
            break;
    }

    return pMessage;
}

/*-----------------------------------------------------------*/

const char * IotMqtt_OperationType( IotMqttOperationType_t operation )
{
    const char * pMessage = NULL;

    switch( operation )
    {
        case IOT_MQTT_CONNECT:
            pMessage = "CONNECT";
            break;

        case IOT_MQTT_PUBLISH_TO_SERVER:
            pMessage = "PUBLISH";
            break;

        case IOT_MQTT_PUBACK:
            pMessage = "PUBACK";
            break;

        case IOT_MQTT_SUBSCRIBE:
            pMessage = "SUBSCRIBE";
            break;

        case IOT_MQTT_UNSUBSCRIBE:
            pMessage = "UNSUBSCRIBE";
            break;

        case IOT_MQTT_PINGREQ:
            pMessage = "PINGREQ";
            break;

        case IOT_MQTT_DISCONNECT:
            pMessage = "DISCONNECT";
            break;

        default:
            pMessage = "INVALID OPERATION";
            break;
    }

    return pMessage;
}

/*-----------------------------------------------------------*/

/* If the MQTT library is being tested, include a file that allows access to
 * internal functions and variables. */
#if IOT_MQTT_TEST == 1
    #include "iot_test_access_mqtt_api.c"
#endif