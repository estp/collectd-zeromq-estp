====================================
Collectd Plugin for ESTP over ZeroMQ
====================================

The ESTP is Extensible Statistics Transmission Protocol. The protocol is
intended to become standard protocol for submission statistics data by zeromq
applications. More info can be found here:

http://github.com/estp/estp

The plugin allows both to receive data from zeromq for storing by collectd
and to publish collectd-provided statistics to zeromq socket.

Note that currently collectd to collectd transmission over zeromq statitics is
not lossless as some collectd-specific types cannot be exactly represented by
current version of ESTP, also multi-value data types will be sent as separate
metrics. Lossless data transmission will be implemented as ESTP extension soon.


Building
========

The following is a basic build process::

    git submodule update --init
    make
    sudo make install

Note: the build process clones full collectd repository and runs "configure" on
it. You can ignore the whole configure output, as nothing except header files
are used from collectd itself. If there is better way to build collectd plugin
please send pull requests.
