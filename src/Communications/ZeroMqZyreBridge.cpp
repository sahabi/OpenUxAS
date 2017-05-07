// ===============================================================================
// Authors: AFRL/RQQA
// Organization: Air Force Research Laboratory, Aerospace Systems Directorate, Power and Control Division
// 
// Copyright (c) 2017 Government of the United State of America, as represented by
// the Secretary of the Air Force.  No copyright is claimed in the United States under
// Title 17, U.S. Code.  All Other Rights Reserved.
// ===============================================================================

#include "ZeroMqZyreBridge.h"

#include "UxAS_ConfigurationManager.h"
#include "UxAS_Log.h"

#include "stdUniquePtr.h"

#if (defined(__APPLE__) && defined(__MACH__))
#define OSX
#endif

#ifdef OSX
#include <unistd.h>
#include <sys/select.h>
#include <termios.h>
#include <set>
#elif (!defined(WIN32))
#include <unistd.h>
#include <sys/select.h>
#include <termio.h>
#include <set>
#endif

namespace uxas
{
namespace communications
{

ZeroMqZyreBridge::ZeroMqZyreBridge()
{
};

ZeroMqZyreBridge::~ZeroMqZyreBridge()
{
    m_isStarted = false;
    m_isTerminate = true;
    terminateZyreNodeAndThread();
};

void
ZeroMqZyreBridge::setZyreEnterMessageHandler(std::function<void(const std::string& zyreRemoteUuid, const std::unordered_map<std::string, std::string>& headerKeyValuePairs)>&& zyreEnterMessageHandler)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_zyreEnterMessageHandler = std::move(zyreEnterMessageHandler);
    m_isZyreEnterMessageHandler = true;
};

void
ZeroMqZyreBridge::setZyreExitMessageHandler(std::function<void(const std::string& zyreRemoteUuid)>&& zyreExitMessageHandler)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_zyreExitMessageHandler = std::move(zyreExitMessageHandler);
    m_isZyreExitMessageHandler = true;
};

void
ZeroMqZyreBridge::setZyreWhisperMessageHandler(std::function<void(const std::string& zyreRemoteUuid, const std::string& messagePayload)>&& zyreWhisperMessageHandler)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_zyreWhisperMessageHandler = std::move(zyreWhisperMessageHandler);
    m_isZyreWhisperMessageHandler = true;
};

bool
ZeroMqZyreBridge::start(const std::string& zyreNetworkDevice, const std::string& zyreNodeId, const std::unique_ptr<std::unordered_map<std::string, std::string>>& headerKeyValuePairs)
{
    if (!zyreNetworkDevice.empty())
    {
        m_zyreNetworkDevice = zyreNetworkDevice;
        LOG_INFORM(s_typeName(), "::start set Zyre network device to ", m_zyreNetworkDevice);
    }
    else
    {
        LOG_ERROR(s_typeName(), "::start failed due to invalid Zyre network device function parameter");
        return (false);
    }
    if (!zyreNodeId.empty())
    {
        m_zyreNodeId = zyreNodeId;
        LOG_INFORM(s_typeName(), "::start set Zyre node ID to ", m_zyreNodeId);
    }
    else
    {
        LOG_ERROR(s_typeName(), "::start failed due to invalid Zyre node ID function parameter");
        return (false);
    }
    
    std::unique_lock<std::mutex> lock(m_mutex);
    m_isStarted = false;
    m_isTerminate = true;
    lock.unlock();
    lock.lock();
    terminateZyreNodeAndThread();

    LOG_INFORM(s_typeName(), "::start creating new Zyre node with node ID ", m_zyreNodeId, " and network device ", m_zyreNetworkDevice);
    m_zyreNode = zyre_new(m_zyreNodeId.c_str());
    zyre_set_interface(m_zyreNode, m_zyreNetworkDevice.c_str()); // associate node with network device

    for (auto hdrKvPairsIt = headerKeyValuePairs->cbegin(), hdrKvPairsItEnd = headerKeyValuePairs->cend(); hdrKvPairsIt != hdrKvPairsItEnd; hdrKvPairsIt++)
    {
        LOG_INFORM(s_typeName(), "::start adding header key/value pair KEY [", hdrKvPairsIt->first, "] VALUE [", hdrKvPairsIt->second, "]");
        n_ZMQ::zyreSetHeaderEntry(m_zyreNode, hdrKvPairsIt->first, hdrKvPairsIt->second);
        m_receivedMessageHeaderKeys.emplace(hdrKvPairsIt->first.substr());
    }
    
    int zyreNodeStartRtnCode = zyre_start(m_zyreNode);
    if (zyreNodeStartRtnCode == 0)
    {
        m_isStarted = true;
        m_isTerminate = false;
        m_zyreEventProcessingThread = uxas::stduxas::make_unique<std::thread>(&ZeroMqZyreBridge::executeZyreEventProcessing, this);
        LOG_INFORM(s_typeName(), "::start Zyre event processing thread [", m_zyreEventProcessingThread->get_id(), "]");
    }
    else
    {
        LOG_ERROR(s_typeName(), "::start failed at Zyre node zyre_start function call");
        return (false);
    }
    
    // un-comment the following line for debugging Zyre
    //zyre_set_verbose(m_zyreNode);
    //n_ZMQ::zyreJoin(m_zyreNode, m_zyreGroup); // group enables Zyre multicast, m_zyreNode is unique ID for a single Zyre node
    LOG_INFORM(s_typeName(), "::start started Zyre node with node ID ", m_zyreNodeId, " and network device ", m_zyreNetworkDevice);
    return (true);
};

bool
ZeroMqZyreBridge::terminate()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_isStarted = false;
    m_isTerminate = true;
    lock.unlock();
    lock.lock();
    terminateZyreNodeAndThread();
    return (true);
};

void
ZeroMqZyreBridge::terminateZyreNodeAndThread()
{
    try
    {
        if (m_zyreNode != nullptr)
        {
            zyre_destroy(&m_zyreNode);
            m_zyreNode = nullptr;
            LOG_INFORM(s_typeName(), "::terminateZyreNodeAndThread destroyed Zyre node");
        }
    }
    catch (std::exception& ex)
    {
        LOG_ERROR(s_typeName(), "::terminateZyreNodeAndThread destroying Zyre node EXCEPTION: ", ex.what());
    }

    if (m_zyreEventProcessingThread && m_zyreEventProcessingThread->joinable())
    {
        m_zyreEventProcessingThread->detach();
        LOG_INFORM(s_typeName(), "::terminateZyreNodeAndThread detached m_zyreEventProcessingThread");
    }
    else
    {
        LOG_INFORM(s_typeName(), "::terminateZyreNodeAndThread did not detach m_zyreEventProcessingThread");
    }
};

void
ZeroMqZyreBridge::executeZyreEventProcessing()
{
    try
    {
        // create a poller and add Zyre node reader
        zpoller_t *poller = zpoller_new(zyre_socket(m_zyreNode), NULL);
        assert(poller);

        while (!m_isTerminate)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_isStarted)
            {
                // wait indefinitely for the next input on returned reader
                zsock_t *readerWithNextInput = (zsock_t *) zpoller_wait(poller, uxas::common::ConfigurationManager::getZeroMqReceiveSocketPollWaitTime_ms());

                // affirm reader input is Zyre node reader and process
                if (readerWithNextInput == zyre_socket(m_zyreNode))
                {
                    zyre_event_t *zyre_event = zyre_event_new(m_zyreNode);

                    if (zyre_event_type(zyre_event) == ZYRE_EVENT_ENTER)
                    {
                        // <editor-fold defaultstate="collapsed" desc="ZYRE_EVENT_ENTER">
                        std::string zyreRemoteUuid(static_cast<const char*> (zyre_event_sender(zyre_event)));
                        LOG_INFORM(s_typeName(), "::executeZyreEventProcessing ZYRE_EVENT_ENTER event from ", zyreRemoteUuid);
                        if (!zyreRemoteUuid.empty())
                        {
                            if (m_isZyreEnterMessageHandler)
                            {
                                std::unordered_map<std::string, std::string> headerKeyValuePairs;
                                zhash_t *headers = zyre_event_headers(zyre_event);
                                if (headers)
                                {
                                    for (auto hdrKeysIt = m_receivedMessageHeaderKeys.cbegin(), hdrKvPairsItEnd = m_receivedMessageHeaderKeys.cend(); hdrKeysIt != hdrKvPairsItEnd; hdrKeysIt++)
                                    {
                                        std::string value;
                                        n_ZMQ::ZhashLookup(headers, hdrKeysIt->substr(), value);
                                        LOG_INFORM(s_typeName(), "::executeZyreEventProcessing received ZYRE_EVENT_ENTER header key/value pair KEY [", hdrKeysIt->substr(), "] VALUE [", value, "]");
                                        headerKeyValuePairs.emplace(std::move(hdrKeysIt->substr()), std::move(value));
                                    }
                                }
                                headers = nullptr; // release borrowed headers (hash) object
                                LOG_DEBUGGING(s_typeName(), "::executeZyreEventProcessing invoking Zyre enter message handler");
                                processReceivedZyreEnterMessage(zyreRemoteUuid, headerKeyValuePairs);
                            }
                            else
                            {
                                LOG_WARN(s_typeName(), "::executeZyreEventProcessing not invoking Zyre enter message handler - callback function not set");
                            }
                        }
                        else
                        {
                            LOG_WARN(s_typeName(), "::executeZyreEventProcessing ignoring ZYRE_EVENT_EXIT event having empty remote UUID");
                        }
                        // </editor-fold>
                    }
                    else if (zyre_event_type(zyre_event) == ZYRE_EVENT_JOIN)
                    {
                        LOG_INFORM(s_typeName(), "::executeZyreEventProcessing ignoring ZYRE_EVENT_JOIN event from ",
                                 static_cast<const char*> (zyre_event_sender(zyre_event)));
                    }
                    else if (zyre_event_type(zyre_event) == ZYRE_EVENT_LEAVE)
                    {
                        LOG_INFORM(s_typeName(), "::executeZyreEventProcessing ignoring ZYRE_EVENT_LEAVE event from ",
                                 static_cast<const char*> (zyre_event_sender(zyre_event)));
                    }
                    else if (zyre_event_type(zyre_event) == ZYRE_EVENT_EXIT)
                    {
                        // <editor-fold defaultstate="collapsed" desc="ZYRE_EVENT_EXIT">
                        std::string zyreRemoteUuid(static_cast<const char*> (zyre_event_sender(zyre_event)));
                        if (!zyreRemoteUuid.empty())
                        {
                            if (m_isZyreExitMessageHandler)
                            {
                                LOG_INFORM(s_typeName(), "::executeZyreEventProcessing ZYRE_EVENT_EXIT event from ", zyreRemoteUuid, " invoking Zyre exit message handler");
                                processReceivedZyreExitMessage(zyreRemoteUuid);
                            }
                            else
                            {
                                LOG_WARN(s_typeName(), "::executeZyreEventProcessing not invoking Zyre exit message handler - callback function not set");
                            }
                        }
                        else
                        {
                            LOG_WARN(s_typeName(), "::executeZyreEventProcessing ignoring ZYRE_EVENT_EXIT event having empty remote UUID");
                        }
                        // </editor-fold>
                    }
                    else if (zyre_event_type(zyre_event) == ZYRE_EVENT_SHOUT)
                    {
                        LOG_INFORM(s_typeName(), "::executeZyreEventProcessing ignoring ZYRE_EVENT_SHOUT event from ",
                                 static_cast<const char*> (zyre_event_sender(zyre_event)));
                    }
                    else if (zyre_event_type(zyre_event) == ZYRE_EVENT_WHISPER)
                    {
                        // <editor-fold defaultstate="collapsed" desc="ZYRE_EVENT_WHISPER">
                        std::string zyreRemoteUuid(static_cast<const char*> (zyre_event_sender(zyre_event)));
                        if (!zyreRemoteUuid.empty())
                        {
                            zmsg_t *msg = zyre_event_msg(zyre_event);
                            std::string messagePayload;
                            n_ZMQ::zmsgPopstr(msg, messagePayload);
                            msg = nullptr; // release borrowed message object
                            if (!messagePayload.empty())
                            {
                                LOG_INFORM(s_typeName(), "::executeZyreEventProcessing ZYRE_EVENT_WHISPER event from ", zyreRemoteUuid);
                                LOG_DEBUGGING(s_typeName(), "::executeZyreEventProcessing ZYRE_EVENT_WHISPER event from ",
                                              zyreRemoteUuid, " with message payload ", messagePayload);
                                if (m_isZyreWhisperMessageHandler)
                                {
                                    LOG_DEBUGGING(s_typeName(), "::executeZyreEventProcessing invoking Zyre whisper message handler");
                                    processReceivedZyreWhisperMessage(zyreRemoteUuid, messagePayload);
                                }
                                else
                                {
                                    LOG_WARN(s_typeName(), "::executeZyreEventProcessing not invoking Zyre whisper message handler - callback function not set");
                                }
                            }
                            else
                            {
                                LOG_ERROR(s_typeName(), "::executeZyreEventProcessing ignoring invalid ZYRE_EVENT_WHISPER event having empty message payload");
                            }
                        }
                        else
                        {
                            LOG_WARN(s_typeName(), "::executeZyreEventProcessing ignoring ZYRE_EVENT_WHISPER event having empty remote UUID");
                        }
                        // </editor-fold>
                    }
                    zyre_event_destroy(&zyre_event);
                }
            }
            else
            {
                int rc = zpoller_remove(poller, zyre_socket(m_zyreNode));
                assert(rc == 0);
                zpoller_destroy(&poller);
                break;
            }
        }
        LOG_INFORM(s_typeName(), "::executeTcpReceiveProcessing exiting infinite loop thread [", std::this_thread::get_id(), "]");
    }
    catch (std::exception& ex)
    {
        LOG_ERROR(s_typeName(), "::executeZyreEventProcessing EXCEPTION: ", ex.what());
    }
};

void
ZeroMqZyreBridge::processReceivedZyreEnterMessage(const std::string& zyreRemoteUuid, const std::unordered_map<std::string, std::string>& headerKeyValuePairs)
{
    LOG_INFORM(s_typeName(), "::processReceivedZyreEnterMessage invoking registered handler");
    m_zyreEnterMessageHandler(zyreRemoteUuid, headerKeyValuePairs);
};

void
ZeroMqZyreBridge::processReceivedZyreExitMessage(const std::string& zyreRemoteUuid)
{
    LOG_INFORM(s_typeName(), "::processReceivedZyreExitMessage invoking registered handler");
    m_zyreExitMessageHandler(zyreRemoteUuid);
};

void
ZeroMqZyreBridge::processReceivedZyreWhisperMessage(const std::string& zyreRemoteUuid, const std::string& messagePayload)
{
    LOG_INFORM(s_typeName(), "::processReceivedZyreWhisperMessage invoking registered handler");
    m_zyreWhisperMessageHandler(zyreRemoteUuid, messagePayload);
};

void
ZeroMqZyreBridge::sendZyreWhisperMessage(const std::string& zyreRemoteUuid, const std::string& messagePayload)
{
    LOG_INFORM(s_typeName(), "::sendZyreWhisperMessage sending Zyre whisper message");
    n_ZMQ::zyreWhisper2(m_zyreNode, zyreRemoteUuid, messagePayload);
};

}; //namespace communications
}; //namespace uxas
