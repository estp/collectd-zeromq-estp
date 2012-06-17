====================================
Collectd Plugin for ESTP over ZeroMQ
====================================

The ESTP is Extensible Statistics Transmission Protocol. The protocol is
intended to become standard protocol for submission statistics data by zeromq
applications. More info can be found here:

http://github.com/estp/estp

The plugin allows both to receive data from zeromq for storing by collectd
and to publish collectd-provided statistics to zeromq socket. So it's capable
to send data between collectd instances too


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

Configuration
=============

The following is example config, which accepts data::

    LoadPlugin zeromq_estp
    <Plugin "zeromq_estp">
      <Socket Subscribe>
        HWM 1000
        Bind "tcp://*:6666"
      </Socket>
    </Plugin>

Any zeromq address can be specified for ``Bind``.

The following is example config, which sends data from collectd to another
collectd instance or any other processing application::

    LoadPlugin zeromq_estp
    <Plugin "zeromq_estp">
      <Socket Publish>
        HWM 1000
        Connect "tcp://host.example.com:6666"
      </Socket>
    </Plugin>

Note: if you have write filters enabled, do not forget to add "zeromq_estp"
plugin to your "write" target.

The ``Connect`` and ``Bind`` directives both can be used in any socket types,
and can be used multiple times. ``Pull`` socket can be used instead
``Subscribe``, likewise ``Push`` socket can be used instead ``Publish``.
However, this is rarely useful in practice. For more information refer to
zeromq documentation.

The ``HWM`` directive shows the maximum amount of messages buffered. You should
set it because otherwise zeromq memory usage may be unbounded.



