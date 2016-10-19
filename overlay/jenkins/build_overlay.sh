#!/bin/sh

git clone https://github.com/dcos/dcos-mesos-modules.git /mesos-mesos-modules
cd /dcos-mesos-modules
git fetch origin pull/$1/head:unit-tests
git checkout unit-tests
mkdir build
./bootstrap
cd build
../configure --with-mesos=/mesos_install/ --with-mesos-root=/mesos-1.0.0 --prefix=/mesos_modules_install
make check
