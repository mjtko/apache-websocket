APXS_LIBEXEC = $(shell apxs -q LIBEXECDIR)

all: mod_websocket_tcp_proxy.la mod_websocket_tcp_proxy_range.la

clean:
	rm -f *.la *.o *.lo *.slo

install: all
	mkdir -p $(DESTDIR)/$(APXS_LIBEXEC)
	-for a in *.la; do apxs -S LIBEXECDIR=$(DESTDIR)/$(APXS_LIBEXEC) -i $$a; done

%.la: %.c
	apxs -c -I .. $<
