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

#ifndef SST_CORE_CORE_EXIT_H
#define SST_CORE_CORE_EXIT_H

#include <sst/core/sst_types.h>
#include <sst/core/serialization.h>

#include <unordered_set>

#include <sst/core/action.h>

namespace SST{

#define _EXIT_DBG( fmt, args...) __DBG( DBG_EXIT, Exit, fmt, ## args )

class Simulation;
class TimeConverter;

/**
 * Exit Event Action
 *
 * Causes the simulation to halt
 */
class Exit : public Action {
public:
    /**
     * Create a new ExitEvent
     * @param sim - Simulation Object
     * @param period - Period upon which to check for exit status
     * @param single_rank - True if there are no parallel ranks
     *
     *  Exit needs to register a handler during constructor time, which
     * requires a simulation object.  But the simulation class creates
     * an Exit object during it's construction, meaning that
     * Simulation::getSimulation() won't work yet.  So Exit is the one
     * exception to the "constructors shouldn't take simulation
     * pointers" rule.  However, it still needs to follow the "classes
     * shouldn't contain pointers back to Simulation" rule.
     */
    Exit(int num_threads, TimeConverter* period, bool single_rank );
    ~Exit();

    /** Increment Reference Count for a given Component ID */
    bool refInc( ComponentId_t, uint32_t thread );
    /** Decrement Reference Count for a given Component ID */
    bool refDec( ComponentId_t, uint32_t thread );

    unsigned int getRefCount();
    SimTime_t getEndTime() { return end_time; }
    
    void execute(void);
    void check();

    void print(const std::string& header, Output &out) const {
        out.output("%s Exit Action to be delivered at %" PRIu64 " with priority %d\n",
                header.c_str(), getDeliveryTime(), getPriority());
    }
    
private:
    Exit() { } // for serialization only
    Exit(const Exit&);           // Don't implement
    void operator=(Exit const&); // Don't implement

//     bool handler( Event* );
    
//     EventHandler< Exit, bool, Event* >* m_functor;
    int num_threads;
    unsigned int    m_refCount;
    unsigned int*   m_thread_counts;
    TimeConverter*  m_period;
    std::unordered_set<ComponentId_t> m_idSet;
    SimTime_t end_time;

    Core::ThreadSafe::Spinlock slock;
    
    bool single_rank;
    
    friend class boost::serialization::access;
    template<class Archive>
    void serialize(Archive & ar, const unsigned int version);
};

} // namespace SST

BOOST_CLASS_EXPORT_KEY(SST::Exit)

#endif // SST_CORE_EXIT_H
