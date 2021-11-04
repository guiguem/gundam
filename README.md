# GUNDAM — 風をあつめて

![](./resources/images/banner.jpg)

GUNDAM, for *Generic fitter for Upgraded Near Detector Analysis Methods*, is a suite
of applications which aims at performing various statistical analysis with different
purposes and setups. It has been developed as a fork of 
[xsllhFitter](https://gitlab.com/cuddandr/xsLLhFitter), in the context of the Upgrade
of ND280 for the T2K neutrino experiment.


The main framework offers a code structure which is capable of  handling parameters/errors
propagation on a model and compare to experimental data. As an example, GUNDAM includes
a likelihood-based fitter which was initially designed  to reproduce T2K's BANFF fit as
a proof of concept.

The applications are intended to be fully configurable with a set of YAML/JSON files, as
the philosophy of this project is to avoid users having to put their hands into the code
for each study. A lot of time and efforts are usually invested by various working groups
to debug and optimize pieces of codes which does generic tasks. As GUNDAM is designed for
maximize flexibility to accommodate various physics works, it allows to share optimizations
and debugging for every project at once.

## Showcase

![](./resources/images/samplesExample.png)

<details>
  <summary><b>Spoiler: More Screenshots</b></summary>

![](./resources/images/postFitCorrExample.png)

</details>



## How do I get setup?

### Prerequisites

There are several requirements for building the fitter:
- GCC 4.8.5+ or Clang 3.3+ (a C++11 enabled compiler)
- CMake 3.5+
- [ROOT 6](https://github.com/root-project/root)
- [JSON for Modern C++](https://github.com/nlohmann/json)
- [yaml-cpp](https://github.com/jbeder/yaml-cpp)

### Shell setup

In this guide, it is assumed you have already defined the following bash environment
variables:

- `$REPO_DIR`: the path to the folder where your git projects are stored. This guide
will download this repo into the subdirectory `$REPO_DIR/gundam`.

- `$BUILD_DIR`: the path where the binaries are built. As for the previous variables,
this guide will work under `$BUILD_DIR/gundam`.

- `$INSTALL_DIR`: the path where the binaries are installed and used by the shell.
Same here: this guide will work under `$INSTALL_DIR/gundam`.

As an example, here is how I personally define those variables. This script is executed
in the `$HOME/.bash_profile` on macOS or `$HOME/.bashrc` on Linux, as they can be used
for other projects as well.

```bash
export INSTALL_DIR="$HOME/Documents/Work/Install/"
export BUILD_DIR="$HOME/Documents/Work/Build/"
export REPO_DIR="$HOME/Documents/Work/Repositories/"
```

If it's the first time you define those, don't forget to `mkdir`!

```bash
mkdir -p $INSTALL_DIR
mkdir -p $BUILD_DIR
mkdir -p $REPO_DIR
```

### Cloning repository

```bash
cd $REPO_DIR
git clone https://github.com/nadrino/gundam.git
cd $REPO_DIR/gundam
```

As a user, it is recommended for you to check out the latest tagged version of this
repository:

```bash
git checkout $(git describe --tags `git rev-list --tags --max-count=1`)
```

GUNDAM depends on additional libraries which are included as submodules of this git
project. It is necessary to download those:

```bash
git submodule update --init --recursive
```

### Updating your repository

Pull the latest version on github with the following commands:

```bash
cd $REPO_DIR/gundam
git pull
git submodule update --remote
git checkout $(git describe --tags `git rev-list --tags --max-count=1`)
cd -
```

### Compiling the code

Let's create the Build and Install folder:

```bash
mkdir -p $BUILD_DIR/gundam
mkdir -p $INSTALL_DIR/gundam
```

Now let's generate binaries:

```bash
cd $BUILD_DIR/gundam
cmake \
  -DCMAKE_INSTALL_PREFIX:PATH=$INSTALL_DIR/gundam \
  -D CMAKE_BUILD_TYPE=Release \
  $REPO_DIR/gundam/.
make -j 4 install
```

If you did get there without error, congratulations! Now GUNDAM is installed on you machine :-D.

To access the executables from anywhere, you have to update you `$PATH` and `$LD_LIBRARY_PATH`
variables:

```bash
export PATH="$INSTALL_DIR/gundam/bin:$PATH"
export LD_LIBRARY_PATH="$INSTALL_DIR/gundam/lib:$LD_LIBRARY_PATH"
```

### Common Issues

#### CMake can't find yaml-cpp

Sometimes cmake can't find the yaml-cpp library on its own. You can
help it by providing the argument YAMLCPP_INSTALL_DIR:

```bash
cmake \
  -DCMAKE_INSTALL_PREFIX:PATH=$INSTALL_DIR/gundam \
  -D CMAKE_BUILD_TYPE=Release \
  -D YAMLCPP_INSTALL_DIR=$YAMLCPP_INSTALL_DIR \
  $REPO_DIR/gundam/.
```

where `$YAMLCPP_INSTALL_DIR` is pointing to the folder containing `include`, `lib`, etc...

##### On CCLyon:
```bash
export YAMLCPP_INSTALL_DIR=/sps/t2k/common/software/install/yaml-cpp
```

### Gathering inputs

## I want to contribute!

## Lineage

GUNDAM was born as a fork of the *xsllhFitter* project which was developped and used by
the cross-section working group of T2K. The original project can be found on *gitlab*:
[https://gitlab.com/cuddandr/xsLLhFitter](https://gitlab.com/cuddandr/xsLLhFitter).

GUNDAM has originally been developed as an new fitter to perform T2K oscillation
analysis, and provide an expandable base on which future studies with the *Upgraded
ND280 Detectors* will be performed.

![](./resources/images/ride.png)
