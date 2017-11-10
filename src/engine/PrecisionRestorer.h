/*********************                                                        */
/*! \file PrecisionRestorer.h
** \verbatim
** Top contributors (to current version):
**   Guy Katz
** This file is part of the Marabou project.
** Copyright (c) 2016-2017 by the authors listed in the file AUTHORS
** in the top-level source directory) and their institutional affiliations.
** All rights reserved. See the file COPYING in the top-level source
** directory for licensing information.\endverbatim
**/

#ifndef __PrecisionRestorer_h__
#define __PrecisionRestorer_h__

#include "EngineState.h"

class PrecisionRestorer
{
public:
    void storeInitialEngineState( const IEngine &engine );

    void restorePrecision( IEngine &engine, ITableau &tableau );

private:
    EngineState _initialEngineState;
};

#endif // __PrecisionRestorer_h__

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//