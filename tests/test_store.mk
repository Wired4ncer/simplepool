test_store_bin = build/test_store
$(test_store_bin): tests/test_store.c src/store.c src/log.c src/pplns.c src/coinbase.c src/sha256.c
	mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $(test_store_bin) tests/test_store.c src/store.c src/log.c src/pplns.c src/coinbase.c src/sha256.c -lsqlite3 -lpthread
test_store: $(test_store_bin)
	./$(test_store_bin)
