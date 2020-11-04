/*********************                                                        */
/*! \file BranchingHeuristics.cpp
** \verbatim
** Top contributors (to current version):
**   Ying Sheng, Haoze Wu
** This file is part of the Marabou project.
** Copyright (c) 2017-2019 by the authors listed in the file AUTHORS
** in the top-level source directory) and their institutional affiliations.
** All rights reserved. See the file COPYING in the top-level source
** directory for licensing information.\endverbatim
**
** [[ Add lengthier description here ]]

**/

#include "BranchingHeuristics.h"

void BranchingHeuristics::initialize( List<PiecewiseLinearConstraint *> &constraints )
{
    for ( const auto &constraint : constraints )
    {
        _constraintToScore[constraint] = 1;
        _scoreConstraintPairs.insert( std::make_pair( 1, constraint ) );
	_constraintToTempScore[constraint] = 0;
    }
}


void BranchingHeuristics::updateScore( PiecewiseLinearConstraint *constraint,
                                       double score )
{
    ASSERT( _constraintToScore.exists( constraint ) );
    _scoreConstraintPairs.erase( std::make_pair( _constraintToScore[constraint], constraint ) );
    _constraintToScore[constraint] = score;
    _scoreConstraintPairs.insert( std::make_pair( score, constraint ) );

}

void BranchingHeuristics::updateSpatial( PiecewiseLinearConstraint *child, PiecewiseLinearConstraint *parent, double numFixed )
{
    double oldValue = _constraintToScore[child];
    _constraintToTempScore[parent] += 0.5 * (oldValue * _decaySpatial + numFixed);
}

void BranchingHeuristics::updateTime( PiecewiseLinearConstraint *constraint )
{
    ASSERT( _constraintToScore.exists( constraint ) );
    _constraintToScore[constraint] = _constraintToScore[constraint] * _decayTime
	    + _constraintToTempScore[constraint] * (1 - _decayTime);
    _constraintToTempScore[constraint] = 0;
}

PiecewiseLinearConstraint *BranchingHeuristics::pickSplittingConstraint()
{
    ASSERT( !_constraintToScore.empty() );
    for ( auto &p : _scoreConstraintPairs )
    {
	PiecewiseLinearConstraint *plc = p.second;
        if (plc->isActive() && !plc->phaseFixed())
            return plc;
    }
    //Set<std::pair<double, PiecewiseLinearConstraint *>>::iterator it;
    //for (it = _scoreConstraintPairs.begin(); it != _scoreConstraintPairs.end(); ++it)
    //{
    //	PiecewiseLinearConstraint *plc = it->second;
    //    if (plc->isActive() && !plc->phaseFixed())
    //        return plc;
    // }
    return NULL;
}
