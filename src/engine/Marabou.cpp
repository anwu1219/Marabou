/*********************                                                        */
/*! \file Marabou.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Guy Katz
 ** This file is part of the Marabou project.
 ** Copyright (c) 2017-2019 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved. See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** \brief [[ Add one-line brief description here ]]
 **
 ** [[ Add lengthier description here ]]
 **/

#include "AcasParser.h"
#include "File.h"
#include "MStringf.h"
#include "LookAheadPreprocessor.h"
#include "Marabou.h"
#include "Options.h"
#include "PropertyParser.h"
#include "MarabouError.h"

#ifdef _WIN32
#undef ERROR
#endif

Marabou::Marabou( unsigned verbosity )
    : _acasParser( NULL )
    , _engine( verbosity )
{
}

Marabou::~Marabou()
{
    if ( _acasParser )
    {
        delete _acasParser;
        _acasParser = NULL;
    }
}

void Marabou::run()
{
    struct timespec start = TimeUtils::sampleMicro();

    prepareInputQuery();
    unsigned long long preprocessTime = solveQuery();

    struct timespec end = TimeUtils::sampleMicro();

    unsigned long long totalElapsed = TimeUtils::timePassed( start, end );
    displayResults( preprocessTime, totalElapsed );
}

void Marabou::prepareInputQuery()
{
    /*
      Step 1: extract the network
    */
    String networkFilePath = Options::get()->getString( Options::INPUT_FILE_PATH );
    if ( !File::exists( networkFilePath ) )
    {
        printf( "Error: the specified network file (%s) doesn't exist!\n", networkFilePath.ascii() );
        throw MarabouError( MarabouError::FILE_DOESNT_EXIST, networkFilePath.ascii() );
    }
    printf( "Network: %s\n", networkFilePath.ascii() );

    // For now, assume the network is given in ACAS format
    _acasParser = new AcasParser( networkFilePath );
    _acasParser->generateQuery( _inputQuery );

    /*
      Step 2: extract the property in question
    */
    String propertyFilePath = Options::get()->getString( Options::PROPERTY_FILE_PATH );
    if ( propertyFilePath != "" )
    {
        printf( "Property: %s\n", propertyFilePath.ascii() );
        PropertyParser().parse( propertyFilePath, _inputQuery );
    }
    else
        printf( "Property: None\n" );

    printf( "\n" );
}

unsigned long long Marabou::solveQuery()
{
    unsigned long long totalElapsed = 0;
    if ( _engine.processInputQuery( _inputQuery ))
    {
        Map<unsigned, unsigned> idToPhase;
        if ( Options::get()->getBool( Options::LOOK_AHEAD_PREPROCESSING ) )
        {
            struct timespec start = TimeUtils::sampleMicro();
            auto lookAheadPreprocessor = new LookAheadPreprocessor
                ( Options::get()->getInt( Options::NUM_WORKERS ),
                  *_engine.getInputQuery() );
            bool feasible = lookAheadPreprocessor->run( idToPhase );
            struct timespec end = TimeUtils::sampleMicro();
            totalElapsed = TimeUtils::timePassed( start, end );
            if ( feasible )
                _engine.applySplits( idToPhase );
            else
            {
                // Solved by preprocessing, we are done!
                _engine._exitCode = Engine::UNSAT;
                return totalElapsed;
            }
        }
        _engine.solve( Options::get()->getInt( Options::TIMEOUT ) );
    }
    if ( _engine.getExitCode() == Engine::SAT )
        _engine.extractSolution( _inputQuery );
    return totalElapsed;
}

void Marabou::displayResults( unsigned long long preprocessTime,
                              unsigned long long microSecondsElapsed ) const
{
    Engine::ExitCode result = _engine.getExitCode();
    String resultString;

    if ( result == Engine::UNSAT )
    {
        resultString = "UNSAT";
        printf( "UNSAT\n" );
    }
    else if ( result == Engine::SAT )
    {
        resultString = "SAT";
        printf( "SAT\n" );

        printf( "Input assignment:\n" );
        for ( unsigned i = 0; i < _inputQuery.getNumInputVariables(); ++i )
            printf( "\tx%u = %lf\n", i, _inputQuery.getSolutionValue( _inputQuery.inputVariableByIndex( i ) ) );

        printf( "\n" );
        printf( "Output:\n" );
        for ( unsigned i = 0; i < _inputQuery.getNumOutputVariables(); ++i )
            printf( "\ty%u = %lf\n", i, _inputQuery.getSolutionValue( _inputQuery.outputVariableByIndex( i ) ) );
        printf( "\n" );
    }
    else if ( result == Engine::TIMEOUT )
    {
        resultString = "TIMEOUT";
        printf( "Timeout\n" );
    }
    else if ( result == Engine::ERROR )
    {
        resultString = "ERROR";
        printf( "Error\n" );
    }
    else
    {
        resultString = "UNKNOWN";
        printf( "UNKNOWN EXIT CODE! (this should not happen)" );
    }

    // Create a summary file, if requested
    String summaryFilePath = Options::get()->getString( Options::SUMMARY_FILE );
    if ( summaryFilePath != "" )
    {
        File summaryFile( summaryFilePath );
        summaryFile.open( File::MODE_WRITE_TRUNCATE );

        // Field #1: result
        summaryFile.write( resultString );

        // Field #2: total elapsed time
        summaryFile.write( Stringf( " %u ", microSecondsElapsed / 1000000 ) ); // In seconds

        // Field #2: total preprocess time
        summaryFile.write( Stringf( "%u ", preprocessTime / 1000000 ) ); // In seconds

        // Field #3: number of visited tree states
        summaryFile.write( Stringf( "%u ",
                                    _engine.getStatistics()->getNumVisitedTreeStates() ) );

        // Field #4: average pivot time in micro seconds
        summaryFile.write( Stringf( "%u",
                                    _engine.getStatistics()->getAveragePivotTimeInMicro() ) );

        summaryFile.write( "\n" );
    }
}

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//
