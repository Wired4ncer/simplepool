build/test_coinbase: tests/test_coinbase.c src/coinbase.c src/sha256.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_coinbase $(filter %.c,$^)
