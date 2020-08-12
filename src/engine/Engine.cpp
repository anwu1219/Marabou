/*********************                                                        */
/*! \file Engine.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Guy Katz, Duligur Ibeling, Andrew Wu
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

#include "AutoConstraintMatrixAnalyzer.h"
#include "Debug.h"
#include "Engine.h"
#include "EngineState.h"
#include "InfeasibleQueryException.h"
#include "InputQuery.h"
#include "MStringf.h"
#include "MalformedBasisException.h"
#include "MarabouError.h"
#include "Options.h"
#include "PiecewiseLinearConstraint.h"
#include "Preprocessor.h"
#include "TableauRow.h"
#include "TimeUtils.h"

Engine::Engine( unsigned verbosity )
    : _rowBoundTightener( *_tableau )
    , _smtCore( this )
    , _numPlConstraintsDisabledByValidSplits( 0 )
    , _preprocessingEnabled( false )
    , _initialStateStored( false )
    , _work( NULL )
    , _basisRestorationRequired( Engine::RESTORATION_NOT_NEEDED )
    , _basisRestorationPerformed( Engine::NO_RESTORATION_PERFORMED )
    , _costFunctionManager( _tableau )
    , _quitRequested( false )
    , _exitCode( Engine::NOT_DONE )
    , _constraintBoundTightener( *_tableau )
    , _numVisitedStatesAtPreviousRestoration( 0 )
    , _networkLevelReasoner( NULL )
    , _verbosity( verbosity )
    , _lastNumVisitedStates( 0 )
    , _lastIterationWithProgress( 0 )
{
    _smtCore.setStatistics( &_statistics );
    _tableau->setStatistics( &_statistics );
    _rowBoundTightener->setStatistics( &_statistics );
    _constraintBoundTightener->setStatistics( &_statistics );
    _preprocessor.setStatistics( &_statistics );

    _activeEntryStrategy = _projectedSteepestEdgeRule;
    _activeEntryStrategy->setStatistics( &_statistics );

    _statistics.stampStartingTime();
}

Engine::~Engine()
{
    if ( _work )
    {
        delete[] _work;
        _work = NULL;
    }
}

void Engine::setVerbosity( unsigned verbosity )
{
    _verbosity = verbosity;
}

void Engine::adjustWorkMemorySize()
{
    if ( _work )
    {
        delete[] _work;
        _work = NULL;
    }

    _work = new double[_tableau->getM()];
    if ( !_work )
        throw MarabouError( MarabouError::ALLOCATION_FAILED, "Engine::work" );
}

bool Engine::solve( unsigned timeoutInSeconds )
{
    printf("Optimize: %d\n", _costFunctionManager->getOptimize());
    if (_costFunctionManager->getOptimize())
    {
        return optimize(timeoutInSeconds);
    }


    SignalHandler::getInstance()->initialize();
    SignalHandler::getInstance()->registerClient( this );

    storeInitialEngineState();

    printf("Engine::Solving SAT Problem");

    if ( _verbosity > 0 )
    {
        printf( "\nEngine::solve: Initial statistics\n" );
        mainLoopStatistics();
        printf( "\n---\n" );
    }

    struct timespec mainLoopStart = TimeUtils::sampleMicro();
    while ( true )
    {
        struct timespec mainLoopEnd = TimeUtils::sampleMicro();
        _statistics.addTimeMainLoop( TimeUtils::timePassed( mainLoopStart, mainLoopEnd ) );
        mainLoopStart = mainLoopEnd;

        if ( shouldExitDueToTimeout( timeoutInSeconds ) )
        {
            if ( _verbosity > 0 )
            {
                printf( "\n\nEngine: quitting due to timeout...\n\n" );
                printf( "Final statistics:\n" );
                _statistics.print();
            }

            _exitCode = Engine::TIMEOUT;
            _statistics.timeout();
            return false;
        }

        if ( _quitRequested )
        {
            if ( _verbosity > 0 )
            {
                printf( "\n\nEngine: quitting due to external request...\n\n" );
                printf( "Final statistics:\n" );
                _statistics.print();
            }

            _exitCode = Engine::QUIT_REQUESTED;
            return false;
        }

        try
        {
            DEBUG( _tableau->verifyInvariants() );

            if ( _verbosity > 1 )
                mainLoopStatistics();

            // Check whether progress has been made recently
            checkOverallProgress();

            // If the basis has become malformed, we need to restore it
            if ( basisRestorationNeeded() )
            {
                if ( _basisRestorationRequired == Engine::STRONG_RESTORATION_NEEDED )
                {
                    performPrecisionRestoration( PrecisionRestorer::RESTORE_BASICS );
                    _basisRestorationPerformed = Engine::PERFORMED_STRONG_RESTORATION;
                }
                else
                {
                    performPrecisionRestoration( PrecisionRestorer::DO_NOT_RESTORE_BASICS );
                    _basisRestorationPerformed = Engine::PERFORMED_WEAK_RESTORATION;
                }

                _numVisitedStatesAtPreviousRestoration = _statistics.getNumVisitedTreeStates();
                _basisRestorationRequired = Engine::RESTORATION_NOT_NEEDED;
                continue;
            }

            // Restoration is not required
            _basisRestorationPerformed = Engine::NO_RESTORATION_PERFORMED;

            // Possible restoration due to preceision degradation
            if ( shouldCheckDegradation() && highDegradation() )
            {
                performPrecisionRestoration( PrecisionRestorer::RESTORE_BASICS );
                continue;
            }

            if ( _tableau->basisMatrixAvailable() )
                explicitBasisBoundTightening();

            // Perform any SmtCore-initiated case splits
            if ( _smtCore.needToSplit() )
            {
                _smtCore.performSplit();

                do
                {
                    performSymbolicBoundTightening();
                }
                while ( applyAllValidConstraintCaseSplits() );
                continue;
            }

            if ( !_tableau->allBoundsValid() )
            {
                // Some variable bounds are invalid, so the query is unsat
                throw InfeasibleQueryException();
            }

            if ( allVarsWithinBounds() )
            {
                // The linear portion of the problem has been solved.
                // Check the status of the PL constraints
                collectViolatedPlConstraints();
                // If all constraints are satisfied, we are possibly done
                if ( allPlConstraintsHold() )
                {
                    if ( _tableau->getBasicAssignmentStatus() !=
                         ITableau::BASIC_ASSIGNMENT_JUST_COMPUTED )
                    {
                        if ( _verbosity > 0 )
                        {
                            printf( "Before declaring SAT, recomputing...\n" );
                        }
                        // Make sure that the assignment is precise before declaring success
                        _tableau->computeAssignment();
                        continue;
                    }
                    if ( _verbosity > 0 )
                    {
                        printf( "\nEngine::solve: SAT assignment found\n" );
                        _statistics.print();
                    }
                    _exitCode = Engine::SAT;
                    return true;
                }

                // We have violated piecewise-linear constraints.
                performConstraintFixingStep();

                // Finally, take this opporunity to tighten any bounds
                // and perform any valid case splits.
                tightenBoundsOnConstraintMatrix();
                applyAllBoundTightenings();
                // For debugging purposes
                checkBoundCompliancyWithDebugSolution();

                while ( applyAllValidConstraintCaseSplits() )
                    performSymbolicBoundTightening();

                continue;
            }

            // We have out-of-bounds variables.
            performSimplexStep();

            continue;
        }
        catch ( const MalformedBasisException & )
        {
            // Debug
            printf( "MalformedBasisException caught!\n" );
            //

            if ( _basisRestorationPerformed == Engine::NO_RESTORATION_PERFORMED )
            {
                if ( _numVisitedStatesAtPreviousRestoration != _statistics.getNumVisitedTreeStates() )
                {
                    // We've tried a strong restoration before, and it didn't work. Do a weak restoration
                    _basisRestorationRequired = Engine::WEAK_RESTORATION_NEEDED;
                }
                else
                {
                    _basisRestorationRequired = Engine::STRONG_RESTORATION_NEEDED;
                }
            }
            else if ( _basisRestorationPerformed == Engine::PERFORMED_STRONG_RESTORATION )
                _basisRestorationRequired = Engine::WEAK_RESTORATION_NEEDED;
            else
            {
                printf( "Engine: Cannot restore tableau!\n" );
                _exitCode = Engine::ERROR;
                return false;
            }
        }
        catch ( const InfeasibleQueryException & )
        {
            // The current query is unsat, and we need to pop.
            // If we're at level 0, the whole query is unsat.
            if ( !_smtCore.popSplit() )
            {
                if ( _verbosity > 0 )
                {
                    printf( "\nEngine::solve: UNSAT query\n" );
                    _statistics.print();
                }
                _exitCode = Engine::UNSAT;
                return false;
            }
        }
        catch ( ... )
        {
            _exitCode = Engine::ERROR;
            printf( "Engine: Unknown error!\n" );
            return false;
        }
    }
}

bool Engine::optimize( unsigned timeoutInSeconds )
{

    SignalHandler::getInstance()->initialize();
    SignalHandler::getInstance()->registerClient( this );

    updateDirections();
    storeInitialEngineState();

    printf("Engine::Solving Optimization Problem!!!!!!\n");
    printf("Reached here!!\n");
    mainLoopStatistics();

    if ( _verbosity > 0 )
    {
        printf( "\nEngine::solve: Initial statistics\n" );
        _statistics.print();
        printf( "\n---\n" );
    }

    bool splitJustPerformed = true;
    struct timespec mainLoopStart = TimeUtils::sampleMicro();

    while ( true )
    {
        //printf("\n Starting main loop :D - best so far: %f -- bounds on it: %f\n", _bestOptValSoFar, _tableau->getUpperBound(_costFunctionManager->getOptimizationVariable()));
        struct timespec mainLoopEnd = TimeUtils::sampleMicro();
        _statistics.addTimeMainLoop( TimeUtils::timePassed( mainLoopStart, mainLoopEnd ) );
        mainLoopStart = mainLoopEnd;

        if ( shouldExitDueToTimeout( timeoutInSeconds ) )
        {
            if ( _verbosity > 0 )
            {
                printf( "\n\nEngine: quitting due to timeout...\n\n" );
                printf( "Final statistics:\n" );
                _statistics.print();
            }

            _exitCode = Engine::TIMEOUT;
            _statistics.timeout();
            return false;
        }

        if (_tableau->getUpperBound(_costFunctionManager->getOptimizationVariable()) <= _bestOptValSoFar)
        {
            printf( "\n Trimming this branch because of upper bound \n" );
            _noEnteringCandidatesLeft = false;
            if ( !_smtCore.popSplit() )
            {
                printf("\nWe've explored the full tree with opt val (print 1): %f\n", _bestOptValSoFar);
                // TODO (Chris Strong): Rethink how we want to return
                _exitCode = Engine::SAT;
                _statistics.print();

                return true;
            }
            else
            {
                splitJustPerformed = true;
            }
            continue; // SHOULD THIS BE HERE?
        }

        if ( _quitRequested )
        {
            if ( _verbosity > 0 )
            {
                printf( "\n\nEngine: quitting due to external request...\n\n" );
                printf( "Final statistics:\n" );
                _statistics.print();
            }

            _exitCode = Engine::QUIT_REQUESTED;
            return false;
        }

        try
        {
            DEBUG( _tableau->verifyInvariants() );

            mainLoopStatistics();
            if ( _verbosity > 1 &&  _statistics.getNumMainLoopIterations() %
                 GlobalConfiguration::STATISTICS_PRINTING_FREQUENCY == 0 )
                _statistics.print();

            // Check whether progress has been made recently
            checkOverallProgress();

            // If the basis has become malformed, we need to restore it
            if ( basisRestorationNeeded() )
            {
                printf("Restoring basis\n");
                if ( _basisRestorationRequired == Engine::STRONG_RESTORATION_NEEDED )
                {
                    performPrecisionRestoration( PrecisionRestorer::RESTORE_BASICS );
                    _basisRestorationPerformed = Engine::PERFORMED_STRONG_RESTORATION;
                }
                else
                {
                    performPrecisionRestoration( PrecisionRestorer::DO_NOT_RESTORE_BASICS );
                    _basisRestorationPerformed = Engine::PERFORMED_WEAK_RESTORATION;
                }

                _numVisitedStatesAtPreviousRestoration = _statistics.getNumVisitedTreeStates();
                _basisRestorationRequired = Engine::RESTORATION_NOT_NEEDED;
                continue;
            }

            // Restoration is not required
            _basisRestorationPerformed = Engine::NO_RESTORATION_PERFORMED;

            // Possible restoration due to preceision degradation
            if ( shouldCheckDegradation() && highDegradation() )
            {
               printf("Performing precision restoration\n");

                performPrecisionRestoration( PrecisionRestorer::RESTORE_BASICS );
                continue;
            }

            if ( _tableau->basisMatrixAvailable() )
                explicitBasisBoundTightening();

            if ( splitJustPerformed )
            {
                do
                {
                    performSymbolicBoundTightening();
                }
                while ( applyAllValidConstraintCaseSplits() );
                splitJustPerformed = false;
            }

            // Perform any SmtCore-initiated case splits
            if ( _smtCore.needToSplit() )
            {
                printf("Splitting Cases\n");
                _smtCore.performSplit();
                _noEnteringCandidatesLeft = false;
                splitJustPerformed = true;
                continue;
            }

            if ( !_tableau->allBoundsValid() )
            {
                printf("Variable Bounds invalid, so unsat query\n");

                // Some variable bounds are invalid, so the query is unsat
                throw InfeasibleQueryException();
            }

            if ( allVarsWithinBounds() && _noEnteringCandidatesLeft)
            {
                //printf("Linear Portion Solved and reached optimum\n");
                double curOptValue = _tableau->getValue(_costFunctionManager->getOptimizationVariable());
                //printf("Value of opt var: %f\n", curOptValue);

                // Trim this branch if this value is less than the best we've seen so far
                // even if the nonlinear constraints are violated
                if (curOptValue < _bestOptValSoFar)
                {
                   //printf( "\n Trimming this branch \n" );
                    _noEnteringCandidatesLeft = false;
                    if ( !_smtCore.popSplit() )
                    {
                        printf("\nWe've explored the full tree with opt val (print 1): %f\n", _bestOptValSoFar);
                        // TODO (Chris Strong): Rethink how we want to return
                        _exitCode = Engine::SAT;
                        _statistics.print();

                        return true;
                    }
                    else
                    {
                        splitJustPerformed = true;
                    }

                    continue; // SHOULD THIS BE HERE?
                }

                //_costFunctionManager->computeCoreCostFunction();

                // The linear portion of the problem has been solved.
                // Check the status of the PL constraints
                collectViolatedPlConstraints();

                // If all constraints are satisfied, we are possibly done
                if ( allPlConstraintsHold() )
                {
                    printf("Piecewise linear solved\n");

                    if ( _tableau->getBasicAssignmentStatus() !=
                         ITableau::BASIC_ASSIGNMENT_JUST_COMPUTED )
                    {
                        if ( _verbosity > 0 )
                        {
                            printf( "Before declaring sat, recomputing...\n" );
                        }
                        // Make sure that the assignment is precise before declaring success
                        _tableau->computeAssignment();
                        continue;
                    }
                    if ( _verbosity > 0 )
                    {
                        printf( "\nEngine::solve: sat assignment found\n" );
                        _statistics.print();
                    }

                    printf("Newly found opt val: %f\n", curOptValue);
                    printf("Best opt val so far: %f\n", _bestOptValSoFar);
                    // We've found an optimum - update our best so far if needed - this completes our search of this subtree
                    if (_bestOptValSoFar < curOptValue)
                    {
                        _bestOptValSoFar = curOptValue;

                        // Update the best input we've seen so far
                        updateBestSolutionSoFar();

                    }
                    // Now, pop one to keep the search going
                    _noEnteringCandidatesLeft = false;
                    if( !_smtCore.popSplit() )
                    {
                        printf("\nWe've explored the full tree with opt val (print 2): %f\n", _bestOptValSoFar);
                        // TODO (Chris Strong): Rethink how we want to return
                        _exitCode = Engine::SAT;
                        _statistics.print();

                        return true;
                    }
                    else
                    {
                        splitJustPerformed = true;

                    }

                    continue;

                }

                // We have violated piecewise-linear constraints.
                performConstraintFixingStep();

                // Finally, take this opporunity to tighten any bounds
                // and perform any valid case splits.
                tightenBoundsOnConstraintMatrix();
                applyAllBoundTightenings();
                // For debugging purposes
                checkBoundCompliancyWithDebugSolution();

                while ( applyAllValidConstraintCaseSplits() )
                    performSymbolicBoundTightening();

                continue;
            }

            // We have out-of-bounds variables.
            performSimplexStep();
            if (_noEnteringCandidatesLeft)
            {
                // We've found an optimum
                if ( allVarsWithinBounds() )
                {
                    //printf("\nWe've found an optimum in our simplex!!!!!!!\n");
                }
                // The query is infeasible if there's no more options but we're not within bounds yet.
                else
                {
                    printf("\n Trimming infeasible query\n");
                    _noEnteringCandidatesLeft = false;
                    if( !_smtCore.popSplit() )
                    {
                        printf("\nWe've explored the full tree with opt val (print 3): %f\n", _bestOptValSoFar);
                        // TODO (Chris Strong): Rethink how we want to return
                        _exitCode = Engine::SAT;
                        _statistics.print();

                        return true;
                    }
                    else
                    {
                        splitJustPerformed = true;
                    }
                }
            }

            continue;
        }
        catch ( const MalformedBasisException & )
        {
            // Debug
            printf( "MalformedBasisException caught!\n" );
            //

            if ( _basisRestorationPerformed == Engine::NO_RESTORATION_PERFORMED )
            {
                if ( _numVisitedStatesAtPreviousRestoration != _statistics.getNumVisitedTreeStates() )
                {
                    // We've tried a strong restoration before, and it didn't work. Do a weak restoration
                    _basisRestorationRequired = Engine::WEAK_RESTORATION_NEEDED;
                }
                else
                {
                    _basisRestorationRequired = Engine::STRONG_RESTORATION_NEEDED;
                }
            }
            else if ( _basisRestorationPerformed == Engine::PERFORMED_STRONG_RESTORATION )
                _basisRestorationRequired = Engine::WEAK_RESTORATION_NEEDED;
            else
            {
                printf( "Engine: Cannot restore tableau!\n" );
                _exitCode = Engine::ERROR;
                return false;
            }
        }
        catch ( const InfeasibleQueryException & )
        {
            // The current query is unsat, and we need to pop.
            // If we're at level 0, the whole query is unsat.
            _noEnteringCandidatesLeft = false;
            if ( !_smtCore.popSplit() )
            {
                if ( _verbosity > 0 )
                {
                    printf( "\nEngine::solve: final pop was an unsat query\n" );
                    printf("Best value so far is %f\n", _bestOptValSoFar);
                }
                // switched to SAT / true b/c as long as the original isnt infeasible there will always be a satisfying solution?
                // Although if we want to run a query where it finds the optimizer within some output set this
                // logic may need to be reworked, but it doesn't seem too hard to get that to happen
                // TODO (Chris Strong): will this ever return before any feasible value has been found?
                _exitCode = Engine::SAT;
                return true;
            }
            else
            {
                splitJustPerformed = true;
            }
        }
        catch ( ... )
        {
            _exitCode = Engine::ERROR;
            printf( "Engine: Unknown error!\n" );
            return false;
        }
    }
}

void Engine::mainLoopStatistics()
{
    struct timespec start = TimeUtils::sampleMicro();

    unsigned activeConstraints = 0;
    for ( const auto &constraint : _plConstraints )
        if ( constraint->isActive() )
            ++activeConstraints;

    _statistics.setNumActivePlConstraints( activeConstraints );
    _statistics.setNumPlValidSplits( _numPlConstraintsDisabledByValidSplits );
    _statistics.setNumPlSMTSplits( _plConstraints.size() -
                                   activeConstraints - _numPlConstraintsDisabledByValidSplits );

    _statistics.incNumMainLoopIterations();

    struct timespec end = TimeUtils::sampleMicro();
    _statistics.addTimeForStatistics( TimeUtils::timePassed( start, end ) );
}

void Engine::performConstraintFixingStep()
{
    // Statistics
    _statistics.incNumConstraintFixingSteps();
    struct timespec start = TimeUtils::sampleMicro();

    // Select a violated constraint as the target
    selectViolatedPlConstraint();

    // Report the violated constraint to the SMT engine
    reportPlViolation();

    // Attempt to fix the constraint
    if(!_costFunctionManager->getOptimize())
        fixViolatedPlConstraintIfPossible();

    struct timespec end = TimeUtils::sampleMicro();
    _statistics.addTimeConstraintFixingSteps( TimeUtils::timePassed( start, end ) );
}

void Engine::performSimplexStep()
{
    // Statistics
    _statistics.incNumSimplexSteps();
    struct timespec start = TimeUtils::sampleMicro();

    /*
      In order to increase numerical stability, we attempt to pick a
      "good" entering/leaving combination, by trying to avoid tiny pivot
      values. We do this as follows:

      1. Pick an entering variable according to the strategy in use.
      2. Find the entailed leaving variable.
      3. If the combination is bad, go back to (1) and find the
         next-best entering variable.
    */

    if ( _costFunctionManager->costFunctionInvalid() )
        _costFunctionManager->computeCoreCostFunction();
    else
        _costFunctionManager->adjustBasicCostAccuracy();

    DEBUG({
            // Since we're performing a simplex step, there are out-of-bounds variables.
            // Therefore, if the cost function is fresh, it should not be zero.
            if ( _costFunctionManager->costFunctionJustComputed() )
            {
                const double *costFunction = _costFunctionManager->getCostFunction();
                unsigned size = _tableau->getN() - _tableau->getM();
                bool found = false;
                for ( unsigned i = 0; i < size; ++i )
                {
                    if ( !FloatUtils::isZero( costFunction[i] ) )
                    {
                        found = true;
                        break;
                    }
                }

                if ( !found )
                {
                    printf( "Error! Have OOB vars but cost function is zero.\n"
                            "Recomputing cost function. New one is:\n" );
                    _costFunctionManager->computeCoreCostFunction();
                    _costFunctionManager->dumpCostFunction();
                    throw MarabouError( MarabouError::DEBUGGING_ERROR,
                                         "Have OOB vars but cost function is zero" );
                }
            }
        });

    // Obtain all eligible entering varaibles
    List<unsigned> enteringVariableCandidates;
    _tableau->getEntryCandidates( enteringVariableCandidates );

    unsigned bestLeaving = 0;
    double bestChangeRatio = 0.0;
    Set<unsigned> excludedEnteringVariables;
    bool haveCandidate = false;
    unsigned bestEntering = 0;
    double bestPivotEntry = 0.0;
    unsigned tries = GlobalConfiguration::MAX_SIMPLEX_PIVOT_SEARCH_ITERATIONS;

    while ( tries > 0 )
    {
        --tries;

        // Attempt to pick the best entering variable from the available candidates
        if ( !_activeEntryStrategy->select( _tableau,
                                            enteringVariableCandidates,
                                            excludedEnteringVariables ) )
        {
            // No additional candidates can be found.
            break;
        }

        // We have a candidate!
        haveCandidate = true;

        // We don't want to re-consider this candidate in future
        // iterations
        excludedEnteringVariables.insert( _tableau->getEnteringVariableIndex() );

        // Pick a leaving variable
        _tableau->computeChangeColumn();
        _tableau->pickLeavingVariable();

        // A fake pivot always wins
        if ( _tableau->performingFakePivot() )
        {
            bestEntering = _tableau->getEnteringVariableIndex();
            bestLeaving = _tableau->getLeavingVariableIndex();
            bestChangeRatio = _tableau->getChangeRatio();
            memcpy( _work, _tableau->getChangeColumn(), sizeof(double) * _tableau->getM() );
            break;
        }

        // Is the newly found pivot better than the stored one?
        unsigned leavingIndex = _tableau->getLeavingVariableIndex();
        double pivotEntry = FloatUtils::abs( _tableau->getChangeColumn()[leavingIndex] );
        if ( pivotEntry > bestPivotEntry )
        {
            bestEntering = _tableau->getEnteringVariableIndex();
            bestPivotEntry = pivotEntry;
            bestLeaving = leavingIndex;
            bestChangeRatio = _tableau->getChangeRatio();
            memcpy( _work, _tableau->getChangeColumn(), sizeof(double) * _tableau->getM() );
        }

        // If the pivot is greater than the sought-after threshold, we
        // are done.
        if ( bestPivotEntry >= GlobalConfiguration::ACCEPTABLE_SIMPLEX_PIVOT_THRESHOLD )
            break;
        else
            _statistics.incNumSimplexPivotSelectionsIgnoredForStability();
    }

    // If we don't have any candidates, this simplex step has failed.
    if ( !haveCandidate )
    {
        if ( _tableau->getBasicAssignmentStatus() != ITableau::BASIC_ASSIGNMENT_JUST_COMPUTED )
        {
            // This failure might have resulted from a corrupt basic assignment.
            _tableau->computeAssignment();
            struct timespec end = TimeUtils::sampleMicro();
            _statistics.addTimeSimplexSteps( TimeUtils::timePassed( start, end ) );
            return;
        }
        else if ( !_costFunctionManager->costFunctionJustComputed() )
        {
            // This failure might have resulted from a corrupt cost function.
            ASSERT( _costFunctionManager->getCostFunctionStatus() ==
                    ICostFunctionManager::COST_FUNCTION_UPDATED );
            _costFunctionManager->invalidateCostFunction();
            struct timespec end = TimeUtils::sampleMicro();
            _statistics.addTimeSimplexSteps( TimeUtils::timePassed( start, end ) );
            return;
        }
        else
        {
            if (_costFunctionManager->getOptimize())
            {
                _noEnteringCandidatesLeft = true;
                return;
            }
            // Cost function is fresh --- failure is real.
            struct timespec end = TimeUtils::sampleMicro();
            _statistics.addTimeSimplexSteps( TimeUtils::timePassed( start, end ) );
            throw InfeasibleQueryException();
        }
    }
    _noEnteringCandidatesLeft = false;

    // Set the best choice in the tableau
    _tableau->setEnteringVariableIndex( bestEntering );
    _tableau->setLeavingVariableIndex( bestLeaving );
    _tableau->setChangeColumn( _work );
    _tableau->setChangeRatio( bestChangeRatio );

    bool fakePivot = _tableau->performingFakePivot();

    if ( !fakePivot &&
         bestPivotEntry < GlobalConfiguration::ACCEPTABLE_SIMPLEX_PIVOT_THRESHOLD )
    {
        /*
          Despite our efforts, we are stuck with a small pivot. If basis factorization
          isn't fresh, refresh it and terminate this step - perhaps in the next iteration
          a better pivot will be found
        */
        if ( !_tableau->basisMatrixAvailable() )
        {
            _tableau->refreshBasisFactorization();
            return;
        }

        _statistics.incNumSimplexUnstablePivots();
    }

    if ( !fakePivot )
    {
        _tableau->computePivotRow();
        _rowBoundTightener->examinePivotRow();
    }

    // Perform the actual pivot
    _activeEntryStrategy->prePivotHook( _tableau, fakePivot );
    _tableau->performPivot();
    _activeEntryStrategy->postPivotHook( _tableau, fakePivot );

    struct timespec end = TimeUtils::sampleMicro();
    _statistics.addTimeSimplexSteps( TimeUtils::timePassed( start, end ) );
}

void Engine::fixViolatedPlConstraintIfPossible()
{
    List<PiecewiseLinearConstraint::Fix> fixes;

    if ( GlobalConfiguration::USE_SMART_FIX )
        fixes = _plConstraintToFix->getSmartFixes( _tableau );
    else
        fixes = _plConstraintToFix->getPossibleFixes();

    // First, see if we can fix without pivoting. We are looking for a fix concerning a
    // non-basic variable, that doesn't set that variable out-of-bounds.
    for ( const auto &fix : fixes )
    {
        if ( !_tableau->isBasic( fix._variable ) )
        {
			if ( _tableau->checkValueWithinBounds( fix._variable, fix._value ) )
			{
                _tableau->setNonBasicAssignment( fix._variable, fix._value, true );
                return;
			}
        }
    }

    // No choice, have to pivot. Look for a fix concerning a basic variable, that
    // doesn't set that variable out-of-bounds. If smart-fix is enabled and implemented,
    // we should probably not reach this point.
    bool found = false;
    auto it = fixes.begin();
    while ( !found && it != fixes.end() )
    {
        if ( _tableau->isBasic( it->_variable ) )
        {
			if ( _tableau->checkValueWithinBounds( it->_variable, it->_value ) )
			{
                found = true;
            }
        }
        if ( !found )
        {
            ++it;
        }
    }

    // If we couldn't find an eligible fix, give up
    if ( !found )
        return;

    PiecewiseLinearConstraint::Fix fix = *it;
    ASSERT( _tableau->isBasic( fix._variable ) );

    TableauRow row( _tableau->getN() - _tableau->getM() );
    _tableau->getTableauRow( _tableau->variableToIndex( fix._variable ), &row );

    // Pick the variable with the largest coefficient in this row for pivoting,
    // to increase numerical stability.
    unsigned bestCandidate = row._row[0]._var;
    double bestValue = FloatUtils::abs( row._row[0]._coefficient );

    unsigned n = _tableau->getN();
    unsigned m = _tableau->getM();
    for ( unsigned i = 1; i < n - m; ++i )
    {
        double contenderValue = FloatUtils::abs( row._row[i]._coefficient );
        if ( FloatUtils::gt( contenderValue, bestValue ) )
        {
            bestValue = contenderValue;
            bestCandidate = row._row[i]._var;
        }
    }

    if ( FloatUtils::isZero( bestValue ) )
    {
        // This can happen, e.g., if we have an equation x = 5, and is legal behavior.
        return;
    }

    // Switch between nonBasic and the variable we need to fix
    _tableau->setEnteringVariableIndex( _tableau->variableToIndex( bestCandidate ) );
    _tableau->setLeavingVariableIndex( _tableau->variableToIndex( fix._variable ) );

    // Make sure the change column and pivot row are up-to-date - strategies
    // such as projected steepest edge need these for their internal updates.
    _tableau->computeChangeColumn();
    _tableau->computePivotRow();

    _activeEntryStrategy->prePivotHook( _tableau, false );
    _tableau->performDegeneratePivot();
    _activeEntryStrategy->postPivotHook( _tableau, false );

    ASSERT( !_tableau->isBasic( fix._variable ) );
    _tableau->setNonBasicAssignment( fix._variable, fix._value, true );
}

bool Engine::processInputQuery( InputQuery &inputQuery )
{
    return processInputQuery( inputQuery, GlobalConfiguration::PREPROCESS_INPUT_QUERY );
}

void Engine::informConstraintsOfInitialBounds( InputQuery &inputQuery ) const
{
    for ( const auto &plConstraint : inputQuery.getPiecewiseLinearConstraints() )
    {
        List<unsigned> variables = plConstraint->getParticipatingVariables();
        for ( unsigned variable : variables )
        {
            plConstraint->notifyLowerBound( variable, inputQuery.getLowerBound( variable ) );
            plConstraint->notifyUpperBound( variable, inputQuery.getUpperBound( variable ) );
        }
    }
}

void Engine::invokePreprocessor( const InputQuery &inputQuery, bool preprocess )
{

    if ( _verbosity > 0 )
        printf( "Engine::processInputQuery: Input query (before preprocessing): "
                "%u equations, %u variables\n",
                inputQuery.getEquations().size(),
                inputQuery.getNumberOfVariables() );

    // If processing is enabled, invoke the preprocessor
    _preprocessingEnabled = preprocess;
    if ( _preprocessingEnabled )
        _preprocessedQuery = _preprocessor.preprocess
            ( inputQuery, GlobalConfiguration::PREPROCESSOR_ELIMINATE_VARIABLES );
    else
        _preprocessedQuery = inputQuery;

    if ( _verbosity > 0 )
        printf( "Engine::processInputQuery: Input query (after preprocessing): "
                "%u equations, %u variables\n\n",
                _preprocessedQuery.getEquations().size(),
                _preprocessedQuery.getNumberOfVariables() );

    unsigned infiniteBounds = _preprocessedQuery.countInfiniteBounds();
    if ( infiniteBounds != 0 )
    {
        _exitCode = Engine::ERROR;
        throw MarabouError( MarabouError::UNBOUNDED_VARIABLES_NOT_YET_SUPPORTED,
                             Stringf( "Error! Have %u infinite bounds", infiniteBounds ).ascii() );
    }
}

void Engine::printInputBounds( const InputQuery &inputQuery ) const
{
    printf( "Input bounds:\n" );
    for ( unsigned i = 0; i < inputQuery.getNumInputVariables(); ++i )
    {
        unsigned variable = inputQuery.inputVariableByIndex( i );
        double lb, ub;
        bool fixed = false;
        if ( _preprocessingEnabled )
        {
            // Fixed variables are easy: return the value they've been fixed to.
            if ( _preprocessor.variableIsFixed( variable ) )
            {
                fixed = true;
                lb = _preprocessor.getFixedValue( variable );
                ub = lb;
            }
            else
            {
                // Has the variable been merged into another?
                while ( _preprocessor.variableIsMerged( variable ) )
                    variable = _preprocessor.getMergedIndex( variable );

                // We know which variable to look for, but it may have been assigned
                // a new index, due to variable elimination
                variable = _preprocessor.getNewIndex( variable );

                lb = _preprocessedQuery.getLowerBound( variable );
                ub = _preprocessedQuery.getUpperBound( variable );
            }
        }
        else
        {
            lb = inputQuery.getLowerBound( variable );
            ub = inputQuery.getUpperBound( variable );
        }

        printf( "\tx%u: [%8.4lf, %8.4lf] %s\n", i, lb, ub, fixed ? "[FIXED]" : "" );
    }
    printf( "\n" );
}

void Engine::storeEquationsInDegradationChecker()
{
    _degradationChecker.storeEquations( _preprocessedQuery );
}

double *Engine::createConstraintMatrix()
{
    const List<Equation> &equations( _preprocessedQuery.getEquations() );
    unsigned m = equations.size();
    unsigned n = _preprocessedQuery.getNumberOfVariables();

    // Step 1: create a constraint matrix from the equations
    double *constraintMatrix = new double[n*m];
    if ( !constraintMatrix )
        throw MarabouError( MarabouError::ALLOCATION_FAILED, "Engine::constraintMatrix" );
    std::fill_n( constraintMatrix, n*m, 0.0 );

    unsigned equationIndex = 0;
    for ( const auto &equation : equations )
    {
        if ( equation._type != Equation::EQ )
        {
            _exitCode = Engine::ERROR;
            throw MarabouError( MarabouError::NON_EQUALITY_INPUT_EQUATION_DISCOVERED );
        }

        for ( const auto &addend : equation._addends )
            constraintMatrix[equationIndex*n + addend._variable] = addend._coefficient;

        ++equationIndex;
    }

    return constraintMatrix;
}

void Engine::removeRedundantEquations( const double *constraintMatrix )
{
    const List<Equation> &equations( _preprocessedQuery.getEquations() );
    unsigned m = equations.size();
    unsigned n = _preprocessedQuery.getNumberOfVariables();

    // Step 1: analyze the matrix to identify redundant rows
    AutoConstraintMatrixAnalyzer analyzer;
    analyzer->analyze( constraintMatrix, m, n );

    ENGINE_LOG( Stringf( "Number of redundant rows: %u out of %u",
                  analyzer->getRedundantRows().size(), m ).ascii() );

    // Step 2: remove any equations corresponding to redundant rows
    Set<unsigned> redundantRows = analyzer->getRedundantRows();

    if ( !redundantRows.empty() )
    {
        _preprocessedQuery.removeEquationsByIndex( redundantRows );
        m = equations.size();
    }
}

void Engine::selectInitialVariablesForBasis( const double *constraintMatrix, List<unsigned> &initialBasis, List<unsigned> &basicRows )
{
    /*
      This method permutes rows and columns in the constraint matrix (prior
      to the addition of auxiliary variables), in order to obtain a set of
      column that constitue a lower triangular matrix. The variables
      corresponding to the columns of this matrix join the initial basis.

      (It is possible that not enough variables are obtained this way, in which
      case the initial basis will have to be augmented later).
    */

    const List<Equation> &equations( _preprocessedQuery.getEquations() );

    unsigned m = equations.size();
    unsigned n = _preprocessedQuery.getNumberOfVariables();

    // Trivial case, or if a trivial basis is requested
    if ( ( m == 0 ) || ( n == 0 ) || GlobalConfiguration::ONLY_AUX_INITIAL_BASIS )
    {
        for ( unsigned i = 0; i < m; ++i )
            basicRows.append( i );

        return;
    }

    unsigned *nnzInRow = new unsigned[m];
    unsigned *nnzInColumn = new unsigned[n];

    std::fill_n( nnzInRow, m, 0 );
    std::fill_n( nnzInColumn, n, 0 );

    unsigned *columnOrdering = new unsigned[n];
    unsigned *rowOrdering = new unsigned[m];

    for ( unsigned i = 0; i < m; ++i )
        rowOrdering[i] = i;

    for ( unsigned i = 0; i < n; ++i )
        columnOrdering[i] = i;

    // Initialize the counters
    for ( unsigned i = 0; i < m; ++i )
    {
        for ( unsigned j = 0; j < n; ++j )
        {
            if ( !FloatUtils::isZero( constraintMatrix[i*n + j] ) )
            {
                ++nnzInRow[i];
                ++nnzInColumn[j];
            }
        }
    }

    DEBUG({
            for ( unsigned i = 0; i < m; ++i )
            {
                ASSERT( nnzInRow[i] > 0 );
            }
        });

    unsigned numExcluded = 0;
    unsigned numTriangularRows = 0;
    unsigned temp;

    while ( numExcluded + numTriangularRows < n )
    {
        // Do we have a singleton row?
        unsigned singletonRow = m;
        for ( unsigned i = numTriangularRows; i < m; ++i )
        {
            if ( nnzInRow[i] == 1 )
            {
                singletonRow = i;
                break;
            }
        }

        if ( singletonRow < m )
        {
            // Have a singleton row! Swap it to the top and update counters
            temp = rowOrdering[singletonRow];
            rowOrdering[singletonRow] = rowOrdering[numTriangularRows];
            rowOrdering[numTriangularRows] = temp;

            temp = nnzInRow[numTriangularRows];
            nnzInRow[numTriangularRows] = nnzInRow[singletonRow];
            nnzInRow[singletonRow] = temp;

            // Find the non-zero entry in the row and swap it to the diagonal
            DEBUG( bool foundNonZero = false );
            for ( unsigned i = numTriangularRows; i < n - numExcluded; ++i )
            {
                if ( !FloatUtils::isZero( constraintMatrix[rowOrdering[numTriangularRows] * n + columnOrdering[i]] ) )
                {
                    temp = columnOrdering[i];
                    columnOrdering[i] = columnOrdering[numTriangularRows];
                    columnOrdering[numTriangularRows] = temp;

                    temp = nnzInColumn[numTriangularRows];
                    nnzInColumn[numTriangularRows] = nnzInColumn[i];
                    nnzInColumn[i] = temp;

                    DEBUG( foundNonZero = true );
                    break;
                }
            }

            ASSERT( foundNonZero );

            // Remove all entries under the diagonal entry from the row counters
            for ( unsigned i = numTriangularRows + 1; i < m; ++i )
            {
                if ( !FloatUtils::isZero( constraintMatrix[rowOrdering[i] * n + columnOrdering[numTriangularRows]] ) )
                    --nnzInRow[i];
            }

            ++numTriangularRows;
        }
        else
        {
            // No singleton rows. Exclude the densest column
            unsigned maxDensity = nnzInColumn[numTriangularRows];
            unsigned column = numTriangularRows;

            for ( unsigned i = numTriangularRows; i < n - numExcluded; ++i )
            {
                if ( nnzInColumn[i] > maxDensity )
                {
                    maxDensity = nnzInColumn[i];
                    column = i;
                }
            }

            // Update the row counters to account for the excluded column
            for ( unsigned i = numTriangularRows; i < m; ++i )
            {
                double element = constraintMatrix[rowOrdering[i]*n + columnOrdering[column]];
                if ( !FloatUtils::isZero( element ) )
                {
                    ASSERT( nnzInRow[i] > 1 );
                    --nnzInRow[i];
                }
            }

            columnOrdering[column] = columnOrdering[n - 1 - numExcluded];
            nnzInColumn[column] = nnzInColumn[n - 1 - numExcluded];
            ++numExcluded;
        }
    }

    // Final basis: diagonalized columns + non-diagonalized rows
    List<unsigned> result;

    for ( unsigned i = 0; i < numTriangularRows; ++i )
    {
        initialBasis.append( columnOrdering[i] );
    }

    for ( unsigned i = numTriangularRows; i < m; ++i )
    {
        basicRows.append( rowOrdering[i] );
    }

    // If optimizing, make sure that the optimization variable starts in the basis
    if (_costFunctionManager->getOptimize())
    {
        // If it isn't already in the basis, check if the basis is full.
        // If it is, remove the last element of the basis
        // Then, add in the optimization variable
        if (!initialBasis.exists(_costFunctionManager->getOptimizationVariable()))
        {
            if (initialBasis.size() == m)
                initialBasis.popBack();

            initialBasis.append(_costFunctionManager->getOptimizationVariable());
        }
    }

    // Cleanup
    delete[] nnzInRow;
    delete[] nnzInColumn;
    delete[] columnOrdering;
    delete[] rowOrdering;
}

void Engine::addAuxiliaryVariables()
{
    List<Equation> &equations( _preprocessedQuery.getEquations() );

    unsigned m = equations.size();
    unsigned originalN = _preprocessedQuery.getNumberOfVariables();
    unsigned n = originalN + m;

    _preprocessedQuery.setNumberOfVariables( n );

    // Add auxiliary variables to the equations and set their bounds
    unsigned count = 0;
    for ( auto &eq : equations )
    {
        unsigned auxVar = originalN + count;
        eq.addAddend( -1, auxVar );
        _preprocessedQuery.setLowerBound( auxVar, eq._scalar );
        _preprocessedQuery.setUpperBound( auxVar, eq._scalar );
        eq.setScalar( 0 );

        ++count;
    }
}

void Engine::augmentInitialBasisIfNeeded( List<unsigned> &initialBasis, const List<unsigned> &basicRows )
{
    unsigned m = _preprocessedQuery.getEquations().size();
    unsigned n = _preprocessedQuery.getNumberOfVariables();
    unsigned originalN = n - m;

    if ( initialBasis.size() != m )
    {
        for ( const auto &basicRow : basicRows )
            initialBasis.append( basicRow + originalN );
    }
}

void Engine::initializeTableau( const double *constraintMatrix, const List<unsigned> &initialBasis )
{
    const List<Equation> &equations( _preprocessedQuery.getEquations() );
    unsigned m = equations.size();
    unsigned n = _preprocessedQuery.getNumberOfVariables();

    _tableau->setDimensions( m, n );

    adjustWorkMemorySize();

    unsigned equationIndex = 0;
    for ( const auto &equation : equations )
    {
        _tableau->setRightHandSide( equationIndex, equation._scalar );
        ++equationIndex;
    }

    // Populate constriant matrix
    _tableau->setConstraintMatrix( constraintMatrix );

    for ( unsigned i = 0; i < n; ++i )
    {
        _tableau->setLowerBound( i, _preprocessedQuery.getLowerBound( i ) );
        _tableau->setUpperBound( i, _preprocessedQuery.getUpperBound( i ) );
    }

    _tableau->registerToWatchAllVariables( _rowBoundTightener );
    _tableau->registerResizeWatcher( _rowBoundTightener );

    _tableau->registerToWatchAllVariables( _constraintBoundTightener );
    _tableau->registerResizeWatcher( _constraintBoundTightener );

    _rowBoundTightener->setDimensions();
    _constraintBoundTightener->setDimensions();

    // Register the constraint bound tightener to all the PL constraints
    for ( auto &plConstraint : _preprocessedQuery.getPiecewiseLinearConstraints() )
        plConstraint->registerConstraintBoundTightener( _constraintBoundTightener );

    _plConstraints = _preprocessedQuery.getPiecewiseLinearConstraints();
    for ( const auto &constraint : _plConstraints )
    {
        constraint->registerAsWatcher( _tableau );
        constraint->setStatistics( &_statistics );
    }

    _tableau->initializeTableau( initialBasis );

    _costFunctionManager->initialize();
    _tableau->registerCostFunctionManager( _costFunctionManager );
    _activeEntryStrategy->initialize( _tableau );

    _statistics.setNumPlConstraints( _plConstraints.size() );
}

void Engine::initializeNetworkLevelReasoning()
{
    _networkLevelReasoner = _preprocessedQuery.getNetworkLevelReasoner();

    if ( _networkLevelReasoner )
        _networkLevelReasoner->setTableau( _tableau );
}

bool Engine::processInputQuery( InputQuery &inputQuery, bool preprocess )
{
    ENGINE_LOG( "processInputQuery starting\n" );

    struct timespec start = TimeUtils::sampleMicro();

    try
    {
        informConstraintsOfInitialBounds( inputQuery );

        invokePreprocessor( inputQuery, preprocess );
        if ( _verbosity > 0 )
            printInputBounds( inputQuery );

        double *constraintMatrix = createConstraintMatrix();
        removeRedundantEquations( constraintMatrix );

        // The equations have changed, recreate the constraint matrix
        delete[] constraintMatrix;
        constraintMatrix = createConstraintMatrix();

        List<unsigned> initialBasis;
        List<unsigned> basicRows;
        selectInitialVariablesForBasis( constraintMatrix, initialBasis, basicRows );
        addAuxiliaryVariables();
        augmentInitialBasisIfNeeded( initialBasis, basicRows );

        storeEquationsInDegradationChecker();

        // The equations have changed, recreate the constraint matrix
        delete[] constraintMatrix;
        constraintMatrix = createConstraintMatrix();

        initializeNetworkLevelReasoning();
        initializeTableau( constraintMatrix, initialBasis );

        if ( GlobalConfiguration::WARM_START )
            warmStart();

        delete[] constraintMatrix;

        performMILPSolverBoundedTightening();

        struct timespec end = TimeUtils::sampleMicro();
        _statistics.setPreprocessingTime( TimeUtils::timePassed( start, end ) );
    }
    catch ( const InfeasibleQueryException & )
    {
        ENGINE_LOG( "processInputQuery done\n" );

        struct timespec end = TimeUtils::sampleMicro();
        _statistics.setPreprocessingTime( TimeUtils::timePassed( start, end ) );

        _exitCode = Engine::UNSAT;
        return false;
    }

    // Update at the end so that the optimization variable can get reassigned if need be.
    _costFunctionManager->setOptimize(_preprocessedQuery.getOptimize());
    _costFunctionManager->setOptimizationVariable(_preprocessedQuery.getOptimizationVariable());
    // Set the divide strategy - it will default to DivideStrategy::None
    _smtCore.setDivideStrategy(_preprocessedQuery.getDivideStrategy());

    ENGINE_LOG( "processInputQuery done\n" );

    _smtCore.storeDebuggingSolution( _preprocessedQuery._debuggingSolution );
    return true;
}

// Update the best solution so far with the current assignment
void Engine::updateBestSolutionSoFar()
{
    for ( unsigned i = 0; i < _preprocessedQuery.getNumberOfVariables(); ++i )
    {
        if ( _preprocessingEnabled )
        {
            // Has the variable been merged into another?
            unsigned variable = i;
            while ( _preprocessor.variableIsMerged( variable ) )
                variable = _preprocessor.getMergedIndex( variable );


            // Fixed variables are easy: return the value they've been fixed to.
            if ( _preprocessor.variableIsFixed( variable ) )
            {
                _bestSolutionSoFar[i] = _preprocessor.getFixedValue( variable );
                continue;
            }

            // We know which variable to look for, but it may have been assigned
            // a new index, due to variable elimination
            variable = _preprocessor.getNewIndex( variable );

            // Finally, set the assigned value
            _bestSolutionSoFar[i] = _tableau->getValue( variable );
        }
        else
        {
            _bestSolutionSoFar[i] = _tableau->getValue( i );

        }
    }
}


void Engine::performMILPSolverBoundedTightening()
{
    if ( _networkLevelReasoner && Options::get()->gurobiEnabled() )
    {
        _networkLevelReasoner->obtainCurrentBounds();

        switch ( GlobalConfiguration::MILP_SOLVER_BOUND_TIGHTENING_TYPE )
        {
        case GlobalConfiguration::LP_RELAXATION:
        case GlobalConfiguration::LP_RELAXATION_INCREMENTAL:
            _networkLevelReasoner->lpRelaxationPropagation();
            break;

        case GlobalConfiguration::MILP_ENCODING:
        case GlobalConfiguration::MILP_ENCODING_INCREMENTAL:
            _networkLevelReasoner->MILPPropagation();
            break;
        case GlobalConfiguration::NONE:
            return;
        }

        List<Tightening> tightenings;
        _networkLevelReasoner->getConstraintTightenings( tightenings );

        for ( const auto &tightening : tightenings )
        {
            if ( tightening._type == Tightening::LB )
                _tableau->tightenLowerBound( tightening._variable, tightening._value );

            else if ( tightening._type == Tightening::UB )
                _tableau->tightenUpperBound( tightening._variable, tightening._value );
        }

        applyAllValidConstraintCaseSplits();
    }
}
void Engine::extractSolution( InputQuery &inputQuery )
{
    // Set it using the best solution found so far
    if (_costFunctionManager->getOptimize()) {
        printf("Preprocessing is %d\n", _preprocessingEnabled);

        for (unsigned i = 0; i < inputQuery.getNumberOfVariables(); ++i) {
            inputQuery.setSolutionValue(i, _bestSolutionSoFar.get(i));
        }
        return;
    }


    for ( unsigned i = 0; i < inputQuery.getNumberOfVariables(); ++i )
    {
        if ( _preprocessingEnabled )
        {
            // Has the variable been merged into another?
            unsigned variable = i;
            while ( _preprocessor.variableIsMerged( variable ) )
                variable = _preprocessor.getMergedIndex( variable );

            // Fixed variables are easy: return the value they've been fixed to.
            if ( _preprocessor.variableIsFixed( variable ) )
            {
                inputQuery.setSolutionValue( i, _preprocessor.getFixedValue( variable ) );
                inputQuery.setLowerBound( i, _preprocessor.getFixedValue( variable ) );
                inputQuery.setUpperBound( i, _preprocessor.getFixedValue( variable ) );
                continue;
            }

            // We know which variable to look for, but it may have been assigned
            // a new index, due to variable elimination
            variable = _preprocessor.getNewIndex( variable );

            // Finally, set the assigned value
            inputQuery.setSolutionValue( i, _tableau->getValue( variable ) );
            inputQuery.setLowerBound( i, _tableau->getLowerBound( variable ) );
            inputQuery.setUpperBound( i, _tableau->getUpperBound( variable ) );
        }
        else
        {
            inputQuery.setSolutionValue( i, _tableau->getValue( i ) );
            inputQuery.setLowerBound( i, _tableau->getLowerBound( i ) );
            inputQuery.setUpperBound( i, _tableau->getUpperBound( i ) );
        }
    }
}

bool Engine::allVarsWithinBounds() const
{
    return !_tableau->existsBasicOutOfBounds();
}

void Engine::collectViolatedPlConstraints()
{
    _violatedPlConstraints.clear();
    for ( const auto &constraint : _plConstraints )
    {
        if ( constraint->isActive() && !constraint->satisfied() )
            _violatedPlConstraints.append( constraint );
    }
}

bool Engine::allPlConstraintsHold()
{
    return _violatedPlConstraints.empty();
}

void Engine::selectViolatedPlConstraint()
{
    ASSERT( !_violatedPlConstraints.empty() );

    _plConstraintToFix = _smtCore.chooseViolatedConstraintForFixing( _violatedPlConstraints );

    ASSERT( _plConstraintToFix );
}

void Engine::reportPlViolation()
{
    _smtCore.reportViolatedConstraint( _plConstraintToFix );
}

void Engine::storeTableauState( TableauState &state ) const
{
    _tableau->storeState( state );
}

void Engine::restoreTableauState( const TableauState &state )
{
    ENGINE_LOG( "\tRestoring tableau state" );
    _tableau->restoreState( state );
}

void Engine::storeState( EngineState &state, bool storeAlsoTableauState ) const
{
    if ( storeAlsoTableauState )
    {
        _tableau->storeState( state._tableauState );
        state._tableauStateIsStored = true;
    }
    else
        state._tableauStateIsStored = false;

    for ( const auto &constraint : _plConstraints )
        state._plConstraintToState[constraint] = constraint->duplicateConstraint();

    state._numPlConstraintsDisabledByValidSplits = _numPlConstraintsDisabledByValidSplits;
}

void Engine::restoreState( const EngineState &state )
{
    ENGINE_LOG( "Restore state starting" );

    if ( !state._tableauStateIsStored )
        throw MarabouError( MarabouError::RESTORING_ENGINE_FROM_INVALID_STATE );

    ENGINE_LOG( "\tRestoring tableau state" );
    _tableau->restoreState( state._tableauState );

    ENGINE_LOG( "\tRestoring constraint states" );
    for ( auto &constraint : _plConstraints )
    {
        if ( !state._plConstraintToState.exists( constraint ) )
            throw MarabouError( MarabouError::MISSING_PL_CONSTRAINT_STATE );

        constraint->restoreState( state._plConstraintToState[constraint] );
    }

    _numPlConstraintsDisabledByValidSplits = state._numPlConstraintsDisabledByValidSplits;

    // Make sure the data structures are initialized to the correct size
    _rowBoundTightener->setDimensions();
    _constraintBoundTightener->setDimensions();
    adjustWorkMemorySize();
    _activeEntryStrategy->resizeHook( _tableau );
    _costFunctionManager->initialize();

    // Reset the violation counts in the SMT core
    _smtCore.resetReportedViolations();
}

void Engine::setNumPlConstraintsDisabledByValidSplits( unsigned numConstraints )
{
    _numPlConstraintsDisabledByValidSplits = numConstraints;
}

bool Engine::attemptToMergeVariables( unsigned x1, unsigned x2 )
{
    /*
      First, we need to ensure that the variables are both non-basic.
    */

    unsigned n = _tableau->getN();
    unsigned m = _tableau->getM();

    if ( _tableau->isBasic( x1 ) )
    {
        TableauRow x1Row( n - m );
        _tableau->getTableauRow( _tableau->variableToIndex( x1 ), &x1Row );

        bool found = false;
        double bestCoefficient = 0.0;
        unsigned nonBasic = 0;
        for ( unsigned i = 0; i < n - m; ++i )
        {
            if ( x1Row._row[i]._var != x2 )
            {
                double contender = FloatUtils::abs( x1Row._row[i]._coefficient );
                if ( FloatUtils::gt( contender, bestCoefficient ) )
                {
                    found = true;
                    nonBasic = x1Row._row[i]._var;
                    bestCoefficient = contender;
                }
            }
        }

        if ( !found )
            return false;

        _tableau->setEnteringVariableIndex( _tableau->variableToIndex( nonBasic ) );
        _tableau->setLeavingVariableIndex( _tableau->variableToIndex( x1 ) );

        // Make sure the change column and pivot row are up-to-date - strategies
        // such as projected steepest edge need these for their internal updates.
        _tableau->computeChangeColumn();
        _tableau->computePivotRow();

        _activeEntryStrategy->prePivotHook( _tableau, false );
        _tableau->performDegeneratePivot();
        _activeEntryStrategy->postPivotHook( _tableau, false );
    }

    if ( _tableau->isBasic( x2 ) )
    {
        TableauRow x2Row( n - m );
        _tableau->getTableauRow( _tableau->variableToIndex( x2 ), &x2Row );

        bool found = false;
        double bestCoefficient = 0.0;
        unsigned nonBasic = 0;
        for ( unsigned i = 0; i < n - m; ++i )
        {
            if ( x2Row._row[i]._var != x1 )
            {
                double contender = FloatUtils::abs( x2Row._row[i]._coefficient );
                if ( FloatUtils::gt( contender, bestCoefficient ) )
                {
                    found = true;
                    nonBasic = x2Row._row[i]._var;
                    bestCoefficient = contender;
                }
            }
        }

        if ( !found )
            return false;

        _tableau->setEnteringVariableIndex( _tableau->variableToIndex( nonBasic ) );
        _tableau->setLeavingVariableIndex( _tableau->variableToIndex( x2 ) );

        // Make sure the change column and pivot row are up-to-date - strategies
        // such as projected steepest edge need these for their internal updates.
        _tableau->computeChangeColumn();
        _tableau->computePivotRow();

        _activeEntryStrategy->prePivotHook( _tableau, false );
        _tableau->performDegeneratePivot();
        _activeEntryStrategy->postPivotHook( _tableau, false );
    }

    // Both variables are now non-basic, so we can merge their columns
    _tableau->mergeColumns( x1, x2 );
    DEBUG( _tableau->verifyInvariants() );

    // Reset the entry strategy
    _activeEntryStrategy->initialize( _tableau );

    return true;
}

void Engine::applySplit( const PiecewiseLinearCaseSplit &split )
{
    ENGINE_LOG( "" );
    ENGINE_LOG( "Applying a split. " );

    DEBUG( _tableau->verifyInvariants() );

    List<Tightening> bounds = split.getBoundTightenings();
    List<Equation> equations = split.getEquations();
    for ( auto &equation : equations )
    {
        /*
          First, adjust the equation if any variables have been merged.
          E.g., if the equation is x1 + x2 + x3 = 0, and x1 and x2 have been
          merged, the equation becomes 2x1 + x3 = 0
        */
        for ( auto &addend : equation._addends )
            addend._variable = _tableau->getVariableAfterMerging( addend._variable );

        List<Equation::Addend>::iterator addend;
        List<Equation::Addend>::iterator otherAddend;

        addend = equation._addends.begin();
        while ( addend != equation._addends.end() )
        {
            otherAddend = addend;
            ++otherAddend;

            while ( otherAddend != equation._addends.end() )
            {
                if ( otherAddend->_variable == addend->_variable )
                {
                    addend->_coefficient += otherAddend->_coefficient;
                    otherAddend = equation._addends.erase( otherAddend );
                }
                else
                    ++otherAddend;
            }

            if ( FloatUtils::isZero( addend->_coefficient ) )
                addend = equation._addends.erase( addend );
            else
                ++addend;
        }

        /*
          In the general case, we just add the new equation to the tableau.
          However, we also support a very common case: equations of the form
          x1 = x2, which are common, e.g., with ReLUs. For these equations we
          may be able to merge two columns of the tableau.
        */
        unsigned x1, x2;
        bool canMergeColumns =
            // Only if the flag is on
            GlobalConfiguration::USE_COLUMN_MERGING_EQUATIONS &&
            // Only if the equation has the correct form
            equation.isVariableMergingEquation( x1, x2 ) &&
            // And only if the variables are not out of bounds
            ( !_tableau->isBasic( x1 ) ||
              !_tableau->basicOutOfBounds( _tableau->variableToIndex( x1 ) ) )
            &&
            ( !_tableau->isBasic( x2 ) ||
              !_tableau->basicOutOfBounds( _tableau->variableToIndex( x2 ) ) );

        bool columnsSuccessfullyMerged = false;
        if ( canMergeColumns )
            columnsSuccessfullyMerged = attemptToMergeVariables( x1, x2 );

        if ( !columnsSuccessfullyMerged )
        {
            // General case: add a new equation to the tableau
            unsigned auxVariable = _tableau->addEquation( equation );
            _activeEntryStrategy->resizeHook( _tableau );

            switch ( equation._type )
            {
            case Equation::GE:
                bounds.append( Tightening( auxVariable, 0.0, Tightening::UB ) );
                break;

            case Equation::LE:
                bounds.append( Tightening( auxVariable, 0.0, Tightening::LB ) );
                break;

            case Equation::EQ:
                bounds.append( Tightening( auxVariable, 0.0, Tightening::LB ) );
                bounds.append( Tightening( auxVariable, 0.0, Tightening::UB ) );
                break;

            default:
                ASSERT( false );
                break;
            }
        }
    }

    adjustWorkMemorySize();

    _rowBoundTightener->resetBounds();
    _constraintBoundTightener->resetBounds();

    for ( auto &bound : bounds )
    {
        unsigned variable = _tableau->getVariableAfterMerging( bound._variable );

        if ( bound._type == Tightening::LB )
        {
            ENGINE_LOG( Stringf( "x%u: lower bound set to %.3lf", variable, bound._value ).ascii() );
            _tableau->tightenLowerBound( variable, bound._value );
        }
        else
        {
            ENGINE_LOG( Stringf( "x%u: upper bound set to %.3lf", variable, bound._value ).ascii() );
            _tableau->tightenUpperBound( variable, bound._value );
        }
    }

    DEBUG( _tableau->verifyInvariants() );
    ENGINE_LOG( "Done with split\n" );
}

void Engine::applyAllRowTightenings()
{
    List<Tightening> rowTightenings;
    _rowBoundTightener->getRowTightenings( rowTightenings );

    for ( const auto &tightening : rowTightenings )
    {
        if ( tightening._type == Tightening::LB )
            _tableau->tightenLowerBound( tightening._variable, tightening._value );
        else
            _tableau->tightenUpperBound( tightening._variable, tightening._value );
    }
}

void Engine::applyAllConstraintTightenings()
{
    List<Tightening> entailedTightenings;

    _constraintBoundTightener->getConstraintTightenings( entailedTightenings );

    for ( const auto &tightening : entailedTightenings )
    {
        _statistics.incNumBoundsProposedByPlConstraints();

        if ( tightening._type == Tightening::LB )
            _tableau->tightenLowerBound( tightening._variable, tightening._value );
        else
            _tableau->tightenUpperBound( tightening._variable, tightening._value );
    }
}

void Engine::applyAllBoundTightenings()
{
    struct timespec start = TimeUtils::sampleMicro();

    applyAllRowTightenings();
    applyAllConstraintTightenings();

    struct timespec end = TimeUtils::sampleMicro();
    _statistics.addTimeForApplyingStoredTightenings( TimeUtils::timePassed( start, end ) );
}

bool Engine::applyAllValidConstraintCaseSplits()
{
    struct timespec start = TimeUtils::sampleMicro();

    bool appliedSplit = false;
    for ( auto &constraint : _plConstraints )
        if ( applyValidConstraintCaseSplit( constraint ) )
            appliedSplit = true;

    struct timespec end = TimeUtils::sampleMicro();
    _statistics.addTimeForValidCaseSplit( TimeUtils::timePassed( start, end ) );

    return appliedSplit;
}

bool Engine::applyValidConstraintCaseSplit( PiecewiseLinearConstraint *constraint )
{
    if ( constraint->isActive() && constraint->phaseFixed() )
    {
        String constraintString;
        constraint->dump( constraintString );
        ENGINE_LOG( Stringf( "A constraint has become valid. Dumping constraint: %s",
                             constraintString.ascii() ).ascii() );

        constraint->setActiveConstraint( false );
        PiecewiseLinearCaseSplit validSplit = constraint->getValidCaseSplit();
        _smtCore.recordImpliedValidSplit( validSplit );
        applySplit( validSplit );
        ++_numPlConstraintsDisabledByValidSplits;

        return true;
    }

    return false;
}

bool Engine::shouldCheckDegradation()
{
    return _statistics.getNumMainLoopIterations() %
        GlobalConfiguration::DEGRADATION_CHECKING_FREQUENCY == 0 ;
}

bool Engine::highDegradation()
{
    struct timespec start = TimeUtils::sampleMicro();

    double degradation = _degradationChecker.computeDegradation( *_tableau );
    _statistics.setCurrentDegradation( degradation );

    bool result = FloatUtils::gt( degradation, GlobalConfiguration::DEGRADATION_THRESHOLD );

    struct timespec end = TimeUtils::sampleMicro();
    _statistics.addTimeForDegradationChecking( TimeUtils::timePassed( start, end ) );

    // Debug
    if ( result )
        printf( "High degradation found!\n" );
    //

    return result;
}

void Engine::tightenBoundsOnConstraintMatrix()
{
    struct timespec start = TimeUtils::sampleMicro();

    if ( _statistics.getNumMainLoopIterations() %
         GlobalConfiguration::BOUND_TIGHTING_ON_CONSTRAINT_MATRIX_FREQUENCY == 0 )
    {
        _rowBoundTightener->examineConstraintMatrix( true );
        _statistics.incNumBoundTighteningOnConstraintMatrix();
    }

    struct timespec end = TimeUtils::sampleMicro();
    _statistics.addTimeForConstraintMatrixBoundTightening( TimeUtils::timePassed( start, end ) );
}

void Engine::explicitBasisBoundTightening()
{
    struct timespec start = TimeUtils::sampleMicro();

    bool saturation = GlobalConfiguration::EXPLICIT_BOUND_TIGHTENING_UNTIL_SATURATION;

    _statistics.incNumBoundTighteningsOnExplicitBasis();

    switch ( GlobalConfiguration::EXPLICIT_BASIS_BOUND_TIGHTENING_TYPE )
    {
    case GlobalConfiguration::COMPUTE_INVERTED_BASIS_MATRIX:
        _rowBoundTightener->examineInvertedBasisMatrix( saturation );
        break;

    case GlobalConfiguration::USE_IMPLICIT_INVERTED_BASIS_MATRIX:
        _rowBoundTightener->examineImplicitInvertedBasisMatrix( saturation );
        break;
    }

    struct timespec end = TimeUtils::sampleMicro();
    _statistics.addTimeForExplicitBasisBoundTightening( TimeUtils::timePassed( start, end ) );
}

void Engine::performPrecisionRestoration( PrecisionRestorer::RestoreBasics restoreBasics )
{
    struct timespec start = TimeUtils::sampleMicro();

    // debug
    double before = _degradationChecker.computeDegradation( *_tableau );
    //

    _precisionRestorer.restorePrecision( *this, *_tableau, _smtCore, restoreBasics );
    struct timespec end = TimeUtils::sampleMicro();
    _statistics.addTimeForPrecisionRestoration( TimeUtils::timePassed( start, end ) );

    _statistics.incNumPrecisionRestorations();
    _rowBoundTightener->clear();
    _constraintBoundTightener->resetBounds();

    // debug
    double after = _degradationChecker.computeDegradation( *_tableau );
    if ( _verbosity > 0 )
        printf( "Performing precision restoration. Degradation before: %.15lf. After: %.15lf\n",
                before,
                after );
    //

    if ( highDegradation() && ( restoreBasics == PrecisionRestorer::RESTORE_BASICS ) )
    {
        // First round, with basic restoration, still resulted in high degradation.
        // Try again!
        start = TimeUtils::sampleMicro();
        _precisionRestorer.restorePrecision( *this, *_tableau, _smtCore,
                                             PrecisionRestorer::DO_NOT_RESTORE_BASICS );
        end = TimeUtils::sampleMicro();
        _statistics.addTimeForPrecisionRestoration( TimeUtils::timePassed( start, end ) );
        _statistics.incNumPrecisionRestorations();

        _rowBoundTightener->clear();
        _constraintBoundTightener->resetBounds();

        // debug
        double afterSecond = _degradationChecker.computeDegradation( *_tableau );
        if ( _verbosity > 0 )
            printf( "Performing 2nd precision restoration. Degradation before: %.15lf. After: %.15lf\n",
                    after,
                    afterSecond );

        if ( highDegradation() )
            throw MarabouError( MarabouError::RESTORATION_FAILED_TO_RESTORE_PRECISION );
    }
}

void Engine::storeInitialEngineState()
{
    if ( !_initialStateStored )
    {
        _precisionRestorer.storeInitialEngineState( *this );
        _initialStateStored = true;
    }
}

bool Engine::basisRestorationNeeded() const
{
    return
        _basisRestorationRequired == Engine::STRONG_RESTORATION_NEEDED ||
        _basisRestorationRequired == Engine::WEAK_RESTORATION_NEEDED;
}

const Statistics *Engine::getStatistics() const
{
    return &_statistics;
}

InputQuery *Engine::getInputQuery()
{
    return &_preprocessedQuery;
}

void Engine::checkBoundCompliancyWithDebugSolution()
{
    if ( _smtCore.checkSkewFromDebuggingSolution() )
    {
        // The stack is compliant, we should not have learned any non-compliant bounds
        for ( const auto &var : _preprocessedQuery._debuggingSolution )
        {
            // printf( "Looking at var %u\n", var.first );

            if ( FloatUtils::gt( _tableau->getLowerBound( var.first ), var.second, 1e-5 ) )
            {
                printf( "Error! The stack is compliant, but learned an non-compliant bound: "
                        "Solution for x%u is %.15lf, but learned lower bound %.15lf\n",
                        var.first,
                        var.second,
                        _tableau->getLowerBound( var.first ) );

                throw MarabouError( MarabouError::DEBUGGING_ERROR );
            }

            if ( FloatUtils::lt( _tableau->getUpperBound( var.first ), var.second, 1e-5 ) )
            {
                printf( "Error! The stack is compliant, but learned an non-compliant bound: "
                        "Solution for %u is %.15lf, but learned upper bound %.15lf\n",
                        var.first,
                        var.second,
                        _tableau->getUpperBound( var.first ) );

                throw MarabouError( MarabouError::DEBUGGING_ERROR );
            }
        }
    }
}

void Engine::quitSignal()
{
    _quitRequested = true;
}

Engine::ExitCode Engine::getExitCode() const
{
    return _exitCode;
}

std::atomic_bool *Engine::getQuitRequested()
{
    return &_quitRequested;
}

List<unsigned> Engine::getInputVariables() const
{
    return _preprocessedQuery.getInputVariables();
}

void Engine::performSymbolicBoundTightening()
{
    if ( ( !GlobalConfiguration::USE_SYMBOLIC_BOUND_TIGHTENING ) ||
         ( !_networkLevelReasoner ) )
        return;

    struct timespec start = TimeUtils::sampleMicro();

    unsigned numTightenedBounds = 0;

    // Step 1: tell the NLR about the current bounds
    _networkLevelReasoner->obtainCurrentBounds();

    // Step 2: perform SBT
    _networkLevelReasoner->symbolicBoundPropagation();

    // Step 3: Extract the bounds
    List<Tightening> tightenings;
    _networkLevelReasoner->getConstraintTightenings( tightenings );

    for ( const auto &tightening : tightenings )
    {

        if ( tightening._type == Tightening::LB &&
             FloatUtils::gt ( tightening._value, _tableau->getLowerBound( tightening._variable ) ) )
        {
            _tableau->tightenLowerBound( tightening._variable, tightening._value );
            ++numTightenedBounds;
        }

        if ( tightening._type == Tightening::UB &&
             FloatUtils::lt ( tightening._value, _tableau->getUpperBound( tightening._variable ) ) )
        {
            _tableau->tightenUpperBound( tightening._variable, tightening._value );
            ++numTightenedBounds;
        }
    }

    struct timespec end = TimeUtils::sampleMicro();
    _statistics.addTimeForSymbolicBoundTightening( TimeUtils::timePassed( start, end ) );
    _statistics.incNumTighteningsFromSymbolicBoundTightening( numTightenedBounds );
}

bool Engine::shouldExitDueToTimeout( unsigned timeout ) const
{
    enum {
        MILLISECONDS_TO_SECONDS = 1000,
    };

    // A timeout value of 0 means no time limit
    if ( timeout == 0 )
        return false;

    return _statistics.getTotalTime() / MILLISECONDS_TO_SECONDS > timeout;
}

void Engine::reset()
{
    resetStatistics();
    clearViolatedPLConstraints();
    resetSmtCore();
    resetBoundTighteners();
    resetExitCode();
}

void Engine::resetStatistics()
{
    Statistics statistics;
    _statistics = statistics;
    _smtCore.setStatistics( &_statistics );
    _tableau->setStatistics( &_statistics );
    _rowBoundTightener->setStatistics( &_statistics );
    _constraintBoundTightener->setStatistics( &_statistics );
    _preprocessor.setStatistics( &_statistics );
    _activeEntryStrategy->setStatistics( &_statistics );

    _statistics.stampStartingTime();
}

void Engine::clearViolatedPLConstraints()
{
    _violatedPlConstraints.clear();
    _plConstraintToFix = NULL;
}

void Engine::resetSmtCore()
{
    _smtCore.freeMemory();
    _smtCore = SmtCore( this );
}

void Engine::resetExitCode()
{
    _exitCode = Engine::NOT_DONE;
}

void Engine::resetBoundTighteners()
{
    _constraintBoundTightener->resetBounds();
    _rowBoundTightener->resetBounds();
}

void Engine::warmStart()
{
    // An NLR is required for a warm start
    if ( !_networkLevelReasoner )
        return;

    // First, choose an arbitrary assignment for the input variables
    unsigned numInputVariables = _preprocessedQuery.getNumInputVariables();
    unsigned numOutputVariables = _preprocessedQuery.getNumOutputVariables();

    if ( numInputVariables == 0 )
    {
        // Trivial case: all inputs are fixed, nothing to evaluate
        return;
    }

    double *inputAssignment = new double[numInputVariables];
    double *outputAssignment = new double[numOutputVariables];

    for ( unsigned i = 0; i < numInputVariables; ++i )
    {
        unsigned variable = _preprocessedQuery.inputVariableByIndex( i );
        inputAssignment[i] = _tableau->getLowerBound( variable );
    }

    // Evaluate the network for this assignment
    _networkLevelReasoner->evaluate( inputAssignment, outputAssignment );

    // Try to update as many variables as possible to match their assignment
    for ( unsigned i = 0; i < _networkLevelReasoner->getNumberOfLayers(); ++i )
    {
        const NLR::Layer *layer = _networkLevelReasoner->getLayer( i );
        unsigned layerSize = layer->getSize();
        const double *assignment = layer->getAssignment();

        for ( unsigned j = 0; j < layerSize; ++j )
        {
            if ( layer->neuronHasVariable( j ) )
            {
                unsigned variable = layer->neuronToVariable( j );
                if ( !_tableau->isBasic( variable ) )
                    _tableau->setNonBasicAssignment( variable, assignment[j], false );
            }
        }
    }

    // We did what we could for the non-basics; now let the tableau compute
    // the basic assignment
    _tableau->computeAssignment();

    delete[] outputAssignment;
    delete[] inputAssignment;
}

void Engine::checkOverallProgress()
{
    // Get fresh statistics
    unsigned numVisitedStates = _statistics.getNumVisitedTreeStates();
    unsigned long long currentIteration = _statistics.getNumMainLoopIterations();

    if ( numVisitedStates > _lastNumVisitedStates )
    {
        // Progress has been made
        _lastNumVisitedStates = numVisitedStates;
        _lastIterationWithProgress = _statistics.getNumMainLoopIterations();
    }
    else
    {
        // No progress has been made. If it's been too long, request a restoration
        if ( currentIteration >
             _lastIterationWithProgress +
             GlobalConfiguration::MAX_ITERATIONS_WITHOUT_PROGRESS )
        {
            ENGINE_LOG( "checkOverallProgress detected cycling. Requesting a precision restoration" );
            _basisRestorationRequired = Engine::STRONG_RESTORATION_NEEDED;
            _lastIterationWithProgress = currentIteration;
        }
    }
}

void Engine::updateDirections()
{
    if ( GlobalConfiguration::USE_POLARITY_BASED_DIRECTION_HEURISTICS )
        for ( const auto &constraint : _plConstraints )
            if ( constraint->supportPolarity() &&
                 constraint->isActive() && !constraint->phaseFixed() )
                constraint->updateDirection();
}

void Engine::updateScores()
{
    DivideStrategy _strategyToUse = (_preprocessedQuery.getDivideStrategy() == DivideStrategy::None) ? GlobalConfiguration::SPLITTING_HEURISTICS : _preprocessedQuery.getDivideStrategy();

    if ( _networkLevelReasoner &&
         _strategyToUse == DivideStrategy::Polarity )
    {
        // We find the earliest K ReLUs that have not been fixed, update
        // their scores, and pop them to the _candidatePlConstraints
        // K is equal to GlobalConfiguration::RUNTIME_ESTIMATE_THRESHOLD
        ENGINE_LOG( Stringf( "Using polarity heuristics..." ).ascii() );

        List<PiecewiseLinearConstraint *> constraints =
            _networkLevelReasoner->getConstraintsInTopologicalOrder();

        for ( auto &plConstraint : constraints )
        {
            if ( plConstraint->isActive() && !plConstraint->phaseFixed() )
            {
                plConstraint->updateScore();
                _candidatePlConstraints.insert( plConstraint );
                if ( _candidatePlConstraints.size() >=
                     GlobalConfiguration::RUNTIME_ESTIMATE_THRESHOLD )
                    break;
            }
        }
    }
    else if ( _strategyToUse ==
              DivideStrategy::EarliestReLU )
    {
        for ( const auto plConstraint : _plConstraints )
        {
            if ( plConstraint->isActive() && !plConstraint->phaseFixed() )
            {
                plConstraint->updateScore();
                _candidatePlConstraints.insert( plConstraint );
            }
        }
    }
    else
    {
        // Otherwise, we fall back to the constraint violation based
        // splitting heuristic - nothing to do.
    }
}

PiecewiseLinearConstraint *Engine::pickSplitPLConstraint()
{
    _candidatePlConstraints.clear();
    ENGINE_LOG( Stringf( "Picking a split PLConstraint..." ).ascii() );
    updateScores();
    ENGINE_LOG( Stringf( "Done updating scores..." ).ascii() );
    if ( _candidatePlConstraints.empty() )
    {
        ENGINE_LOG( Stringf( "Unable to pick using the current strategy..." ).ascii() );
        return NULL;
    }
    else
    {
        auto constraint = *_candidatePlConstraints.begin();
        ENGINE_LOG( Stringf( "Picked..." ).ascii() );
        return constraint;
    }
}

void Engine::setConstraintViolationThreshold( unsigned threshold )
{
    _smtCore.setConstraintViolationThreshold( threshold );
}

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//
