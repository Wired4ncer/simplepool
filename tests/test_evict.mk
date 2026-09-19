build/test_evict: tests/test_evict.c src/evict.c src/coinbase.c src/share.c src/sha256.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_evict $(filter %.c,$^)
