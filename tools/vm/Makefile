# Makefile for vm tools

CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -m32 -static

all: page-types slabinfo
%: %.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	$(RM) page-types slabinfo
