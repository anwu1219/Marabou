/*********************                                                        */
/*! \file DnCMarabou.h
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

#ifndef __DnCMarabou_h__
#define __DnCMarabou_h__

#include "BiasStrategy.h"
#include "DivideStrategy.h"
#include "DnCManager.h"
#include "Options.h"
#include "InputQuery.h"

class DnCMarabou
{
public:
    DnCMarabou();

    /*
      Entry point of this class
    */
    void run();

private:
    std::shared_ptr<Engine> _baseEngine;

    std::unique_ptr<DnCManager> _dncManager;

    bool lookAheadPreprocessing( Map<unsigned, unsigned> &idToPhase, unsigned splitThreshold );

    InputQuery _inputQuery;

    /*
      Display the results
    */
    void displayResults( unsigned long long microSecondsElapsed ) const;

    /*
      Set the bias strategy according to the command line argument
    */
    BiasStrategy setBiasStrategyFromOptions( const String strategy );

    /*
      Set the divide strategy according to the command line argument
    */
    DivideStrategy setDivideStrategyFromOptions( const String strategy );
};

#endif // __DnCMarabou_h__

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//
