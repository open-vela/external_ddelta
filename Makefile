CFLAGS += -Wall -Wextra -O2 -g

all: ddelta_generate ddelta_apply

ddelta_generate: LDLIBS=-ldivsufsort -lz
ddelta_generate: ddelta_generate.c

ddelta_apply: LDLIBS=-lz
ddelta_apply: ddelta_apply.c
