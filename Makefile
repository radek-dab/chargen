CC=gcc
#CFLAGS=-DDEBUG -Wall -g
CFLAGS=-DNDEBUG -Wall -O3
LIBS=
NAME=chargen

INSTALL=install
SYSTEMCTL=systemctl
BINDIR=/usr/local/bin
UNITSDIR=/usr/local/lib/systemd/system

$(NAME): $(NAME).o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(NAME).o: $(NAME).c
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: clean install

clean:
	rm -f $(NAME) $(NAME).o

install: $(NAME) $(NAME).service
	$(INSTALL) -Dm755 $(NAME) $(BINDIR)/$(NAME)
	$(INSTALL) -Dm644 $(NAME).service $(UNITSDIR)/$(NAME).service
	$(SYSTEMCTL) daemon-reload
