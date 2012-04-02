SUBDIRS = examples handlers
APXS_LIBEXEC = $(shell apxs -q LIBEXECDIR)

all: mod_websocket.la mod_websocket_draft76.la $(SUBDIRS)

.PHONY: clean
clean:
	-rm -f *.la *.o *.lo *.slo
	-for d in $(SUBDIRS); do (cd $$d; $(MAKE) clean ); done

.PHONY: install
install: all
	mkdir -p $(DESTDIR)/$(APXS_LIBEXEC)
	-for a in *.la; do apxs -S LIBEXECDIR=$(DESTDIR)/$(APXS_LIBEXEC) -i $$a; done
	-for d in $(SUBDIRS); do (cd $$d; $(MAKE) install ); done

%.la: %.c
	apxs -c $<

.PHONY: subdirs $(SUBDIRS)
subdirs: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@
