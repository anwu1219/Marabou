/*********************                                                        */
/*! \file LookAheadPreprocessor.cpp
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

#include "GetCPUData.h"
#include <InputQuery.h>
#include <LookAheadPreprocessor.h>
#include "GlobalConfiguration.h"

#include <thread>

void LookAheadPreprocessor::preprocessWorker( LookAheadPreprocessor::WorkerQueue
                                              *workload, Engine *engine,
                                              InputQuery *inputQuery,
                                              unsigned threadId,
                                              Map<unsigned, unsigned>
                                              &idToPhase,
                                              std::atomic_bool
                                              &shouldQuitPreprocessing,
                                              std::mutex &mtx,
                                              std::atomic_int &lastFixed )
{
    //unsigned cpuId = 0;
    //getCPUId( cpuId );
    //printf( "Thread #%u on CPU %u\n", threadId, cpuId );

    if ( !engine->_processed )
    {
        engine->processInputQuery( *inputQuery, false );
    }

    // Apply all splits
    engine->applySplits( idToPhase );
    do
    {
        engine->performSymbolicBoundTightening();
    } while ( engine->applyAllValidConstraintCaseSplits() );

    mtx.lock();
    for ( const auto &entry : engine->_smtCore._impliedIdToPhaseAtRoot )
        idToPhase[entry.first] = entry.second;
    mtx.unlock();

    unsigned prevSize = idToPhase.size();

    unsigned numPlConstraints = engine->getInputQuery()->getPiecewiseLinearConstraints().size();
    // Repeatedly pop from queue
    while ( !workload->empty() )
    {
        unsigned id = 0;
        workload->pop( id );

        std::cout << id << " " << idToPhase.size() << std::endl;

        if ( (int) id == lastFixed.load() )
        {
            std::cout << "No new info for subsequent constraints!" << std::endl;
            shouldQuitPreprocessing = true;
            return;
        }

        // Sync up
        if ( idToPhase.size() > prevSize )
        {
            prevSize = idToPhase.size();
            engine->applySplits( idToPhase );
        }
        PiecewiseLinearConstraint *plConstraint = engine->
            getConstraintFromId( id );

        if ( (!plConstraint->isActive()) || plConstraint->phaseFixed() )
            continue;

        engine->storeInitialEngineState();

        // Try to propagate
        auto caseSplits = plConstraint->getCaseSplits();

        EngineState *stateBeforeSplit = new EngineState();
        engine->storeState( *stateBeforeSplit, true );

        Map<unsigned, unsigned> commonImpliedIdToPhase;
        Map<unsigned, unsigned> idToCount;
        Vector<List<PiecewiseLinearCaseSplit>> feasibleImpliedSplits;
        Vector<Map<unsigned, unsigned>> feasibleImpliedIdToPhase;
        Vector<unsigned> feasibleStatus;
        for ( const auto &caseSplit : caseSplits )
        {
            engine->applySplit( caseSplit );

            unsigned depthThreshold = (numPlConstraints - id) /
                GlobalConfiguration::QUICK_SOLVE_STACK_DEPTH_THRESHOLD;
            if ( depthThreshold > 0 )
                engine->quickSolve( depthThreshold );

            // print stats
            //engine->_statistics.print();

            if ( engine->_exitCode == IEngine::QUIT_REQUESTED )
                {
                return;
                }
            if ( engine->_exitCode == IEngine::ERROR )
                return;
            if ( engine->_exitCode != IEngine::UNSAT )
            {
                List<PiecewiseLinearCaseSplit> temp = engine->
                    _smtCore._impliedValidSplitsAtRoot;
                feasibleImpliedSplits.append( temp );

                Map<unsigned, unsigned> tempMap = engine->
                    _smtCore._impliedIdToPhaseAtRoot;
                feasibleImpliedIdToPhase.append( tempMap );

                for ( const auto &entry : tempMap )
                {
                    if ( !commonImpliedIdToPhase.exists( entry.first ) )
                    {
                        commonImpliedIdToPhase[entry.first] = entry.second;
                        idToCount[entry.first] = 0;
                    }
                    if ( commonImpliedIdToPhase[entry.first] == entry.second )
                        idToCount[entry.first] += 1;
                }
                feasibleStatus.append( ( ( ReluConstraint * ) plConstraint )
                                       ->getPhaseStatus() );
            }
            engine->reset();
            engine->restoreState( *stateBeforeSplit );
        }
        if ( feasibleImpliedSplits.size() == 0 )
        {
            engine->_exitCode = IEngine::UNSAT;
            std::cout << "UNSAT! Finished preprocessing early!" << std::endl;
            lastFixed = -2;
            shouldQuitPreprocessing = true;
            return;
        }
        else if ( feasibleImpliedSplits.size() == 1 )
        {
            printf("Thread %u fixed relu %u\n", threadId, plConstraint->getId() );
            lastFixed = plConstraint->getId();
            engine->applySplits( feasibleImpliedIdToPhase[0] );
            mtx.lock();
            for ( const auto &entry : feasibleImpliedIdToPhase[0] )
                idToPhase[entry.first] = entry.second;
            prevSize = idToPhase.size();
            mtx.unlock();
        }
        else
        {
            unsigned commonCount = 0;
            for ( const auto &entry : commonImpliedIdToPhase )
            {
                if ( idToCount[entry.first] == caseSplits.size() )
                {
                    mtx.lock();
                    idToPhase[entry.first] = entry.second;
                    prevSize = idToPhase.size();
                    mtx.unlock();
                    commonCount += 1;
                }
            }
        }
        engine->applyAllValidConstraintCaseSplits();
    }
}

LookAheadPreprocessor::LookAheadPreprocessor( unsigned numWorkers,
                                              const InputQuery &inputQuery )
    : _numWorkers ( numWorkers )
    , _baseInputQuery( inputQuery )
{
    createEngines();
    _workload = new LookAheadPreprocessor::WorkerQueue( 0 );
}

bool LookAheadPreprocessor::run( Map<unsigned, unsigned> &idToPhase )
{
    bool progressMade = true;
    //Vector<Map<unsigned, unsigned>> allIdToPhase;

    // Prepare the mechanism through which we can ask the engines to quit
    List<std::atomic_bool *> quitThreads;
    for ( unsigned i = 0; i < _numWorkers; ++i )
        quitThreads.append( _engines[i]->getQuitRequested() );

    std::atomic_bool shouldQuitPreprocessing( false );

    for ( const auto plConstraint : _baseInputQuery.getPiecewiseLinearConstraints() )
        _allPiecewiseLinearConstraints.append( plConstraint->getId() );

    std::mutex mtx;
    std::atomic_int lastFixed ( -1 );

    while ( progressMade )
    {
        std::cout << "new look ahead preprocessing iteration" << std::endl;
        unsigned previousSize = idToPhase.size();
        for ( auto id : _allPiecewiseLinearConstraints )
            if ( !_workload->push( id ) )
                std::cout << "Pushed failed!" << std::endl;
        // Spawn threads and start solving
        std::list<std::thread> threads;
        for ( unsigned threadId = 0; threadId < _numWorkers; ++threadId )
        {
            threads.push_back( std::thread( preprocessWorker, _workload,
                                            _engines[threadId],
                                            _inputQueries[threadId],
                                            threadId,
                                            std::ref( idToPhase ),
                                            std::ref( shouldQuitPreprocessing ),
                                            std::ref( mtx ),
                                            std::ref( lastFixed ) ) );
        }

        while ( (!shouldQuitPreprocessing.load()) && (!_workload->empty()) )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        }

        if ( shouldQuitPreprocessing.load() )
            for ( auto &quitThread : quitThreads )
                *quitThread =true;

        for ( auto &thread : threads )
            thread.join();

        if ( shouldQuitPreprocessing.load() )
        {
            std::cout << "Preprocessing done!" << std::endl;
            std::cout << "Number of fixed Relus: " << idToPhase.size() << std::endl;
            return lastFixed.load() != -2;
        }

        if ( idToPhase.size() > previousSize && lastFixed.load() != -1 )
        {
            progressMade = true;
        }
        else
            progressMade = false;
        std::cout << "Number of fixed Relus: " << idToPhase.size() << std::endl;
    }
    std::cout << "Preprocessing done!" << std::endl;
    std::cout << "Number of fixed Relus: " << idToPhase.size() << std::endl;
    return true;
}

void LookAheadPreprocessor::createEngines()
{
    // Create engines for each thread
    for ( unsigned i = 0; i < _numWorkers; ++i )
    {
        Engine *engine = new Engine( 0 );
        InputQuery *inputQuery = new InputQuery();
        *inputQuery = _baseInputQuery;
        _engines.append( engine );
        _inputQueries.append( inputQuery );

    }
}

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//
