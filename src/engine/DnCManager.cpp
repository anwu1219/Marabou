/*********************                                                        */
/*! \file DnCManager.cpp
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
#include "DivideStrategy.h"
#include "DnCManager.h"
#include "DnCWorker.h"
#include "GetCPUData.h"
#include "LargestIntervalDivider.h"
#include "MStringf.h"
#include "MarabouError.h"
#include "PiecewiseLinearCaseSplit.h"
#include "QueryDivider.h"
#include "ReluDivider.h"
#include "TimeUtils.h"
#include "Vector.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <fstream>

#include "util/util.hh"

#include "Map.h"

void DnCManager::dncSolve( WorkerQueue *workload, InputQuery *inputQuery,
                           std::shared_ptr<Engine> engine,
                           std::atomic_uint &numUnsolvedSubQueries,
                           std::atomic_bool &shouldQuitSolving,
                           unsigned threadId, unsigned onlineDivides,
                           float timeoutFactor, DivideStrategy divideStrategy,
                           bool restoreTreeStates, Map<unsigned, unsigned>
                           idToPhase, unsigned biasedLayer,
                           BiasStrategy biasStrategy, unsigned maxDepth )
{
    unsigned cpuId = 0;
    getCPUId( cpuId );
    log( Stringf( "Thread #%u on CPU %u", threadId, cpuId ) );

    engine->processInputQuery( *inputQuery, false );
    engine->applySplits( idToPhase );
    engine->propagate();
    engine->setBiasedPhases( biasedLayer, biasStrategy );
    engine->numberOfActive();

    DnCWorker worker( workload, engine, std::ref( numUnsolvedSubQueries ),
                      std::ref( shouldQuitSolving ), threadId, onlineDivides,
                      timeoutFactor, divideStrategy, maxDepth );
    while ( !shouldQuitSolving.load() )
    {
        worker.popOneSubQueryAndSolve( restoreTreeStates, biasedLayer,
                                       biasStrategy );
    }
}

DnCManager::DnCManager( unsigned numWorkers, unsigned initialDivides,
                        unsigned initialTimeout, unsigned onlineDivides,
                        float timeoutFactor, DivideStrategy divideStrategy,
                        InputQuery *inputQuery, unsigned verbosity,
                        Map<unsigned, unsigned> idToPhase, unsigned biasedLayer,
                        BiasStrategy biasStrategy, unsigned maxDepth )
    : _exitCode( DnCManager::NOT_DONE )
    , _numWorkers( numWorkers )
    , _initialDivides( initialDivides )
    , _initialTimeout( initialTimeout )
    , _onlineDivides( onlineDivides )
    , _timeoutFactor( timeoutFactor )
    , _divideStrategy( divideStrategy )
    , _baseInputQuery( inputQuery )
    , _timeoutReached( false )
    , _numUnsolvedSubQueries( 0 )
    , _verbosity( verbosity )
    , _idToPhase( idToPhase )
    , _biasedLayer( biasedLayer )
    , _biasStrategy( biasStrategy )
    , _maxDepth( maxDepth )
{
}

DnCManager::~DnCManager()
{
    freeMemoryIfNeeded();
}

void DnCManager::freeMemoryIfNeeded()
{
    if ( _workload )
    {
        SubQuery *subQuery;
        while ( !_workload->empty() )
        {
            _workload->pop( subQuery );
            delete subQuery;
        }
    }
    delete _workload;
}

void DnCManager::solve( unsigned timeoutInSeconds, bool restoreTreeStates )
{
    enum {
        MICROSECONDS_IN_SECOND = 1000000
    };

    unsigned long long timeoutInMicroSeconds = timeoutInSeconds * MICROSECONDS_IN_SECOND;
    struct timespec startTime = TimeUtils::sampleMicro();

    // Preprocess the input query and create an engine for each of the threads
    if ( !createEngines() )
    {
        _exitCode = DnCManager::UNSAT;
        return;
    }

    // Prepare the mechanism through which we can ask the engines to quit
    List<std::atomic_bool *> quitThreads;
    for ( unsigned i = 0; i < _numWorkers; ++i )
        quitThreads.append( _engines[i]->getQuitRequested() );

    // Partition the input query into initial subqueries, and place these
    // queries in the queue
    SubQueries subQueries;
    initialDivide( subQueries );

    // Create objects shared across workers
    _numUnsolvedSubQueries = subQueries.size();
    std::atomic_bool shouldQuitSolving( false );

    _workload = new WorkerQueue( 0 );
    for ( auto &subQuery : subQueries )
    {
        if ( !_workload->push( subQuery ) )
        {
            // This should never happen
            ASSERT( false );
        }
    }

    // Spawn threads and start solving
    std::list<std::thread> threads;
    for ( unsigned threadId = 0; threadId < _numWorkers; ++threadId )
    {
        InputQuery *inputQuery = new InputQuery();
        *inputQuery = *_baseInputQuery;
        threads.push_back( std::thread( dncSolve, _workload,
                                        inputQuery, _engines[ threadId ],
                                        std::ref( _numUnsolvedSubQueries ),
                                        std::ref( shouldQuitSolving ),
                                        threadId, _onlineDivides,
                                        _timeoutFactor, _divideStrategy,
                                        restoreTreeStates, _idToPhase,
                                        _biasedLayer, _biasStrategy,
                                        _maxDepth ) );
    }

    // Wait until either all subQueries are solved or a satisfying assignment is
    // found by some worker
    while ( !shouldQuitSolving.load() )
    {
        updateTimeoutReached( startTime, timeoutInMicroSeconds );
        if ( _timeoutReached )
            shouldQuitSolving = true;
        else
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }


    // Now that we are done, tell all workers to quit
    for ( auto &quitThread : quitThreads )
        *quitThread = true;

    for ( auto &thread : threads )
        thread.join();

    updateDnCExitCode();
    return;
}

DnCManager::DnCExitCode DnCManager::getExitCode() const
{
    return _exitCode;
}

void DnCManager::updateDnCExitCode()
{
    bool hasSat = false;
    bool hasError = false;
    bool hasQuitRequested = false;
    for ( auto &engine : _engines )
    {
        Engine::ExitCode result = engine->getExitCode();
        if ( result == Engine::SAT )
        {
            _engineWithSATAssignment = engine;
            hasSat = true;
            break;
        }
        else if ( result == Engine::ERROR )
            hasError = true;
        else if ( result == Engine::QUIT_REQUESTED )
            hasQuitRequested = true;
    }
    if ( hasSat )
        _exitCode = DnCManager::SAT;
    else if ( _timeoutReached )
        _exitCode = DnCManager::TIMEOUT;
    else if ( hasQuitRequested )
        _exitCode = DnCManager::QUIT_REQUESTED;
    else if ( hasError )
        _exitCode = DnCManager::ERROR;
    else if ( _numUnsolvedSubQueries.load() == 0 )
        _exitCode = DnCManager::UNSAT;
    else
    {
        ASSERT( false ); // This should never happen
        _exitCode = DnCManager::NOT_DONE;
    }
}

String DnCManager::getResultString()
{
    switch ( _exitCode )
    {
    case DnCManager::SAT:
        return "SAT";
    case DnCManager::UNSAT:
        return "UNSAT";
    case DnCManager::ERROR:
        return "ERROR";
    case DnCManager::NOT_DONE:
        return "NOT_DONE";
    case DnCManager::QUIT_REQUESTED:
        return "QUIT_REQUESTED";
    case DnCManager::TIMEOUT:
        return "TIMEOUT";
    default:
        ASSERT( false );
        return "";
    }
}

void DnCManager::getSolution( std::map<int, double> &ret )
{
    ASSERT( _engineWithSATAssignment != nullptr );

    InputQuery *inputQuery = _engineWithSATAssignment->getInputQuery();
    _engineWithSATAssignment->extractSolution( *( inputQuery ) );

    for ( unsigned i = 0; i < inputQuery->getNumberOfVariables(); ++i )
        ret[i] = inputQuery->getSolutionValue( i );

    return;
}

void DnCManager::printResult()
{
    std::cout << std::endl;
    switch ( _exitCode )
    {
    case DnCManager::SAT:
    {
        std::cout << "SAT\n" << std::endl;

        ASSERT( _engineWithSATAssignment != nullptr );

        InputQuery *inputQuery = _engineWithSATAssignment->getInputQuery();
        _engineWithSATAssignment->extractSolution( *( inputQuery ) );

        Vector<double> inputVector( inputQuery->getNumInputVariables() );
        Vector<double> outputVector( inputQuery->getNumOutputVariables() );
        double *inputs( inputVector.data() );
        double *outputs( outputVector.data() );

        printf( "Input assignment:\n" );
        for ( unsigned i = 0; i < inputQuery->getNumInputVariables(); ++i )
        {
            printf( "\tx%u = %lf\n", i, inputQuery->getSolutionValue( inputQuery->inputVariableByIndex( i ) ) );
            inputs[i] = inputQuery->getSolutionValue( inputQuery->inputVariableByIndex( i ) );
        }

        NetworkLevelReasoner *nlr = inputQuery->getNetworkLevelReasoner();
        if ( nlr )
            nlr->evaluate( inputs, outputs );

        printf( "\n" );
        printf( "Output:\n" );
        for ( unsigned i = 0; i < inputQuery->getNumOutputVariables(); ++i )
        {
            if ( nlr )
                printf( "\tnlr y%u = %lf\n", i, outputs[i] );
            else
                printf( "\ty%u = %lf\n", i, inputQuery->getSolutionValue( inputQuery->outputVariableByIndex( i ) ) );            
        }
        printf( "\n" );
        break;
    }
    case DnCManager::UNSAT:
        std::cout << "UNSAT" << std::endl;
        break;
    case DnCManager::ERROR:
        std::cout << "ERROR" << std::endl;
        break;
    case DnCManager::NOT_DONE:
        std::cout << "NOT_DONE" << std::endl;
        break;
    case DnCManager::QUIT_REQUESTED:
        std::cout << "QUIT_REQUESTED" << std::endl;
        break;
    case DnCManager::TIMEOUT:
        std::cout << "TIMEOUT" << std::endl;
        break;
    default:
        ASSERT( false );
    }
}

bool DnCManager::createEngines()
{
    // Create the base engine
    _baseEngine = std::make_shared<Engine>( _verbosity );
    _baseEngine->processInputQuery( *_baseInputQuery, false );
    _baseEngine->applySplits( _idToPhase );

    // Create engines for each thread
    for ( unsigned i = 0; i < _numWorkers; ++i )
    {
        auto engine = std::make_shared<Engine>( _verbosity );
        _engines.append( engine );
    }

    return true;
}

void DnCManager::initialDivide( SubQueries &subQueries )
{
    String queryId = "";
    auto split = std::unique_ptr<PiecewiseLinearCaseSplit>
        ( new PiecewiseLinearCaseSplit() );

    const List<unsigned> inputVariables( _baseEngine->getInputVariables() );
    std::unique_ptr<QueryDivider> queryDivider = nullptr;
    if ( _divideStrategy == DivideStrategy::LargestInterval )
    {
        queryDivider = std::unique_ptr<QueryDivider>
            ( new LargestIntervalDivider( inputVariables ) );

        // If we are dividing the input region, we add all the original bounds
        QueryDivider::InputRegion initialRegion;
        InputQuery *inputQuery = _baseEngine->getInputQuery();
        for ( const auto &variable : inputVariables )
        {
            initialRegion._lowerBounds[variable] =
                inputQuery->getLowerBounds()[variable];
            initialRegion._upperBounds[variable] =
                inputQuery->getUpperBounds()[variable];
        }

        // Add bound as equations for each input variable
        for ( const auto &variable : inputVariables )
        {
            double lb = initialRegion._lowerBounds[variable];
            double ub = initialRegion._upperBounds[variable];
            split->storeBoundTightening( Tightening( variable, lb,
                                                     Tightening::LB ) );
            split->storeBoundTightening( Tightening( variable, ub,
                                                     Tightening::UB ) );
        }
    }
    else
    {
        // Default
        queryDivider = std::unique_ptr<QueryDivider>
            ( new ReluDivider( *_baseEngine ) );
    }

    queryDivider->createSubQueries( pow( 2, _initialDivides ), queryId,
                                    *split, _initialTimeout, subQueries );
}

void DnCManager::updateTimeoutReached( timespec startTime, unsigned long long
                                       timeoutInMicroSeconds )
{
    if ( timeoutInMicroSeconds == 0 )
        return;
    struct timespec now = TimeUtils::sampleMicro();
    _timeoutReached = TimeUtils::timePassed( startTime, now ) >=
        timeoutInMicroSeconds;
}

void DnCManager::log( const String &message )
{
    if ( GlobalConfiguration::DNC_MANAGER_LOGGING )
        printf( "DnCManager: %s\n", message.ascii() );
}

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//
