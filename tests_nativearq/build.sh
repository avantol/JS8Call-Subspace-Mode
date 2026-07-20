#!/bin/sh
# [TODO #107] Offline harness build for the native-binary logic.
# Run from this directory:
#   ./build.sh && ./nativebinary_test && ./nativearq_fsm_test
set -e
cd "$(dirname "$0")"

MOC=$(command -v moc-qt6 || echo /usr/lib/qt6/libexec/moc)

g++ -fPIC -I.. $(pkg-config --cflags Qt6Core) \
    nativebinary_test.cpp \
    ../JS8_Main/NativeBinary.cpp \
    ../JS8_Main/FileTransfer.cpp \
    $(pkg-config --libs Qt6Core) \
    -o nativebinary_test
echo "built: ./nativebinary_test"

"$MOC" ../JS8_Main/ChunkedArq.h -o moc_ChunkedArq.cpp
g++ -fPIC -I.. $(pkg-config --cflags Qt6Core) \
    nativearq_fsm_test.cpp \
    ../JS8_Main/ChunkedArq.cpp \
    ../JS8_Main/NativeBinary.cpp \
    ../JS8_Main/Radio.cpp \
    moc_ChunkedArq.cpp \
    $(pkg-config --libs Qt6Core) \
    -o nativearq_fsm_test
echo "built: ./nativearq_fsm_test"
