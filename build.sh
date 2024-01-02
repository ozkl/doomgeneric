#!/bin/bash

set -e

mkdir -p build

cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -B build
