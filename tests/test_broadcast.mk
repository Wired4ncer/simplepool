build/test_broadcast: tests/test_broadcast.c src/broadcast.c src/log.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_broadcast $(filter %.c,$^) -lhiredis -lpthread
