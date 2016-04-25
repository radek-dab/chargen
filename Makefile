CC=gcc
CFLAGS=-g -Wall -DDEBUG
LIBS=
NAME=chargen

$(NAME): $(NAME).o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(NAME).o: $(NAME).c
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: clean

clean:
	rm -f $(NAME) $(NAME).o
