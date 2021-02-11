CC      = gcc
CFLAGS  = -O2 -g -D_FORTIFY_SOURCE=2
LD      = gcc
LDFLAGS =
LIBS    = -ludev -lpthread

OBJS = main.o daemon.o autofs.o changed.o device.o config.o \
	   mount.o fsoptions.o aliases.o mcond.o mtab.o util.o

DESTDIR = 
BINDIR  = /sbin
ETCDIR  = /etc
LIBDIR  = /lib
MAN5DIR = /usr/share/man/man5
MAN8DIR = /usr/share/man/man8

mediad: $(OBJS)
	$(LD) -o $@ $^ $(LIBS)

$(OBJS): mediad.h

clean:
	rm -f mediad $(OBJS) core build

install: mediad
	install -d $(DESTDIR)$(BINDIR)
	install -s -m 755 mediad $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(ETCDIR)/mediad
	[ -f $(DESTDIR)$(ETCDIR)/mediad/mediad.rules ] || \
		install -m 644 mediad.rules $(DESTDIR)$(ETCDIR)/mediad/
	[ -f $(DESTDIR)$(ETCDIR)/mediad/mediad.conf ] || \
		install -m 644 mediad.conf $(DESTDIR)$(ETCDIR)/mediad/
	install -d $(DESTDIR)$(LIBDIR)/udev/rules.d
	ln -s ../../../$(ETCDIR)/mediad/mediad.rules $(DESTDIR)$(LIBDIR)/udev/rules.d/z51_mediad.rules
	install -d $(DESTDIR)$(MAN5DIR) $(DESTDIR)$(MAN8DIR)
	install -m 644 mediad.conf.5 $(DESTDIR)$(MAN5DIR)
	install -m 644 mediad.8 $(DESTDIR)$(MAN8DIR)
