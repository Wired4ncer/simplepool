build/test_bitcoind: tests/test_bitcoind.c src/bitcoind.c src/log.c src/cjson/cJSON.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_bitcoind $(filter %.c,$^) -lcurl -lpthread
