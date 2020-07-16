/*********************                                                        */
/*! \file SubQuery.h
 ** \verbatim
 ** Top contributors (to current version):
 **   Haoze Wu
 ** This file is part of the Marabou project.
 ** Copyright (c) 2017-2019 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved. See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** [[ Add lengthier description here ]]

**/

#ifndef __SubQuery_h__
#define __SubQuery_h__

#include "List.h"
#include "MString.h"
#include "PiecewiseLinearCaseSplit.h"

#include <boost/lockfree/queue.hpp>
#include <utility>

// Struct representing a subquery
struct SubQuery
{
    SubQuery()
    {
    }

    SubQuery( const String &queryId, std::unique_ptr<PiecewiseLinearCaseSplit> &split, unsigned timeoutInSeconds )
        : _queryId( queryId )
        , _split( std::move( split ) )
        , _timeoutInSeconds( timeoutInSeconds )
    {
    }

SubQuery( const String &queryId, std::unique_ptr<PiecewiseLinearCaseSplit> &split, unsigned timeoutInSeconds, List<unsigned> &targetsToCheck )
        : _queryId( queryId )
        , _split( std::move( split ) )
        , _timeoutInSeconds( timeoutInSeconds )
	, _targetsToCheck( targetsToCheck )
    {
    }

    
    String _queryId;
    std::unique_ptr<PiecewiseLinearCaseSplit> _split;
    unsigned _timeoutInSeconds;
    List<unsigned> _targetsToCheck;
};

// Synchronized Queue containing the Sub-Queries shared by workers
typedef boost::lockfree::queue<SubQuery *, boost::lockfree::
  fixed_sized<false>>WorkerQueue;

// A vector of Sub-Queries

// Guy: consider using our wrapper class Vector instead of std::vector
typedef List<SubQuery *> SubQueries;


struct Hypercube
{
    Hypercube()
    {
    }

Hypercube( PiecewiseLinearCaseSplit &split, Map<unsigned, double> &assignment )
    : _split( split )
    , _assignment( assignment )
    {
    }

    PiecewiseLinearCaseSplit _split;
    Map<unsigned, double> _assignment;
};

typedef boost::lockfree::queue<Hypercube *, boost::lockfree::
    fixed_sized<false>> Hypercubes;

#endif // __SubQuery_h__

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//
