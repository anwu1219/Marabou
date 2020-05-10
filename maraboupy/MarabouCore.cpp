/*********************                                                        */
/*! \file MarabouCore.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Christopher Lazarus, Andrew Wu, Shantanu Thakoor
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

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <map>
#include <vector>
#include <set>
#include <string>
#include <utility>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include "AcasParser.h"
#include "BiasStrategy.h"
#include "DnCManager.h"
#include "DivideStrategy.h"
#include "Engine.h"
#include "FloatUtils.h"
#include "File.h"
#include "FixedReluParser.h"
#include "InputQuery.h"
#include "LookAheadPreprocessor.h"
#include "MarabouError.h"
#include "Map.h"
#include "MString.h"
#include "MStringf.h"
#include "MaxConstraint.h"
#include "PiecewiseLinearConstraint.h"
#include "PropertyParser.h"
#include "QueryLoader.h"
#include "ReluConstraint.h"
#include "Set.h"

#ifdef _WIN32
#define STDOUT_FILENO 1
#else
#include <unistd.h>
#endif

namespace py = pybind11;

int redirectOutputToFile(std::string outputFilePath){
    // Redirect standard output to a file
    int outputFile = open(outputFilePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if ( outputFile < 0 )
    {
        printf( "Error redirecting output to file\n");
        exit( 1 );
    }

    int outputStream = dup( STDOUT_FILENO );
    if (outputStream < 0)
    {
        printf( "Error duplicating standard output\n" );
        exit(1);
    }

    if ( dup2( outputFile, STDOUT_FILENO ) < 0 )
    {
        printf("Error duplicating to standard output\n");
        exit(1);
    }

    close( outputFile );
    return outputStream;
}

void restoreOutputStream(int outputStream)
{
    // Restore standard output
    fflush( stdout );
    if (dup2( outputStream, STDOUT_FILENO ) < 0){
        printf( "Error restoring output stream\n" );
        exit( 1 );
    }
    close(outputStream);
}

void addReluConstraint(InputQuery& ipq, unsigned var1, unsigned var2, unsigned id){
    PiecewiseLinearConstraint* r = new ReluConstraint(var1, var2, id);
    ipq.addPiecewiseLinearConstraint(r);
}

void addMaxConstraint(InputQuery& ipq, std::set<unsigned> elements, unsigned v){
    Set<unsigned> e;
    for(unsigned var: elements)
        e.insert(var);
    PiecewiseLinearConstraint* m = new MaxConstraint(v, e);
    ipq.addPiecewiseLinearConstraint(m);
}

void setDirection(InputQuery& ipq, unsigned id, unsigned phase){
    ipq.setDirection(id, phase);
}

void createInputQuery(InputQuery &inputQuery, std::string networkFilePath, std::string propertyFilePath){
  AcasParser* acasParser = new AcasParser( String(networkFilePath) );
  acasParser->generateQuery( inputQuery );
  String propertyFilePathM = String(propertyFilePath);
  if ( propertyFilePath != "" )
    {
      printf( "Property: %s\n", propertyFilePathM.ascii() );
      PropertyParser().parse( propertyFilePathM, inputQuery );
    }
  else
    printf( "Property: None\n" );
}

struct MarabouOptions {
    MarabouOptions()
        : _numWorkers( 4 )
        , _initialTimeout( -1 )
        , _initialDivides( 0 )
        , _onlineDivides( 2 )
        , _timeoutInSeconds( 0 )
        , _focusLayer( 0 )
        , _timeoutFactor( 1.5 )
        , _verbosity( 2 )
        , _dnc( false )
        , _restoreTreeStates( true )
        , _lookAheadPreprocessing( false )
        , _preprocessOnly( false )
        , _divideStrategy( "auto" )
        , _biasStrategy( "estimate" )
        , _maxDepth( 4 )
	, _maxTreeDepth( 10 )  
	, _splitThreshold( 20 )
    {};

    unsigned _numWorkers;
    int _initialTimeout;
    unsigned _initialDivides;
    unsigned _onlineDivides;
    unsigned _timeoutInSeconds;
    unsigned _focusLayer;
    float _timeoutFactor;
    unsigned _verbosity;
    bool _dnc;
    bool _restoreTreeStates;
    bool _lookAheadPreprocessing;
    bool _preprocessOnly;
    std::string _divideStrategy;
    std::string _biasStrategy;
    unsigned _maxDepth;
    unsigned _maxTreeDepth;
    unsigned _splitThreshold;
};

BiasStrategy setBiasStrategyFromOptions( const String strategy )
{
    if ( strategy == "centroid" )
        return BiasStrategy::Centroid;
    else if ( strategy == "sampling" )
        return BiasStrategy::Sampling;
    else if ( strategy == "random" )
        return BiasStrategy::Random;
    else if ( strategy == "estimate" )
        return BiasStrategy::Estimate;
    else
        {
            printf ("Unknown divide strategy, using default (centroid).\n");
            return BiasStrategy::Estimate;
        }
}

DivideStrategy setDivideStrategyFromOptions( const String strategy )
{
    if ( strategy == "split-relu" )
        return DivideStrategy::SplitRelu;
    else if ( strategy == "largest-interval" )
        return DivideStrategy::LargestInterval;
    else
        {
            printf ("Unknown divide strategy, using default (SplitRelu).\n");
            return DivideStrategy::SplitRelu;
        }
}

/* The default parameters here are just for readability, you should specify
 * them in the to make them work*/
std::pair<std::map<int, double>, Statistics> solve(InputQuery &inputQuery, MarabouOptions &options,
                                                   std::string summaryFilePath, std::string fixedReluFilePath,
                                                   std::string redirect="" )
{
    // Arguments: InputQuery object, filename to redirect output
    // Returns: map from variable number to value
    std::map<int, double> ret;
    Statistics retStats;
    int output=-1;
    if(redirect.length()>0)
        output=redirectOutputToFile(redirect);
    try{
        bool verbosity = options._verbosity;
        unsigned timeoutInSeconds = options._timeoutInSeconds;
        bool dnc = options._dnc;
        bool lookAheadPreprocessing = options._lookAheadPreprocessing;
        bool preprocessOnly = options._preprocessOnly;
        unsigned focusLayer = options._focusLayer;
        unsigned numWorkers = options._numWorkers;
        int initialTimeoutInt = options._initialTimeout;
        unsigned initialTimeout = 0;
        if ( initialTimeoutInt < 0 )
            initialTimeout = inputQuery.getPiecewiseLinearConstraints().size() / 10;
        else
            initialTimeout = static_cast<unsigned>(initialTimeoutInt);
	
	unsigned splitThreshold = options._splitThreshold;

        DivideStrategy divideStrategy = DivideStrategy::SplitRelu;
        if ( options._divideStrategy == "auto" )
        {
            if ( inputQuery.getInputVariables().size() < 10  )
                divideStrategy = DivideStrategy::LargestInterval;
            else
                divideStrategy = DivideStrategy::SplitRelu;
        }
        else
            divideStrategy = setDivideStrategyFromOptions( options._divideStrategy );
        BiasStrategy biasStrategy = setBiasStrategyFromOptions( options._biasStrategy );

        Engine engine;
        engine.setVerbosity(verbosity);

        if(!engine.processInputQuery(inputQuery)) return std::make_pair(ret, *(engine.getStatistics()));

        Map<unsigned, unsigned> idToPhase;

        if ( fixedReluFilePath != "" )
        {
            String fixedReluFilePathM = String( fixedReluFilePath );
            FixedReluParser().parse( fixedReluFilePathM, idToPhase );
        }

        if ( lookAheadPreprocessing )
        {
            struct timespec start = TimeUtils::sampleMicro();
            auto lookAheadPreprocessor = new LookAheadPreprocessor
	      ( numWorkers, *(engine.getInputQuery()), splitThreshold );
	    List<unsigned> maxTimes;
            bool feasible = lookAheadPreprocessor->run( idToPhase, maxTimes );
            struct timespec end = TimeUtils::sampleMicro();
            unsigned long long totalElapsed = TimeUtils::timePassed( start, end );
            if ( summaryFilePath != "" )
            {
                File summaryFile( summaryFilePath + ".preprocess" );
                summaryFile.open( File::MODE_WRITE_TRUNCATE );

                // Field #1: result
                summaryFile.write( ( feasible ? "UNKNOWN" : "UNSAT" ) );

                // Field #2: total elapsed time
                summaryFile.write( Stringf( " %u ", totalElapsed / 1000000 ) ); // In seconds

                // Field #3: number of fixed relus by look ahead preprocessing
                summaryFile.write( Stringf( "%u ", idToPhase.size() ) );

		for ( const auto& maxTime : maxTimes )
		    summaryFile.write( Stringf( "%u ", maxTime ) );
                summaryFile.write( "\n" );
            }
            if ( summaryFilePath != "" )
            {
                File fixedFile( summaryFilePath + ".fixed" );
                fixedFile.open( File::MODE_WRITE_TRUNCATE );
                for ( const auto entry : idToPhase )
                    fixedFile.write( Stringf( "%u %u\n", entry.first, entry.second ) );
            }

            if ( (!feasible) || preprocessOnly ) return std::make_pair(ret, *(engine.getStatistics()));
        }
        if ( dnc )
        {
            unsigned initialDivides = options._initialDivides;
            unsigned onlineDivides = options._onlineDivides;
            float timeoutFactor = options._timeoutFactor;
            bool restoreTreeStates = options._restoreTreeStates;
            unsigned maxDepth = options._maxDepth;

            std::cout << "Initial Divides: " << initialDivides << std::endl;
            std::cout << "Initial Timeout: " << initialTimeout << std::endl;
            std::cout << "Number of Workers: " << numWorkers << std::endl;
            std::cout << "Online Divides: " << onlineDivides  << std::endl;
            std::cout << "Verbosity: " << verbosity << std::endl;
            std::cout << "Timeout: " << timeoutInSeconds  << std::endl;
            std::cout << "Timeout Factor: "  << timeoutFactor << std::endl;
            std::cout << "Divide Strategy: " << ( divideStrategy ==
                                                  DivideStrategy::LargestInterval ?
                                                  "Largest Interval" : "Split Relu" )
                      << std::endl;
            std::cout << "Focus Layers: " << focusLayer << std::endl;
            std::cout << "Max Depth: " << maxDepth << std::endl;
            std::cout << "Perform tree state restoration: " << ( restoreTreeStates ? "Yes" : "No" )
                      << std::endl;

            auto dncManager = std::unique_ptr<DnCManager>
	      ( new DnCManager( numWorkers, initialDivides, initialTimeout, onlineDivides,
				timeoutFactor, divideStrategy,
				engine.getInputQuery(), verbosity, idToPhase,
				focusLayer, biasStrategy, maxDepth ) );
	    
	    dncManager->setConstraintViolationThreshold( splitThreshold );
            dncManager->solve( timeoutInSeconds, restoreTreeStates );
            switch ( dncManager->getExitCode() )
	    {
	    case DnCManager::SAT:
	    {
	      retStats = Statistics();
	      dncManager->getSolution( ret );
	      break;
            }
            case DnCManager::TIMEOUT:
            {
                retStats = Statistics();
                retStats.timeout();
                return std::make_pair( ret, retStats );
            }
            default:
                return std::make_pair( ret, Statistics() ); // TODO: meaningful DnCStatistics
            }
        } else
        {
            engine.applySplits( idToPhase );
            engine.setConstraintViolationThreshold( splitThreshold );
	    if(!engine.solve(timeoutInSeconds)) return std::make_pair(ret, *(engine.getStatistics()));

            if (engine.getExitCode() == Engine::SAT)
                engine.extractSolution(inputQuery);
            retStats = *(engine.getStatistics());
            for(unsigned int i=0; i<inputQuery.getNumberOfVariables(); ++i)
                ret[i] = inputQuery.getSolutionValue(i);
        }
    }
    catch(const MarabouError &e){
        printf( "Caught a MarabouError. Code: %u. Message: %s\n", e.getCode(), e.getUserMessage() );
        return std::make_pair(ret, retStats);
    }
    if(output != -1)
        restoreOutputStream(output);
    return std::make_pair(ret, retStats);
}

void saveQuery(InputQuery& inputQuery, std::string filename){
    inputQuery.saveQuery(String(filename));
}

InputQuery loadQuery(std::string filename){
    return QueryLoader::loadQuery(String(filename));
}

// Code necessary to generate Python library
// Describes which classes and functions are exposed to API
PYBIND11_MODULE(MarabouCore, m) {
    m.doc() = "Marabou API Library";
    m.def("createInputQuery", &createInputQuery, "Create input query from network and property file");
    m.def("solve", &solve, "Takes in a description of the InputQuery and returns the solution", py::arg("inputQuery"), py::arg("options"),
          py::arg("summaryFilePath"), py::arg("fixedReluFilePath"),  py::arg("redirect") = "");
    m.def("saveQuery", &saveQuery, "Serializes the inputQuery in the given filename");
    m.def("loadQuery", &loadQuery, "Loads and returns a serialized inputQuery from the given filename");
    m.def("addReluConstraint", &addReluConstraint, "Add a Relu constraint to the InputQuery");
    m.def("addMaxConstraint", &addMaxConstraint, "Add a Max constraint to the InputQuery");
    m.def("setDirection", &setDirection, "Set direction of a relu");
    py::class_<InputQuery>(m, "InputQuery")
        .def(py::init())
        .def("setUpperBound", &InputQuery::setUpperBound)
        .def("setLowerBound", &InputQuery::setLowerBound)
        .def("getUpperBound", &InputQuery::getUpperBound)
        .def("getLowerBound", &InputQuery::getLowerBound)
        .def("dump", &InputQuery::dump)
        .def("setNumberOfVariables", &InputQuery::setNumberOfVariables)
        .def("addEquation", &InputQuery::addEquation)
        .def("getSolutionValue", &InputQuery::getSolutionValue)
        .def("getNumberOfVariables", &InputQuery::getNumberOfVariables)
        .def("getNumInputVariables", &InputQuery::getNumInputVariables)
        .def("getNumOutputVariables", &InputQuery::getNumOutputVariables)
        .def("inputVariableByIndex", &InputQuery::inputVariableByIndex)
        .def("markInputVariable", &InputQuery::markInputVariable)
        .def("markOutputVariable", &InputQuery::markOutputVariable)
        .def("outputVariableByIndex", &InputQuery::outputVariableByIndex)
        .def("setSymbolicBoundTightener", &InputQuery::setSymbolicBoundTightener);
    py::class_<MarabouOptions>(m, "Options")
        .def(py::init())
        .def_readwrite("_numWorkers", &MarabouOptions::_numWorkers)
        .def_readwrite("_initialTimeout", &MarabouOptions::_initialTimeout)
        .def_readwrite("_initialDivides", &MarabouOptions::_initialDivides)
        .def_readwrite("_onlineDivides", &MarabouOptions::_onlineDivides)
        .def_readwrite("_timeoutInSeconds", &MarabouOptions::_timeoutInSeconds)
        .def_readwrite("_focusLayer", &MarabouOptions::_focusLayer)
        .def_readwrite("_timeoutFactor", &MarabouOptions::_timeoutFactor)
        .def_readwrite("_verbosity", &MarabouOptions::_verbosity)
        .def_readwrite("_dnc", &MarabouOptions::_dnc)
        .def_readwrite("_restoreTreeStates", &MarabouOptions::_restoreTreeStates)
        .def_readwrite("_lookAheadPreprocessing", &MarabouOptions::_lookAheadPreprocessing)
        .def_readwrite("_preprocessOnly", &MarabouOptions::_preprocessOnly)
        .def_readwrite("_divideStrategy", &MarabouOptions::_divideStrategy)
        .def_readwrite("_biasStrategy", &MarabouOptions::_biasStrategy)
        .def_readwrite("_maxDepth", &MarabouOptions::_maxDepth)
        .def_readwrite("_maxTreeDepth", &MarabouOptions::_maxTreeDepth )
        .def_readwrite("_splitThreshold", &MarabouOptions::_splitThreshold);
    py::class_<SymbolicBoundTightener, std::unique_ptr<SymbolicBoundTightener,py::nodelete>>(m, "SymbolicBoundTightener")
        .def(py::init())
        .def("setNumberOfLayers", &SymbolicBoundTightener::setNumberOfLayers)
        .def("setLayerSize", &SymbolicBoundTightener::setLayerSize)
        .def("allocateWeightAndBiasSpace", &SymbolicBoundTightener::allocateWeightAndBiasSpace)
        .def("setBias", &SymbolicBoundTightener::setBias)
        .def("setWeight", &SymbolicBoundTightener::setWeight)
        .def("setInputLowerBound", &SymbolicBoundTightener::setInputLowerBound)
        .def("setInputUpperBound", &SymbolicBoundTightener::setInputUpperBound)
        .def("setReluBVariable", &SymbolicBoundTightener::setReluBVariable)
        .def("setReluFVariable", &SymbolicBoundTightener::setReluFVariable);
    py::class_<Equation> eq(m, "Equation");
    eq.def(py::init());
    eq.def(py::init<Equation::EquationType>());
    eq.def("addAddend", &Equation::addAddend);
    eq.def("setScalar", &Equation::setScalar);
    py::enum_<Equation::EquationType>(eq, "EquationType")
        .value("EQ", Equation::EquationType::EQ)
        .value("GE", Equation::EquationType::GE)
        .value("LE", Equation::EquationType::LE)
        .export_values();
    py::class_<Statistics>(m, "Statistics")
        .def("getMaxStackDepth", &Statistics::getMaxStackDepth)
        .def("getNumPops", &Statistics::getNumPops)
        .def("getNumVisitedTreeStates", &Statistics::getNumVisitedTreeStates)
        .def("getNumSplits", &Statistics::getNumSplits)
        .def("getTotalTime", &Statistics::getTotalTime)
        .def("getNumTableauPivots", &Statistics::getNumTableauPivots)
        .def("getMaxDegradation", &Statistics::getMaxDegradation)
        .def("getNumPrecisionRestorations", &Statistics::getNumPrecisionRestorations)
        .def("getNumSimplexPivotSelectionsIgnoredForStability", &Statistics::getNumSimplexPivotSelectionsIgnoredForStability)
        .def("getNumSimplexUnstablePivots", &Statistics::getNumSimplexUnstablePivots)
        .def("getNumMainLoopIterations", &Statistics::getNumMainLoopIterations)
        .def("getTimeSimplexStepsMicro", &Statistics::getTimeSimplexStepsMicro)
        .def("getNumConstraintFixingSteps", &Statistics::getNumConstraintFixingSteps)
        .def("hasTimedOut", &Statistics::hasTimedOut);
}
