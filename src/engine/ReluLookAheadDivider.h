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

#ifndef __ReluLookAheadDivider_h__
#define __ReluLookAheadDivider_h__

#include "Engine.h"
#include "List.h"
#include "PiecewiseLinearConstraint.h"
#include "QueryDivider.h"

#include <math.h>

class ReluLookAheadDivider : public QueryDivider
{
public:
    ReluLookAheadDivider( std::shared_ptr<Engine> engine );

    void createSubQueries( unsigned numNewSubQueries,
                           const String queryIdPrefix,
                           const PiecewiseLinearCaseSplit
                           &previousSplit,
                           const unsigned timeoutInSeconds,
                           SubQueries &subQueries );

    /*
      Returns the variable with the largest range
    */
    PiecewiseLinearConstraint *getPLConstraintToSplit( const
                                                       PiecewiseLinearCaseSplit
                                                       &split );

    PiecewiseLinearConstraint *computeBestChoice();

private:
    /*
      The engine
    */
    std::shared_ptr<Engine> _engine;
};

#endif // __ReluLookAheadDivider_h__

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//
