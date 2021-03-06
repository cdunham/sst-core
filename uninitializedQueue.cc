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
#include "sst/core/serialization.h"

#include <ostream>

#include <sst/core/uninitializedQueue.h>

namespace SST {

    UninitializedQueue::UninitializedQueue(std::string message) :
	ActivityQueue(), message(message) {}

    UninitializedQueue::UninitializedQueue() :
	ActivityQueue() {}

    UninitializedQueue::~UninitializedQueue() {}

    bool UninitializedQueue::empty()
    {
	std::cout << message << std::endl;
	abort();
    }
    
    int UninitializedQueue::size()
    {
	std::cout << message << std::endl;
	abort();
    }
    
    void UninitializedQueue::insert(Activity* activity)
    {
	std::cout << message << std::endl;
	abort();
    }
    
    Activity* UninitializedQueue::pop()
    {
	std::cout << message << std::endl;
	abort();
    }

    Activity* UninitializedQueue::front()
    {
	std::cout << message << std::endl;
	abort();
    }


} // namespace SST

BOOST_CLASS_EXPORT_IMPLEMENT(SST::UninitializedQueue)
