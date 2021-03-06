// ===============================================================================
// Authors: AFRL/RQQA
// Organization: Air Force Research Laboratory, Aerospace Systems Directorate, Power and Control Division
// 
// Copyright (c) 2017 Government of the United State of America, as represented by
// the Secretary of the Air Force.  No copyright is claimed in the United States under
// Title 17, U.S. Code.  All Other Rights Reserved.
// ===============================================================================

/* 
 * File:   Task_Base.cpp
 * Author: steve
 * 
 * Created on September 2, 2015, 4:17 PM
 */

#include "TaskServiceBase.h"


#include "FileSystemUtilities.h"

#include "afrl/cmasi/AirVehicleConfiguration.h"
#include "afrl/cmasi/AirVehicleState.h"
#include "afrl/impact/GroundVehicleConfiguration.h"
#include "afrl/impact/GroundVehicleState.h"
#include "afrl/impact/SurfaceVehicleConfiguration.h"
#include "afrl/impact/SurfaceVehicleState.h"
#include "avtas/lmcp/LmcpXMLReader.h"
#include "uxas/messages/task/TaskComplete.h"
#include "uxas/messages/task/TaskInitialized.h"
#include "uxas/messages/task/TaskActive.h"

#include <algorithm>      //std::find_if
#include <sstream>      //std::stringstream
#include <iomanip>  //setfill

#define MAX_TOTAL_COST_MS (INT64_MAX / 1000)
#define STRING_XML_TYPE "Type"
#define STRING_XML_ENTITY_CONFIGURATIONS "EntityConfigurations"

namespace uxas
{
namespace service
{
namespace task
{

#define COUT_INFO_MSG(MESSAGE) std::cout << "<>Task_Base::" << MESSAGE << std::endl;std::cout.flush();
#define COUT_FILE_LINE_MSG(MESSAGE) std::cout << "<>Task_Base:" << __FILE__ << ":" << __LINE__ << ":" << MESSAGE << std::endl;std::cout.flush();
#define CERR_FILE_LINE_MSG(MESSAGE) std::cerr << "<>Task_Base:" << __FILE__ << ":" << __LINE__ << ":" << MESSAGE << std::endl;std::cerr.flush();

const int64_t TaskOptionClass::m_routeIdFromLastTask{1}; // id of the route from the last position to the start of this task option  
const int64_t TaskOptionClass::m_firstImplementationRouteId{2}; // first id to use for the routes in this task option
//XML STRINGS    
const std::string TaskServiceBase::m_taskOptions_XmlTag{"TaskOptions"};

TaskOptionClass::TaskOptionClass(std::shared_ptr<uxas::messages::task::TaskOption>& taskOption)
: m_taskOption(taskOption) { };

TaskServiceBase::TaskServiceBase(const std::string& typeName, const std::string& directoryName)
: ServiceBase(typeName, directoryName) 
{
    //    COUT_INFO_MSG("Task Type[" << typeName << "] m_serviceId[" << m_serviceId << "] CREATED")
}

TaskServiceBase::~TaskServiceBase()
{
    if (m_task)
    {
        //        COUT_INFO_MSG("TaskID[" << m_task->getTaskID() << "] m_serviceId[" << m_serviceId << "] DESTROYED")
    }
};

bool TaskServiceBase::configure(const pugi::xml_node& serviceXmlNode)
{
    bool isSuccessful(true);

    if (m_workDirectoryPath.empty())
    {
        m_workDirectoryPath = "./";
    }

    std::string strNewPathName;
    std::stringstream sstrErrors;
    std::stringstream sstrNewDirectoryPrefix;
    sstrNewDirectoryPrefix << m_serviceType << "_ID" << std::setfill('0') << std::setw(4) << m_serviceId;
    std::string strComponentPath = m_workDirectoryPath + "/Tasks/" + m_serviceType + "/";
    isSuccessful = uxas::common::utilities::c_FileSystemUtilities::bCreateUniqueDirectory(strComponentPath, m_serviceType, strNewPathName, sstrErrors);
    m_strSavePath = strNewPathName;

    m_task = generateTaskObject(serviceXmlNode);
    if (!m_task)
    {
        sstrErrors << "ERROR:: **Task_Base::bConfigure failed: could find a task in [" << serviceXmlNode.name() << "]" << std::endl;
        CERR_FILE_LINE_MSG(sstrErrors.str())
        isSuccessful = false;
    }
    pugi::xml_node entityConfiguratons = serviceXmlNode.child(STRING_XML_ENTITY_CONFIGURATIONS);
    if (entityConfiguratons)
    {
        for (auto ndEntityConfiguration = entityConfiguratons.first_child(); ndEntityConfiguration; ndEntityConfiguration = ndEntityConfiguration.next_sibling())
        {
            auto entityConfiguration = generateEntityConfiguration(ndEntityConfiguration);
            if (entityConfiguration)
            {
                auto foundEntity = std::find(m_task->getEligibleEntities().begin(), m_task->getEligibleEntities().end(), entityConfiguration->getID());
                if(m_task->getEligibleEntities().empty() || foundEntity != m_task->getEligibleEntities().end())
                {
                    m_idVsEntityConfiguration.insert(std::make_pair(entityConfiguration->getID(), entityConfiguration));
                    auto nominalSpeedToOneDecimalPlace_mps = std::round(entityConfiguration->getNominalSpeed()*10.0) / 10.0;
                    auto nominalAltitudeRounded = std::round(entityConfiguration->getNominalAltitude());
                    m_speedAltitudeVsEligibleEntityIds[std::make_pair(nominalSpeedToOneDecimalPlace_mps, nominalAltitudeRounded)].push_back(entityConfiguration->getID());
                }
            }
            else
            {
                std::stringstream errorStream;
                ndEntityConfiguration.print(errorStream);

                sstrErrors << "ERROR:: **Task_Base::bConfigure failed: could not find the EntityConfiguration in ["
                        << errorStream.str() << std::endl;
                CERR_FILE_LINE_MSG(sstrErrors.str())
                isSuccessful = false;
            }
        }
    }

    pugi::xml_node linesOfInterest = serviceXmlNode.child("LinesOfInterest");
    if (linesOfInterest)
    {
        for (auto ndLineOfInterest = linesOfInterest.first_child(); ndLineOfInterest; ndLineOfInterest = ndLineOfInterest.next_sibling())
        {
            std::stringstream stringStream;
            ndLineOfInterest.print(stringStream);
            std::shared_ptr<afrl::impact::LineOfInterest> lineOfInterest;
            avtas::lmcp::Object* object = avtas::lmcp::xml::readXML(stringStream.str());
            if ((object != nullptr) && (afrl::impact::isLineOfInterest(object)))
            {
                lineOfInterest.reset(static_cast<afrl::impact::LineOfInterest*> (object));
                m_idVsLineOfInterest[lineOfInterest->getLineID()] = lineOfInterest;
                object = nullptr;
            }
            else
            {
                sstrErrors << "ERROR:: **Task_Base::bConfigure failed: could not convert a LineOfInterest in [" << ndLineOfInterest << "]" << std::endl;
                CERR_FILE_LINE_MSG(sstrErrors.str())
                isSuccessful = false;
            }
        }
    }

    pugi::xml_node missionCommands = serviceXmlNode.child("MissionCommands");
    if (missionCommands)
    {
        for (auto ndMissionCommand = missionCommands.first_child(); ndMissionCommand; ndMissionCommand = ndMissionCommand.next_sibling())
        {
            std::stringstream stringStream;
            ndMissionCommand.print(stringStream);
            std::shared_ptr<afrl::cmasi::MissionCommand> missionCommand;
            avtas::lmcp::Object* object = avtas::lmcp::xml::readXML(stringStream.str());
            if ((object != nullptr) && (afrl::cmasi::isMissionCommand(object)))
            {
                missionCommand.reset(static_cast<afrl::cmasi::MissionCommand*> (object));
                m_vehicleIdVsCurrentMission[missionCommand->getVehicleID()] = missionCommand;
                object = nullptr;
            }
            else
            {
                sstrErrors << "ERROR:: **Task_Base::bConfigure failed: could not convert a MissionCommand in [" << ndMissionCommand << "]" << std::endl;
                CERR_FILE_LINE_MSG(sstrErrors.str())
                isSuccessful = false;
            }
        }
    }

    pugi::xml_node areaOfInterestNode = serviceXmlNode.child("AreaOfInterest");
    if (areaOfInterestNode)
    {
        std::stringstream stringStream;
        areaOfInterestNode.print(stringStream);
        avtas::lmcp::Object* object = avtas::lmcp::xml::readXML(stringStream.str());
        if ((object != nullptr) && (afrl::impact::isAreaOfInterest(object)))
        {
            m_areaOfInterest.reset(static_cast<afrl::impact::AreaOfInterest*> (object));
            object = nullptr;
        }
        else
        {
            sstrErrors << "ERROR:: **Task_Base::bConfigure failed: could find a AreaOfInterest in [" << serviceXmlNode.name() << "]" << std::endl;
            CERR_FILE_LINE_MSG(sstrErrors.str())
            isSuccessful = false;
        }
    }
    pugi::xml_node lineOfInterestNode = serviceXmlNode.child("LineOfInterest");
    if (lineOfInterestNode)
    {
        std::stringstream stringStream;
        lineOfInterestNode.print(stringStream);
        avtas::lmcp::Object* object = avtas::lmcp::xml::readXML(stringStream.str());
        if ((object != nullptr) && (afrl::impact::isLineOfInterest(object)))
        {
            m_lineOfInterest.reset(static_cast<afrl::impact::LineOfInterest*> (object));
            object = nullptr;
        }
        else
        {
            sstrErrors << "ERROR:: **Task_Base::bConfigure failed: could find a LineOfInterest in [" << serviceXmlNode.name() << "]" << std::endl;
            CERR_FILE_LINE_MSG(sstrErrors.str())
            isSuccessful = false;
        }
    }
    pugi::xml_node pointOfInterestNode = serviceXmlNode.child("PointOfInterest");
    if (pointOfInterestNode)
    {
        std::stringstream stringStream;
        pointOfInterestNode.print(stringStream);
        avtas::lmcp::Object* object = avtas::lmcp::xml::readXML(stringStream.str());
        if ((object != nullptr) && (afrl::impact::isPointOfInterest(object)))
        {
            m_pointOfInterest.reset(static_cast<afrl::impact::PointOfInterest*> (object));
            object = nullptr;
        }
        else
        {
            sstrErrors << "ERROR:: **Task_Base::bConfigure failed: could find a PointOfInterest in [" << serviceXmlNode.name() << "]" << std::endl;
            CERR_FILE_LINE_MSG(sstrErrors.str())
            isSuccessful = false;
        }
    }

    addSubscriptionAddress(afrl::cmasi::EntityState::Subscription);
    addSubscriptionAddress(afrl::cmasi::EntityConfiguration::Subscription);
    addSubscriptionAddress(afrl::cmasi::AirVehicleState::Subscription);
    addSubscriptionAddress(afrl::cmasi::AirVehicleConfiguration::Subscription);
    addSubscriptionAddress(afrl::impact::GroundVehicleState::Subscription);
    addSubscriptionAddress(afrl::impact::GroundVehicleConfiguration::Subscription);
    addSubscriptionAddress(afrl::impact::SurfaceVehicleState::Subscription);
    addSubscriptionAddress(afrl::impact::SurfaceVehicleConfiguration::Subscription);
    addSubscriptionAddress(uxas::messages::task::UniqueAutomationRequest::Subscription);
    addSubscriptionAddress(uxas::messages::task::UniqueAutomationResponse::Subscription);
    addSubscriptionAddress(uxas::messages::route::RoutePlanResponse::Subscription);
    addSubscriptionAddress(uxas::messages::task::TaskImplementationRequest::Subscription);

    isSuccessful = isSuccessful && configureTask(serviceXmlNode);

    return (isSuccessful);
};

bool TaskServiceBase::initialize()
{
    bool isSuccessful(true);
    isSuccessful = isSuccessful && initializeTask();
    return (isSuccessful);
};

bool TaskServiceBase::start()
{
    bool isSuccessful{true};
    isSuccessful = isSuccessful && startTask();
    if (isSuccessful)
    {
        auto taskStarted = std::make_shared<uxas::messages::task::TaskInitialized>();
        taskStarted->setTaskID(m_task->getTaskID());
        sendSharedLmcpObjectBroadcastMessage(taskStarted);
    }
    return (isSuccessful);
};

bool TaskServiceBase::terminate()
{
    bool isKillTheService(true);
    isKillTheService = terminateTask();
    return (isKillTheService);
};

bool TaskServiceBase::processReceivedLmcpMessage(std::unique_ptr<uxas::communications::data::LmcpMessage> receivedLmcpMessage)
{
    bool isKillService = false;

    auto entityState = std::dynamic_pointer_cast<afrl::cmasi::EntityState>(receivedLmcpMessage->m_object);
    auto entityConfiguration = std::dynamic_pointer_cast<afrl::cmasi::EntityConfiguration>(receivedLmcpMessage->m_object);

    if (entityState)
    {
        if (m_assignedVehicleIds.find(entityState->getID()) != m_assignedVehicleIds.end())
        {
            bool isOnTask = std::find(entityState->getAssociatedTasks().begin(),
                                      entityState->getAssociatedTasks().end(),
                                      m_task->getTaskID()) != entityState->getAssociatedTasks().end();
            if (isOnTask)
            {
                activeEntityState(entityState); //virtual
                if (m_activeEntities.find(entityState->getID()) == m_activeEntities.end())
                {
                    // task just became active for this vehicle
                    m_activeEntities.insert(entityState->getID());
                    // send TaskActive message
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    COUT_INFO_MSG("Sending TaskActive !!!!")
                    auto taskActive = std::make_shared<uxas::messages::task::TaskActive>();
                    taskActive->setTaskID(m_task->getTaskID());
                    taskActive->setEntityID(entityState->getID());
                    taskActive->setTimeTaskActivated(uxas::common::Time::getInstance().getUtcTimeSinceEpoch_ms());
                    auto newMessage = std::static_pointer_cast<avtas::lmcp::Object>(taskActive);
                    sendSharedLmcpObjectBroadcastMessage(newMessage);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
            else
            {
                if (m_activeEntities.find(entityState->getID()) != m_activeEntities.end())
                {
                    // was active last state update, send taskcomplete message for this vehicle
                    m_activeEntities.erase(entityState->getID());
                    COUT_INFO_MSG("Sending TaskComplete !!!!")
                            // send out task complete - uxas
                            auto taskCompleteUxas = std::make_shared<uxas::messages::task::TaskComplete>();
                    for (auto& assignedVehicleId : m_assignedVehicleIds)
                    {
                        taskCompleteUxas->getEntitiesInvolved().push_back(assignedVehicleId);
                    }
                    taskCompleteUxas->setTaskID(m_task->getTaskID());
                    taskCompleteUxas->setTimeTaskCompleted(uxas::common::Time::getInstance().getUtcTimeSinceEpoch_ms());
                    auto newMessageUxas = std::static_pointer_cast<avtas::lmcp::Object>(taskCompleteUxas);
                    sendSharedLmcpObjectBroadcastMessage(newMessageUxas);
                }
            }
        }
    }
    else if (entityConfiguration)
    {
        auto foundEntity = std::find(m_task->getEligibleEntities().begin(), m_task->getEligibleEntities().end(), entityConfiguration->getID());
        if(m_task->getEligibleEntities().empty() || foundEntity != m_task->getEligibleEntities().end())
        {
            m_idVsEntityConfiguration.insert(std::make_pair(entityConfiguration->getID(), entityConfiguration));
            auto nominalSpeedToOneDecimalPlace_mps = std::round(entityConfiguration->getNominalSpeed()*10.0) / 10.0;
            auto nominalAltitudeRounded = std::round(entityConfiguration->getNominalAltitude());
            m_speedAltitudeVsEligibleEntityIds[std::make_pair(nominalSpeedToOneDecimalPlace_mps, nominalAltitudeRounded)].push_back(entityConfiguration->getID());
        }
    }
    else if (uxas::messages::task::isUniqueAutomationRequest(receivedLmcpMessage->m_object))
    {
        auto uniqueAutomationRequest = std::static_pointer_cast<uxas::messages::task::UniqueAutomationRequest>(receivedLmcpMessage->m_object);
//COUT_FILE_LINE_MSG("uniqueAutomationRequest->getRequestID()[" << uniqueAutomationRequest->getRequestID() << "]")
        if (m_task && uniqueAutomationRequest)
        {
//COUT_FILE_LINE_MSG("uniqueAutomationRequest->getRequestID()[" << uniqueAutomationRequest->getRequestID() << "]")
            m_latestUniqueAutomationRequestId = uniqueAutomationRequest->getRequestID();
            m_idVsUniqueAutomationRequest[uniqueAutomationRequest->getRequestID()] = uniqueAutomationRequest;
            if (std::find(uniqueAutomationRequest->getOriginalRequest()->getTaskList().begin(),
                          uniqueAutomationRequest->getOriginalRequest()->getTaskList().end(),
                          m_task->getTaskID()) != uniqueAutomationRequest->getOriginalRequest()->getTaskList().end())
            {
                m_pendingImplementationRouteRequests.clear();
                // select requested eligible entities, defaults to use them all
                m_speedAltitudeVsEligibleEntityIdsRequested.clear();
                if (!uniqueAutomationRequest->getOriginalRequest()->getEntityList().empty())
                {
                    for (auto itEligibleEntities = m_speedAltitudeVsEligibleEntityIds.begin();
                            itEligibleEntities != m_speedAltitudeVsEligibleEntityIds.end();
                            itEligibleEntities++)
                    {
                        for (auto& eligibleEntity : itEligibleEntities->second)
                        {
                            if (std::find(uniqueAutomationRequest->getOriginalRequest()->getEntityList().begin(),
                                          uniqueAutomationRequest->getOriginalRequest()->getEntityList().end(),
                                          eligibleEntity) !=
                                    uniqueAutomationRequest->getOriginalRequest()->getEntityList().end())
                            {
                                m_speedAltitudeVsEligibleEntityIdsRequested[itEligibleEntities->first].push_back(eligibleEntity);
                            }
                        }
                    }
                }
                else
                {
                    m_speedAltitudeVsEligibleEntityIdsRequested = m_speedAltitudeVsEligibleEntityIds;
                }

                // set/reset task plan options
                m_taskPlanOptions = std::make_shared<uxas::messages::task::TaskPlanOptions>();
                m_taskPlanOptions->setCorrespondingAutomationRequestID(uniqueAutomationRequest->getRequestID());
                m_taskPlanOptions->setTaskID(m_task->getTaskID());
                m_optionIdVsTaskOptionClass.clear();
                m_routeIdVsTaskImplementationRequest.clear();
                m_pendingOptionRouteRequests.clear();
                m_pendingImplementationRouteRequests.clear();

                //build and send out a 'TaskPlanOptions' message
                buildTaskPlanOptions();
            }
        }
        else
        {
            if (!uniqueAutomationRequest)
            {
                //TODO:: error unable to decode UniqueAutomationRequest
            }
            if (!m_task)
            {
                //TODO:: error invalid task object encountered when receiving a UniqueAutomationRequest
            }
        }
    }
    else if (uxas::messages::task::isUniqueAutomationResponse(receivedLmcpMessage->m_object))
    {
        // UniqueAutomationResponse(s) to determine which vehicles are assigned to this task 
        auto uniqueAutomationResponse = std::static_pointer_cast<uxas::messages::task::UniqueAutomationResponse>(receivedLmcpMessage->m_object);

        if (m_idVsUniqueAutomationRequest.find(uniqueAutomationResponse->getResponseID()) == m_idVsUniqueAutomationRequest.end())
        {
            //TODO:: "warning received uniqueAutomationResponse[",uniqueAutomationResponse->getResponseID(),"] with no corresponding uniqueAutomationRequest"
        }
        else
        {
            auto currentAutomationRequest = m_idVsUniqueAutomationRequest[uniqueAutomationResponse->getResponseID()];

            //TODO:: change to look up uniqueautomationrequest and delete it when finished
            if (!currentAutomationRequest->getSandBoxRequest())
            {
                // remove any assigned entities that appear in the uniqueAutomationResponse.
                // Note: if an entity has been reassigned, then it will be added back below
                for (auto& missionCommand : uniqueAutomationResponse->getOriginalResponse()->getMissionCommandList())
                {
                    m_assignedVehicleIds.erase(missionCommand->getVehicleID());
                }
                // search through the waypoints to find vehicles that have been assigned
                for (auto& missionCommand : uniqueAutomationResponse->getOriginalResponse()->getMissionCommandList())
                {
                    for (auto& waypoint : missionCommand->getWaypointList())
                    {
                        bool isOnTask = std::find(waypoint->getAssociatedTasks().begin(),
                                                  waypoint->getAssociatedTasks().end(),
                                                  m_task->getTaskID()) != waypoint->getAssociatedTasks().end();
                        if (isOnTask)
                        {
                            m_assignedVehicleIds.insert(missionCommand->getVehicleID());
                        }
                    }
                }
            }
            m_idVsUniqueAutomationRequest.erase(uniqueAutomationResponse->getResponseID());
        }
    }
    else if (uxas::messages::task::isTaskImplementationRequest(receivedLmcpMessage->m_object))
    {
        if (m_task)
        {
            auto taskImplementationRequest = std::static_pointer_cast<uxas::messages::task::TaskImplementationRequest>(receivedLmcpMessage->m_object);
            if (taskImplementationRequest->getTaskID() == m_task->getTaskID())
            {
                auto itOption = m_optionIdVsTaskOptionClass.find(taskImplementationRequest->getOptionID());
                if (itOption != m_optionIdVsTaskOptionClass.end())
                {
                    buildAndSendImplementationRouteRequestBase(taskImplementationRequest->getOptionID(),
                                                               taskImplementationRequest, itOption->second->m_taskOption);
                }
                else
                {
                    CERR_FILE_LINE_MSG("ERROR::c_Task_CmasiLineSearch::ProcessMessage: for TaskId[" << m_task->getTaskID()
                                       << "] OptionId[" << taskImplementationRequest->getOptionID()
                                       << "] does not exist, but was specified in a TaskImplementationRequest.")
                }
            }
        } //if(m_pointSearchTask)
    }
    else if (uxas::messages::route::isRoutePlanResponse(receivedLmcpMessage->m_object))
    {
        auto routePlanResponse = std::static_pointer_cast<uxas::messages::route::RoutePlanResponse>(receivedLmcpMessage->m_object);
        if (routePlanResponse->getAssociatedTaskID() == m_task->getTaskID())
        {
            auto routeType = getRouteTypeFromRouteId(routePlanResponse->getResponseID());
            switch (routeType)
            {
                default:
                    CERR_FILE_LINE_MSG("ERROR::isProcessedMessageBase:: unknown RoutePlanResponseEncountered for  ResponseID[" << routePlanResponse->getResponseID() << "]")
                    break;
                case RouteTypeEnum::OPTION:
                    processOptionsRoutePlanResponseBase(routePlanResponse);
                    break;
                case RouteTypeEnum::IMPLEMENTATION:
                    processImplementationRoutePlanResponseBase(routePlanResponse);
                    break;
            }
        }
    }

    isKillService = isKillService || processReceivedLmcpMessageTask(receivedLmcpMessage->m_object);

    return (isKillService);
};

int64_t TaskServiceBase::getOptionRouteId(const int64_t& OptionId)
{
    return (getRouteId(RouteTypeEnum::OPTION, OptionId));
}

int64_t TaskServiceBase::getImplementationRouteId(const int64_t& OptionId)
{
    return (getRouteId(RouteTypeEnum::IMPLEMENTATION, OptionId));
}

int64_t TaskServiceBase::getRouteId(const RouteTypeEnum& routeTypeEnum, const int64_t& OptionId)
{
    return ((static_cast<int64_t> (routeTypeEnum) * m_implementationMangleMultiplier) + (m_task->getTaskID() * m_taskMangleMultiplier) + OptionId);
}

int64_t TaskServiceBase::getOptionIdFromRouteId(const int64_t& routeId)
{
    int64_t returnValue = routeId % m_implementationMangleMultiplier;
    returnValue %= m_taskMangleMultiplier;
    return (returnValue);
}

int64_t TaskServiceBase::getTaskFromRouteId(const int64_t& routeId)
{
    int64_t returnValue = routeId % m_implementationMangleMultiplier;
    returnValue /= m_taskMangleMultiplier;
    return (returnValue);
}

TaskServiceBase::RouteTypeEnum TaskServiceBase::getRouteTypeFromRouteId(const int64_t& routeId)
{
    int64_t returnValue = routeId / m_implementationMangleMultiplier;
    RouteTypeEnum returnType = RouteTypeEnum::UNKNOWN;
    if (returnValue < static_cast<int64_t> (RouteTypeEnum::NUMBER_OF_VALUES))
    {
        returnType = static_cast<RouteTypeEnum> (returnValue);
    }
    return (returnType);
}

void TaskServiceBase::buildAndSendImplementationRouteRequestBase(const int64_t& optionId,
                                                                 const std::shared_ptr<uxas::messages::task::TaskImplementationRequest>& taskImplementationRequest,
                                                                 const std::shared_ptr<uxas::messages::task::TaskOption>& taskOption)
{
    // check to see is there is a virtual class handler
    if (!isBuildAndSendImplementationRouteRequest(optionId, taskImplementationRequest, taskOption))
    {
        auto itTaskOptionClass = m_optionIdVsTaskOptionClass.find(optionId);
        if (itTaskOptionClass != m_optionIdVsTaskOptionClass.end())
        {
            auto routePlanRequest = std::make_shared<uxas::messages::route::RoutePlanRequest>();
            routePlanRequest->setRequestID(getImplementationRouteId(optionId));
            routePlanRequest->setAssociatedTaskID(m_task->getTaskID());
            routePlanRequest->setIsCostOnlyRequest(false);
            routePlanRequest->setOperatingRegion(taskImplementationRequest->getRegionID());
            routePlanRequest->setVehicleID(taskImplementationRequest->getVehicleID());
            m_pendingImplementationRouteRequests.insert(routePlanRequest->getRequestID());

            auto routeConstraints = new uxas::messages::route::RouteConstraints();
            int64_t routeId = itTaskOptionClass->second->m_routeIdFromLastTask;
            m_transitionRouteRequestId = routeId;
            itTaskOptionClass->second->m_pendingRouteIds.insert(routeId);
            routeConstraints->setRouteID(routeId);
            routeConstraints->setStartLocation(taskImplementationRequest->getStartPosition()->clone());
            routeConstraints->setStartHeading(taskImplementationRequest->getStartHeading());
            routeConstraints->setEndLocation(taskOption->getStartLocation()->clone());
            routeConstraints->setEndHeading(taskOption->getStartHeading());

            if (itTaskOptionClass->second->m_routePlanRequest)
            {
                routeId = itTaskOptionClass->second->m_firstImplementationRouteId;
                for (auto& routeConstraints : itTaskOptionClass->second->m_routePlanRequest->getRouteRequests())
                {
                    routePlanRequest->getRouteRequests().push_back(routeConstraints->clone());
                    routePlanRequest->getRouteRequests().back()->setRouteID(routeId);
                    itTaskOptionClass->second->m_pendingRouteIds.insert(routeId);
                    routeId++;
                }
            }

            routePlanRequest->getRouteRequests().push_back(routeConstraints);
            routeConstraints = nullptr; //just gave up ownership

            m_routeIdVsTaskImplementationRequest[routePlanRequest->getRequestID()] = taskImplementationRequest;

            auto newMessage = std::static_pointer_cast<avtas::lmcp::Object>(routePlanRequest);
            sendSharedLmcpObjectBroadcastMessage(newMessage);
        }
        else //if ((itTaskOptionClass != m_optionIdVsTaskOptionClass.end()))
        {
            CERR_FILE_LINE_MSG("ERROR::buildAndSendImplementationRouteRequestBase:: TaskOptionClass not found for optionId[" << optionId << "]")
        }
    }
}

void TaskServiceBase::processOptionsRoutePlanResponseBase(const std::shared_ptr<uxas::messages::route::RoutePlanResponse>& routePlanResponse)
{
    auto itResponseId = m_pendingOptionRouteRequests.find(routePlanResponse->getResponseID());
    if (itResponseId != m_pendingOptionRouteRequests.end())
    {
        m_pendingOptionRouteRequests.erase(routePlanResponse->getResponseID());
        auto optionId = getOptionIdFromRouteId(routePlanResponse->getResponseID());
        auto vehicleId = routePlanResponse->getVehicleID();
        auto operatingRegion = routePlanResponse->getOperatingRegion();
        auto itTaskOptionClass = m_optionIdVsTaskOptionClass.find(optionId);
        if (itTaskOptionClass != m_optionIdVsTaskOptionClass.end())
        {
            for (auto routePlan : routePlanResponse->getRouteResponses())
            {
                // we are waiting for this one
                auto route = std::shared_ptr<uxas::messages::route::RoutePlan>(routePlan->clone());
                // call virtual function
                if (!isHandleOptionsRouteResponse(vehicleId, optionId, operatingRegion, route));
                {
                    if (itTaskOptionClass->second->m_pendingRouteIds.find(route->getRouteID()) != itTaskOptionClass->second->m_pendingRouteIds.end())
                    {
                        // remove the route from the pending list
                        itTaskOptionClass->second->m_pendingRouteIds.erase(route->getRouteID());
                        // add this route's cost to the total for this option
                        auto totalCost = MAX_TOTAL_COST_MS;
                        if (route->getRouteCost() > 0)
                        {
                            totalCost = itTaskOptionClass->second->m_taskOption->getCost() + route->getRouteCost();
                        }
                        itTaskOptionClass->second->m_taskOption->setCost(totalCost);
                        if (itTaskOptionClass->second->m_pendingRouteIds.empty())
                        {
                            // once this option is complete, add to the options and remove it from the list
                            m_taskPlanOptions->getOptions().push_back(itTaskOptionClass->second->m_taskOption->clone());
                        }
                    }
                    bool isAllOptionsComplete = true;
                    for (auto& taskOptionClass : m_optionIdVsTaskOptionClass)
                    {
                        if (!taskOptionClass.second->m_pendingRouteIds.empty())
                        {
                            isAllOptionsComplete = false;
                            break;
                        }
                    }
                    if (isAllOptionsComplete)
                    {
                        // once all options are complete, send out the message
                        auto objectTaskPlanOptions = std::static_pointer_cast<avtas::lmcp::Object>(m_taskPlanOptions);
                        sendSharedLmcpObjectBroadcastMessage(objectTaskPlanOptions);
                    }
                }
            } //for (auto routePlan : routePlanResponse->getRouteResponses())
        }
        else //if (itTaskOptionClass != m_optionIdVsTaskOptionClass.end())
        {
            CERR_FILE_LINE_MSG("ERROR::processOptionsRouteResponse:: TaskOptionClass not found for optionId[" << optionId << "]")
        }
    }
    else //if (itResponseId != m_pendingOptionRouteRequests.end())
    {
        CERR_FILE_LINE_MSG("ERROR::processOptionsRoutePlanResponseBase:: RoutePlanResponse received that was not expected, RoutePlanId[" << routePlanResponse->getResponseID() << "]")
    }
}

void TaskServiceBase::processImplementationRoutePlanResponseBase(const std::shared_ptr<uxas::messages::route::RoutePlanResponse>& routePlanResponse)
{
    if (m_idVsUniqueAutomationRequest.find(m_latestUniqueAutomationRequestId) == m_idVsUniqueAutomationRequest.end())
    {
        //TODO:: "warning received uniqueAutomationResponse[",m_latestUniqueAutomationRequestId,"] with no corresponding uniqueAutomationRequest"
    }
    else
    {
        auto currentAutomationRequest = m_idVsUniqueAutomationRequest[m_latestUniqueAutomationRequestId];

        auto itPlanResponseId = m_pendingImplementationRouteRequests.find(routePlanResponse->getResponseID());
        if (itPlanResponseId != m_pendingImplementationRouteRequests.end())
        {
            m_pendingImplementationRouteRequests.erase(routePlanResponse->getResponseID());

            auto optionId = getOptionIdFromRouteId(routePlanResponse->getResponseID());
            auto vehicleId = routePlanResponse->getVehicleID();
            auto operatingRegion = routePlanResponse->getOperatingRegion();
            auto itTaskOptionClass = m_optionIdVsTaskOptionClass.find(optionId);
            auto itTaskImplementationRequest = m_routeIdVsTaskImplementationRequest.find(routePlanResponse->getResponseID());
            if ((itTaskOptionClass != m_optionIdVsTaskOptionClass.end()) &&
                    (itTaskImplementationRequest != m_routeIdVsTaskImplementationRequest.end()))
            {
                for (auto routePlan : routePlanResponse->getRouteResponses())
                {
                    if (itTaskOptionClass->second->m_pendingRouteIds.find(routePlan->getRouteID()) != itTaskOptionClass->second->m_pendingRouteIds.end())
                    {
                        // remove the routePlan from the pending list
                        itTaskOptionClass->second->m_pendingRouteIds.erase(routePlan->getRouteID());
                        // add this routePlan's cost to the total for this option
                        auto totalCost = itTaskOptionClass->second->m_taskOption->getCost();
                        if (routePlan->getRouteID() == m_transitionRouteRequestId)
                        {
                            if (routePlan->getRouteCost() > 0)
                            {
                                totalCost += routePlan->getRouteCost();
                            }
                            else
                            {
                                // this is an error!!!!
                                totalCost = MAX_TOTAL_COST_MS;
                            }
                        }
                        /////////////////////////////////////////////////////////////////////////////////////////////////////
                        itTaskOptionClass->second->m_taskOption->setCost(totalCost);
                        auto pRoutePlan = std::shared_ptr<uxas::messages::route::RoutePlan>(routePlan->clone());
                        itTaskOptionClass->second->m_orderedRouteIdVsPlan[routePlan->getRouteID()] = pRoutePlan;
                        // once all of the routePlans have been received, build the response and send it out
                        if (itTaskOptionClass->second->m_pendingRouteIds.empty())
                        {
                            auto itEntityConfiguration = m_idVsEntityConfiguration.find(vehicleId);
                            if (itEntityConfiguration != m_idVsEntityConfiguration.end())
                            {
                                // build a TaskImplementationResponse
                                auto taskImplementationResponse = std::make_shared<uxas::messages::task::TaskImplementationResponse>();
                                taskImplementationResponse->setResponseID(itTaskImplementationRequest->second->getRequestID());
                                taskImplementationResponse->setTaskID(m_task->getTaskID());
                                taskImplementationResponse->setOptionID(optionId);
                                taskImplementationResponse->setVehicleID(vehicleId);
                                taskImplementationResponse->setFinalLocation(itTaskOptionClass->second->m_taskOption->getEndLocation()->clone());
                                taskImplementationResponse->setFinalHeading(itTaskOptionClass->second->m_taskOption->getEndHeading());
                                // calculate the cost
                                auto assignmentCost = MAX_TOTAL_COST_MS;
                                if (routePlan->getRouteCost() > 0)
                                {
                                    assignmentCost = routePlan->getRouteCost() + itTaskOptionClass->second->m_taskOption->getCost();
                                }
                                taskImplementationResponse->setFinalTime(itTaskImplementationRequest->second->getStartTime() + assignmentCost);
                                int64_t waypointId = itTaskImplementationRequest->second->getStartingWaypointID() + (optionId * 1000) + 1;
                                // waypoints from the saved routes
                                bool isFirstWaypoint = true;
                                for (auto& plan : itTaskOptionClass->second->m_orderedRouteIdVsPlan)
                                {
                                    bool isRouteFromLastToTask = (plan.second->getRouteID() == m_transitionRouteRequestId);
                                    for (auto& planWaypoint : plan.second->getWaypoints())
                                    {
                                        auto waypoint = planWaypoint->clone();
                                        waypoint->setNumber(waypointId);
                                        waypoint->setAltitude(itEntityConfiguration->second->getNominalAltitude());
                                        waypoint->setAltitudeType(itEntityConfiguration->second->getNominalAltitudeType());
                                        waypoint->setSpeed(itEntityConfiguration->second->getNominalSpeed());
                                        if (!isFirstWaypoint)
                                        {
                                            taskImplementationResponse->getTaskWaypoints().back()->setNextWaypoint(waypointId);
                                        }
                                        isFirstWaypoint = false;

                                        // add task active waypoints
                                        if ((!isRouteFromLastToTask || m_isMakeTransitionWaypointsActive) && !currentAutomationRequest->getSandBoxRequest())
                                        {
                                            waypoint->getAssociatedTasks().push_back(m_task->getTaskID());
                                        }
                                        taskImplementationResponse->getTaskWaypoints().push_back(waypoint);
                                        waypoint = nullptr; // gave up ownership
                                        waypointId++;
                                    }
                                    //                                if (isRouteFromLastToTask && !taskImplementationResponse->getTaskWaypoints().empty())
                                    //                                {
                                    //                                    delete taskImplementationResponse->getTaskWaypoints().back();
                                    //                                    taskImplementationResponse->getTaskWaypoints().pop_back();
                                    //                                    if (!taskImplementationResponse->getTaskWaypoints().empty())
                                    //                                    {
                                    //                                        taskImplementationResponse->getTaskWaypoints().back()->setNextWaypoint(0);
                                    //                                    }
                                    //                                    else
                                    //                                    {
                                    //                                        isFirstWaypoint = true;
                                    //                                    }
                                    //                                }
                                }
                                isProcessTaskImplementationRouteResponse(taskImplementationResponse, itTaskOptionClass->second, waypointId, pRoutePlan);
                                if (!taskImplementationResponse->getTaskWaypoints().empty())
                                {
                                    taskImplementationResponse->getTaskWaypoints().back()->setNextWaypoint(0);
                                    if (taskImplementationResponse->getTaskWaypoints().back()->getAssociatedTasks().empty())
                                    {
                                        taskImplementationResponse->getTaskWaypoints().back()->getAssociatedTasks().push_back(m_task->getTaskID());
                                    }
                                    // disassociate the last waypoint in the plan from the tasks, allows tasks to complete
                                    auto waypointLast = taskImplementationResponse->getTaskWaypoints().back()->clone();
                                    auto newNumber = waypointLast->getNumber() + 1;
                                    waypointLast->setNumber(newNumber);
                                    waypointLast->setNextWaypoint(newNumber);
                                    taskImplementationResponse->getTaskWaypoints().back()->setNextWaypoint(newNumber);
                                    waypointLast->getAssociatedTasks().clear();
                                    taskImplementationResponse->getTaskWaypoints().push_back(waypointLast);
                                    waypointLast = nullptr;

                                    // send out the response
                                    auto newMessage = std::static_pointer_cast<avtas::lmcp::Object>(taskImplementationResponse);
                                    sendSharedLmcpObjectBroadcastMessage(newMessage);
                                }
                                else //if(!taskImplementationResponse->getTaskWaypoints().empty())
                                {
                                    CERR_FILE_LINE_MSG("ERROR::TaskServiceBase::processRouteResponse: for TaskId[" << m_task->getTaskID()
                                                       << "] OptionId[" << itTaskOptionClass->second->m_taskOption->getOptionID()
                                                       << "], the plan waypoints are empty.")
                                } //if(!taskImplementationResponse->getTaskWaypoints().empty())
                                // done with this request
                                m_transitionRouteRequestId = 0;
                                //m_optionIdVsTaskOptionClass.clear();    // TODO:: should be able to reuse this for other requests
                            }
                            else //if (itEntityConfiguration != m_idVsEntityConfiguration.end())
                            {
                                // Assignment algorithm selected an invalid vehicle/option combination.
                                // This should never happen; fallback, send empty task implementation response
                                CERR_FILE_LINE_MSG("ERROR::c_Task_Base::isProcessedMessageBase: for TaskId[" << m_task->getTaskID()
                                                   << "] there is not an EntityConfiguration for EntityId[" << vehicleId << "].")

                                // send out the blank response
                                auto taskImplementationResponse = std::make_shared<uxas::messages::task::TaskImplementationResponse>();
                                taskImplementationResponse->setResponseID(itTaskImplementationRequest->second->getRequestID());
                                taskImplementationResponse->setTaskID(m_task->getTaskID());
                                taskImplementationResponse->setOptionID(optionId);
                                taskImplementationResponse->setVehicleID(vehicleId);
                                taskImplementationResponse->setFinalLocation(itTaskImplementationRequest->second->getStartPosition()->clone());
                                taskImplementationResponse->setFinalHeading(itTaskImplementationRequest->second->getStartHeading());
                                sendSharedLmcpObjectBroadcastMessage(taskImplementationResponse);
                            } //if (itEntityConfiguration != m_idVsEntityConfiguration.end())
                        } //if(itTaskOptionClass->second->m_pendingRouteIds.empty())
                    } //if(itTaskOptionClass->second->m_pendingRouteIds.find(routePlan->getRouteID()) != itTaskOptionClass->second->m_pendingRouteIds.end())
                } //for (auto& routePlan : routePlanResponse->getRouteResponses())
                m_routeIdVsTaskImplementationRequest.erase(routePlanResponse->getResponseID());
            }
            else //if(itTaskOptionClass != m_optionIdVsTaskOptionClass.end())
            {
                CERR_FILE_LINE_MSG("ERROR::processImplementationRouteResponseBase:: TaskOptionClass not found for optionId[" << optionId << "]")
            } //if(itTaskOptionClass != m_optionIdVsTaskOptionClass.end())
        }
        else //if (itPlanResponseId != m_pendingImplementationRouteRequests.end())
        {
            CERR_FILE_LINE_MSG("ERROR::processImplementationRouteResponseBase:: RoutePlanResponse received that was not expected, RoutePlanId[" << routePlanResponse->getResponseID() << "]")
        }
    }
}

std::shared_ptr<afrl::cmasi::Task> TaskServiceBase::generateTaskObject(const pugi::xml_node& taskNode)
{
    std::shared_ptr<afrl::cmasi::Task> taskPointer;

    pugi::xml_node taskRequestNode = taskNode.child("TaskRequest");
    if (!taskRequestNode.empty())
    {
        std::stringstream taskStringStream;
        taskRequestNode.first_child().print(taskStringStream);
        avtas::lmcp::Object* object = avtas::lmcp::xml::readXML(taskStringStream.str());
        if (object != nullptr)
        {
            auto task = dynamic_cast<afrl::cmasi::Task*> (object);
            taskPointer.reset(task);
            task = nullptr;
            object = nullptr;
        }
    }
    return (taskPointer);
}

std::shared_ptr<afrl::cmasi::EntityConfiguration> TaskServiceBase::generateEntityConfiguration(pugi::xml_node& entityConfigNode)
{
    std::shared_ptr<afrl::cmasi::EntityConfiguration> entityConfiguration;

    std::stringstream stringStream;
    entityConfigNode.print(stringStream);
    avtas::lmcp::Object* object = avtas::lmcp::xml::readXML(stringStream.str());
    if (object != nullptr)
    {
        entityConfiguration.reset(static_cast<afrl::cmasi::EntityConfiguration*> (object));
        object = nullptr;
    }
    return (entityConfiguration);
}


}; //namespace task
}; //namespace service
}; //namespace uxas