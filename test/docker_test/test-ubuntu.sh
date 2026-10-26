#!/bin/bash

set -e
set -x

ENV_FILE=/tmp/env.sh

. ${ENV_FILE}

# setup MODULE_SRC_DIR env var
cwd=$(pwd)
if [ -z "${MODULE_SRC_DIR}" ]; then
    if [ -e "$cwd/src/sqlite3module.cc" ]; then
        MODULE_SRC_DIR=$cwd
    else
        MODULE_SRC_DIR=$WORKDIR/module-sqlite3
    fi
fi
echo "export MODULE_SRC_DIR=${MODULE_SRC_DIR}" >> ${ENV_FILE}

echo "export QORE_UID=999" >> ${ENV_FILE}
echo "export QORE_GID=999" >> ${ENV_FILE}

. ${ENV_FILE}

export MAKE_JOBS=4

# install sqlite3 development package
apt-get update
apt-get install -y libsqlite3-dev

# build module and install
echo && echo "-- building module --"
mkdir -p ${MODULE_SRC_DIR}/build
cd ${MODULE_SRC_DIR}/build
cmake .. -DCMAKE_BUILD_TYPE=debug -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX}
make -j${MAKE_JOBS}
make install

# add Qore user and group
groupadd -o -g ${QORE_GID} qore
useradd -o -m -d /home/qore -u ${QORE_UID} -g ${QORE_GID} qore

# own everything by the qore user
chown -R qore:qore ${MODULE_SRC_DIR}

# run the tests
cd ${MODULE_SRC_DIR}
# the tests must load the module that this job built and installed, not one that the image already has
loaded=$(gosu qore:qore qore -l sqlite3 -e 'printf("%s", get_module_hash().sqlite3.filename);')
if ! grep -qxF "${loaded}" ${MODULE_SRC_DIR}/build/install_manifest.txt; then
    echo "ERROR: the tests would load '${loaded}', which this job did not install" >&2
    exit 1
fi
echo "-- the tests load ${loaded}"

for test in test/*.qtest; do
    gosu qore:qore qore -b --enable-debug "$test" -vv
done
gosu qore:qore env QORE_TEST_SQLITE_INTERRUPT=1 \
    LD_PRELOAD="${MODULE_SRC_DIR}/build/libsqlite3_open_interrupt.so" \
    qore -b --enable-debug test/connection-setup.qtest -vv
