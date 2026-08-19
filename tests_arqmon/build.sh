#!/bin/sh
# [TODO #153] Offline harness for the passive ARQ monitor.
#   ./build.sh && ./arqmon_test
set -e
cd "$(dirname "$0")"

MOC=$(command -v moc-qt6 || echo /usr/lib/qt6/libexec/moc)

"$MOC" ../JS8_Main/ArqMonitor.h -o moc_ArqMonitor.cpp
"$MOC" ../JS8_Main/ChunkedArq.h -o moc_ChunkedArq.cpp

g++ -std=c++20 -DJS8_ENABLE_FT2=1 -fPIC -I.. $(pkg-config --cflags Qt6Core) \
    arqmon_test.cpp \
    ../JS8_Main/ArqMonitor.cpp \
    ../JS8_Main/ChunkedArq.cpp \
    ../JS8_Main/NativeBinary.cpp \
    ../JS8_Main/FileTransfer.cpp \
    ../JS8_Main/Radio.cpp \
    ../JS8_Mode/JS8Submode.cpp \
    moc_ArqMonitor.cpp \
    moc_ChunkedArq.cpp \
    $(pkg-config --libs Qt6Core) \
    -o arqmon_test
echo "built: ./arqmon_test"
