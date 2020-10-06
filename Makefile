ifeq ($(PREFIX),)
    PREFIX := /usr/local
endif

all: fsmu

clean:
	rm -f *.o fsmu

fsmu: fsmu.c
	gcc `pkg-config fuse --cflags` fsmu.c `pkg-config fuse --libs` -o fsmu

test: fsmu
	prove t/*.t

install: fsmu
	install -d $(DESTDIR)$(PREFIX)/lib/
