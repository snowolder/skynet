#!/bin/bash
Dir=`pwd`
OutDir="${Dir}/../bin"
if [ $# -ge 1 ]; then
	OutDir=$1
fi

mkdir -p $OutDir
mkdir -p $OutDir/cservice
mkdir -p $OutDir/service
mkdir -p $OutDir/luaclib
mkdir -p $OutDir/lualib

#cd $Dir/3rd/jemalloc
#chmod -R 777 *

cd $Dir
#make cleanall

#cd $Dir/3rd/lua-cjson
#make clean
#make
#\cp cjson.so $OutDir/luaclib/ -fr
#make clean
#cd $Dir

make linux
\cp skynet $OutDir/ -fr
\cp cservice/* $OutDir/cservice/ -fr
\cp service/* $OutDir/service/ -fr
\cp luaclib/* $OutDir/luaclib/ -fr
\cp lualib/* $OutDir/lualib/ -fr
\cp 3rd/lua/luac $OutDir/ -fr

#make cleanall




