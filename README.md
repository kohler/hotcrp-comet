HotCRP Comet
============

HotCRP (http://github.com/kohler/hotcrp) is the best available
software for managing the conference review process, including
paper submission, review and comment management, rebuttals, and
the PC meeting. This is a Comet server for use with HotCRP.

Prerequisites
-------------

HotCRP Comet runs on Unix, including Mac OS X. Building it requires CMake
3.20 or later and a C++23 compiler (such as GCC 13 or Clang 17 or later).
It also uses the [llhttp](https://github.com/nodejs/llhttp) and
[JSON for Modern C++](https://github.com/nlohmann/json) (nlohmann-json)
libraries, which the build downloads if they aren’t installed. To use
installed copies: on Debian and Ubuntu, `apt install libllhttp-dev
nlohmann-json3-dev`; with Homebrew, `brew install llhttp nlohmann-json`.

Building
--------

HotCRP Comet uses the [Cotamer](https://github.com/readablesystems/cotamer)
coroutine library, included as a git submodule.

    git submodule update --init
    cmake -B build
    cmake --build build
    sudo cmake --install build      # installs `hotcrp-comet` in /usr/local/bin

To build against a different Cotamer checkout, configure with
`cmake -B build -DCOTAMER_DIR=/path/to/cotamer`.

Testing
-------

`test/comet-test.py build/hotcrp-comet` runs black-box tests against a fake
HotCRP back end. Add `--slow` to include tests that take several seconds.

Authors
-------

Eddie Kohler, Harvard
