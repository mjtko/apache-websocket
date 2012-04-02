APXS_LIBEXEC = $(shell apxs -q LIBEXECDIR)

all: mod_websocket_dumb_increment.la mod_websocket_echo.la

clean:
	rm -f *.la *.o *.lo *.slo

install: all
	mkdir -p $(DESTDIR)/$(APXS_LIBEXEC)
	-for a in *.la; do apxs -S LIBEXECDIR=$(DESTDIR)/$(APXS_LIBEXEC) -n `basename $$a` -i $$a; done

%.la: %.c
	apxs -c -I .. $<
