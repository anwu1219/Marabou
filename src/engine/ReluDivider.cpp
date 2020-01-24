/*********************                                                        */
/*! \file ReluLookAheadDivider.h
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

#include "Debug.h"
#include "EngineState.h"
#include "ReluDivider.h"
#include "MStringf.h"
#include "PiecewiseLinearCaseSplit.h"

ReluDivider::ReluDivider( IEngine& engine )
    : _engine( engine )
{
    _threshold = _engine.numberOfConstraints() / 20;
    if ( _threshold < 5 )	
	_threshold = 5;
}

void ReluDivider::createSubQueries( unsigned numNewSubqueries, const String
                                    queryIdPrefix, const
                                    PiecewiseLinearCaseSplit &previousSplit,
                                    const unsigned timeoutInSeconds,
                                    SubQueries &subQueries )
{
    unsigned numBisects = (unsigned)log2( numNewSubqueries );

    _engine.applySplit( previousSplit );
    _engine.propagate();
    _engine.getEstimates( _balanceEstimates, _runtimeEstimates );

    List<PiecewiseLinearCaseSplit *> splits;
    auto split = new PiecewiseLinearCaseSplit();
    *split = previousSplit;
    splits.append( split );

    for ( unsigned i = 0; i < numBisects; ++i )
    {
        List<PiecewiseLinearCaseSplit *> newSplits;
        for ( const auto &split : splits )
        {
            PiecewiseLinearConstraint *pLConstraintToSplit =
                getPLConstraintToSplit( *split );
            if ( pLConstraintToSplit == NULL )
            {
                auto newSplit = new PiecewiseLinearCaseSplit();
                *newSplit = *split;
                newSplits.append( newSplit );
            }
            else
            {
                auto caseSplits = pLConstraintToSplit->getCaseSplits();
                for ( const auto &caseSplit : caseSplits )
                {
                    auto newSplit = new PiecewiseLinearCaseSplit();
                    *newSplit = caseSplit;
                    newSplit->addReluPhase( pLConstraintToSplit->getId(), caseSplit.getEquations().size() > 0);

                    for ( const auto &tightening : split->getBoundTightenings() )
                        newSplit->storeBoundTightening( tightening );

                    // Only store bounds for now. Storing Equation results in segfault
                    // some time for some reason.
                    //for ( const auto &equation : split->getEquations() )
                    //    newSplit->addEquation( equation );

		    for ( const auto &reluPhase : split->getReluPhases() )
			newSplit->addReluPhase( reluPhase.first(), reluPhase.second() );
		    
                    newSplits.append( newSplit );
                }
            }
            delete split;
        }
        splits = newSplits;
    }

    unsigned queryIdSuffix = 1; // For query id
    // Create a new subquery for each newly created input region
    for ( const auto &split : splits )
    {
        // Create a new query id
        String queryId;
        if ( queryIdPrefix == "" )
            queryId = queryIdPrefix + Stringf( "%u", queryIdSuffix++ );
        else
            queryId = queryIdPrefix + Stringf( "-%u", queryIdSuffix++ );

        // Construct the new subquery and add it to subqueries
        SubQuery *subQuery = new SubQuery;
        subQuery->_queryId = queryId;
        subQuery->_split.reset(split);
        subQuery->_timeoutInSeconds = timeoutInSeconds;
        subQueries.append( subQuery );
    }
}

PiecewiseLinearConstraint *ReluDivider::getPLConstraintToSplit
( const PiecewiseLinearCaseSplit &split )
{
    PiecewiseLinearConstraint *constraintToSplit = NULL;
    //if ( _engine.propagate() )
    constraintToSplit = computeBestChoice( split );
    return constraintToSplit;
}

PiecewiseLinearConstraint *ReluDivider::computeBestChoice( const PiecewiseLinearCaseSplit &split )
{
    PiecewiseLinearConstraint *best = NULL;
    double bestRank = _balanceEstimates.size() * 2;
    for ( const auto &entry : _runtimeEstimates ){
        if ( (!split.hasRelu( entry.first )) &&
             entry.second < _threshold )
        {
            double newRank = _balanceEstimates[entry.first];
            if ( newRank < bestRank )
            {
                best = _engine.getConstraintFromId( entry.first );
                bestRank = newRank;
            }
        }
    }
    return best;
}

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//
