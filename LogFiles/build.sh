#!/bin/bash
mkdir -p build/ && cd build/ && cmake .. -DCMAKE_BUILD_TYPE=release && make -j4
