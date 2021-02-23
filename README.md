[![codecov.io](
  https://codecov.io/github/NeuralNetworkVerification/Marabou/coverage.svg?branch=master)](
    https://codecov.io/github/NeuralNetworkVerification/Marabou?branch=master)

#  Marabou
Deep neural networks are revolutionizing the way complex systems are designed.
Instead of spending long hours hand-crafting complex software, many engineers
now opt to use deep neural networks (DNNs) - machine learning models, created by
training algorithms that generalize from a finite set of examples to previously
unseen inputs. Consequently, there is a pressing need for tools and techniques
for network analysis and certification. To help address that need, we present
Marabou, a framework for verifying deep neural networks. 

Marabou is an SMT-based tool that can answer queries about a network’s
properties by transforming these queries into constraint satisfaction problems.
It can accommodate networks with different activation functions and topologies,
and it performs high-level reasoning on the network that can curtail the search
space and improve performance. It also supports parallel execution to further
enhance scalability. Marabou accepts multiple input formats, including protocol
buffer files generated by the popular TensorFlow framework for neural networks.

A DNN verification query consists of two parts: (i) a neural network, and (ii) a
property to be checked; and its result is either a formal guarantee that the
network satisfies the property, or a concrete input for which the property is
violated (a counter-example). There are several types of verification queries
that Marabou can answer: 
* Reachability queries: if inputs is in a given range is the output
guaranteed to be in some, typically safe, range.
* Robustness queries: test whether there exist adversarial points around a given
  input point that change the output of the network.

Marabou supports fully connected feed-forward and convolutional NNs with
piece-wise linear activation functions, in the .nnet and TensorFlow formats.
Properties can be specified using inequalites over input and output variables or
via Python interface. 

For more details about the features of Marabou check out the [tool
paper](https://aisafety.stanford.edu/marabou/MarabouCAV2019.pdf) and the
[slides](https://aisafety.stanford.edu/marabou/fomlas19.html). 

For more information about the input formats please check the
[wiki](https://github.com/NeuralNetworkVerification/Marabou/wiki/Marabou-Input-Formats).

Download
------------------------------------------------------------------------------
The latest version of Marabou is available on [https://github.com/NeuralNetworkVerification/Marabou].

Build and Dependencies
------------------------------------------------------------------------------

Marabou depends on the Boost library,
which is automatically downloaded and built when you run make. Library CXXTEST
comes included in the repository.

The marabou build process uses CMake version 3.12 (or later).
You can get CMake [here](https://cmake.org/download/).

Marabou can be built for Linux, MacOS, or Windows machines.

### Build Instructions for Linux or MacOS

To build marabou using CMake run:
```
cd path/to/marabou/repo/folder
mkdir build 
cd build
cmake ..
```
For configuring to build a static Marabou binary, use the following flag
```
cmake .. -DBUILD_STATIC_MARABOU=ON
```
To build, run the following:
```
cmake --build .
```
To enable multiprocess build change the last command to:
```
cmake --build . -j PROC_NUM
```
To compile in debug mode (default is release)
```
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .
```

The compiled binary will be in the *build* directory, named _Marabou_

To run tests we use [ctest](https://cmake.org/cmake/help/v3.15/manual/ctest.1.html).
The tests have labels according to level (unit/system/regress0/regress1...), and the code they are testing (engine/common etc...).  
For example to run all unit tests execute in the build directory:
```
ctest -L unit
```
On every build we run the unit tests, and on every pull request we run unit,
system, regress0 and regress1.

Another option to build and run all of the tests is: 
```
cd path/to/marabou/repo/folder
mkdir build 
cd build
cmake ..
make check -j PROC_NUM
```
### Build Instructions for Windows using Visual Studio

First, install Visual Studio 2017 or later and select the "Desktop development with C++" workload. 
Ensure that CMake is installed and added to your PATH.

Open a command prompt and run:
```
cd path\to\marabou\repo\folder
mkdir build 
cd build
cmake .. -G"Visual Studio 15 2017 Win64"
cmake --build . --config Release
```
This process builds Marabou using the generator "Visual Studio 15 2017 Win64". 
For 32-bit machines, omit Win64. Other generators and older versions of Visual Studio can likely be used as well, 
but only "Visual Studio 15 2017 Win64" has been tested.

The Marabou executable file will be written to the build/Release folder. To build in 
Debug mode, simply run "cmake --build . --config Debug", and the executables will be 
written to build/Debug.

### Python API
It may be useful to set up a Python virtual environment, see
[here](https://docs.python.org/3/tutorial/venv.html) for more information.

The python interface was tested only on versions >3.5 and >2.7. The build process prefers python3 but will work if there is only python 2.7 available. (To control the default change the DEFAULT_PYTHON_VERSION variable).  
The Python interface requires *pybind11* (which is automatically downloaded). 
By default Marabou builds also the python API, the BUILD_PYTHON variable
controls that.
This process will produce the binary file and the shared library for the Python 
API. The shared library will be in the maraboupy folder for Linux and MacOS. 
On Windows, the shared library is written to a Release subfolder in maraboupy, 
so you will need to move the Release/\*pyd file to the maraboupy folder:
```
cd path\to\marabou\repo\folder\maraboupy
move Release\*pyd .
```

Export maraboupy folder to Python and Jupyter paths:
```
PYTHONPATH=PYTHONPATH:/path/to/marabou/folder
JUPYTER_PATH=JUPYTER_PATH:/path/to/marabou/folder
```
and Marabou is ready to be used from a Python or a Jupyter script. On Windows, 
edit your environmental variables so PYTHONPATH includes the marabou folder.

#### Troubleshooting

- On Windows - Make sure the detected python ("Found PythonInterp: ....") is a windows python and not cygwin or something like that (if it is cygwin, use -DPYTHON_EXECUTABLE flag to override the default python, or manuialy download the linux pybind and locate it in the tools directory)

- 32bit Python - By default we install a 64bit Marabou and consequently a 64bit
  python interface, the maraboupy/build_python_x86.sh file builds a 32bit
  version.



Getting Started
-----------------------------------------------------------------------------
### To run Marabou from Command line 
After building Marabou the binary is located at *build/Marabou* (or *build\Release\Marabou.exe* for Windows). The
repository contains sample networks and properties in the *resources* folder.
For more information see [resources/README.md](resources/README.md).

To run Marabou, execute from the repo directory, for example:
```
./build/Marabou resources/nnet/acasxu/ACASXU_experimental_v2a_2_7.nnet resources/properties/acas_property_3.txt
```
on Linux or MacOS, or 
```
build\Release\Marabou.exe resources\nnet\acasxu\ACASXU_experimental_v2a_2_7.nnet resources\properties\acas_property_3.txt
```
on Windows.

### Using Python interface 
Please see our [documentation](https://neuralnetworkverification.github.io/Marabou/)
for the python interface, which contains examples, API documentation, and a developer's guide.

### Using the Split and Conquer (SNC) mode
In the SNC mode, activated by *--snc* Marabou decomposes the problem into *2^n0*
sub-problems, specified by *--initial-divides=n0*. Each sub-problem will be
solved with initial timeout of *t0*, supplied by *--initial-timeout=t0*. Every
sub-problem that times out will be divided into *2^n* additional sub-problems,
*--num-online-divides=n*, and the timeout is multiplied by a factor of *f*,
*--timeout-factor=f*. Number of parallel threads *t* is specified by
*--num-workers=t*.

So to solve a problem in SNC mode with 4 initial splits and initial timeout of 5s, 4 splits on each timeout and a timeout factor of 1.5:
```
build/Marabou resources/nnet/acasxu/ACASXU_experimental_v2a_2_7.nnet resources/properties/acas_property_3.txt --snc --initial-divides=4 --initial-timeout=5 --num-online-divides=4 --timeout-factor=1.5 --num-workers=4
```

### Use LP Relaxation
Marabou has an option to use LP relaxation for bound tightening.
For now we use Gurobi as an LP solver. Gurobi requires a license (a free
academic license is available), after getting one the software can be downloaded
[here](https://www.gurobi.com/downloads/gurobi-optimizer-eula/) and [here](https://www.gurobi.com/documentation/9.0/quickstart_linux/software_installation_guid.html#section:Installation) are
installation steps, there is a [compatibility
issue](https://support.gurobi.com/hc/en-us/articles/360039093112-C-compilation-on-Linux) that should be addressed.
A quick installation reference:
```
export INSTALL_DIR=/opt
sudo tar xvfz gurobi9.1.1_linux64.tar.gz -C $INSTALL_DIR
cd $INSTALL_DIR/gurobi911/linux64/src/build
sudo make
sudo cp libgurobi_c++.a ../../lib/
```
Next it is recommended to add the following to the .bashrc (but not necessary) 
```
export GUROBI_HOME="/opt/gurobi911/linux64"
export PATH="${PATH}:${GUROBI_HOME}/bin"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${GUROBI_HOME}/lib"

```

After installing Gurobi compile marabou as follows:
```
cmake .. -DENABLE_GUROBI=ON
cmake --build . 
```
If you did not set the GUROBI_HOME environment variable, then use the following:
```
cmake .. -DENABLE_GUROBI=ON -DGUROBI_DIR=<PATH_TO_GUROBI>
```

### Tests
We have three types of tests:  
* unit tests - test specific small components, the tests are located alongside the code in a _tests_ folder (for example: _src/engine/tests_), to add a new set of tests, add a file named *Test_FILENAME* (where *FILENAME* is what you want to test), and add it to the CMakeLists.txt file (for example src/engine/CMakeLists.txt)
* system tests - test an end to end use case but still have access to internal functionality. Those tests are located in _src/system_tests_. To add new set of tests create a file named *Test_FILENAME*, and add it also to _src/system_tests/CMakeLists.txt_.
* regression tests - test end to end functionality thorugh the API, each test is defined by:  
  * network_file - description of the "neural network" supporting nnet and mps formats (using the extension to decdie on the format)  
  * property_file - optional, constraint on the input and output variables  
  * expected_result - sat/unsat  

The tests are divided into 5 levels to allow variability in running time, to add a new regression tests: 
  * add the description of the network and property to the _resources_ sub-folder 
  * add the test to: _regress/regressLEVEL/CMakeLists.txt_ (where LEVEL is within 0-5) 
In each build we run unit_tests and system_tests, on pull request we run regression 0 & 1, in the future we will run other levels of regression weekly / monthly. 

Acknowledgments
-----------------------------------------------------------------------------

The Marabou project is partially supported by grants from the Binational Science
Foundation (2017662), the Defense Advanced Research Projects Agency
(FA8750-18-C-0099), the Semiconductor Research Corporation (2019-AU-2898), the
Federal Aviation Administration, Ford Motor Company, Intel Corporation, the
Israel Science Foundation (683/18), the National Science Foundation (1814369,
DGE-1656518), Siemens Corporation, General Electric, and the Stanford CURIS program.


Marabou is used in a number of flagship projects at [Stanford's AISafety
center](http://aisafety.stanford.edu).



People
-----------------------------------------------------------------------------
[Guy Katz](https://www.katz-lab.com/)

[Clark Barrett](http://theory.stanford.edu/~barrett/)

[Aleksandar Zeljic](https://profiles.stanford.edu/aleksandar-zeljic)

[Ahmed Irfan](https://profiles.stanford.edu/ahmed-irfan)

[Haoze Wu](http://www.haozewu.com/)

[Christopher Lazarus](https://profiles.stanford.edu/christopher-lazarus-garcia)

[Kyle Julian](https://www.linkedin.com/in/kyjulian) 

[Chelsea Sidrane](https://www.linkedin.com/in/chelseasidrane)

[Parth Shah](https://www.linkedin.com/in/parthshah1995)

[Shantanu Thakoor](https://in.linkedin.com/in/shantanu-thakoor-4b2630142) 

[Rachel Lim](https://rachellim.github.io/)

Derek A. Huang 

Duligur Ibeling

Elazar Cohen

Ben Goldberger

Omri Cohen
