# st - simple terminal
# See LICENSE file for copyright and license details.
.POSIX:

include config.mk

SRC = st.c x.c rowcolumn_diacritics_helpers.c graphics.c boxdraw.c hb.c st_config.c
OBJ = $(SRC:.c=.o)

all: st

config.h:
	cp config.def.h config.h

.c.o:
	$(CC) $(STCFLAGS) -c $<

st.o: config.h st.h win.h graphics.h
x.o: arg.h config.h st.h win.h graphics.h hb.h st_config.h
graphics.c: graphics.h khash.h kvec.h st.h
boxdraw.o: config.h st.h boxdraw_data.h
hb.o: st.h
st_config.o: st_config.h

$(OBJ): config.h config.mk

st: $(OBJ)
	$(CC) -o $@ $(OBJ) $(STLDFLAGS)

clean:
	rm -f st $(OBJ) st-$(VERSION).tar.gz

dist: clean
	mkdir -p st-$(VERSION)
	cp -R FAQ LEGACY TODO LICENSE Makefile README config.mk\
		config.def.h st.info st.1 arg.h st.h win.h $(SRC)\
		st-$(VERSION)
	tar -cf - st-$(VERSION) | gzip > st-$(VERSION).tar.gz
	rm -rf st-$(VERSION)

install: st
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f st $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/st
	[ -f st-urlhandler ] && cp -f st-urlhandler $(DESTDIR)$(PREFIX)/bin && chmod 755 $(DESTDIR)$(PREFIX)/bin/st-urlhandler || :
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < st.1 > $(DESTDIR)$(MANPREFIX)/man1/st.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/st.1
	tic -sx st.info
	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	[ -f st.desktop ] && cp -f st.desktop $(DESTDIR)$(PREFIX)/share/applications/st.desktop || :
	mkdir -p $(DESTDIR)$(PREFIX)/share/pixmaps
	[ -f st.png ] && cp -f st.png $(DESTDIR)$(PREFIX)/share/pixmaps/st.png || :
	mkdir -p $(DESTDIR)/etc/st
	[ -f st.conf.example ] && cp -f st.conf.example $(DESTDIR)/etc/st/st.conf.example || :
	@echo Please see the README file regarding the terminfo entry of st.

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/st
	rm -f $(DESTDIR)$(PREFIX)/bin/st-urlhandler
	rm -f $(DESTDIR)$(MANPREFIX)/man1/st.1
	rm -f $(DESTDIR)$(PREFIX)/share/applications/st.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/pixmaps/st.png

.PHONY: all clean dist install uninstall
