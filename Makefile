all: build

build: zeromq_estp.so

zeromq_estp.so: zeromq_estp.c collectd/src/config.h
	gcc zeromq_estp.c -o zeromq_estp.so -shared -fPIC -lzmq -Icollectd/src -DHAVE_CONFIG_H -Wall

collectd/configure: collectd/configure.in
	cd collectd; ./build.sh

collectd/src/config.h: collectd/configure
	cd collectd; ./configure $(CONFIGURE_ARGS)

clean:
	rm zeromq_estp.so

install: build
	install -d $(DESTDIR)/usr/lib/collectd
	install zeromq_estp.so $(DESTDIR)/usr/lib/collectd

