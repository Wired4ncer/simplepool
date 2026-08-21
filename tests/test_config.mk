build/test_config: tests/test_config.c src/config.c src/log.c src/coinbase.c src/sha256.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_config $^
