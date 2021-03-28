SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

CFLAGS = -O3 -mtune=native -march=native

# in debian lua headers are not in include path
CPPFLAGS = -I/usr/include/luajit-2.1

LDFLAGS = -lluajit-5.1

LIB = luazlib.so

all: $(LIB)

$(OBJ): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -fPIC $<

$(LIB): $(OBJ)
	$(LD) $(LDFLAGS) -shared -o $@ $<

clean:
	rm -f $(LIB) $(OBJ)

.PHONY: clean all
