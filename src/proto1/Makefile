INST = $(HOME)/inst/ct-64
APACHECTL = $(INST)/bin/apachectl
APXS = $(INST)/bin/apxs
SSLSRC = $(HOME)/svn/httpd-ct/modules/ssl
OPENSSLINST = $(HOME)/inst/omaster

SOURCES = mod_ssl_ct.c Makefile *.py

all: mod_ssl_ct.la

clean:
	rm *.la *.lo *.o *.slo

mod_ssl_ct.la: mod_ssl_ct.c
	$(APXS) -c -I$(SSLSRC) -I$(OPENSSLINST)/include mod_ssl_ct.c

install: mod_ssl_ct.la stop pure-install

pure-install: mod_ssl_ct.la
	$(APXS) -i mod_ssl_ct.la
	cp statuscgi.py $(INST)/cgi-bin
	chmod 0755 $(INST)/cgi-bin/statuscgi.py

TS := $(shell date)
LINES := $(shell cat $(SOURCES) | grep -v '^$$' | wc -l)

start:
	$(APACHECTL) -k start

stop:
	$(APACHECTL) -k stop

test: install
	python testdotconf.py

config:
	python conf.py ssl_ct $(INST)
	$(APACHECTL) -t
	$(APACHECTL) -M | grep ssl_ct

metrics:
	@echo $(TS) $(LINES) >> metrics.txt
	cat metrics.txt