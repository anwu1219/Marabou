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

BranchingHeuristics::BranchingHeuristics( IEngine *engine )
    : _engine( engine )
{
    initialize();
}

void BranchingHeuristics::initialize()
{
    const List<PiecewiseLinearConstraint *> &constraints =
        _engine->getPiecewiseLinearConstraints();
    for ( const auto &constraint : constraints )
    {
        _constraintToScore[constraint] = 1;
        _scoreConstraintPairs.insert( std::make_pair( 1, constraint ) );
	_constraintToTempScore[constraint] = 0;
    }
}


void BranchingHeuristics::updateScore( const PiecewiseLinearConstraint *constraint,
                                       double score )
{
    ASSERT( _constraintToScore.exists( constraint ) );
    _constraintToScore[constraint] = score;
}

void BranchingHeuristics::updateSpatial( const PiecewiseLinearConstraint *child, const PiecewiseLinearConstraint *parent, double numFixed )
{
    double oldValue = _constraintToScore[child];
    _constraintToTempScore[parent] += 0.5 * (oldValue * _decaySpatial + numFixed);
}

void BranchingHeuristics::updateTime( const PiecewiseLinearConstraint *constraint )
{
    ASSERT( _constraintToScore.exist( constraint ) );
    _constraintToScore[constraint] = _constraintToScore[constraint] * _decayTime
	    + _constraintToTempScore[constraint] * (1 - _decayTime);
    _constraintToTempScore[constraint] = 0;
}

PiecewiseLinearConstraint *BranchingHeuristics::pickSplittingConstraint()
{
    ASSERT( !_constraintToScore.empty() );
    Set<std::pair<double, PiecewiseLinearConstraint *>>::iterator it;
    for (it = _scoreConstraintPairs.begin(); it != _scoreConstraintPairs.end(); ++it)
    {
	PiecewiseLinearConstraint *plc = it->second;
        if (plc->isActive() && !plc->phaseFixed())
            return plc;
    }
    return NULL;
}
