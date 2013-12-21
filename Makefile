OBJS = ctr-nandmount.o
PACKAGES = fuse
CFLAGS = $(shell pkg-config --cflags $(PACKAGES)) -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=25
LDFLAGS = $(shell pkg-config --libs $(PACKAGES))
LIBS = 
OUTPUT = ctr-nandmount
main: $(OBJS)
	gcc $(CFLAGS) -o $(OUTPUT) $(LIBS) $(OBJS) $(LDFLAGS)
clean:
	rm -f $(OUTPUT) $(OBJS)

