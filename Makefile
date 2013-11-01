CC := gcc
CFLAGS := -std=gnu99 -Os -Wall
LDFLAGS := -lxcb -s
SRC := buffalo.c

buffalo: ${SRC}
	${CC} -o $@ ${SRC} ${CFLAGS} ${LDFLAGS}
	
