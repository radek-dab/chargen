CC=gcc
#CFLAGS=-DDEBUG -Wall -g
CFLAGS=-DNDEBUG -Wall -O3
LIBS=
NAME=chargen

INSTALL=install
SYSTEMCTL=systemctl
BINDIR=/usr/bin
UNITSDIR=/usr/lib/systemd/system

$(NAME): $(NAME).o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(NAME).o: $(NAME).c
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: clean install uninstall

clean:
	rm -f $(NAME) $(NAME).o

install: $(NAME) $(NAME).service
	$(INSTALL) -m755 $(NAME) $(BINDIR)
	$(INSTALL) -m644 $(NAME).service $(UNITSDIR)
	$(SYSTEMCTL) daemon-reload

uninstall:
	rm -f $(BINDIR)/$(NAME)
	rm -f $(UNITSDIR)/$(NAME).service
	$(SYSTEMCTL) daemon-reload
