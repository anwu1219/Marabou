/*********************                                                        */
/*! \file BranchingHeuristics.h
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

#ifndef __BranchingHeuristics_h__
#define __BranchingHeuristics_h__

#include "IEngine.h"
#include "Map.h"
#include "Set.h"
#include "PiecewiseLinearConstraint.h"

class BranchingHeuristics
{
public:

    BranchingHeuristics( IEngine *engine );

    void initialize();

    void updateScore( const PiecewiseLinearConstraint *constraint, double score );

    void updateSpatial( const PiecewiseLinearConstraint *child, const PiecewiseLinearConstraint *parent, double numFixed );

    void updateTime( const PiecewiseLinearConstraint *constraint );

    PiecewiseLinearConstraint *pickSplittingConstraint();

private:

    const double _decayTime = 0.4;
    const double _decaySpatial = 0.8;

    /*
      Mapping from a PLConstraint to its score
    */
    Map<const PiecewiseLinearConstraint *, double> _constraintToScore;

    /*
      Mapping from a PLConstraint to its temporary score
    */
    Map<const PiecewiseLinearConstraint *, double> _constraintToTempScore;

    /*
      TreeSet of <score, constraint> pair
    */
    Set<std::pair<double, PiecewiseLinearConstraint *>> _scoreConstraintPairs;

    /*
      Engine
    */
    IEngine *_engine;

};

#endif // __BranchingHeuristics_h__
