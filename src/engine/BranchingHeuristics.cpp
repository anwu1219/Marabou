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
}

void BranchingHeuristics::initialize()
{
    const List<PiecewiseLinearConstraint *> &constraints =
        _engine->getPiecewiseLinearConstraints();
    for ( const auto &constraint : constraints )
    {
        _constraintToScore[constraint] = 1;
        _scoreConstraintPairs.insert( std::make_pair( 1, constraint ) );
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

PiecewiseLinearConstraint *BranchingHeuristics::pickMaxScore()
{
    ASSERT( !_constraintToScore.empty() );
    return _scoreConstraintPairs.begin()->second;
}
