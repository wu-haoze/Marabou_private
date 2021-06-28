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
#include "SnCDivideStrategy.h"
#include "DnCManager.h"
#include "DnCWorker.h"
#include "GetCPUData.h"
#include "GlobalConfiguration.h"
#include "LargestIntervalDivider.h"
#include "MStringf.h"
#include "MarabouError.h"
#include "Options.h"
#include "PiecewiseLinearCaseSplit.h"
#include "PolarityBasedDivider.h"
#include "QueryDivider.h"
#include "TimeUtils.h"
#include "Vector.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

void DnCManager::dncSolve( WorkerQueue *workload, std::shared_ptr<Engine> engine,
                           std::unique_ptr<InputQuery> inputQuery,
                           std::atomic_uint &numUnsolvedSubQueries,
                           std::atomic_bool &shouldQuitSolving,
                           unsigned threadId, unsigned onlineDivides,
                           float timeoutFactor, SnCDivideStrategy divideStrategy,
                           bool , unsigned verbosity )
{
    unsigned cpuId = 0;
    (void) threadId;
    (void) cpuId;

    getCPUId( cpuId );
    DNC_MANAGER_LOG( Stringf( "Thread #%u on CPU %u", threadId, cpuId ).ascii() );

    engine->processInputQuery( *inputQuery, false );

    DnCWorker worker( workload, engine, std::ref( numUnsolvedSubQueries ),
                      std::ref( shouldQuitSolving ), threadId, onlineDivides,
                      timeoutFactor, divideStrategy, verbosity );
    while ( !shouldQuitSolving.load() )
    {
        worker.popOneSubQueryAndSolve();
    }
}

DnCManager::DnCManager( InputQuery *inputQuery )
    : _baseInputQuery( *inputQuery )
    , _exitCode( DnCManager::NOT_DONE )
    , _workload( NULL )
    , _timeoutReached( false )
    , _numUnsolvedSubQueries( 0 )
    , _verbosity( Options::get()->getInt( Options::VERBOSITY ) )
{
    SnCDivideStrategy sncSplittingStrategy = Options::get()->getSnCDivideStrategy();
    if ( sncSplittingStrategy == SnCDivideStrategy::Auto )
    {
        DNC_MANAGER_LOG( Stringf( "Deciding splitting strategy automatically...\n" ).ascii() );
        if ( inputQuery->getNumInputVariables() <
             GlobalConfiguration::INTERVAL_SPLITTING_THRESHOLD )
        {
            DNC_MANAGER_LOG( Stringf( "\tUsing Largest Interval Heuristics\n" ).ascii() );
            _sncSplittingStrategy = SnCDivideStrategy::LargestInterval;
        }
        else
        {
            DNC_MANAGER_LOG( Stringf( "\tUsing Polarity-based Heuristics\n" ).ascii() );
            _sncSplittingStrategy = SnCDivideStrategy::Polarity;
        }
    }
    else
        _sncSplittingStrategy = sncSplittingStrategy;
}

DnCManager::~DnCManager()
{
    freeMemoryIfNeeded();
}

void DnCManager::freeMemoryIfNeeded()
{
    if ( _workload )
    {
        SubQuery *subQuery = NULL;
        while ( !_workload->empty() )
        {
            _workload->pop( subQuery );
            delete subQuery;
        }

        delete _workload;
        _workload = NULL;
    }
}

void DnCManager::solve( unsigned resources, unsigned id )
{
    enum {
        MICROSECONDS_IN_SECOND = 1000000
    };

    unsigned timeoutInSeconds = Options::get()->getInt( Options::TIMEOUT );
    unsigned long long timeoutInMicroSeconds = (unsigned long long)timeoutInSeconds * (unsigned long long)MICROSECONDS_IN_SECOND;
    DNC_MANAGER_LOG( Stringf( "timeout in micro seconds: %llu", timeoutInMicroSeconds ).ascii());

    struct timespec startTime = TimeUtils::sampleMicro();

    unsigned numWorkers = resources;
    // Preprocess the input query and create an engine for each of the threads
    if ( !createEngines( numWorkers, id ) )
    {
        _exitCode = DnCManager::UNSAT;
        return;
    }

    // Prepare the mechanism through which we can ask the engines to quit
    List<std::atomic_bool *> quitThreads;
    for ( unsigned i = 0; i < numWorkers; ++i )
        quitThreads.append( _engines[i]->getQuitRequested() );

    // Partition the input query into initial subqueries, and place these
    // queries in the queue
    _workload = new WorkerQueue( 0 );
    if ( !_workload )
        throw MarabouError( MarabouError::ALLOCATION_FAILED, "DnCManager::workload" );

    SubQueries subQueries;
    initialDivide( subQueries );

    // Create objects shared across workers
    _numUnsolvedSubQueries = subQueries.size();
    std::atomic_bool shouldQuitSolving( false );
    WorkerQueue *workload = new WorkerQueue( 0 );
    for ( auto &subQuery : subQueries )
    {
        if ( !workload->push( subQuery ) )
        {
            // This should never happen
            ASSERT( false );
        }
    }

    unsigned onlineDivides = Options::get()->getInt( Options::NUM_ONLINE_DIVIDES );
    float timeoutFactor = Options::get()->getFloat( Options::TIMEOUT_FACTOR );
    bool restoreTreeStates = Options::get()->getBool( Options::RESTORE_TREE_STATES );

    // Spawn threads and start solving
    std::list<std::thread> threads;
    for ( unsigned threadId = 0; threadId < numWorkers; ++threadId )
    {
        // Get the processed input query from the base engine
        auto inputQuery = std::unique_ptr<InputQuery>
            ( new InputQuery( *( _baseEngine->getInputQuery() ) ) );
        threads.push_back( std::thread( dncSolve, workload, _engines[ threadId ],
                                        std::move( inputQuery ),
                                        std::ref( _numUnsolvedSubQueries ),
                                        std::ref( shouldQuitSolving ),
                                        threadId, onlineDivides,
                                        timeoutFactor, _sncSplittingStrategy,
                                        restoreTreeStates, _verbosity ) );
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
        return "sat";
    case DnCManager::UNSAT:
        return "unsat";
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

void DnCManager::getSolution( std::map<int, double> &ret,
                              InputQuery &inputQuery )
{
    ASSERT( _engineWithSATAssignment != nullptr );
    InputQuery *solvedInputQuery = _engineWithSATAssignment->getInputQuery();
    _engineWithSATAssignment->extractSolution( *( solvedInputQuery ) );

    double value;
    for ( unsigned i = 0; i < inputQuery.getNumberOfVariables(); ++i )
    {
        if ( _baseEngine->preprocessingEnabled() )
        {
            const Preprocessor *basePreprocessor = _baseEngine->getPreprocessor();

            unsigned variable = i;
            while ( basePreprocessor->variableIsMerged( variable ) )
                variable = basePreprocessor->getMergedIndex( variable );

            if ( basePreprocessor->variableIsFixed( variable ) )
            {
                value = basePreprocessor->getFixedValue( variable );
                inputQuery.setSolutionValue( i, value );
                inputQuery.setLowerBound( i, value );
                inputQuery.setUpperBound( i, value );
                ret[i] = value;
                continue;
            }

            variable = basePreprocessor->getNewIndex( variable );
            value = solvedInputQuery->getSolutionValue( variable );
            inputQuery.setSolutionValue( i, value );
            ret[i] = value;
        }
        else
        {
            double value = solvedInputQuery->getSolutionValue( i );
            inputQuery.setSolutionValue( i, value );
            ret[i] = value;
        }
    }

    return;
}

void DnCManager::printResult()
{
    std::cout << std::endl;
    switch ( _exitCode )
    {
    case DnCManager::SAT:
    {
        std::cout << "sat\n" << std::endl;

        ASSERT( _engineWithSATAssignment != nullptr );

        InputQuery *inputQuery = _engineWithSATAssignment->getInputQuery();
        _engineWithSATAssignment->extractSolution( *( inputQuery ) );
        if ( _engineWithSATAssignment->_solutionFoundAndStoredInOriginalQuery )
            return;
        Vector<double> inputVector( inputQuery->getNumInputVariables() );
        Vector<double> outputVector( inputQuery->getNumOutputVariables() );
        double *inputs( inputVector.data() );
        double *outputs( outputVector.data() );

        //printf( "Input assignment:\n" );
        for ( unsigned i = 0; i < inputQuery->getNumInputVariables(); ++i )
        {
            //printf( "x%u = %lf\n", i, inputQuery->getSolutionValue( inputQuery->inputVariableByIndex( i ) ) );
            inputs[i] = inputQuery->getSolutionValue( inputQuery->inputVariableByIndex( i ) );
        }

        NLR::NetworkLevelReasoner *nlr = inputQuery->getNetworkLevelReasoner();
        if ( nlr )
            nlr->evaluate( inputs, outputs );

        printf( "\n" );
        printf( "Output:\n" );
        for ( unsigned i = 0; i < inputQuery->getNumOutputVariables(); ++i )
        {
            if ( nlr )
                printf( "y%u = %lf\n", i, outputs[i] );
            else
                printf( "y%u = %lf\n", i, inputQuery->getSolutionValue( inputQuery->outputVariableByIndex( i ) ) );
        }
        printf( "\n" );
        break;
    }
    case DnCManager::UNSAT:
        std::cout << "unsat" << std::endl;
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

bool DnCManager::createEngines( unsigned numberOfEngines, unsigned id )
{
    // Create the base engine
    _baseEngine = std::make_shared<Engine>();
    _baseEngine->setVerbosity( 0 );
    _baseEngine->_numWorkers = numberOfEngines;

    _baseEngine->processInputQuery( _baseInputQuery, false );

    // Create engines for each thread
    for ( unsigned i = 0; i < numberOfEngines; ++i )
    {
        auto engine = std::make_shared<Engine>();
        engine->setVerbosity( 0 );
        if ( id == 0 || id == 1 )
            engine->setSeed( 1995 );
        else if ( id == 2 || id == 3 )
            engine->setSeed( 1219 );
        if ( id == 0 || id == 2 )
            engine->setBranchingHeuristic( DivideStrategy::Polarity );
        else if ( id == 1 || id == 3 )
            engine->setBranchingHeuristic( DivideStrategy::SOI );

        engine->_numWorkers = 1;
        _engines.append( engine );
    }

    return true;
}

void DnCManager::initialDivide( SubQueries &subQueries )
{
    auto split = std::unique_ptr<PiecewiseLinearCaseSplit>
        ( new PiecewiseLinearCaseSplit() );
    std::unique_ptr<QueryDivider> queryDivider = nullptr;
    if ( _sncSplittingStrategy == SnCDivideStrategy::Polarity )
    {
        queryDivider = std::unique_ptr<QueryDivider>
            ( new PolarityBasedDivider( _baseEngine ) );
    }
    else // Default is LargestInterval
    {
        const List<unsigned> inputVariables( _baseEngine->getInputVariables() );
        queryDivider = std::unique_ptr<QueryDivider>
            ( new LargestIntervalDivider( inputVariables ) );
        InputQuery *inputQuery = _baseEngine->getInputQuery();
        // Add bound as equations for each input variable
        for ( const auto &variable : inputVariables )
        {
            double lb = inputQuery->getLowerBounds()[variable];
            double ub = inputQuery->getUpperBounds()[variable];
            split->storeBoundTightening( Tightening( variable, lb,
                                                     Tightening::LB ) );
            split->storeBoundTightening( Tightening( variable, ub,
                                                     Tightening::UB ) );
        }
    }

    unsigned initialDivides = Options::get()->getInt( Options::NUM_INITIAL_DIVIDES );
    unsigned initialTimeout = Options::get()->getInt( Options::INITIAL_TIMEOUT );

    String queryId;

    // Create subqueries
    queryDivider->createSubQueries( pow( 2, initialDivides ), queryId, 0,
                                    *split, initialTimeout, subQueries );
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
