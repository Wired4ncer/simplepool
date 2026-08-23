build/test_pplns: tests/test_pplns.c src/pplns.c src/coinbase.c src/sha256.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_pplns $(filter %.c,$^)
