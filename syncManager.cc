// Copyright 2009-2015 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
// 
// Copyright (c) 2009-2015, Sandia Corporation
// All rights reserved.
// 
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include "sst_config.h"

#include "sst/core/syncManager.h"

#include "sst/core/exit.h"
#include "sst/core/simulation.h"
#include "sst/core/syncBase.h"
#include "sst/core/threadSyncQueue.h"
#include "sst/core/timeConverter.h"

#include "sst/core/rankSyncSerialSkip.h"
#include "sst/core/threadSyncSimpleSkip.h"

namespace SST {

// Static data members
std::mutex SyncManager::sync_mutex;
NewRankSync* SyncManager::rankSync = NULL;
SimTime_t SyncManager::next_rankSync = MAX_SIMTIME_T;

class EmptyRankSync : public NewRankSync {
public:
    EmptyRankSync() {
        nextSyncTime = MAX_SIMTIME_T;
    }
    ~EmptyRankSync() {}

    /** Register a Link which this Sync Object is responsible for */
    ActivityQueue* registerLink(const RankInfo& to_rank, const RankInfo& from_rank, LinkId_t link_id, Link* link) { return NULL; }

    void execute(int thread) {}
    void exchangeLinkInitData(int thread, std::atomic<int>& msg_count) {}
    void finalizeLinkConfigurations() {}

    SimTime_t getNextSyncTime() { return nextSyncTime; }

    TimeConverter* getMaxPeriod() {return max_period;}

    uint64_t getDataSize() const { return 0; }    
};

class EmptyThreadSync : public NewThreadSync {
public:
    EmptyThreadSync () {
        nextSyncTime = MAX_SIMTIME_T;
    }
    ~EmptyThreadSync() {}

    void before() {}
    void after() {}
    void execute() {}
    void processLinkInitData() {}
    void finalizeLinkConfigurations() {}

    /** Register a Link which this Sync Object is responsible for */
    void registerLink(LinkId_t link_id, Link* link) {}
    ActivityQueue* getQueueForThread(int tid) { return NULL; }
};


SyncManager::SyncManager(const RankInfo& rank, const RankInfo& num_ranks, Core::ThreadSafe::Barrier& barrier, TimeConverter* minPartTC, const std::vector<SimTime_t>& interThreadLatencies) :
    Action(),
    rank(rank),
    num_ranks(num_ranks),
    barrier(barrier),
    threadSync(NULL),
    next_threadSync(0)
{
    // TraceFunction trace(CALL_INFO_LONG);    
    if ( rank.thread == 0  ) {
        if ( num_ranks.rank > 1 ) {
            rankSync = new RankSyncSerialSkip(barrier, minPartTC);
        }
        else {
            rankSync = new EmptyRankSync();
        }
        
    }

    if ( num_ranks.thread > 1 ) {
        threadSync = new ThreadSyncSimpleSkip(num_ranks.thread, rank.thread, Simulation::getSimulation());
    }
    else {
        threadSync = new EmptyThreadSync();
    }

    sim = Simulation::getSimulation();
    exit = sim->getExit();

}


SyncManager::~SyncManager() {}

/** Register a Link which this Sync Object is responsible for */
ActivityQueue*
SyncManager::registerLink(const RankInfo& to_rank, const RankInfo& from_rank, LinkId_t link_id, Link* link)
{
    // TraceFunction trace(CALL_INFO_LONG);    
    if ( to_rank == from_rank ) {
        // trace.getOutput().output(CALL_INFO, "The impossible happened\n");
        return NULL;  // This should never happen
    }

    if ( to_rank.rank == from_rank.rank ) {
        // TraceFunction trace2(CALL_INFO);
        // Same rank, different thread.  Need to send the right data
        // to the two ThreadSync objects for the threads on either
        // side of the link

        // For the local ThreadSync, just need to register the link
        threadSync->registerLink(link_id, link);

        // Need to get target queue from the remote ThreadSync
        NewThreadSync* remoteSync = Simulation::instanceVec[to_rank.thread]->syncManager->threadSync;
        // trace.getOutput().output(CALL_INFO,"queue = %p\n",remoteSync->getQueueForThread(from_rank.thread));
        return remoteSync->getQueueForThread(from_rank.thread);
    }
    else {
        // Different rank.  Send info onto the RankSync
        return rankSync->registerLink(to_rank, from_rank, link_id, link);
    }
}

void
SyncManager::execute(void)
{
    // TraceFunction trace(CALL_INFO_LONG);    
    // trace.getOutput().output(CALL_INFO, "next_sync_type @ switch = %d\n", next_sync_type);
    switch ( next_sync_type ) {
    case RANK:

        // Need to make sure all threads have reached the sync to
        // guarantee that all events have been sent to the appropriate
        // queues.
        barrier.wait();
        
        // For a rank sync, we will force a thread sync first.  This
        // will ensure that all events sent between threads will be
        // flushed into their repective TimeVortices.  We need to do
        // this to enable any skip ahead optimizations.
        threadSync->before();
        // trace.getOutput().output(CALL_INFO, "Complete threadSync->before()\n");

        // Need to make sure everyone has made it through the mutex
        // and the min time computation is complete
        barrier.wait();
        
        // Now call the actual RankSync
        // trace.getOutput().output(CALL_INFO, "About to enter rankSync->execute()\n");
        rankSync->execute(rank.thread);
        // trace.getOutput().output(CALL_INFO, "Complete rankSync->execute()\n");

        barrier.wait();

        // Now call the threadSync after() call
        threadSync->after();
        // trace.getOutput().output(CALL_INFO, "Complete threadSync->after()\n");

        barrier.wait();
        
        if ( exit != NULL && rank.thread == 0 ) exit->check();

        barrier.wait();
        break;
    case THREAD:

        threadSync->execute();

        if ( num_ranks.rank == 1 ) {
            if ( exit->getRefCount() == 0 ) {
                endSimulation(exit->getEndTime());
            }
        }
        // if ( exit != NULL ) exit->check();
        
        break;
    default:
        break;
    }
    computeNextInsert();
    // trace.getOutput().output(CALL_INFO, "next_sync_type = %d\n", next_sync_type);
}

/** Cause an exchange of Initialization Data to occur */
void
SyncManager::exchangeLinkInitData(std::atomic<int>& msg_count)
{
    // TraceFunction trace(CALL_INFO_LONG);    
    barrier.wait();
    threadSync->processLinkInitData();
    barrier.wait();
    rankSync->exchangeLinkInitData(rank.thread, msg_count);
    barrier.wait();
}

/** Finish link configuration */
void
SyncManager::finalizeLinkConfigurations()
{
    // TraceFunction trace(CALL_INFO_LONG);    
    threadSync->finalizeLinkConfigurations();
    // Only thread 0 should call finalize on rankSync
    if ( rank.thread == 0 ) rankSync->finalizeLinkConfigurations();

    // Need to figure out what sync comes first and insert object into
    // TimeVortex
    computeNextInsert();
}

void
SyncManager::computeNextInsert()
{
    // TraceFunction trace(CALL_INFO_LONG);    
    if ( rankSync->getNextSyncTime() <= threadSync->getNextSyncTime() ) {
        next_sync_type = RANK;
        sim->insertActivity(rankSync->getNextSyncTime(), this);
        // sim->getSimulationOutput().output("Next insert at: %" PRIu64 " (rank)\n",rankSync->getNextSyncTime());
    }
    else {
        next_sync_type = THREAD;
        sim->insertActivity(threadSync->getNextSyncTime(), this);
        // sim->getSimulationOutput().output("Next insert at: %" PRIu64 " (thread)\n",threadSync->getNextSyncTime());
    }
}

#if 0
/*********************************************
 *  NewThreadSync
 ********************************************/

void
NewThreadSync::before()
{
    // TraceFunction trace(CALL_INFO_LONG);
    // totalWaitTime += barrier.wait();
    barrier.wait();
    if ( disabled ) return;
    // Empty all the queues and send events on the links
    for ( int i = 0; i < queues.size(); i++ ) {
        ThreadSyncQueue* queue = queues[i];
        std::vector<Activity*>& vec = queue->getVector();
        for ( int j = 0; j < vec.size(); j++ ) {
            Event* ev = static_cast<Event*>(vec[j]);
            auto link = link_map.find(ev->getLinkId());
            if (link == link_map.end()) {
                printf("Link not found in map!\n");
                abort();
            } else {
                SimTime_t delay = ev->getDeliveryTime() - sim->getCurrentSimCycle();
                link->second->send(delay,ev);
            }
        }
        queue->clear();
    }
/*
    Exit* exit = sim->getExit();
    // std::cout << "NewThreadSync(" << Simulation::getSimulation()->getRank().thread << ")::ref_count = " << exit->getRefCount() << std::endl;
    if ( single_rank && exit->getRefCount() == 0 ) {
        endSimulation(exit->getEndTime());
    }
*/
    // totalWaitTime += barrier.wait();
    barrier.wait();

    SimTime_t next = sim->getCurrentSimCycle() + max_period->getFactor();
}

void
NewThreadSync::after()
{
}

void
NewThreadSync::execute()
{
    before();
    after();
}

int
NewThreadSync::exchangeLinkInitData(int msg_count)
{
    // Need to walk through all the queues and send the data to the
    // correct links
    for ( int i = 0; i < num_threads; i++ ) {
        ThreadSyncQueue* queue = queues[i];
        std::vector<Activity*>& vec = queue->getVector();
        for ( int j = 0; j < vec.size(); j++ ) {
            Event* ev = static_cast<Event*>(vec[j]);
            auto link = link_map.find(ev->getLinkId());
            if (link == link_map.end()) {
                printf("Link not found in map!\n");
                abort();
            } else {
//                link->second->sendInitData_sync(ev);
            }
        }
        queue->clear();
    }
}

void
NewThreadSync::finalizeLinkConfigurations()
{
    // TraceFunction(CALL_INFO_LONG);    
    for (auto i = link_map.begin() ; i != link_map.end() ; ++i) {
        // i->second->finalizeConfiguration();
    }
}
#endif
} // namespace SST


