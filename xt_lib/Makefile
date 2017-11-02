CFLAGS += -Wall -Werror

libxt_qmark.so: libxt_qmark.o
	$(CC) $(LDFLAGS) -shared -o $@ $^

libxt_qmark.o: libxt_qmark.c
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

# remove object files and lib when user executes "make clean"
clean:
	rm -rf *.o *.so
